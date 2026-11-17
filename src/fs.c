/* room - box tree helpers, the disk quota, directory rules and the sandbox root. */
#include "fs.h"
#include "room.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* glibc 2.36 and later define these in sys/mount.h. Older glibc and musl don't. */
#ifndef MOUNT_ATTR_RDONLY
#define MOUNT_ATTR_RDONLY 0x1
#define MOUNT_ATTR_NOSUID 0x2
#define MOUNT_ATTR_NODEV 0x4
#define MOUNT_ATTR_NOEXEC 0x8
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442 /* same number on every architecture */
#endif

/* Own name, so it can't clash with the struct mount_attr of newer glibc. */
struct room_mount_attr {
    uint64_t attr_set, attr_clr, propagation, userns_fd;
};

/* openat2() has no wrapper in glibc or musl, and older kernel headers lack linux/openat2.h. */
#ifndef RESOLVE_IN_ROOT
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_IN_ROOT 0x10
#endif
#ifndef SYS_openat2
#define SYS_openat2 437 /* same number on every architecture */
#endif

struct room_open_how {
    uint64_t flags, mode, resolve;
};

void make_dir(const char *path)
{
    char *p = xstrdup(path), *s;
    struct stat st;

    for (s = p + 1;; s++) {
        if (*s && *s != '/')
            continue;
        char c = *s;
        *s = 0;
        if (mkdir(p, 0755) < 0 && errno != EEXIST)
            die("Cannot create %s: %m", p);
        if (!c)
            break;
        *s = c;
    }
    if (stat(path, &st) < 0)
        die("Cannot stat %s: %m", path);
    if (!S_ISDIR(st.st_mode))
        die("%s is not a directory", path);
    free(p);
}

/*
 * Removes everything inside the directory cur and closes cur, without
 * recursing. walk() uses it below the depth it recurses to, where one
 * descriptor per level could run out. dev is the top's filesystem,
 * anything else is a mount point. It holds one descriptor for the current
 * directory, deletes what it meets, enters the first directory that isn't
 * empty as soon as it meets one, and climbs back with ".." once a listing
 * finds the current directory empty. Nothing should change the tree
 * meanwhile (the box is dead or locked), but in case something does it
 * recognises the directory it started in by device and inode and never
 * climbs above it.
 * Every climb lists the parent again from its start, so a directory with n
 * subdirectories that aren't empty is listed about n times. A wide
 * directory down here costs up to the square of its width.
 * The full path can be longer than PATH_MAX, so messages show top and the
 * name with "..." for the levels in between.
 */
