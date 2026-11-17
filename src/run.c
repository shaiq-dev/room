/* room - --run: the keeper, the proxy, the inside process and the meta file. */
#include "run.h"
#include "cg.h"
#include "fs.h"
#include "room.h"
#include "seccomp.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/close_range.h>
#include <linux/sched.h>

extern char **environ;

static const char **env_rules;
static size_t num_env_rules;

bool env_rule_add(const char *arg)
{
    if (!*arg || *arg == '=')
        return false;
    /* NOLINTNEXTLINE(bugprone-suspicious-realloc-usage): die() exits, so nothing leaks */
    env_rules = realloc(env_rules, (num_env_rules + 1) * sizeof(*env_rules));
    if (!env_rules)
        die("Out of memory");
    env_rules[num_env_rules++] = arg;
    return true;
}

/* Index of the entry for the variable named by the first len bytes of name, or n. */
static size_t env_find(char **env, size_t n, const char *name, size_t len)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!strncmp(env[i], name, len) && env[i][len] == '=')
            break;
    return i;
}

static void env_set(char **env, size_t *n, char *entry)
{
    size_t i = env_find(env, *n, entry, strchr(entry, '=') - entry);

    env[i] = entry;
    if (i == *n)
        (*n)++;
}

static char **build_env(bool full_env)
{
    size_t n = 0, i, cap = num_env_rules + 2;
    char **env;

    if (full_env)
        for (char **e = environ; *e; e++)
            cap++;
    env = xmalloc(cap * sizeof(*env));
    if (full_env)
        for (char **e = environ; *e; e++)
            if (strchr(*e, '='))
                env_set(env, &n, *e);
    env_set(env, &n, "LIBC_FATAL_STDERR_=1");

    for (i = 0; i < num_env_rules; i++) {
        const char *r = env_rules[i], *eq = strchr(r, '=');

        if (!eq) {
            const char *v = getenv(r);

            if (v)
                env_set(env, &n, xasprintf("%s=%s", r, v));
        } else if (!eq[1]) {
            size_t j = env_find(env, n, r, eq - r);

            if (j < n)
                memmove(env + j, env + j + 1, (--n - j) * sizeof(*env));
        } else {
            env_set(env, &n, (char *)r);
        }
    }
    env[n] = NULL;
    return env;
}

static const struct run_opts *opts;
static int err_pipe[2], status_pipe[2];
static pid_t proxy_pid;
static FILE *meta;
static volatile sig_atomic_t interrupted;

static const int keeper_signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
#define NUM_KEEPER_SIGNALS (sizeof(keeper_signals) / sizeof(keeper_signals[0]))

/*
 * The inside process's last write to the error pipe before execve(). No
 * message and no marker means the box died during setup.
 */
#define START_MARKER 1

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static pid_t clone3_fork(struct clone_args *args)
{
    /* the child gets a copy of unflushed stdio buffers */
    fflush(NULL);
    return syscall(SYS_clone3, args, sizeof(*args));
}

/* Proxy and inside process: hand the message to the keeper. */
static void child_die(const char *m)
{
    ssize_t r = write(err_pipe[1], m, strlen(m));

    (void)r;
    _exit(2);
}

static void bring_up_lo(void)
{
    struct ifreq ifr = { 0 };
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (s < 0)
        die("Cannot create a socket: %m");
    strcpy(ifr.ifr_name, "lo");
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
        die("Cannot read the flags of lo: %m");
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
        if (errno != EPERM)
            die("Cannot bring up lo: %m");
        msg("Cannot bring up lo without NET_ADMIN, the box has no network\n");
    }
    close(s);
}

static void set_limit(int res, rlim_t v)
{
    struct rlimit rl = { v, v };

    if (setrlimit(res, &rl) == 0)
        return;
    /* raising past the hard limit needs CAP_SYS_RESOURCE, take what we can get */
    if (errno == EPERM && getrlimit(res, &rl) == 0 && v > rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(res, &rl) == 0)
            return;
    }
    die("Cannot set resource limit %d: %m", res);
}

static void redirect(const char *path, int target, int flags)
{
    int fd = open(path, flags, 0666);

    if (fd < 0)
        die("Cannot open %s: %m", path);
    if (fd != target) {
        /* the copy on target is the program's stdio, not a leak as -fanalyzer thinks */
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
        if (dup2(fd, target) < 0)
            die("dup2: %m");
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
        close(fd);
    }
}

