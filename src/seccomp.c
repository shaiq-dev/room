/* room - the system call filter. See "System call filter" in docs/DESIGN.md. */
#include "seccomp.h"
#include "util.h"

#include <errno.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <linux/seccomp.h>

/*
 * The argument checks read the low 32 bits of an argument from its first
 * four bytes, so only little-endian 64-bit targets are supported. An x32
 * build is refused because its syscall numbers carry the x32 bit.
 */
#if defined(__x86_64__) && !defined(__ILP32__)
#define ROOM_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ROOM_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "room's seccomp filter supports only x86_64 and little-endian aarch64"
#endif

/*
 * System calls numbered 424 and up have the same number on every
 * architecture, so they can be filled in when the headers are too old to
 * know them. Dropping the rule instead would silently open a hole.
 */
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif

#define X32_SYSCALL_BIT 0x40000000u
#define CLONE_NEW_MASK 0x7E020000u

#define LOAD(field) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, field))
#define RET(action) BPF_STMT(BPF_RET | BPF_K, (action))
#define RET_ERRNO(e) RET(SECCOMP_RET_ERRNO | ((e) & SECCOMP_RET_DATA))

// clang-format off: one BPF instruction per line, which clang-format would join
/* Each rule is a self-contained block that leaves the call number in A. */
#define DENY(sys, e) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (sys), 0, 1), \
    RET_ERRNO(e)

/*
 * Denies sys when the low half of argument i passes the test (JEQ or JSET)
 * against k. A different call and an argument that fails the test both land
 * on the final reload of the call number.
 */
#define DENY_ARG(sys, i, test, k, e) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (sys), 0, 3), \
    LOAD(args[i]), \
    BPF_JUMP(BPF_JMP | (test) | BPF_K, (k), 0, 1), \
    RET_ERRNO(e), \
    LOAD(nr)

/* Denies sys unless the low half of argument i equals k. */
#define ONLY_ARG(sys, i, k, e) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (sys), 0, 3), \
    LOAD(args[i]), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (k), 1, 0), \
    RET_ERRNO(e), \
    LOAD(nr)

/*
 * socket() only for the families the program in the box needs, and netlink
 * only for NETLINK_ROUTE, which libc uses to list the interfaces. Every jump
 * stays inside the block and lands on its final reload of the call number.
 */
#define SOCKET_RULE \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 9), \
    LOAD(args[0]), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 7, 0), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 6, 0), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 5, 0), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 1, 0), \
    RET_ERRNO(EAFNOSUPPORT), \
    LOAD(args[2]), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NETLINK_ROUTE, 1, 0), \
    RET_ERRNO(EPROTONOSUPPORT), \
    LOAD(nr)
// clang-format on

static struct sock_filter filter[] = {
    LOAD(arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ROOM_AUDIT_ARCH, 1, 0),
    RET(SECCOMP_RET_KILL_PROCESS),
    LOAD(nr),
#ifdef __x86_64__
    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, X32_SYSCALL_BIT, 0, 1),
    RET_ERRNO(ENOSYS),
#endif

    DENY(__NR_unshare, EPERM),
    DENY(__NR_setns, EPERM),
    DENY_ARG(__NR_clone, 0, BPF_JSET, CLONE_NEW_MASK, EPERM),

    /* ENOSYS makes libc fall back to clone, where the flags can be checked. */
    DENY(__NR_clone3, ENOSYS),

    DENY(__NR_add_key, ENOSYS),
    DENY(__NR_request_key, ENOSYS),
    DENY(__NR_keyctl, ENOSYS),

    DENY(__NR_io_uring_setup, ENOSYS),
    DENY(__NR_io_uring_enter, ENOSYS),
    DENY(__NR_io_uring_register, ENOSYS),

    SOCKET_RULE,
    ONLY_ARG(__NR_socketpair, 0, AF_UNIX, EAFNOSUPPORT),
    DENY(__NR_fanotify_init, EPERM),
    DENY_ARG(__NR_ioctl, 1, BPF_JEQ, TIOCSTI, EPERM),
    DENY_ARG(__NR_ioctl, 1, BPF_JEQ, TIOCLINUX, EPERM),

    DENY(__NR_bpf, EPERM),
    DENY(__NR_perf_event_open, EPERM),
    DENY(__NR_userfaultfd, EPERM),

    RET(SECCOMP_RET_ALLOW),
};

void seccomp_install(void)
{
    struct sock_fprog prog = {
        .len = sizeof(filter) / sizeof(filter[0]),
        .filter = filter,
    };

    /* The caller is no longer root, so the kernel only takes a filter with this set. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        die("Cannot set no_new_privs: %m");
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0)
        die("Cannot install the seccomp filter: %m");
    msg("Installed seccomp filter, %u instructions\n", prog.len);
}
