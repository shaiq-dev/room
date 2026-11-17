/*
 * Test helper for tests/run.sh. It runs in the box as the untrusted program,
 * and some tests run it outside the box too. One subcommand per behaviour,
 * see usage() for the list.
 *
 * Build: gcc -O2 -static -pthread -o t t.c
 * Only libc is used so the static binary runs in a box with no libraries.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>

/* Newer call numbers are the same on x86_64 and aarch64. */
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif
#ifndef SYS_io_uring_enter
#define SYS_io_uring_enter 426
#endif
#ifndef SYS_io_uring_register
#define SYS_io_uring_register 427
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif
#ifndef TIOCSTI
#define TIOCSTI 0x5412
#endif
#ifndef TIOCLINUX
#define TIOCLINUX 0x541C
#endif

extern char **environ;

static const char *ename(int e)
{
    static char buf[16];
    // clang-format off: a table of names, which clang-format would indent as code
#define E(n) case n: return #n;
    switch (e) {
    case 0: return "OK";
    E(EPERM) E(ENOENT) E(ESRCH) E(EINTR) E(EIO) E(ENXIO) E(E2BIG) E(ENOEXEC)
    E(EBADF) E(ECHILD) E(EAGAIN) E(ENOMEM) E(EACCES) E(EFAULT) E(EBUSY)
    E(EEXIST) E(EXDEV) E(ENODEV) E(ENOTDIR) E(EISDIR) E(EINVAL) E(ENFILE)
    E(EMFILE) E(ENOTTY) E(ETXTBSY) E(EFBIG) E(ENOSPC) E(ESPIPE) E(EROFS)
    E(EMLINK) E(EPIPE) E(ERANGE) E(ENAMETOOLONG) E(ENOSYS) E(ELOOP)
    E(ENOTSOCK) E(EPROTONOSUPPORT) E(EOPNOTSUPP) E(EAFNOSUPPORT)
    E(EADDRNOTAVAIL) E(ENETDOWN) E(ENETUNREACH) E(ECONNRESET) E(ETIMEDOUT)
    E(ECONNREFUSED) E(EHOSTUNREACH) E(EINPROGRESS) E(EDQUOT)
    }
#undef E
    // clang-format on
    snprintf(buf, sizeof buf, "E%d", e);
    return buf;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static double cpu_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static volatile unsigned long sink;

static void burn(void)
{
    for (int i = 0; i < 100000; i++)
        sink++;
}

static void copy_fd(int in, int out)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n)
            break;
}

/* Forks up to n children, kills them and prints "created=N err=E" with the first fork error. */
static int do_fork(int n)
{
    pid_t *kids = calloc(n, sizeof *kids);
    int made = 0, err = 0;
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) {
            pause();
            _exit(0);
        }
        if (p < 0) {
            if (!err)
                err = errno;
            continue;
        }
        kids[made++] = p;
    }
    for (int i = 0; i < made; i++) {
        kill(kids[i], SIGKILL);
        waitpid(kids[i], NULL, 0);
    }
    printf("created=%d err=%s\n", made, err ? ename(err) : "none");
    return 0;
}

static void *thread_fn(void *arg)
{
    (void)arg;
    usleep(200000);
    return NULL;
}

static int do_threads(int n)
{
    pthread_t *th = calloc(n, sizeof *th);
    int made = 0, err = 0;
    for (int i = 0; i < n; i++) {
        int e = pthread_create(&th[made], NULL, thread_fn, NULL);
        if (e) {
            if (!err)
                err = e;
            continue;
        }
        made++;
    }
    for (int i = 0; i < made; i++)
        pthread_join(th[i], NULL);
    printf("threads=%d err=%s\n", made, err ? ename(err) : "none");
    return made == n ? 0 : 1;
}

