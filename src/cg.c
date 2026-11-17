/* room - cgroup v2 setup and per box cgroup operations. */
#include "cg.h"
#include "room.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define CGROUP2_MAGIC 0x63677270
#define CONTROLLERS "+memory +pids"
#define UNIT_CG ROOM_CGROUP_FS "/system.slice/room.service"

static char *room_cg; /* room/, parent of main/ and the boxes */
static char *box_cg;  /* room/box-N, the claim on the number. Never holds a process */
static char *run_cg;  /* room/box-N/run, made new for every run */
static int box_cg_id; /* the box these belong to. -b auto tries several */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static void set_paths(void)
{
    if (box_cg && box_cg_id == box_id)
        return;
    free(box_cg);
    free(run_cg);
    box_cg = xasprintf("%s/box-%d", room_cg, box_id);
    run_cg = xasprintf("%s/run", box_cg);
    box_cg_id = box_id;
}

static const char *box_path(void)
{
    set_paths();
    return box_cg;
}

static const char *run_path(void)
{
    set_paths();
    return run_cg;
}

/* True if word is one of the space separated words in list. */
static bool has_word(const char *list, const char *word)
{
    size_t n = strlen(word);

    for (const char *p = list; (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return true;
    return false;
}

static void cg_write(const char *dir, const char *file, const char *val, bool optional)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (write_file(path, val) < 0 && !(optional && errno == ENOENT))
        die("Cannot write \"%s\" to %s: %m", val, path);
}

/*
 * Reads the value of key from a flat keyed file such as cpu.stat. With
 * missing_ok a file that doesn't exist gives -1.
 */
static long long read_key(const char *dir, const char *file, const char *key, bool missing_ok)
{
    char path[PATH_MAX], buf[4096], pat[64], *p;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    /* a leading newline lets every key, the first one too, match "\nkey " */
    buf[0] = '\n';
    if (read_file(path, buf + 1, sizeof(buf) - 1) < 0) {
        if (missing_ok && errno == ENOENT)
            return -1;
        die("Cannot read %s: %m", path);
    }
    snprintf(pat, sizeof(pat), "\n%s ", key);
    p = strstr(buf, pat);
    if (!p)
        die("No \"%s\" in %s", key, path);
    return atoll(p + strlen(pat));
}

/* Only --cpus needs the cpuset controller, so room carries on without it. */
static bool add_cpuset(const char *dir)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", dir);
    if (write_file(path, "+cpuset") == 0)
        return true;
    msg("Cannot enable cpuset in %s: %m. --cpus won't work\n", path);
    return false;
}

static void make_cg(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        die("Cannot create cgroup %s: %m", path);
}

/* Without systemd, returns <base>, the cgroup room was started in before it moved anything. */
static char *find_base(void)
{
    char buf[PATH_MAX + 64], *p, *end;
    size_t n;

    buf[0] = '\n';
    if (read_file("/proc/self/cgroup", buf + 1, sizeof(buf) - 1) < 0)
        die("Cannot read /proc/self/cgroup: %m");
    p = strstr(buf, "\n0::");
    if (!p)
        die("No cgroup v2 entry in /proc/self/cgroup");
    p += 4;
    if ((end = strchr(p, '\n')))
        *end = 0;
    /* once room has moved the processes of <base>, they live in <base>/room/main */
    n = strlen(p);
    if (n >= 10 && !strcmp(p + n - 10, "/room/main"))
        p[n - 10] = 0;
    if (!strcmp(p, "/"))
        *p = 0;
    return xasprintf("%s%s", ROOM_CGROUP_FS, p);
}

/*
 * Docker mounts the cgroup fs read-only per mount while the superblock is
 * writable. A bind remount changes only the per-mount flags, and it has to
 * repeat the ones we keep or the kernel clears them.
 */
