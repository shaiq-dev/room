/* room - command line, box identity, locking, --init and --cleanup. */
#include "cg.h"
#include "fs.h"
#include "room.h"
#include "run.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int box_id;
uid_t box_uid;
gid_t box_gid;
char box_dir[PATH_MAX];

enum {
    OPT_CG_MEM = 256,
    OPT_STDERR_TO_STDOUT,
    OPT_SHARE_NET,
    OPT_CG,
    OPT_CPUS,
    OPT_ROOT,
    OPT_INIT,
    OPT_RUN,
    OPT_CLEANUP,
    OPT_VERSION,
    OPT_HELP,
};

static const struct option long_opts[] = {
    { "box-id", required_argument, NULL, 'b' },
    { "quota", required_argument, NULL, 'q' },
    { "meta", required_argument, NULL, 'M' },
    { "time", required_argument, NULL, 't' },
    { "wall-time", required_argument, NULL, 'w' },
    { "extra-time", required_argument, NULL, 'x' },
    { "mem", required_argument, NULL, 'm' },
    { "cg-mem", required_argument, NULL, OPT_CG_MEM },
    { "stack", required_argument, NULL, 'k' },
    { "fsize", required_argument, NULL, 'f' },
    { "open-files", required_argument, NULL, 'n' },
    { "processes", optional_argument, NULL, 'p' },
    { "stdin", required_argument, NULL, 'i' },
    { "stdout", required_argument, NULL, 'o' },
    { "stderr", required_argument, NULL, 'r' },
    { "stderr-to-stdout", no_argument, NULL, OPT_STDERR_TO_STDOUT },
    { "chdir", required_argument, NULL, 'c' },
    { "env", required_argument, NULL, 'E' },
    { "full-env", no_argument, NULL, 'e' },
    { "dir", required_argument, NULL, 'd' },
    { "no-default-dirs", no_argument, NULL, 'D' },
    { "share-net", no_argument, NULL, OPT_SHARE_NET },
    { "silent", no_argument, NULL, 's' },
    { "verbose", no_argument, NULL, 'v' },
    { "cpus", required_argument, NULL, OPT_CPUS },
    { "root", required_argument, NULL, OPT_ROOT },
    { "cg", no_argument, NULL, OPT_CG },
    { "init", no_argument, NULL, OPT_INIT },
    { "run", no_argument, NULL, OPT_RUN },
    { "cleanup", no_argument, NULL, OPT_CLEANUP },
    { "version", no_argument, NULL, OPT_VERSION },
    { "help", no_argument, NULL, OPT_HELP },
    { NULL, 0, NULL, 0 },
};

/* "+" stops at the first non-option, so the program's own options are never ours. */
static const char short_opts[] = "+b:q:M:t:w:x:m:k:f:n:p::i:o:r:c:E:ed:Dsv";

static const char usage_text[] =
    "Usage: room [options] --init\n"
    "       room [options] --run -- program [arguments]\n"
    "       room [options] --cleanup\n"
    "       room --version | --help\n"
    "\n"
    "Options:\n"
    "  -b, --box-id=N          box number (default 0), or auto with --init\n"
    "  -q, --quota=KB,INODES   put the box on a tmpfs with these limits, for --init\n"
    "  -M, --meta=FILE         write the meta file, - for stdout\n"
    "  -t, --time=SEC          CPU time limit for the box\n"
    "  -w, --wall-time=SEC     wall clock limit (default 10 x (-t + -x), at least 30)\n"
    "  -x, --extra-time=SEC    grace period on top of --time\n"
    "  -m, --mem=KB            address space limit per process\n"
    "      --cg-mem=KB         memory limit for the whole box\n"
    "  -k, --stack=KB          stack limit (default unlimited)\n"
    "  -f, --fsize=KB          largest file the program may write\n"
    "  -n, --open-files=N      open file limit (default 1024, 0 keeps it)\n"
    "  -p, --processes[=N]     process and thread limit (default 1, no N for none)\n"
    "  -i, --stdin=FILE        redirect stdin, opened inside the box\n"
    "  -o, --stdout=FILE       redirect stdout, opened inside the box\n"
    "  -r, --stderr=FILE       redirect stderr, opened inside the box\n"
    "      --stderr-to-stdout  send stderr to wherever stdout goes\n"
    "  -c, --chdir=DIR         working directory inside the box\n"
    "  -E, --env=VAR[=VALUE]   pass, set or (with an empty value) remove VAR\n"
    "  -e, --full-env          pass room's whole environment\n"
    "  -d, --dir=RULE          directory rule IN[=OUT][:OPTION]...\n"
    "  -D, --no-default-dirs   start without the default directory rules\n"
    "      --share-net         keep the caller's network namespace\n"
    "  -s, --silent            don't print the status line\n"
    "  -v, --verbose           say what room is doing\n"
    "      --cpus=LIST         CPUs the box may run on, like 2 or 0-3,6\n"
    "      --root=DIR          take the system directories from DIR\n"
    "      --cg                accepted and ignored\n";