static int do_bigfile(long kb)
{
    char buf[1024];
    memset(buf, 'x', sizeof buf);
    int fd = open("big", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("bigfile open %s\n", ename(errno));
        return 1;
    }
    for (long i = 0; i < kb; i++) {
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) {
            printf("bigfile %s after %ld KB\n", ename(errno), i);
            return 1;
        }
    }
    close(fd);
    printf("bigfile OK\n");
    return 0;
}

/* Prints "PATH RESULT" for each path. Never writes data, so a test that
 * goes wrong can't damage a file it was not supposed to open. */
static int do_paths(const char *op, char **paths)
{
    for (; *paths; paths++) {
        int e = 0;
        if (!strcmp(op, "stat")) {
            struct stat st;
            if (lstat(*paths, &st) < 0)
                e = errno;
        } else {
            int fd = open(*paths, !strcmp(op, "write") ? O_WRONLY | O_CREAT : O_RDONLY, 0666);
            if (fd < 0) {
                e = errno;
            } else {
                char c;
                if (!strcmp(op, "read") && read(fd, &c, 1) < 0)
                    e = errno;
                close(fd);
            }
        }
        printf("%s %s\n", *paths, ename(e));
    }
    return 0;
}

static int do_ids(void)
{
    uid_t r, e, s;
    gid_t gr, ge, gs;
    gid_t groups[256];
    getresuid(&r, &e, &s);
    getresgid(&gr, &ge, &gs);
    int n = getgroups(256, groups);
    printf("uid=%d %d %d\n", (int)r, (int)e, (int)s);
    printf("gid=%d %d %d\n", (int)gr, (int)ge, (int)gs);
    printf("groups=");
    for (int i = 0; i < n; i++)
        printf("%s%d", i ? " " : "", (int)groups[i]);
    printf("\n");
    printf("session=%s\n", getsid(0) == getpid() ? "leader" : "member");

    FILE *f = fopen("/proc/self/status", "r");
    if (!f) {
        printf("status=%s\n", ename(errno));
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        static const char *keys[] = { "CapEff:", "CapPrm:",     "CapBnd:",
                                      "CapAmb:", "NoNewPrivs:", "Seccomp:" };
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
            size_t len = strlen(keys[i]);
            if (!strncmp(line, keys[i], len)) {
                char *v = line + len;
                v += strspn(v, " \t");
                v[strcspn(v, "\n")] = 0;
                printf("%.*s=%s\n", (int)len - 1, keys[i], v);
            }
        }
    }
    fclose(f);
    return 0;
}

static int do_fds(void)
{
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536)
        max = 65536;
    printf("fds=");
    int first = 1;
    for (int fd = 0; fd < max; fd++) {
        if (fcntl(fd, F_GETFD) != -1) {
            printf("%s%d", first ? "" : " ", fd);
            first = 0;
        }
    }
    printf("\n");
    return 0;
}

static int do_env(void)
{
    size_t n = 0;
    while (environ[n])
        n++;
    char **v = malloc((n + 1) * sizeof *v);
    memcpy(v, environ, n * sizeof *v);
    qsort(v, n, sizeof *v, cmp_str);
    for (size_t i = 0; i < n; i++)
        printf("%s\n", v[i]);
    return 0;
}

/* Calls that may create a child return 0 in the child, which must leave. */
static long child_guard(long ret)
{
    if (ret == 0)
        _exit(0);
    if (ret > 0)
        waitpid(ret, NULL, 0);
    return ret;
}

struct clone_args_v0 {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
};