/* The inside process, born in the run cgroup. Builds the box, drops root and calls execve(). */
static void NORETURN inside(char **argv)
{
    const struct run_opts *o = opts;
    const char marker = START_MARKER;
    sigset_t all;
    char **env;

    /* so /proc/self/cgroup in the box shows / and not the box cgroup's path */
    if (unshare(CLONE_NEWCGROUP) < 0)
        die("Cannot create a cgroup namespace: %m");
    if (sethostname("room", 4) < 0)
        die("Cannot set the hostname: %m");
    fs_setup_root();
    if (!o->share_net)
        bring_up_lo();

    if (o->mem_kb)
        set_limit(RLIMIT_AS, o->mem_kb * 1024);
    if (o->fsize_kb)
        set_limit(RLIMIT_FSIZE, o->fsize_kb * 1024);
    set_limit(RLIMIT_STACK, o->stack_kb ? (rlim_t)o->stack_kb * 1024 : RLIM_INFINITY);
    set_limit(RLIMIT_MEMLOCK, 0);
    set_limit(RLIMIT_CORE, 0);

    if (setsid() < 0)
        die("setsid: %m");
    if (setgroups(0, NULL) < 0 || setresgid(box_gid, box_gid, box_gid) < 0 ||
        setresuid(box_uid, box_uid, box_uid) < 0)
        die("Cannot switch to the box user: %m");

    /* opened as the box user so the program can't reach files it couldn't open itself */
    if (o->stdin_path)
        redirect(o->stdin_path, 0, O_RDONLY);
    if (o->stdout_path)
        redirect(o->stdout_path, 1, O_WRONLY | O_CREAT | O_TRUNC);
    if (o->stderr_path)
        redirect(o->stderr_path, 2, O_WRONLY | O_CREAT | O_TRUNC);
    /* the copy on 2 is the program's stderr, not a leak as -fanalyzer thinks */
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
    if (o->stderr_to_stdout && dup2(1, 2) < 0)
        die("dup2: %m");
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
    if (o->chdir_to && chdir(o->chdir_to) < 0)
        die("Cannot change to %s: %m", o->chdir_to);
    /* only now, room's own descriptors would count against a small limit and block the redirects */
    if (o->open_files)
        set_limit(RLIMIT_NOFILE, o->open_files);

    env = build_env(o->full_env);

    /* ignored signals survive execve, and the program must not inherit what room or its caller ignored */
    for (int sig = 1; sig < NSIG; sig++)
        signal(sig, SIG_DFL);
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);

    if (syscall(SYS_close_range, 3, ~0U, CLOSE_RANGE_CLOEXEC) < 0)
        die("close_range: %m");
    seccomp_install();
    if (write(err_pipe[1], &marker, 1) != 1)
        die("Cannot write the start marker: %m");

    execve(argv[0], argv, env);
    dprintf(2, "execve(\"%s\"): %m\n", argv[0]);
    _exit(127);
}

/*
 * The proxy is PID 1 of the box's PID namespace, so the program isn't. PID 1
 * ignores the signals it has no handler for, which would break abort() and
 * the reporting of fatal signals. The proxy stays in the caller's cgroup,
 * starts the inside process in the run cgroup and passes its wait status to
 * the keeper.
 */
static void NORETURN proxy(char **argv)
{
    struct pollfd pf = { .fd = status_pipe[1] };
    struct clone_args ca = { .flags = CLONE_INTO_CGROUP, .exit_signal = SIGCHLD };
    pid_t child, p;
    size_t i;
    int st;

    die_hook = child_die;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    close(err_pipe[0]);
    close(status_pipe[0]);
    /* no reader left means the keeper died before PDEATHSIG was armed */
    if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLERR))
        _exit(2);

    /* the keeper decides what a signal means, and the inside process resets these */
    for (i = 0; i < NUM_KEEPER_SIGNALS; i++)
        signal(keeper_signals[i], SIG_IGN);

    ca.cgroup = cg_box_fd();
    child = clone3_fork(&ca);
    if (child < 0)
        die("Cannot start the program in the box cgroup: %m");
    if (child == 0)
        inside(argv);
    close(ca.cgroup);

    /* orphans of the program are reparented to us and must be reaped too */
    for (;;) {
        p = waitpid(-1, &st, 0);
        if (p < 0) {
            if (errno == EINTR)
                continue;
            die("waitpid: %m");
        }
        if (p == child) {
            if (write(status_pipe[1], &st, sizeof(st)) != sizeof(st))
                die("Cannot report the wait status: %m");
            _exit(0);
        }
    }
}