static void NORETURN PRINTF(1, 2) usage_error(const char *fmt, ...)
{
    va_list ap;

    fputs("room: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs("\nTry 'room --help' for more information.\n", stderr);
    exit(2);
}

#define DIGITS "0123456789"

/* Plain decimal digits. strtoll alone would also take spaces and a sign. */
static long long parse_num(const char *s, long long max, const char *what)
{
    long long v;

    errno = 0;
    v = strtoll(s, NULL, 10);
    if (!*s || s[strspn(s, DIGITS)] || errno || v > max)
        usage_error("Invalid %s: '%s'", what, s);
    return v;
}

/*
 * Seconds as digits with an optional fraction, returned in milliseconds.
 * strtod alone would also take spaces, signs, exponents and hex.
 */
static int parse_time(const char *s, const char *what)
{
    size_t n = strspn(s, DIGITS);
    char *end;
    double v;

    if (!n || (s[n] && (s[n] != '.' || !s[n + 1] || s[n + 1 + strspn(s + n + 1, DIGITS)])))
        usage_error("Invalid %s: '%s'", what, s);
    errno = 0;
    v = strtod(s, &end);
    if (errno || end == s || *end || !(v >= 0) || v > INT_MAX / 1000)
        usage_error("Invalid %s: '%s'", what, s);
    return (int)(v * 1000 + 0.5);
}

/* --quota, only for --init */
static bool quota;
static long long quota_kb, quota_inodes;

static const char *box_root;
static char *lock_path;
static int num_boxes;

/* box_uid_of() skips 65534 and 65535 from below, which needs ROOM_FIRST_UID under them */
_Static_assert(ROOM_FIRST_UID <= 65534, "ROOM_FIRST_UID must be at most 65534");

/* 65534 is nobody and 65535 is -1 as a 16-bit uid, so no box gets them. */
static uid_t box_uid_of(int id)
{
    long long uid = ROOM_FIRST_UID + (long long)id;

    return uid < 65534 ? uid : uid + 2;
}

/* The end of the range in a uid or gid map that holds ROOM_FIRST_UID, or 0 if none does. */
static long long map_end(const char *path)
{
    long long in, out, count, end = 0;
    FILE *f = fopen(path, "re");

    if (!f)
        die("Cannot open %s: %m", path);
    while (fscanf(f, "%lld %lld %lld", &in, &out, &count) == 3)
        if (in <= ROOM_FIRST_UID && ROOM_FIRST_UID - in < count)
            end = in + count;
    fclose(f);
    return end;
}

/*
 * A box needs a uid and a gid that exist here. In a user namespace only
 * part of the uid space does. Some software takes a uid as a signed 32-bit
 * number, so none goes above 2147483647.
 */
static int count_boxes(void)
{
    long long end = map_end("/proc/self/uid_map"), gid_end = map_end("/proc/self/gid_map");

    if (gid_end < end)
        end = gid_end;
    if (!end)
        die("Uid and gid %d are not both mapped in this user namespace, so room has no uids "
            "for boxes",
            ROOM_FIRST_UID);
    if (end > 2147483648LL)
        end = 2147483648LL;
    return (int)(end - ROOM_FIRST_UID - (end > 65535 ? 2 : end > 65534 ? 1 : 0));
}

static void set_box(int id)
{
    box_id = id;
    box_uid = box_uid_of(id);
    box_gid = box_uid;
    if (snprintf(box_dir, sizeof(box_dir), "%s/%d", box_root, id) >= (int)sizeof(box_dir))
        die("Box root path is too long");
    free(lock_path);
    lock_path = xasprintf("%s/.locks/%d", box_root, id);
}

/*
 * The lock is a file of its own because the box directory gets mounted
 * over, emptied and removed. The file is never removed, or two processes
 * could each hold "the" lock on a different file. Returns the locked
 * descriptor, or -1 if another room process holds the lock.
 */
static int try_lock_box(void)
{
    char *dir = xasprintf("%s/.locks", box_root);
    int fd;

    make_dir(dir);
    free(dir);
    fd = open(lock_path, O_RDONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        die("Cannot open %s: %m", lock_path);
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK)
            die("Cannot lock %s: %m", lock_path);
        close(fd);
        return -1;
    }
    return fd;
}

static int lock_box(void)
{
    int fd = try_lock_box();

    if (fd < 0)
        die("Box %d is in use by another room process", box_id);
    return fd;
}