/* Tries one system call. Returns 0, its errno, -1 to skip it or -2 for an unknown name. */
static int try_sys(const char *name)
{
    long r;
    char buf[256] = { 0 };
    errno = 0;
    if (!strcmp(name, "unshare-user")) {
        r = syscall(SYS_unshare, CLONE_NEWUSER);
    } else if (!strcmp(name, "unshare-0")) {
        r = syscall(SYS_unshare, 0);
    } else if (!strcmp(name, "setns")) {
        r = syscall(SYS_setns, 0, 0);
    } else if (!strcmp(name, "clone-newuser")) {
        r = child_guard(syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0));
    } else if (!strcmp(name, "clone-newns")) {
        r = child_guard(syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0));
    } else if (!strcmp(name, "clone-plain")) {
        r = child_guard(syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0));
    } else if (!strcmp(name, "clone3")) {
        struct clone_args_v0 ca = { .exit_signal = SIGCHLD };
        r = child_guard(syscall(SYS_clone3, &ca, sizeof ca));
    } else if (!strcmp(name, "add_key")) {
        r = syscall(SYS_add_key, "user", "room-test", "x", 1, -2);
    } else if (!strcmp(name, "request_key")) {
        r = syscall(SYS_request_key, "user", "room-test", NULL, 0);
    } else if (!strcmp(name, "keyctl")) {
        r = syscall(SYS_keyctl, 0, -2, 0);
    } else if (!strcmp(name, "io_uring_setup")) {
        r = syscall(SYS_io_uring_setup, 1, buf);
    } else if (!strcmp(name, "io_uring_enter")) {
        r = syscall(SYS_io_uring_enter, -1, 0, 0, 0, NULL, 0);
    } else if (!strcmp(name, "io_uring_register")) {
        r = syscall(SYS_io_uring_register, -1, 0, NULL, 0);
    } else if (!strcmp(name, "vsock")) {
        r = syscall(SYS_socket, AF_VSOCK, SOCK_STREAM, 0);
    } else if (!strcmp(name, "socket-inet")) {
        r = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    } else if (!strcmp(name, "socket-unix")) {
        r = syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    } else if (!strcmp(name, "socket-inet6")) {
        r = syscall(SYS_socket, AF_INET6, SOCK_STREAM, 0);
    } else if (!strcmp(name, "packet")) {
        r = syscall(SYS_socket, AF_PACKET, SOCK_DGRAM, 0);
    } else if (!strcmp(name, "alg")) {
        r = syscall(SYS_socket, AF_ALG, SOCK_SEQPACKET, 0);
    } else if (!strcmp(name, "netlink-route")) {
        r = syscall(SYS_socket, AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    } else if (!strcmp(name, "netlink-diag")) {
        r = syscall(SYS_socket, AF_NETLINK, SOCK_RAW, NETLINK_SOCK_DIAG);
    } else if (!strcmp(name, "socketpair-unix")) {
        int sv[2];
        r = syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv);
    } else if (!strcmp(name, "socketpair-inet")) {
        int sv[2];
        r = syscall(SYS_socketpair, AF_INET, SOCK_STREAM, 0, sv);
    } else if (!strcmp(name, "fanotify")) {
        r = syscall(SYS_fanotify_init, 0, 0);
    } else if (!strcmp(name, "getifaddrs")) {
        struct ifaddrs *ifa;
        r = getifaddrs(&ifa);
        if (r == 0)
            freeifaddrs(ifa);
    } else if (!strcmp(name, "tiocsti")) {
        r = syscall(SYS_ioctl, 0, TIOCSTI, "x");
    } else if (!strcmp(name, "tioclinux")) {
        r = syscall(SYS_ioctl, 0, TIOCLINUX, buf);
    } else if (!strcmp(name, "bpf")) {
        r = syscall(SYS_bpf, 0, buf, 0);
    } else if (!strcmp(name, "perf_event_open")) {
        r = syscall(SYS_perf_event_open, buf, 0, -1, -1, 0);
    } else if (!strcmp(name, "userfaultfd")) {
        r = syscall(SYS_userfaultfd, 0);
    } else if (!strcmp(name, "x32")) {
#ifdef __x86_64__
        r = syscall(SYS_getpid | 0x40000000);
#else
        return -1;
#endif
    } else {
        return -2;
    }
    return r < 0 ? errno : 0;
}