static void rm_tree(int cur, const char *top, dev_t dev)
{
    struct stat start, st;
    int depth = 0;

    if (fstat(cur, &start) < 0)
        die("Cannot stat %s: %m", top);
    for (;;) {
        const char *mid = depth ? "/..." : "";
        int fd = openat(cur, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC), next = -1;
        DIR *d = fd < 0 ? NULL : fdopendir(fd);
        bool empty = true;
        struct dirent *e;

        if (!d)
            die("Cannot read %s%s: %m", top, mid);
        while ((errno = 0, e = readdir(d))) {
            const char *name = e->d_name;

            if (!strcmp(name, ".") || !strcmp(name, ".."))
                continue;
            empty = false;
            if (fstatat(cur, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
                die("Cannot stat %s%s/%s: %m", top, mid, name);
            if (st.st_dev != dev)
                die("Refusing to remove %s%s/%s: it is a mount point", top, mid, name);
            if (!S_ISDIR(st.st_mode)) {
                if (unlinkat(cur, name, 0) < 0)
                    die("Cannot remove %s%s/%s: %m", top, mid, name);
            } else if (unlinkat(cur, name, AT_REMOVEDIR) < 0) {
                if (errno != ENOTEMPTY && errno != EEXIST)
                    die("Cannot remove %s%s/%s: %m", top, mid, name);
                next = openat(cur, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (next < 0)
                    die("Cannot open %s%s/%s: %m", top, mid, name);
                break;
            }
        }
        if (!e && errno)
            die("Cannot read %s%s: %m", top, mid);
        closedir(d);

        if (next >= 0) {
            depth++;
        } else if (!empty) {
            /* list again, an entry removed while listing may have hidden another */
            continue;
        } else {
            if (fstat(cur, &st) < 0)
                die("Cannot stat %s%s: %m", top, mid);
            if (depth == 0 || (st.st_dev == start.st_dev && st.st_ino == start.st_ino)) {
                close(cur);
                return;
            }
            next = openat(cur, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (next < 0)
                die("Cannot open the parent of %s%s: %m", top, mid);
            depth--;
        }
        close(cur);
        cur = next;
    }
}

#define WALK_MAX_DEPTH 64

enum walk_op { W_RM, W_CHOWN, W_SPECIAL };

struct walk {
    enum walk_op op;
    dev_t dev; /* the top's filesystem. Removal stops at anything else, a mount point */
    uid_t uid;
    gid_t gid;
};

/*
 * Walks the tree below dfd, which is depth levels below the top, and takes
 * ownership of dfd. The program that filled the tree is dead but may have
 * planted symlinks, so every step is relative to a directory fd and nothing
 * is ever followed. Keeping each directory open while below it makes the
 * cost proportional to the number of entries. That needs a descriptor per
 * level, so a directory deeper than WALK_MAX_DEPTH is removed with rm_tree()
 * instead of entered. path is only for messages.
 */
static void walk(int dfd, const char *path, const struct walk *w, int depth)
{
    DIR *d = fdopendir(dfd);
    struct dirent *e;
    struct stat st;

    if (!d)
        die("Cannot read %s: %m", path);
    /* NOLINTBEGIN(clang-analyzer-unix.StdCLibraryFunctions): dirfd() of an open DIR can't fail */
    while ((errno = 0, e = readdir(d))) {
        const char *name = e->d_name;
        char *p;

        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        p = xasprintf("%s/%s", path, name);
        if (fstatat(dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW) < 0)
            die("Cannot stat %s: %m", p);
        if (w->op == W_RM && st.st_dev != w->dev)
            die("Refusing to remove %s: it is a mount point", p);
        if (w->op == W_CHOWN && (st.st_uid != w->uid || st.st_gid != w->gid)) {
            /* a hard link from outside box/ must not hand the original file to the box user */
            if (S_ISREG(st.st_mode) && st.st_nlink > 1 && st.st_uid != w->uid)
                msg("Keeping the owner of %s, it has more than one hard link\n", p);
            else if (fchownat(dirfd(d), name, w->uid, w->gid, AT_SYMLINK_NOFOLLOW) < 0)
                die("Cannot chown %s: %m", p);
        }
        if (S_ISDIR(st.st_mode)) {
            int fd = openat(dirfd(d), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

            if (fd < 0)
                die("Cannot open %s: %m", p);
            if (depth < WALK_MAX_DEPTH) {
                walk(fd, p, w, depth + 1);
            } else {
                msg("Removing %s, it is nested more than %d levels deep\n", p, WALK_MAX_DEPTH);
                rm_tree(fd, p, w->dev);
            }
            if ((w->op == W_RM || depth >= WALK_MAX_DEPTH) &&
                unlinkat(dirfd(d), name, AT_REMOVEDIR) < 0)
                die("Cannot remove %s: %m", p);
        } else if (w->op == W_RM || (w->op == W_SPECIAL && !S_ISREG(st.st_mode))) {
            if (unlinkat(dirfd(d), name, 0) < 0)
                die("Cannot remove %s: %m", p);
        }
        free(p);
    }
    /* NOLINTEND(clang-analyzer-unix.StdCLibraryFunctions) */
    if (errno)
        die("Cannot read %s: %m", path);
    closedir(d);
}

static int open_top(const char *dir)
{
    return open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

void rm_contents(const char *dir)
{
    struct walk w = { .op = W_RM };
    struct stat st;
    int fd = open_top(dir);

    if (fd < 0 && errno == ENOENT)
        return;
    if (fd < 0 || fstat(fd, &st) < 0)
        die("Cannot open %s: %m", dir);
    w.dev = st.st_dev;
    walk(fd, dir, &w, 0);
}

void tree_chown(const char *dir, uid_t uid, gid_t gid)
{
    struct walk w = { .op = W_CHOWN, .uid = uid, .gid = gid };
    struct stat st;
    int fd = open_top(dir);

    if (fd < 0 || fstat(fd, &st) < 0 || fchown(fd, uid, gid) < 0)
        die("Cannot chown %s: %m", dir);
    w.dev = st.st_dev;
    walk(fd, dir, &w, 0);
}

void tree_remove_special(const char *dir)
{
    struct walk w = { .op = W_SPECIAL };
    struct stat st;
    int fd = open_top(dir);

    if (fd < 0 || fstat(fd, &st) < 0)
        die("Cannot open %s: %m", dir);
    w.dev = st.st_dev;
    walk(fd, dir, &w, 0);
}

#define QUOTA_SOURCE "room-quota"

void quota_mount(const char *dir, long long kb, long long inodes)
{
    char opts[96];

    /* both, always. main.c accepts no 0, which tmpfs would take as no limit */
    snprintf(opts, sizeof(opts), "mode=755,size=%lldk,nr_inodes=%lld", kb, inodes);
    msg("Mounting a tmpfs with %s on %s\n", opts, dir);
    if (mount(QUOTA_SOURCE, dir, "tmpfs", MS_NOSUID | MS_NODEV, opts) < 0)
        die("Cannot mount the quota tmpfs on %s: %m", dir);
}

/*
 * Something else mounted on a box directory, a volume for example, belongs
 * to the user and must be treated as a plain directory. So this checks the
 * filesystem type and the source of the mount, not only that there is one.
 */
bool quota_mounted(const char *dir)
{
    char *parent = xasprintf("%s/..", dir), *line = NULL, type[64], source[64];
    struct stat st, pst;
    unsigned int maj, min;
    bool found = false;
    size_t cap = 0;
    FILE *f;

    /* ".." climbs out of a mount, so the box root's device differs if dir is one */
    if (stat(dir, &st) < 0)
        die("Cannot stat %s: %m", dir);
    if (stat(parent, &pst) < 0)
        die("Cannot stat %s: %m", parent);
    free(parent);
    if (st.st_dev == pst.st_dev)
        return false;
    f = fopen("/proc/self/mountinfo", "re");
    if (!f)
        die("Cannot open /proc/self/mountinfo: %m");
    /* the fields after " - " are the type and the source. Spaces in fields are escaped */
    while (!found && getline(&line, &cap, f) > 0) {
        char *sep = strstr(line, " - ");

        found = sep && sscanf(line, "%*s %*s %u:%u", &maj, &min) == 2 &&
                makedev(maj, min) == st.st_dev && sscanf(sep + 3, "%63s %63s", type, source) == 2 &&
                !strcmp(type, "tmpfs") && !strcmp(source, QUOTA_SOURCE);
    }
    free(line);
    fclose(f);
    return found;
}

void quota_detach(const char *dir)
{
    msg("Detaching the quota tmpfs from %s\n", dir);
    if (umount2(dir, MNT_DETACH | UMOUNT_NOFOLLOW) < 0)
        die("Cannot detach the quota tmpfs from %s: %m", dir);
}

enum { R_RW = 1, R_DEV = 2, R_NOEXEC = 4, R_MAYBE = 8, R_NOREC = 16, R_FS = 32, R_TMP = 64 };
#define R_BUILTIN_DEV 128 /* the private /dev, out is NULL */
#define R_DEFAULT 256     /* skipped when default_dirs is off */

struct rule {
    struct rule *next;
    char *in; /* inside the box, no leading or trailing slash */
    /* bind source, filesystem type, or NULL for tmp, built-in dev and a rule without OUT */
    char *out;
    int flags;
};

static struct rule *rules;
bool default_dirs = true;
const char *rule_root;

static const struct {
    const char *name;
    int flag;
} options[] = {
    { "rw", R_RW },       { "dev", R_DEV }, { "noexec", R_NOEXEC }, { "maybe", R_MAYBE },
    { "norec", R_NOREC }, { "fs", R_FS },   { "tmp", R_TMP },
};

static bool has_dotdot(const char *p)
{
    for (;;) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || !p[2]))
            return true;
        p = strchr(p, '/');
        if (!p)
            return false;
        p++;
    }
}

/* True if a component of p is empty or ".". */
static bool has_empty_or_dot(const char *p)
{
    for (;;) {
        if (*p == '/' || !*p || (p[0] == '.' && (p[1] == '/' || !p[1])))
            return true;
        p = strchr(p, '/');
        if (!p)
            return false;
        p++;
    }
}

static bool known_fs(const char *t)
{
    return !strcmp(t, "proc") || !strcmp(t, "sysfs") || !strcmp(t, "tmpfs") || !strcmp(t, "devpts");
}

const char *dir_rule_add(const char *arg)
{
    char *s = xstrdup(arg), *in = s, *out, *opts, *o;
    struct rule **p;
    size_t i, len;
    int flags = 0;

    opts = strchr(s, ':');
    if (opts)
        *opts++ = 0;
    out = strchr(s, '=');
    if (out)
        *out++ = 0;
    while (*in == '/')
        in++;
    len = strlen(in);
    while (len > 0 && in[len - 1] == '/')
        in[--len] = 0;
    if (!*in)
        return "The path inside the box is empty";
    if (has_dotdot(in))
        return "The path inside the box must not contain ..";
    if (has_empty_or_dot(in))
        return "The path inside the box must not have an empty or . component";

    while (opts && (o = strsep(&opts, ":"))) {
        for (i = 0; i < sizeof(options) / sizeof(*options) && strcmp(o, options[i].name); i++)
            ;
        if (i == sizeof(options) / sizeof(*options))
            return xasprintf("Unknown directory option '%s'", o);
        flags |= options[i].flag;
    }

    for (p = &rules; *p && strcmp((*p)->in, in); p = &(*p)->next)
        ;
    if (out && !*out) {
        if (*p)
            *p = (*p)->next;
        return NULL;
    }

    if ((flags & R_TMP) && (flags & R_FS))
        return "Options tmp and fs can't be combined";
    if (flags & R_TMP) {
        if (out)
            return "A tmp rule takes no outside path";
    } else if (flags & R_FS) {
        if (!out || !known_fs(out))
            return xasprintf("Unsupported filesystem '%s', use proc, sysfs, tmpfs or devpts",
                             out ? out : "");
    } else if (out && out[0] != '/' && strncmp(out, "./", 2)) {
        return "The outside path must be absolute or start with ./";
    }

    /* a rule without OUT keeps NULL. Where it binds from depends on --root, which may come later */
    if (!*p) {
        *p = xmalloc(sizeof(**p));
        (*p)->next = NULL;
    }
    (*p)->in = in;
    (*p)->out = out;
    (*p)->flags = flags;
    return NULL;
}

void dir_rules_init(void)
{
    static const char *const defaults[] = {
        "box=./box:rw", "bin", "dev", "lib", "lib64:maybe", "proc=proc:fs", "tmp:tmp", "usr",
    };

    for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); i++)
        dir_rule_add(defaults[i]);
    for (struct rule *r = rules; r; r = r->next) {
        r->flags |= R_DEFAULT;
        if (!strcmp(r->in, "dev")) {
            r->flags |= R_BUILTIN_DEV;
            r->out = NULL;
        }
    }
}

static void do_mount(const char *src, const char *dst, const char *type, unsigned long flags,
                     const char *data)
{
    if (mount(src, dst, type, flags, data) < 0)
        die("Cannot mount %s on %s: %m", src, dst);
}

static void set_attr(const char *path, uint64_t attr, bool rec)
{
    struct room_mount_attr a = { .attr_set = attr | MOUNT_ATTR_NOSUID };

    if (syscall(SYS_mount_setattr, AT_FDCWD, path, rec ? AT_RECURSIVE : 0, &a, sizeof(a)) < 0)
        die("Cannot set mount flags on %s: %m", path);
}

/*
 * Creates base/in, a directory or an empty file. Each step is relative to
 * a directory fd and refuses symlinks, so a link that an earlier bind
 * brought in can't make room create or mount anything outside base.
 */
static void make_mount_point(const char *base, const char *in, bool is_dir)
{
    char *path = xstrdup(in), *name = path, *slash;
    int fd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC), r;
    struct stat st;

    if (fd < 0)
        die("Cannot open %s: %m", base);
    for (;;) {
        slash = strchr(name, '/');
        if (slash)
            *slash = 0;
        bool want_dir = slash || is_dir;

        if (want_dir) {
            r = mkdirat(fd, name, 0755);
            /* don't let room's umask make a parent untraversable for the box user */
            if (r == 0)
                r = fchmodat(fd, name, 0755, 0);
        } else {
            r = openat(fd, name, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (r >= 0)
                r = close(r);
        }
        if (r < 0 && errno != EEXIST)
            die("Cannot create mount point /%s: %m", in);
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
            die("Cannot stat mount point /%s: %m", in);
        if (S_ISLNK(st.st_mode) || S_ISDIR(st.st_mode) != want_dir)
            die("Mount point /%s: %s is not a %s", in, name, want_dir ? "directory" : "file");
        if (!slash)
            break;
        r = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (r < 0)
            die("Cannot open mount point /%s: %m", in);
        close(fd);
        fd = r;
        name = slash + 1;
    }
    close(fd);
}

/*
 * The backing directory of a tmp rule, created on first use and kept until
 * the box is reset. Slashes in IN become ':', which IN can't contain, so
 * every rule gets a directory of its own.
 */
static char *tmp_dir(const char *in)
{
    char *tmp = xasprintf("%s/tmp", box_dir);
    char *path = xasprintf("%s/%s", tmp, in);
    int fd;

    for (char *c = path + strlen(tmp) + 1; *c; c++)
        if (*c == '/')
            *c = ':';
    make_dir(tmp);
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        die("Cannot create %s: %m", path);
    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || fchown(fd, box_uid, box_gid) < 0 || fchmod(fd, 0700) < 0)
        die("Cannot prepare %s: %m", path);
    close(fd);
    return path;
}