/* -b auto: the caller never learns the number if --init fails, so nobody else would free it. */
static void unclaim(const char *m)
{
    fprintf(stderr, "room: %s\n", m);
    die_hook = NULL;
    cg_box_remove();
    exit(2);
}

/*
 * -b auto. An initialised box keeps its cgroup until --cleanup, so
 * cg_box_claim() never hands out its number. The listing only suggests a
 * number. On a lost race room asks again above it, so a pick doesn't try
 * every taken number.
 */
static void pick_box(void)
{
    for (int id = cg_box_first_free(0); id < num_boxes; id = cg_box_first_free(id + 1)) {
        int fd;

        set_box(id);
        fd = try_lock_box();
        if (fd < 0)
            continue;
        if (cg_box_claim()) {
            die_hook = unclaim;
            return; /* keeps holding the lock */
        }
        close(fd);
    }
    die("All %d boxes are in use", num_boxes);
}

static void do_init(bool pick)
{
    char *box;

    cg_init();
    make_dir(box_root);
    if (pick) {
        pick_box();
    } else {
        lock_box();
        cg_box_create();
    }
    make_dir(box_dir);
    if (quota_mounted(box_dir)) {
        quota_detach(box_dir);
        /*
         * Programs running in other boxes hold copies of the mount in their
         * mount namespaces, which keep its memory in use. Removing the
         * directory drops them. EBUSY means a mount of the user's is below
         * it, and that stays a plain directory.
         */
        rm_contents(box_dir);
        if (rmdir(box_dir) < 0 && errno != EBUSY)
            die("Cannot remove %s: %m", box_dir);
        make_dir(box_dir);
    }
    rm_contents(box_dir);
    if (quota)
        quota_mount(box_dir, quota_kb, quota_inodes);
    box = xasprintf("%s/box", box_dir);
    /* chmod because mkdir is subject to the umask */
    if (mkdir(box, 0700) < 0 || chown(box, box_uid, box_gid) < 0 || chmod(box, 0700) < 0)
        die("Cannot create %s: %m", box);
    puts(box_dir);
}

static void do_cleanup(void)
{
    struct stat st;

    cg_init();
    lock_box();
    if (stat(box_dir, &st) < 0) {
        if (errno != ENOENT)
            die("Cannot stat %s: %m", box_dir);
        cg_box_remove();
        return;
    }
    cg_box_remove();
    /* detaching throws the files away without walking them */
    if (quota_mounted(box_dir))
        quota_detach(box_dir);
    rm_contents(box_dir);
    if (rmdir(box_dir) < 0)
        die("Cannot remove %s: %m", box_dir);
}

static void NORETURN do_run(const struct run_opts *o, char **argv)
{
    char *box;
    struct stat st;

    run_begin(o->meta_path);
    box = xasprintf("%s/box", box_dir);
    cg_init();
    /* first, so a concurrent --init or --cleanup can't change the box between the check and the run */
    lock_box();
    if (stat(box, &st) < 0 || !S_ISDIR(st.st_mode) || !cg_box_exists())
        die("Box %d is not initialised, run room --init first", box_id);
    run_box(o, argv);
}