static void meta_printf(const char *fmt, ...) PRINTF(1, 2);
static void meta_printf(const char *fmt, ...)
{
    va_list ap;

    if (!meta)
        return;
    va_start(ap, fmt);
    vfprintf(meta, fmt, ap);
    va_end(ap);
}

static void meta_close(void)
{
    FILE *f = meta;

    meta = NULL;
    if (f && (f == stdout ? fflush(f) : fclose(f)) != 0)
        die("Cannot write the meta file: %m");
}

/*
 * Writes the XX result and exits. keeper_die() ends here, and from
 * run_begin() until run_box() this is the die hook by itself. It only
 * reports then, because until the lock is held the box may be another
 * command's, and the keeper's cleanup would destroy that command's run.
 */
static void NORETURN report_failure(const char *m)
{
    char clean[1024];

    snprintf(clean, sizeof(clean), "%s", m);
    /* a message can't add lines to the meta file */
    for (char *c = clean; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7f)
            *c = '?';
    meta_printf("status:XX\nmessage:%s\n", clean);
    if (meta) {
        FILE *f = meta;

        meta = NULL;
        if (f != stdout)
            fclose(f);
    }
    fprintf(stderr, "room: %s\n", clean);
    exit(2);
}

static void reap_proxy(int *st, struct rusage *ru)
{
    while (wait4(proxy_pid, st, 0, ru) < 0)
        if (errno != EINTR)
            die("wait4: %m");
    proxy_pid = 0;
}

static void keeper_die(const char *m)
{
    static char first[1024];
    static int step; /* the next cleanup step, 0 before the first failure */
    struct rusage ru;
    int st;

    /*
     * A cleanup step can die too. That comes back here and goes on with the
     * next step, so every step runs and the first failure, the cause, is
     * the one reported.
     */
    if (!step) {
        step = 1;
        snprintf(first, sizeof(first), "%s", m);
    }
    while (step <= 3) {
        switch (step++) {
        case 1:
            if (proxy_pid > 0) {
                kill(proxy_pid, SIGKILL);
                reap_proxy(&st, &ru);
            }
            break;
        case 2: cg_box_kill(); break;
        case 3:
            /* last, once nothing in the box can make new ones */
            tree_remove_special(xasprintf("%s/box", box_dir));
            break;
        }
    }
    report_failure(first);
}

static void on_signal(int sig)
{
    interrupted = sig;
}

void run_begin(const char *meta_path)
{
    int fd;

    if (meta_path && !strcmp(meta_path, "-")) {
        meta = stdout;
    } else if (meta_path) {
        fd = open(meta_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || !(meta = fdopen(fd, "w")))
            die("Cannot open the meta file %s: %m", meta_path);
    }
    die_hook = report_failure;
}

