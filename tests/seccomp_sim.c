/*
 * Runs seccomp.c's filter array through a tiny cBPF interpreter against a
 * table of system calls and the results they must get. Built natively it
 * checks the filter for this machine. Cross compiled and run under
 * emulation it checks the other architecture's filter.
 *
 * Build: gcc -O2 -D_GNU_SOURCE -o seccomp_sim seccomp_sim.c ../src/util.c
 */
#include "../src/seccomp.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

static uint32_t run(const struct seccomp_data *d)
{
    const struct sock_filter *f = filter;
    unsigned n = sizeof(filter) / sizeof(filter[0]), pc = 0;
    uint32_t a = 0;

    while (pc < n) {
        const struct sock_filter *i = &f[pc++];
        switch (i->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (i->k % 4 || i->k + 4 > sizeof(*d)) {
                printf("bad load %u\n", i->k);
                exit(1);
            }
            memcpy(&a, (const char *)d + i->k, 4);
            break;
        case BPF_JMP | BPF_JEQ | BPF_K: pc += a == i->k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JSET | BPF_K: pc += a & i->k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JGE | BPF_K: pc += a >= i->k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JGT | BPF_K: pc += a > i->k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JA: pc += i->k; break;
        case BPF_RET | BPF_K: return i->k;
        default: printf("unknown opcode 0x%x at %u\n", i->code, pc - 1); exit(1);
        }
    }
    printf("fell off the end\n");
    exit(1);
}

#define ALLOW SECCOMP_RET_ALLOW
#define KILL SECCOMP_RET_KILL_PROCESS
#define ERR(e) (SECCOMP_RET_ERRNO | (e))

/* Numbers and constants are typed in from the kernel's tables, not taken from headers. */
#ifdef __x86_64__
#define ARCH 0xC000003Eu
#define FOREIGN 0x40000003u /* i386 */
enum {
    UNSHARE = 272,
    SETNS = 308,
    CLONE = 56,
    CLONE3 = 435,
    ADD_KEY = 248,
    REQUEST_KEY = 249,
    KEYCTL = 250,
    SOCKET = 41,
    SOCKETPAIR = 53,
    IOCTL = 16,
    BPF = 321,
    PERF = 298,
    UFFD = 323,
    FANOTIFY_INIT = 300,
    READ = 0,
    WRITE = 1,
    FLOCK = 73,
    FCNTL = 72,
    EXECVE = 59,
    MMAP = 9
};
#else
#define ARCH 0xC00000B7u
#define FOREIGN 0x40000028u /* 32-bit arm */
enum {
    UNSHARE = 97,
    SETNS = 268,
    CLONE = 220,
    CLONE3 = 435,
    ADD_KEY = 217,
    REQUEST_KEY = 218,
    KEYCTL = 219,
    SOCKET = 198,
    SOCKETPAIR = 199,
    IOCTL = 29,
    BPF = 280,
    PERF = 241,
    UFFD = 282,
    FANOTIFY_INIT = 262,
    READ = 63,
    WRITE = 64,
    FLOCK = 32,
    FCNTL = 25,
    EXECVE = 221,
    MMAP = 222
};
#endif

/* a2 comes last so the cases that don't need it can leave it out, it's 0 then */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
struct tc {
    const char *name;
    uint32_t arch;
    int nr;
    uint64_t a0, a1;
    uint32_t want;
    uint64_t a2;
};

enum {
    AF_UNSPEC_ = 0,
    AF_UNIX_ = 1,
    AF_INET_ = 2,
    AF_INET6_ = 10,
    AF_NETLINK_ = 16,
    AF_PACKET_ = 17,
    AF_BLUETOOTH_ = 31,
    AF_ALG_ = 38,
    AF_VSOCK_ = 40,
    NETLINK_ROUTE_ = 0,
    NETLINK_SOCK_DIAG_ = 4,
    NETLINK_AUDIT_ = 9,
    NETLINK_KOBJECT_UEVENT_ = 15
};

