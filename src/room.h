/*
 * room - run untrusted programs in a sandbox built from Linux namespaces,
 * cgroup v2 and a seccomp filter. Works inside a plain Docker container.
 *
 * The version, the compile-time defaults and the current box.
 */
#ifndef ROOM_H
#define ROOM_H

#include <limits.h>
#include <sys/types.h>

#define ROOM_VERSION "0.1.0"

#define NORETURN __attribute__((noreturn))
#define PRINTF(a, b) __attribute__((format(printf, a, b)))

#define ROOM_BOX_ROOT "/var/lib/room" /* override with $ROOM_BOX_ROOT */
#define ROOM_CGROUP_FS "/sys/fs/cgroup"
/*
 * Box N runs as uid and gid 60000+N, or 60002+N from box 5534 on, which
 * skips 65534 and 65535. The uid map decides how many boxes there are.
 */
#define ROOM_FIRST_UID 60000

/*
 * The current box, set by main.c. With -b auto they change while it looks
 * for a free number, after cg_init() has run. The cg_box_ functions,
 * fs_setup_root() and run_box() act on the box these name.
 * box_dir is <box root>/<id> and holds box/ (the program's working
 * directory), tmp/ (backing for "tmp" directory rules) and root/ (mount
 * point of the sandbox root, only populated inside the mount namespace).
 */
extern int box_id;
extern uid_t box_uid;
extern gid_t box_gid;
extern char box_dir[PATH_MAX];

#endif