void run_box(const struct run_opts *o, char **argv)
{
    char *box = xasprintf("%s/box", box_dir);
    struct sigaction sa = { .sa_handler = on_signal };
    struct clone_args ca = { .exit_signal = SIGCHLD };
    long long start, end = 0, wall, cpu;
    int pidfd = -1, st = 0, ws = 0, killed = 0, sig = 0, exitcode = -1, exitsig = 0;
    bool started = false, have_status = false;
    struct rusage ru = { 0 };
    struct cg_stats cs;
    char err[1024];
    const char *status = NULL, *message = NULL;
    ssize_t n = 0;
    size_t i;

    opts = o;
    die_hook = keeper_die;
    for (i = 0; i < NUM_KEEPER_SIGNALS; i++) {
        if (keeper_signals[i] == SIGPIPE)
            signal(SIGPIPE, SIG_IGN);
        else
            sigaction(keeper_signals[i], &sa, NULL);
    }

    /* first, it also kills what a killed keeper left, so nothing is alive while room walks box/ */
    cg_box_prepare(o->cg_mem_kb, o->max_procs, o->cpus);
    tree_chown(box, box_uid, box_gid);

    if (pipe2(err_pipe, O_CLOEXEC) < 0 || pipe2(status_pipe, O_CLOEXEC) < 0)
        die("pipe: %m");
    start = now_ms();
    if (interrupted) {
        /* the signal came before the program, so don't start it */
        sig = interrupted;
        end = start;
    } else {
        ca.flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_PIDFD;
        if (!o->share_net)
            ca.flags |= CLONE_NEWNET;
        ca.pidfd = (uintptr_t)&pidfd;
        proxy_pid = clone3_fork(&ca);
        if (proxy_pid < 0)
            die("Cannot create the box namespaces: %m");
        if (proxy_pid == 0)
            proxy(argv);
        started = true;
        close(err_pipe[1]);
        close(status_pipe[1]);
        msg("Started the box, proxy pid %d\n", (int)proxy_pid);

        for (;;) {
            struct pollfd pf = { .fd = pidfd, .events = POLLIN };
            int timeout = 50;

            wall = now_ms() - start;
            if (interrupted ||
                (o->time_ms && cg_box_cpu_ms() > (long long)o->time_ms + o->extra_ms) ||
                (o->wall_ms && wall > o->wall_ms)) {
                /* only a signal that made room kill the box counts, not one after the program ended */
                sig = interrupted;
                end = now_ms();
                msg("Killing the box\n");
                kill(proxy_pid, SIGKILL);
                cg_box_kill();
                killed = 1;
                break;
            }
            if (o->wall_ms && o->wall_ms - wall + 1 < timeout)
                timeout = o->wall_ms - wall + 1;
            n = poll(&pf, 1, timeout);
            if (n > 0)
                break;
            if (n < 0 && errno != EINTR)
                die("poll: %m");
        }
        if (!end)
            end = now_ms();

        reap_proxy(&st, &ru);
        cg_box_kill();

        /* every writer is gone, so these reads can't block */
        n = read(err_pipe[0], err, sizeof(err) - 1);
        have_status = read(status_pipe[0], &ws, sizeof(ws)) == sizeof(ws);
    }

    tree_remove_special(box);

    /* the proxy can still fail after the inside process has written its marker */
    if (n > 1 || (n == 1 && err[0] != START_MARKER)) {
        err[n] = 0;
        die("%s", err + (err[0] == START_MARKER));
    }
    cg_box_stats(&cs);
    if (started && n <= 0 && !killed) {
        /* no message and no marker: it never reached execve() */
        const char *how = "died";

        if (have_status && WIFSIGNALED(ws))
            how = xasprintf("was killed by signal %d", WTERMSIG(ws));
        die("The box %s before the program started%s", how,
            cs.oom_killed
                ? ". The OOM killer fired, the memory limit is too small for room's own setup"
                : "");
    }

    cpu = cg_box_cpu_ms();
    wall = end - start;

    if (sig) {
        exitsig = sig;
        status = "SG";
        message = "Interrupted";
    } else if (o->time_ms && cpu > o->time_ms) {
        status = "TO";
        message = "Time limit exceeded";
    } else if (o->wall_ms && wall > o->wall_ms) {
        status = "TO";
        message = "Time limit exceeded (wall clock)";
    } else if (!have_status) {
        die("The box ended without reporting how the program finished (proxy status %d)", st);
    } else if (WIFEXITED(ws)) {
        exitcode = WEXITSTATUS(ws);
        if (exitcode) {
            status = "RE";
            message = xasprintf("Exited with error status %d", exitcode);
        }
    } else if (WIFSIGNALED(ws)) {
        exitsig = WTERMSIG(ws);
        status = "SG";
        message = xasprintf("Caught fatal signal %d", exitsig);
    } else {
        die("Unexpected wait status %#x", ws);
    }

    /* all keys at the end, so a failure above leaves only status and message */
    if (exitcode >= 0)
        meta_printf("exitcode:%d\n", exitcode);
    if (exitsig)
        meta_printf("exitsig:%d\n", exitsig);
    meta_printf("time:%lld.%03lld\ntime-wall:%lld.%03lld\n", cpu / 1000, cpu % 1000, wall / 1000,
                wall % 1000);
    meta_printf("max-rss:%ld\ncsw-voluntary:%ld\ncsw-forced:%ld\n", ru.ru_maxrss, ru.ru_nvcsw,
                ru.ru_nivcsw);
    if (cs.mem_peak_kb >= 0)
        meta_printf("cg-mem:%lld\n", cs.mem_peak_kb);
    if (cs.oom_killed)
        meta_printf("cg-oom-killed:1\n");
    if (killed)
        meta_printf("killed:1\n");
    if (status)
        meta_printf("status:%s\nmessage:%s\n", status, message);
    meta_close();

    if (!o->silent) {
        if (status)
            fprintf(stderr, "%s\n", message);
        else
            fprintf(stderr, "OK (%lld.%03lld sec real, %lld.%03lld sec wall)\n", cpu / 1000,
                    cpu % 1000, wall / 1000, wall % 1000);
    }
    exit(status ? 1 : 0);
}