int main(int argc, char **argv)
{
    struct run_opts o = { .open_files = 1024, .max_procs = 1 };
    bool pick = false, wall_given = false;
    struct stat st;
    char *s, *comma;
    int cmd = 0;
    int c;

    /* a caller that ignores SIGCHLD would make every wait fail */
    signal(SIGCHLD, SIG_DFL);
    /* otherwise room's own pipes can land on 0, 1 or 2 and a redirect in the box replaces them */
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", O_RDWR) < 0)
            die("Cannot open /dev/null: %m");

    dir_rules_init();
    while ((c = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
        const char *err;

        switch (c) {
        case 'b':
            pick = !strcmp(optarg, "auto");
            if (!pick)
                box_id = parse_num(optarg, INT_MAX, "box id");
            break;
        case 'q':
            /*
             * Both limits, checked before anything in the box is touched. A
             * box without one of them could use up the container's memory.
             */
            s = xstrdup(optarg);
            comma = strchr(s, ',');
            if (!comma)
                usage_error("Invalid quota: '%s', expected KB,INODES", optarg);
            *comma = 0;
            quota_kb = parse_num(s, LLONG_MAX / 1024, "quota size");
            quota_inodes = parse_num(comma + 1, INT_MAX, "quota inode count");
            if (quota_kb < 1 || quota_inodes < 16)
                usage_error("Invalid quota: '%s'. KB must be at least 1 and INODES at least 16",
                            optarg);
            quota = true;
            free(s);
            break;
        case 'M': o.meta_path = optarg; break;
        case 't': o.time_ms = parse_time(optarg, "time limit"); break;
        case 'w':
            o.wall_ms = parse_time(optarg, "wall time limit");
            wall_given = true;
            break;
        case 'x': o.extra_ms = parse_time(optarg, "extra time"); break;
        case 'm': o.mem_kb = parse_num(optarg, LLONG_MAX / 1024, "memory limit"); break;
        case OPT_CG_MEM: o.cg_mem_kb = parse_num(optarg, LLONG_MAX / 1024, "memory limit"); break;
        case 'k': o.stack_kb = parse_num(optarg, LLONG_MAX / 1024, "stack limit"); break;
        case 'f': o.fsize_kb = parse_num(optarg, LLONG_MAX / 1024, "file size limit"); break;
        case 'n': o.open_files = parse_num(optarg, INT_MAX, "open file limit"); break;
        case 'p': o.max_procs = optarg ? parse_num(optarg, INT_MAX, "process limit") : 0; break;
        case 'i': o.stdin_path = optarg; break;
        case 'o': o.stdout_path = optarg; break;
        case 'r': o.stderr_path = optarg; break;
        case OPT_STDERR_TO_STDOUT: o.stderr_to_stdout = true; break;
        case 'c': o.chdir_to = optarg; break;
        case 'E':
            if (!env_rule_add(optarg))
                usage_error("Invalid environment rule: '%s'", optarg);
            break;
        case 'e': o.full_env = true; break;
        case 'd':
            if ((err = dir_rule_add(optarg)))
                usage_error("Invalid directory rule '%s': %s", optarg, err);
            break;
        case 'D': default_dirs = false; break;
        case OPT_SHARE_NET: o.share_net = true; break;
        case 's': o.silent = true; break;
        case 'v': verbose++; break;
        case OPT_CPUS:
            /* the kernel checks the rest. This keeps anything odd out of the cgroup file */
            if (!isdigit((unsigned char)optarg[0]) || optarg[strspn(optarg, DIGITS ",-")] ||
                !isdigit((unsigned char)optarg[strlen(optarg) - 1]))
                usage_error("Invalid CPU list: '%s'", optarg);
            o.cpus = optarg;
            break;
        case OPT_ROOT:
            if (optarg[0] != '/')
                usage_error("--root must be an absolute path, not '%s'", optarg);
            if (stat(optarg, &st) < 0 || !S_ISDIR(st.st_mode))
                usage_error("--root must be an existing directory: '%s'", optarg);
            rule_root = optarg;
            break;
        case OPT_CG: break;
        case OPT_INIT:
        case OPT_RUN:
        case OPT_CLEANUP:
            if (cmd)
                usage_error("Give exactly one of --init, --run and --cleanup");
            cmd = c;
            break;
        case OPT_VERSION: printf("room %s\n", ROOM_VERSION); return 0;
        case OPT_HELP: fputs(usage_text, stdout); return 0;
        default:
            /* getopt has already said what is wrong */
            fputs("Try 'room --help' for more information.\n", stderr);
            return 2;
        }
    }

    if (!cmd)
        usage_error("Give one of --init, --run and --cleanup");
    if (cmd == OPT_RUN && optind >= argc)
        usage_error("--run needs a program to run");
    if (cmd != OPT_RUN && optind < argc)
        usage_error("Unexpected argument: '%s'", argv[optind]);
    if (quota && cmd != OPT_INIT)
        usage_error("--quota only works with --init");
    if (pick && cmd != OPT_INIT)
        usage_error("-b auto only works with --init");

    /* otherwise a program that sleeps holds the box forever when the caller only set -t */
    if (o.time_ms && !wall_given) {
        long long w = 10 * ((long long)o.time_ms + o.extra_ms);

        o.wall_ms = w < 30000 ? 30000 : w > INT_MAX ? INT_MAX : w;
    }
    if (cmd == OPT_RUN) {
        if (o.wall_ms)
            msg("Wall clock limit: %d.%03d sec%s\n", o.wall_ms / 1000, o.wall_ms % 1000,
                wall_given ? "" : " (default)");
        else
            msg("Wall clock limit: none\n");
    }

    if (geteuid() != 0)
        die("Must be run as root");

    box_root = getenv("ROOM_BOX_ROOT");
    if (!box_root || !*box_root)
        box_root = ROOM_BOX_ROOT;
    else if (box_root[0] != '/')
        die("ROOM_BOX_ROOT must be an absolute path, not '%s'", box_root);
    /* before anything is created, for every command */
    num_boxes = count_boxes();
    if (!pick && box_id >= num_boxes)
        die("There is no box %d. This machine has %d boxes, 0 to %d", box_id, num_boxes,
            num_boxes - 1);
    /* -b auto settles the box in do_init() */
    if (!pick)
        set_box(box_id);

    if (cmd == OPT_INIT)
        do_init(pick);
    else if (cmd == OPT_CLEANUP)
        do_cleanup();
    else
        do_run(&o, argv + optind);
    return 0;
}