static void remount_rw(void)
{
    static const struct {
        unsigned long st, ms;
    } keep[] = {
        { ST_NOSUID, MS_NOSUID },           { ST_NODEV, MS_NODEV },
        { ST_NOEXEC, MS_NOEXEC },           { ST_NOATIME, MS_NOATIME },
        { ST_NODIRATIME, MS_NODIRATIME },   { ST_RELATIME, MS_RELATIME },
#if defined(ST_NOSYMFOLLOW) && defined(MS_NOSYMFOLLOW)
        { ST_NOSYMFOLLOW, MS_NOSYMFOLLOW },
#endif
    };
    unsigned long flags = MS_REMOUNT | MS_BIND;
    struct statvfs sv;

    msg("Remounting %s read-write\n", ROOM_CGROUP_FS);
    if (statvfs(ROOM_CGROUP_FS, &sv) < 0)
        die("Cannot statvfs %s: %m", ROOM_CGROUP_FS);
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
        if (sv.f_flag & keep[i].st)
            flags |= keep[i].ms;
    if (mount(NULL, ROOM_CGROUP_FS, NULL, flags, NULL) < 0)
        die("%s is read-only and remounting it read-write failed: %m. "
            "Start the container with --cap-add SYS_ADMIN",
            ROOM_CGROUP_FS);
}

/* Moves every process in from/ to to/ until from/ stays empty. */
static void move_procs(const char *from, const char *to)
{
    char src[PATH_MAX], dst[PATH_MAX], s[16];
    int fd, pid, n;

    snprintf(src, sizeof(src), "%s/cgroup.procs", from);
    snprintf(dst, sizeof(dst), "%s/cgroup.procs", to);
    fd = open(dst, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        die("Cannot open %s: %m", dst);
    /* docker exec can add processes while we move, so read the list again */
    for (int round = 0; round < 100; round++) {
        FILE *f = fopen(src, "r");
        int moved = 0;

        if (!f)
            die("Cannot open %s: %m", src);
        while (fscanf(f, "%d", &pid) == 1) {
            /* the kernel takes one pid per write. ESRCH means it already exited */
            n = snprintf(s, sizeof(s), "%d", pid);
            if (write(fd, s, n) < 0 && errno != ESRCH)
                die("Cannot move process %d into %s: %m", pid, to);
            moved++;
        }
        fclose(f);
        if (!moved) {
            close(fd);
            return;
        }
    }
    die("%s does not stay empty, processes keep appearing", src);
}

/*
 * A cgroup that holds processes can't hand controllers to its children,
 * so everything in <base> moves to room/main first. The machine's root
 * cgroup is exempt from that rule and has no cgroup.events file.
 */
static void delegate(const char *base, const char *main_cg)
{
    char path[PATH_MAX];
    bool is_root;

    snprintf(path, sizeof(path), "%s/cgroup.events", base);
    is_root = access(path, F_OK) < 0;
    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", base);
    msg("Moving the processes in %s to %s\n", base, main_cg);
    for (int tries = 0;; tries++) {
        if (!is_root)
            move_procs(base, main_cg);
        if (write_file(path, CONTROLLERS) == 0)
            return;
        /* EBUSY: a process slipped into <base> after we emptied it */
        if (errno != EBUSY || tries == 10)
            die("Cannot write \"%s\" to %s: %m", CONTROLLERS, path);
    }
}

void cg_init(void)
{
    struct statfs sfs;
    char path[PATH_MAX], buf[512], *base, *main_cg;
    const char *env = getenv("ROOM_CG_ROOT");
    bool cpuset;

    if (statfs(ROOM_CGROUP_FS, &sfs) < 0 || sfs.f_type != CGROUP2_MAGIC)
        die("%s is not a cgroup v2 filesystem. Cgroup v1 and hybrid setups are not supported",
            ROOM_CGROUP_FS);

    if (env && *env) {
        /* room appends at most "/box-2147483647/run/cgroup.subtree_control" to it in PATH_MAX buffers */
        if (strlen(env) > PATH_MAX - 64)
            die("ROOM_CG_ROOT is too long, the limit is %d characters", PATH_MAX - 64);
        room_cg = xstrdup(env);
        snprintf(path, sizeof(path), "%s/cgroup.subtree_control", room_cg);
        if (write_file(path, CONTROLLERS) < 0)
            die("Cannot write \"%s\" to %s: %m. ROOM_CG_ROOT must be an empty cgroup "
                "whose parent has memory and pids enabled",
                CONTROLLERS, path);
        snprintf(path, sizeof(path), "%s/cgroup.controllers", room_cg);
        if (read_file(path, buf, sizeof(buf)) >= 0 && has_word(buf, "cpuset"))
            add_cpuset(room_cg);
        return;
    }

    /*
     * Under systemd room may only arrange a cgroup that systemd delegated,
     * the one of room.service. Its sleeping process is moved like a
     * container's processes.
     */
    if (access("/run/systemd/system", F_OK) == 0) {
        if (access(UNIT_CG, F_OK) < 0)
            die("systemd manages the cgroups here and %s does not exist. "
                "Start the unit with systemctl enable --now room.service, or set ROOM_CG_ROOT",
                UNIT_CG);
        base = xstrdup(UNIT_CG);
    } else {
        base = find_base();
    }
    if (access(base, W_OK) < 0) {
        if (errno != EROFS)
            die("Cannot write to %s: %m", base);
        remount_rw();
    }

    snprintf(path, sizeof(path), "%s/cgroup.controllers", base);
    if (read_file(path, buf, sizeof(buf)) < 0)
        die("Cannot read %s: %m", path);
    if (!has_word(buf, "memory"))
        die("The memory controller is not available in %s", path);
    if (!has_word(buf, "pids"))
        die("The pids controller is not available in %s", path);
    cpuset = has_word(buf, "cpuset");

    room_cg = xasprintf("%s/room", base);
    main_cg = xasprintf("%s/main", room_cg);
    make_cg(room_cg);
    make_cg(main_cg);

    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", base);
    if (read_file(path, buf, sizeof(buf)) < 0)
        die("Cannot read %s: %m", path);
    if (!has_word(buf, "memory") || !has_word(buf, "pids"))
        delegate(base, main_cg);

    cg_write(room_cg, "cgroup.subtree_control", CONTROLLERS, false);
    /* not in delegate(), which is skipped once <base> has memory and pids */
    if (cpuset && add_cpuset(base))
        add_cpuset(room_cg);
    free(main_cg);
    free(base);
}

bool cg_box_exists(void)
{
    return access(box_path(), F_OK) == 0;
}

/* Removes an empty cgroup. Fine if it is already gone. */
static void remove_cg(const char *path)
{
    /* a cgroup can stay busy for a moment after its last task is reaped */
    for (int i = 0; rmdir(path) < 0; i++) {
        if (errno == ENOENT)
            return;
        if (errno != EBUSY || i == 100)
            die("Cannot remove cgroup %s: %m", path);
        sleep_ms(10);
    }
}

static void remove_run(void)
{
    cg_box_kill();
    remove_cg(run_path());
}

/* box-N holds no processes, so it can hand the controllers on to run/. */
static void enable_box(void)
{
    const char *box = box_path();
    char path[PATH_MAX], buf[512];

    cg_write(box, "cgroup.subtree_control", CONTROLLERS, false);
    snprintf(path, sizeof(path), "%s/cgroup.controllers", box);
    if (read_file(path, buf, sizeof(buf)) >= 0 && has_word(buf, "cpuset"))
        add_cpuset(box);
}

bool cg_box_claim(void)
{
    const char *box = box_path();

    if (mkdir(box, 0755) < 0) {
        if (errno == EEXIST)
            return false;
        die("Cannot create cgroup %s: %m", box);
    }
    enable_box();
    return true;
}

/*
 * The number in a name room gave a box cgroup, or -1. Only "box-" and the
 * digits of a number without a leading zero count. Anything else in room/
 * (main, the cgroup files, a cgroup someone else made) doesn't claim one.
 */
static int box_number(const char *name)
{
    const char *p = name + 4;
    long long v = 0;

    if (strncmp(name, "box-", 4) || !*p || (*p == '0' && p[1]))
        return -1;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || (v = v * 10 + (*p - '0')) > INT_MAX)
            return -1;
    }
    return (int)v;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return (x > y) - (x < y);
}