/* The built-in /dev. It must not expose the caller's devices, which matters in a privileged container. */
static void make_dev(const char *dev)
{
    static const char *const nodes[] = { "null", "zero", "full", "random", "urandom" };
    static const char *const links[][2] = {
        { "fd", "/proc/self/fd" },
        { "stdin", "/proc/self/fd/0" },
        { "stdout", "/proc/self/fd/1" },
        { "stderr", "/proc/self/fd/2" },
    };
    size_t i;
    char *p;

    do_mount("tmpfs", dev, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=755");
    for (i = 0; i < sizeof(nodes) / sizeof(*nodes); i++) {
        p = xasprintf("%s/%s", dev, nodes[i]);
        make_mount_point(dev, nodes[i], false);
        do_mount(xasprintf("/dev/%s", nodes[i]), p, NULL, MS_BIND, NULL);
        /* writing to a device node works on a read-only mount */
        set_attr(p, MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOEXEC, false);
    }
    for (i = 0; i < sizeof(links) / sizeof(*links); i++) {
        p = xasprintf("%s/%s", dev, links[i][0]);
        if (symlink(links[i][1], p) < 0)
            die("Cannot create %s: %m", p);
    }
    p = xasprintf("%s/shm", dev);
    make_mount_point(dev, "shm", true);
    do_mount("tmpfs", p, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    /* not recursive, so shm stays writable */
    set_attr(dev, MOUNT_ATTR_RDONLY | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC, false);
}

/*
 * --root: opens in as it is found inside rule_root. Every link on the way,
 * absolute or with "..", is resolved as if rule_root were /, so it can't
 * lead out. Magic links, such as those of a procfs inside rule_root, are
 * refused. Returns an O_PATH descriptor, or -1 with errno set.
 */
static int open_in_root(const char *in)
{
    static int root_fd = -1;
    struct room_open_how how = {
        .flags = O_PATH | O_CLOEXEC,
        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
    };

    if (root_fd < 0) {
        root_fd = open(rule_root, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (root_fd < 0)
            die("Cannot open %s: %m", rule_root);
    }
    return syscall(SYS_openat2, root_fd, in, &how, sizeof(how));
}

static void apply_rule(const char *root, const struct rule *r)
{
    char *dst = xasprintf("%s/%s", root, r->in);
    uint64_t attr = 0;

    if (r->flags & R_BUILTIN_DEV) {
        msg("Building /%s\n", r->in);
        make_mount_point(root, r->in, true);
        make_dev(dst);
        return;
    }
    if (r->flags & R_FS) {
        bool proc = !strcmp(r->out, "proc");

        msg("Mounting %s on /%s\n", r->out, r->in);
        make_mount_point(root, r->in, true);
        /* nodev comes from set_attr(), which couldn't clear it for a fs rule with dev */
        do_mount(r->out, dst, r->out, MS_NOSUID, proc ? "hidepid=2" : NULL);
        if (proc)
            attr |= MOUNT_ATTR_NOEXEC;
    } else {
        bool in_root = !r->out && !(r->flags & R_TMP) && rule_root, ok;
        struct stat st;
        char *src;
        int fd = -1;

        if (r->flags & R_TMP)
            src = tmp_dir(r->in);
        else if (in_root)
            src = xasprintf("%s/%s", rule_root, r->in);
        else if (!r->out)
            src = xasprintf("/%s", r->in);
        else if (r->out[0] == '.')
            src = xasprintf("%s/%s", box_dir, r->out + 2);
        else
            src = xstrdup(r->out);
        if (in_root) {
            fd = open_in_root(r->in);
            ok = fd >= 0 && fstat(fd, &st) == 0;
        } else {
            ok = stat(src, &st) == 0;
        }
        if (!ok) {
            if (errno == ENOENT && (r->flags & R_MAYBE)) {
                msg("Skipping /%s, %s does not exist\n", r->in, src);
                return;
            }
            die("Cannot bind %s: %m", src);
        }
        msg("Binding %s on /%s\n", src, r->in);
        make_mount_point(root, r->in, S_ISDIR(st.st_mode));
        /* the descriptor and not src, which would follow the links in rule_root again from / */
        do_mount(fd >= 0 ? xasprintf("/proc/self/fd/%d", fd) : src, dst, NULL,
                 MS_BIND | ((r->flags & R_NOREC) ? 0 : MS_REC), NULL);
        if (fd >= 0)
            close(fd);
    }
    if (!(r->flags & (R_RW | R_TMP)))
        attr |= MOUNT_ATTR_RDONLY;
    if (!(r->flags & R_DEV))
        attr |= MOUNT_ATTR_NODEV;
    if (r->flags & R_NOEXEC)
        attr |= MOUNT_ATTR_NOEXEC;
    set_attr(dst, attr, !(r->flags & R_NOREC));
}

void fs_setup_root(void)
{
    char *root = xasprintf("%s/root", box_dir);

    /* otherwise the mounts below would propagate back to the caller's namespace */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die("Cannot make mounts private: %m");
    make_dir(root);
    do_mount("tmpfs", root, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755");
    for (struct rule *r = rules; r; r = r->next)
        if (default_dirs || !(r->flags & R_DEFAULT))
            apply_rule(root, r);
    /* the mount points are in place. Not recursive, the rules set their own flags */
    set_attr(root, MOUNT_ATTR_RDONLY, false);

    if (chdir(root) < 0)
        die("Cannot enter %s: %m", root);
    if (syscall(SYS_pivot_root, ".", ".") == 0) {
        /* the old root now sits on top of the new one, detach it so nothing of it is reachable */
        if (umount2(".", MNT_DETACH) < 0)
            die("Cannot detach the old root: %m");
    } else if (errno == EPERM) {
        /*
         * Docker's default seccomp profile refuses pivot_root even with
         * CAP_SYS_ADMIN. Move the new root over the old one and chroot into
         * it instead. The old root stays in the namespace but no path from
         * the new root leads there, and the box user has no capability to
         * chroot back out.
         */
        msg("pivot_root refused, using a move mount and chroot\n");
        if (mount(".", "/", NULL, MS_MOVE, NULL) < 0 || chroot(".") < 0)
            die("Cannot switch to the new root: %m");
    } else {
        die("pivot_root failed: %m");
    }
    if (chdir("/box") < 0)
        die("Cannot enter /box: %m");
}