static int do_sys(char **names)
{
    for (; *names; names++) {
        int e = try_sys(*names);
        printf("%s %s\n", *names, e == -1 ? "SKIP" : e == -2 ? "UNKNOWN" : ename(e));
    }
    return 0;
}

/* A system call through the i386 entry point. Should never return. */
static int do_foreign(void)
{
#ifdef __x86_64__
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(20L) : "memory");
    printf("foreign returned %ld\n", ret);
    return 0;
#else
    printf("foreign SKIP\n");
    return 0;
#endif
}

static int do_net(void)
{
    struct if_nameindex *ifs = if_nameindex();
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (!ifs)
        printf("if_nameindex %s\n", ename(errno));
    for (struct if_nameindex *i = ifs; i && i->if_index; i++) {
        struct ifreq ifr = { 0 };
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", i->if_name);
        const char *state = "unknown";
        if (s >= 0 && ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
            state = (ifr.ifr_flags & IFF_UP) ? "up" : "down";
        printf("if %s %s\n", i->if_name, state);
    }
    if (s >= 0)
        close(s);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        printf("connect %s\n", ename(errno));
        return 0;
    }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(80) };
    inet_pton(AF_INET, "1.1.1.1", &sa.sin_addr);
    int e = 0;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        e = errno;
        if (e == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, 2000) == 0) {
                e = ETIMEDOUT;
            } else {
                socklen_t len = sizeof e;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len);
            }
        }
    }
    printf("connect %s\n", ename(e));
    return 0;
}

static int do_procs(void)
{
    DIR *d = opendir("/proc");
    if (!d) {
        printf("procs %s\n", ename(errno));
        return 1;
    }
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        char path[300], line[256], name[64] = "?";
        int uid = -1;
        snprintf(path, sizeof path, "/proc/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof line, f)) {
                if (!strncmp(line, "Name:", 5))
                    sscanf(line + 5, "%63s", name);
                if (!strncmp(line, "Uid:", 4))
                    sscanf(line + 4, "%d", &uid);
            }
            fclose(f);
        }
        printf("proc %s %d %s\n", de->d_name, uid, name);
        count++;
    }
    closedir(d);
    printf("count=%d\n", count);
    return 0;
}

static int do_ls(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        printf("%s\n", ename(errno));
        return 1;
    }
    char *names[1024];
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 1024)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (size_t i = 0; i < n; i++)
        printf("%s%s", i ? " " : "", names[i]);
    printf("\n");
    return 0;
}

static void print_limit(const char *name, int res)
{
    struct rlimit rl;
    getrlimit(res, &rl);
    if (rl.rlim_cur == RLIM_INFINITY)
        printf("%s unlimited\n", name);
    else
        printf("%s %llu\n", name, (unsigned long long)rl.rlim_cur);
}