int cg_box_first_free(int from)
{
    DIR *d = opendir(room_cg);
    struct dirent *e;
    int *taken = NULL, *p;
    size_t n = 0, cap = 0;
    long long next = from;

    if (!d)
        die("Cannot open cgroup %s: %m", room_cg);
    while ((e = readdir(d))) {
        int id = box_number(e->d_name);

        if (id < from)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            if (!(p = realloc(taken, cap * sizeof(*taken))))
                die("Out of memory");
            taken = p;
        }
        taken[n++] = id;
    }
    closedir(d);
    if (n)
        qsort(taken, n, sizeof(*taken), cmp_int);
    for (size_t i = 0; i < n && taken[i] <= next; i++)
        if (taken[i] == next)
            next++;
    free(taken);
    return next > INT_MAX ? INT_MAX : (int)next;
}

void cg_box_create(void)
{
    if (cg_box_claim())
        return;
    remove_run();
    /* again, in case an earlier --init died between the mkdir() and this */
    enable_box();
}

void cg_box_remove(void)
{
    if (!cg_box_exists())
        return;
    remove_run();
    remove_cg(box_path());
}

/* box-N never holds a process, so only its run cgroup needs killing. */
void cg_box_kill(void)
{
    const char *box = run_path();
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/cgroup.kill", box);
    if (write_file(path, "1") < 0) {
        if (errno == ENOENT)
            return;
        die("Cannot write \"1\" to %s: %m", path);
    }
    /* cgroup.kill only sends the signals. The tasks still have to exit. */
    for (int i = 0; i < 5000; i++) {
        /* -1: the cgroup went away meanwhile, for example because room.service stopped */
        if (read_key(box, "cgroup.events", "populated", true) <= 0)
            return;
        sleep_ms(1);
    }
    die("Processes in %s are still alive 5 seconds after cgroup.kill", box);
}

