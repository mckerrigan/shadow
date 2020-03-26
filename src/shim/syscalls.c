// Defines functions for libc functions that are trivial wrappers around a
// system call.

// We avoid including headers that define the functions we're wrapping, as that
// simplifies things a bit, especially for functions that would otherwise use
// varargs to implement optional arguments.
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/select.h>
#include <stdarg.h>

#include "shim.h"
#include "shim-event.h"

// Handle to the real syscall function, initialized once at load-time for
// thread-safety.
static long (*_real_syscall)(long n, ...);
__attribute__((constructor(SHIM_CONSTRUCTOR_PRIORITY - 1))) static void
_init_real_syscall() {
    _real_syscall = dlsym(RTLD_NEXT, "syscall");
    assert(_real_syscall != NULL);
}

static long shadow_retval_to_errno(long retval) {
    if (retval >= 0) {
        return retval;
    } else {
        errno = -retval;
        return -1;
    }
}

static SysCallReg _shadow_syscall_event(const ShimEvent* ev) {
    shim_disableInterposition();

    const int fd = shim_thisThreadEventFD();
    SHD_SHIM_LOG("sending event on %d\n", fd);
    shimevent_sendEvent(fd, ev);

    SHD_SHIM_LOG("waiting for event on %d\n", fd);
    ShimEvent res;
    shimevent_recvEvent(fd, &res);
    SHD_SHIM_LOG("got response on %d\n", fd);
    assert(res.event_id == SHD_SHIM_EVENT_SYSCALL_COMPLETE);

    shim_enableInterposition();
    return res.event_data.syscall_complete.retval;
}

static long _shadow_syscall(ShimEvent* event) {
    SysCallArgs* args = &event->event_data.syscall.syscall_args;

    // Some temporary special cases.
    switch (args->number) {
        case SYS_nanosleep: {
            // FIXME: temporarily using registers until memory APIs are in
            // place.
            const struct timespec* req = (void*)args->args[0].as_u64;
            struct timespec* res = (void*)args->args[1].as_u64;
            args->args[0].as_i64 = req->tv_sec;
            args->args[1].as_i64 = req->tv_nsec;
            SysCallReg rv = _shadow_syscall_event(event);
            return shadow_retval_to_errno(rv.as_i64);
        }
        case SYS_clock_gettime: {
            // FIXME: temporarily using registers until memory APIs are in
            // place.
            clockid_t clk_id = args->args[0].as_u64;
            struct timespec* res = (void*)args->args[1].as_u64;
            SysCallReg rv = _shadow_syscall_event(event);

            // In the meantime, shadow passes the result as literal nanos.
            const int64_t nano = 1000000000LL;
            res->tv_sec = rv.as_i64 / nano;
            res->tv_nsec = rv.as_i64 % nano;
            return 0;
        }
    }
    // Common path
    return shadow_retval_to_errno(_shadow_syscall_event(event).as_i64);
}

long syscall(long n, ...) {
    ShimEvent e;
    e.event_id = SHD_SHIM_EVENT_SYSCALL;
    e.event_data.syscall.syscall_args.number = n;
    va_list(args);
    va_start(args, n);
    SysCallReg* regs = e.event_data.syscall.syscall_args.args;
    for (int i = 0; i < 6; ++i) {
        regs[i].as_u64 = va_arg(args, uint64_t);
    }
    va_end(args);
    if (shim_interpositionEnabled()) {
        return _shadow_syscall(&e);
    } else {
        return _real_syscall(
            n, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]);
    }
}

// General-case macro for defining a thin wrapper function `fnname` that invokes
// the syscall `sysname`.
#define REMAP(type, fnname, sysname, params, ...)                              \
    type fnname params {                                                       \
        if (shim_interpositionEnabled())                                       \
            return (type)syscall(SYS_##sysname, __VA_ARGS__);                  \
        else                                                                   \
            return (type)_real_syscall(SYS_##sysname, __VA_ARGS__);            \
    }

// Specialization of REMAP for defining a function `fnname` that invokes a
// syscall of the same name.
#define NOREMAP(type, fnname, params, ...) REMAP(type, fnname, fnname, params,  __VA_ARGS__)

// Sorted by function name (e.g. using `sort -t',' -k2`).
// clang-format off

NOREMAP(int, bind, (int a, const struct sockaddr* b, socklen_t c), a,b,c);
NOREMAP(int, clock_gettime, (clockid_t a, struct timespec* b), a,b);
NOREMAP(int, connect, (int a, const struct sockaddr* b, socklen_t c), a,b,c);
NOREMAP(int, nanosleep, (const struct timespec* a, struct timespec* b), a,b);
NOREMAP(ssize_t, recvfrom, (int a, void* b, size_t c, int d, struct sockaddr* e, socklen_t* f), a,b,c,d,e,f);
REMAP(ssize_t, recv, recvfrom, (int a, void* b, size_t c, int d), a,b,c,d,NULL,NULL);
NOREMAP(ssize_t, recvmsg, (int a, struct msghdr* b, int c), a,b,c);
NOREMAP(ssize_t, sendmsg, (int a, const struct msghdr* b, int c), a,b,c);
REMAP(ssize_t, send, sendto, (int a, const void* b, size_t c, int d), a,b,c,d,NULL,0);
NOREMAP(ssize_t, sendto, (int a, const void* b, size_t c, int d, const struct sockaddr* e, socklen_t f), a,b,c,d,e,f);
NOREMAP(int, socket, (int a, int b, int c), a,b,c);

// clang-format on