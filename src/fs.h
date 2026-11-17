/* room - box directory tree and the sandbox root. */
#ifndef ROOM_FS_H
#define ROOM_FS_H

#include <stdbool.h>
#include <sys/types.h>

/* mkdir -p with mode 0755. Dies on failure or if the path is not a directory. */
void make_dir(const char *path);

/*
 * Removes everything inside dir and keeps dir itself. Never follows
 * symlinks and dies if it meets a mount point. A missing dir is fine.
 */
void rm_contents(const char *dir);

/*
 * Gives dir and everything below it to uid:gid. Never follows symlinks.
 * A directory nested more than 64 levels below dir is removed instead.
 * A regular file with more than one hard link keeps its owner, unless it
 * already belongs to uid. Dies if dir is missing.
 */
void tree_chown(const char *dir, uid_t uid, gid_t gid);

/*
 * Deletes everything below dir that is not a regular file or a directory
 * (symlinks, fifos, sockets, device nodes), so the caller can read the box
 * without being tricked by a symlink. Call it once nothing runs in the box.
 * A directory nested more than 64 levels below dir is removed too. Dies if
 * dir is missing.
 */
void tree_remove_special(const char *dir);

/* Installs the default directory rules. Call once before parsing options. */
void dir_rules_init(void);

/*
 * Parses one --dir argument and copies it. Returns NULL, or a message that
 * says what is wrong with it. The message must not be freed.
 */
const char *dir_rule_add(const char *arg);

/* Cleared by --no-default-dirs. */
extern bool default_dirs;

/*
 * --root, or NULL. When set, a bind rule without an OUT binds IN as found
 * inside this directory, which is treated as the root when links are
 * resolved.
 */
extern const char *rule_root;

/*
 * The disk quota of a box: a tmpfs mounted on the box directory by --init.
 *
 * quota_mount() mounts it. kb and inodes are its limits, both more than 0.
 * quota_mounted() says whether dir is such a mount. It is exact: any other
 * mount on dir gives false.
 * quota_detach() unmounts it lazily, which throws its files away.
 */
void quota_mount(const char *dir, long long kb, long long inodes);
bool quota_mounted(const char *dir);
void quota_detach(const char *dir);

/*
 * Runs in the inside process, as root in a new mount namespace. Builds the
 * sandbox root under box_dir/root from the directory rules, makes it the
 * root and leaves the process in /box. That is pivot_root() with the old
 * root detached, or a move mount and chroot() where pivot_root() is
 * refused. Nothing it allocates is freed.
 */
void fs_setup_root(void);

#endif
