#include "shim/shim_event.h"

#include <unistd.h>

int shadow_set_ptrace_allow_native_syscalls(bool val) {
    return syscall(SYS_shadow_set_ptrace_allow_native_syscalls, val);
}

int shadow_get_ipc_blk(ShMemBlockSerialized* ipc_blk_serialized) {
    return syscall(SYS_shadow_get_ipc_blk, ipc_blk_serialized);
}

static inline void shim_determinedSend(int sock_fd, const void* ptr,
                                       size_t nbytes) {
    const char* buf = (const char*)(ptr);
    size_t nbytes_sent = 0;
    ssize_t rc = 0;

    while (nbytes_sent != nbytes) {
        rc = send(sock_fd, buf + nbytes_sent, (nbytes - nbytes_sent), 0);
        if (rc > -1) {
            nbytes_sent += rc;
        }
    }
}

static inline void shim_determinedRecv(int sock_fd, void* ptr, size_t nbytes) {
    char* buf = (char*)(ptr);
    size_t nbytes_recv = 0;
    ssize_t rc = 0;

    while (nbytes_recv != nbytes) {
        rc = recv(sock_fd, buf + nbytes_recv, (nbytes - nbytes_recv), 0);
        if (rc > -1) {
            nbytes_recv += rc;
        }
    }
}

static inline void shim_sendUint32_t(int sock_fd, uint32_t value) {
    value = htonl(value);
    shim_determinedSend(sock_fd, &value, sizeof(uint32_t));
}

static inline uint32_t shim_recvUint32_t(int sock_fd) {
    uint32_t value;
    shim_determinedRecv(sock_fd, &value, sizeof(uint32_t));
    return ntohl(value);
}