/*
 * The kernel quietly drops the CPUs the parent doesn't have, or gives the
 * box all of the parent's when none are left. So room compares what it
 * asked for with what the box got.
 */
static void set_cpus(const char *run, const char *cpus)
{
    char path[PATH_MAX], avail[1024], want[1024], got[1024];
    int err = 0;

    snprintf(path, sizeof(path), "%s/cpuset.cpus", run);
    if (write_file(path, cpus) < 0)
        err = errno;
    if (err == ENOENT)
        die("--cpus needs the cpuset controller, and it isn't available in %s", box_path());
    snprintf(path, sizeof(path), "%s/cpuset.cpus.effective", box_path());
    if (read_file(path, avail, sizeof(avail)) < 0)
        die("Cannot read %s: %m", path);
    if (err) {
        errno = err;
        die("Cannot run the box on CPUs %s: %m. The available CPUs are %s", cpus, avail);
    }
    snprintf(path, sizeof(path), "%s/cpuset.cpus", run);
    if (read_file(path, want, sizeof(want)) < 0)
        die("Cannot read %s: %m", path);
    snprintf(path, sizeof(path), "%s/cpuset.cpus.effective", run);
    if (read_file(path, got, sizeof(got)) < 0)
        die("Cannot read %s: %m", path);
    if (strcmp(want, got))
        die("Not all of the CPUs %s are available. The available CPUs are %s", cpus, avail);
}

void cg_box_prepare(long long mem_kb, int max_procs, const char *cpus)
{
    const char *run = run_path();
    char val[32];

    /*
     * A fresh run cgroup per run. Its counters start at zero, so the numbers
     * cover this run only. And kernel 7.0 kills a clone3(CLONE_INTO_CGROUP)
     * child at birth when the target cgroup has seen a different number of
     * cgroup.kill writes than the caller's cgroup. The keeper kills run/
     * after every run, so a reused one would break the next run.
     */
    remove_run();
    /* NOLINTNEXTLINE(clang-analyzer-unix.Malloc): xasprintf() never returns NULL */
    make_cg(run);

    if (mem_kb)
        snprintf(val, sizeof(val), "%lld", mem_kb * 1024);
    else
        strcpy(val, "max");
    cg_write(run, "memory.max", val, false);
    if (mem_kb)
        cg_write(run, "memory.swap.max", "0", true);
    /* on OOM kill the whole box, not one process the program might survive */
    cg_write(run, "memory.oom.group", "1", true);

    if (max_procs)
        snprintf(val, sizeof(val), "%d", max_procs);
    else
        strcpy(val, "max");
    cg_write(run, "pids.max", val, false);

    if (cpus)
        set_cpus(run, cpus);
}

int cg_box_fd(void)
{
    int fd = open(run_path(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0)
        die("Cannot open cgroup %s: %m", run_path());
    return fd;
}

long long cg_box_cpu_ms(void)
{
    return read_key(run_path(), "cpu.stat", "usage_usec", false) / 1000;
}

void cg_box_stats(struct cg_stats *st)
{
    char path[PATH_MAX], buf[32];

    snprintf(path, sizeof(path), "%s/memory.peak", run_path());
    st->mem_peak_kb = -1;
    if (read_file(path, buf, sizeof(buf)) >= 0)
        st->mem_peak_kb = atoll(buf) / 1024;
    else if (errno != ENOENT)
        die("Cannot read %s: %m", path);
    st->oom_killed = read_key(run_path(), "memory.events", "oom_kill", false) > 0;
}