static const struct tc tcs[] = {
    { "unshare(NEWUSER)", ARCH, UNSHARE, 0x10000000, 0, ERR(1) },
    { "unshare(0)", ARCH, UNSHARE, 0, 0, ERR(1) },
    { "setns", ARCH, SETNS, 3, 0, ERR(1) },
    { "clone NEWNS", ARCH, CLONE, 0x00020000 | 17, 0, ERR(1) },
    { "clone NEWCGROUP", ARCH, CLONE, 0x02000000 | 17, 0, ERR(1) },
    { "clone NEWUTS", ARCH, CLONE, 0x04000000 | 17, 0, ERR(1) },
    { "clone NEWIPC", ARCH, CLONE, 0x08000000 | 17, 0, ERR(1) },
    { "clone NEWUSER", ARCH, CLONE, 0x10000000 | 17, 0, ERR(1) },
    { "clone NEWPID", ARCH, CLONE, 0x20000000 | 17, 0, ERR(1) },
    { "clone NEWNET", ARCH, CLONE, 0x40000000 | 17, 0, ERR(1) },
    { "clone NEWNET, high junk", ARCH, CLONE, 0xffffffff40000000ull, 0, ERR(1) },
    { "clone3", ARCH, CLONE3, 0, 88, ERR(38) },
    { "add_key", ARCH, ADD_KEY, 0, 0, ERR(38) },
    { "request_key", ARCH, REQUEST_KEY, 0, 0, ERR(38) },
    { "keyctl", ARCH, KEYCTL, 0, 0, ERR(38) },
    { "io_uring_setup", ARCH, 425, 0, 0, ERR(38) },
    { "io_uring_enter", ARCH, 426, 0, 0, ERR(38) },
    { "io_uring_register", ARCH, 427, 0, 0, ERR(38) },
    { "socket(AF_VSOCK)", ARCH, SOCKET, AF_VSOCK_, 1, ERR(97) },
    { "socket(AF_VSOCK | 1<<32)", ARCH, SOCKET, AF_VSOCK_ | 1ull << 32, 1, ERR(97) },
    { "socket(AF_UNSPEC)", ARCH, SOCKET, AF_UNSPEC_, 1, ERR(97) },
    { "socket(AF_PACKET)", ARCH, SOCKET, AF_PACKET_, 2, ERR(97) },
    { "socket(AF_BLUETOOTH)", ARCH, SOCKET, AF_BLUETOOTH_, 1, ERR(97) },
    { "socket(AF_ALG)", ARCH, SOCKET, AF_ALG_, 5, ERR(97) },
    { "socket(0xffffffff)", ARCH, SOCKET, 0xffffffffu, 1, ERR(97) },
    { "socket(3, AF_AX25)", ARCH, SOCKET, 3, 1, ERR(97) },
    { "socket(1<<32 | AF_PACKET)", ARCH, SOCKET, 1ull << 32 | AF_PACKET_, 2, ERR(97) },
    { "socket(AF_NETLINK, SOCK_DIAG)", ARCH, SOCKET, AF_NETLINK_, 3, ERR(93), NETLINK_SOCK_DIAG_ },
    { "socket(AF_NETLINK, AUDIT)", ARCH, SOCKET, AF_NETLINK_, 3, ERR(93), NETLINK_AUDIT_ },
    { "socket(AF_NETLINK, KOBJECT_UEVENT)", ARCH, SOCKET, AF_NETLINK_, 2, ERR(93),
      NETLINK_KOBJECT_UEVENT_ },
    { "socket(AF_NETLINK, 1<<32 | AUDIT)", ARCH, SOCKET, AF_NETLINK_, 3, ERR(93),
      1ull << 32 | NETLINK_AUDIT_ },
    { "socketpair(AF_INET)", ARCH, SOCKETPAIR, AF_INET_, 1, ERR(97) },
    { "socketpair(AF_NETLINK)", ARCH, SOCKETPAIR, AF_NETLINK_, 3, ERR(97) },
    { "socketpair(1<<32 | AF_INET)", ARCH, SOCKETPAIR, 1ull << 32 | AF_INET_, 1, ERR(97) },
    { "fanotify_init", ARCH, FANOTIFY_INIT, 0, 0, ERR(1) },
    { "ioctl TIOCSTI", ARCH, IOCTL, 0, 0x5412, ERR(1) },
    { "ioctl TIOCLINUX", ARCH, IOCTL, 0, 0x541C, ERR(1) },
    { "ioctl TIOCSTI | 1<<32", ARCH, IOCTL, 0, 0x5412 | 1ull << 32, ERR(1) },
    { "bpf", ARCH, BPF, 0, 0, ERR(1) },
    { "perf_event_open", ARCH, PERF, 0, 0, ERR(1) },
    { "userfaultfd", ARCH, UFFD, 0, 0, ERR(1) },
    { "clone fork flags", ARCH, CLONE, 0x01200011, 0, ALLOW },
    { "clone thread flags", ARCH, CLONE, 0x003d0f00, 0, ALLOW },
    { "clone 0x80 (NEWTIME bit = CSIGNAL)", ARCH, CLONE, 0x80, 0, ALLOW },
    { "socket(AF_INET)", ARCH, SOCKET, AF_INET_, 1, ALLOW },
    { "socket(AF_UNIX)", ARCH, SOCKET, AF_UNIX_, 1, ALLOW },
    { "socket(AF_INET6)", ARCH, SOCKET, AF_INET6_, 1, ALLOW },
    { "socket(1<<32 | AF_INET)", ARCH, SOCKET, 1ull << 32 | AF_INET_, 1, ALLOW },
    { "socket(AF_UNIX), protocol 9", ARCH, SOCKET, AF_UNIX_, 1, ALLOW, 9 },
    { "socket(AF_NETLINK, ROUTE)", ARCH, SOCKET, AF_NETLINK_, 3, ALLOW, NETLINK_ROUTE_ },
    { "socket(AF_NETLINK, 1<<32 | ROUTE)", ARCH, SOCKET, AF_NETLINK_, 3, ALLOW,
      1ull << 32 | NETLINK_ROUTE_ },
    { "socketpair(AF_UNIX)", ARCH, SOCKETPAIR, AF_UNIX_, 1, ALLOW },
    { "socketpair(1<<32 | AF_UNIX)", ARCH, SOCKETPAIR, 1ull << 32 | AF_UNIX_, 1, ALLOW },
    { "ioctl TIOCGWINSZ", ARCH, IOCTL, 0, 0x5413, ALLOW },
    { "ioctl FIONREAD", ARCH, IOCTL, 0, 0x541B, ALLOW },
    { "ioctl, TIOCSTI in arg0", ARCH, IOCTL, 0x5412, 0x5413, ALLOW },
    { "read", ARCH, READ, 0, 0, ALLOW },
    { "write", ARCH, WRITE, 1, 0, ALLOW },
    { "flock", ARCH, FLOCK, 3, 2, ALLOW },
    { "fcntl", ARCH, FCNTL, 3, 6, ALLOW },
    { "execve", ARCH, EXECVE, 0, 0, ALLOW },
    { "mmap", ARCH, MMAP, 0, 4096, ALLOW },
    { "nr 1000", ARCH, 1000, 0, 0, ALLOW },
    /* foreign arch is killed even for harmless calls */
    { "foreign arch, nr 0", FOREIGN, 0, 0, 0, KILL },
    { "foreign arch, unshare nr", FOREIGN, UNSHARE, 0, 0, KILL },
    { "arch 0", 0, READ, 0, 0, KILL },
#ifdef __x86_64__
    { "x32 read", ARCH, 0x40000000 | 0, 0, 0, ERR(38) },
    { "x32 clone NEWUSER", ARCH, 0x40000000 | 56, 0x10000000, 0, ERR(38) },
    { "x32 ioctl TIOCGWINSZ", ARCH, 0x40000000 | 16, 0, 0x5413, ERR(38) },
    { "nr 0xffffffff (x32 bit set)", ARCH, -1, 0, 0, ERR(38) },
#else
    { "nr with 0x40000000 (no x32 on arm64)", ARCH, 0x40000000, 0, 0, ALLOW },
#endif
};

int main(void)
{
    int fails = 0;
    unsigned n = sizeof(tcs) / sizeof(tcs[0]);

#ifdef __x86_64__
    printf("simulating the x86_64 filter (%zu instructions)\n", sizeof(filter) / sizeof(filter[0]));
#else
    printf("simulating the aarch64 filter (%zu instructions)\n",
           sizeof(filter) / sizeof(filter[0]));
#endif
    for (unsigned i = 0; i < n; i++) {
        struct seccomp_data d = { .nr = tcs[i].nr,
                                  .arch = tcs[i].arch,
                                  .args = { tcs[i].a0, tcs[i].a1, tcs[i].a2 } };
        uint32_t got = run(&d);
        int ok = got == tcs[i].want;
        fails += !ok;
        printf("%-4s %-38s want 0x%08x got 0x%08x\n", ok ? "ok" : "FAIL", tcs[i].name, tcs[i].want,
               got);
    }
    printf("%u cases, %d failed\n", n, fails);
    return fails != 0;
}