static int usage(void)
{
    fprintf(stderr, "usage: t COMMAND [ARGS]\n"
                    "  exit N | spin | spin-ms N | sleep SEC | alloc MB | abort | segv\n"
                    "  fork N | forkbomb | threads N | bigfile KB | openmany N | daemon\n"
                    "  write PATH... | read PATH... | stat PATH... | cat PATH | ls DIR\n"
                    "  ids | fds | env | cwd | limits | procs | net | flock | mklinks\n"
                    "  sys NAME... | foreign | stdin-echo [MSG] | stderr MSG | echo ARGS...\n"
                    "  deep N [DIR] | wide DEPTH N | oomchild MB | ignore SIG,... PROG [ARGS]\n"
                    "  cpus\n");
    return 100;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    const char *arg = argc > 2 ? argv[2] : NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!strcmp(cmd, "exit") && arg)
        return atoi(arg);
    if (!strcmp(cmd, "spin"))
        for (;;)
            burn();
    if (!strcmp(cmd, "spin-ms") && arg) {
        double until = atof(arg);
        while (cpu_ms() < until)
            burn();
        return 0;
    }
    if (!strcmp(cmd, "sleep") && arg) {
        double sec = atof(arg);
        struct timespec ts = { (time_t)sec, (long)((sec - (time_t)sec) * 1e9) };
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
            ;
        return 0;
    }
    if (!strcmp(cmd, "alloc") && arg) {
        size_t size = (size_t)atol(arg) << 20;
        char *p = malloc(size);
        if (!p) {
            printf("alloc failed\n");
            return 3;
        }
        for (size_t i = 0; i < size; i += 4096)
            p[i] = 1;
        printf("alloc OK\n");
        return 0;
    }
    if (!strcmp(cmd, "abort"))
        abort();
    if (!strcmp(cmd, "segv")) {
        volatile int *volatile p = NULL;
        *p = 1;
        return 0;
    }
    if (!strcmp(cmd, "fork") && arg)
        return do_fork(atoi(arg));
    if (!strcmp(cmd, "forkbomb"))
        for (;;)
            fork();
    if (!strcmp(cmd, "threads") && arg)
        return do_threads(atoi(arg));
    if (!strcmp(cmd, "bigfile") && arg)
        return do_bigfile(atol(arg));
    if (!strcmp(cmd, "openmany") && arg) {
        int n = atoi(arg), k = 0, err = 0;
        for (; k < n; k++) {
            if (open("/dev/null", O_RDONLY) < 0) {
                err = errno;
                break;
            }
        }
        printf("opened=%d err=%s\n", k, err ? ename(err) : "none");
        return 0;
    }
    if (!strcmp(cmd, "daemon")) {
        pid_t p = fork();
        if (p == 0) {
            sleep(100);
            _exit(0);
        }
        if (p < 0) {
            printf("fork %s\n", ename(errno));
            return 1;
        }
        return 0;
    }
    if ((!strcmp(cmd, "write") || !strcmp(cmd, "read") || !strcmp(cmd, "stat")) && arg)
        return do_paths(cmd, argv + 2);
    if (!strcmp(cmd, "cat") && arg) {
        int fd = open(arg, O_RDONLY);
        if (fd < 0) {
            printf("%s\n", ename(errno));
            return 1;
        }
        copy_fd(fd, 1);
        return 0;
    }
    if (!strcmp(cmd, "ls") && arg)
        return do_ls(arg);
    if (!strcmp(cmd, "ids"))
        return do_ids();
    if (!strcmp(cmd, "fds"))
        return do_fds();
    if (!strcmp(cmd, "env"))
        return do_env();
    if (!strcmp(cmd, "cwd")) {
        char buf[4096];
        printf("%s\n", getcwd(buf, sizeof buf) ? buf : ename(errno));
        return 0;
    }
    if (!strcmp(cmd, "limits")) {
        print_limit("stack", RLIMIT_STACK);
        print_limit("nofile", RLIMIT_NOFILE);
        print_limit("as", RLIMIT_AS);
        print_limit("fsize", RLIMIT_FSIZE);
        print_limit("core", RLIMIT_CORE);
        print_limit("memlock", RLIMIT_MEMLOCK);
        return 0;
    }
    if (!strcmp(cmd, "procs"))
        return do_procs();
    if (!strcmp(cmd, "net"))
        return do_net();
    if (!strcmp(cmd, "flock")) {
        int fd = open("lockfile", O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            printf("open %s\n", ename(errno));
            return 1;
        }
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        printf("flock %s\n", ename(flock(fd, LOCK_EX) < 0 ? errno : 0));
        printf("fcntl %s\n", ename(fcntl(fd, F_SETLK, &fl) < 0 ? errno : 0));
        return 0;
    }
    if (!strcmp(cmd, "mklinks")) {
        printf("symlink %s\n", ename(symlink("/etc/passwd", "link") < 0 ? errno : 0));
        printf("fifo %s\n", ename(mkfifo("fifo", 0644) < 0 ? errno : 0));
        int fd = open("regular", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        printf("regular %s\n", ename(fd < 0 ? errno : 0));
        if (fd >= 0) {
            if (write(fd, "data\n", 5) != 5)
                printf("regular write %s\n", ename(errno));
            close(fd);
        }
        return 0;
    }
    if (!strcmp(cmd, "sys") && arg)
        return do_sys(argv + 2);
    if (!strcmp(cmd, "foreign"))
        return do_foreign();
    if (!strcmp(cmd, "stdin-echo")) {
        copy_fd(0, 1);
        if (arg)
            fprintf(stderr, "%s\n", arg);
        return 0;
    }
    if (!strcmp(cmd, "stderr") && arg) {
        fprintf(stderr, "%s\n", arg);
        return 0;
    }
    if (!strcmp(cmd, "wide") && arg && argc > 3) {
        /* DEPTH nested directories, then N directories with a file in each */
        int depth = atoi(arg), n = atoi(argv[3]);
        char name[32];
        for (int i = 0; i < depth; i++) {
            if (mkdir("d", 0755) < 0 || chdir("d") < 0) {
                printf("wide %s at depth %d\n", ename(errno), i);
                return 1;
            }
        }
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof name, "w%d", i);
            if (mkdir(name, 0755) < 0) {
                printf("wide mkdir %s after %d\n", ename(errno), i);
                return 1;
            }
            snprintf(name, sizeof name, "w%d/f", i);
            int fd = open(name, O_WRONLY | O_CREAT, 0644);
            if (fd < 0) {
                printf("wide open %s after %d\n", ename(errno), i);
                return 1;
            }
            close(fd);
        }
        printf("wide OK\n");
        return 0;
    }
    if (!strcmp(cmd, "oomchild") && arg) {
        /* a child that blows the memory limit. Only the box's oom.group kills the parent too */
        pid_t p = fork();
        if (p == 0) {
            size_t size = (size_t)atol(arg) << 20;
            char *m = malloc(size);
            for (size_t i = 0; m && i < size; i += 4096)
                m[i] = 1;
            _exit(0);
        }
        if (p < 0) {
            printf("fork %s\n", ename(errno));
            return 1;
        }
        waitpid(p, NULL, 0);
        printf("parent survived\n");
        return 0;
    }
    if (!strcmp(cmd, "ignore") && arg && argc > 3) {
        /* runs PROG with these signals ignored, like a careless caller would */
        for (char *s = argv[2]; *s;) {
            signal(strtol(s, &s, 10), SIG_IGN);
            if (*s == ',')
                s++;
            else if (*s)
                return usage();
        }
        execv(argv[3], argv + 3);
        printf("execv %s\n", ename(errno));
        return 127;
    }
    if (!strcmp(cmd, "deep") && arg) {
        /* N nested directories. mkdir and chdir, so the path length is no limit */
        int n = atoi(arg);
        if (argc > 3 && chdir(argv[3]) < 0) {
            printf("chdir %s\n", ename(errno));
            return 1;
        }
        for (int i = 0; i < n; i++) {
            if (mkdir("d", 0755) < 0 || chdir("d") < 0) {
                printf("deep %s after %d\n", ename(errno), i);
                return 1;
            }
        }
        printf("deep OK\n");
        return 0;
    }
    if (!strcmp(cmd, "cpus")) {
        /* the CPUs this process may run on, as numbers */
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof set, &set) < 0) {
            printf("cpus %s\n", ename(errno));
            return 1;
        }
        printf("cpus");
        for (int i = 0; i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &set))
                printf(" %d", i);
        printf("\n");
        return 0;
    }
    if (!strcmp(cmd, "echo")) {
        for (int i = 2; i < argc; i++)
            printf("%s%s", i > 2 ? " " : "", argv[i]);
        printf("\n");
        return 0;
    }
    return usage();
}
