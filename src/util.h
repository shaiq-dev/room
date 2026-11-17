/* room - small helpers shared by every module. */
#ifndef ROOM_UTIL_H
#define ROOM_UTIL_H

#include "room.h"

#include <stddef.h>
#include <sys/types.h>

extern int verbose;

/*
 * die() formats the message, cut to 1023 bytes, and never returns. If
 * die_hook is set it gets the message and must not return. run.c's hooks
 * kill the box and write the meta file (keeper), or pass the message up the
 * error pipe (proxy and inside process). main.c's gives back a number that
 * -b auto claimed. Without a hook, die() prints "room: <message>" to stderr
 * and exits with 2.
 */
extern void (*die_hook)(const char *msg);
void die(const char *fmt, ...) NORETURN PRINTF(1, 2);

/* Prints to stderr when verbose is on. The caller supplies the newline. */
void msg(const char *fmt, ...) PRINTF(1, 2);

/* Die on out of memory, so they never return NULL. The caller owns the result. */
void *xmalloc(size_t size);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) PRINTF(1, 2);

/*
 * Reads a small file into buf, NUL-terminates it and drops one trailing
 * newline. Returns the length, or -1 with errno set. A file of size bytes
 * or more fails with EFBIG.
 */
ssize_t read_file(const char *path, char *buf, size_t size);

/*
 * Writes str to an existing file in a single write(). Returns 0, or -1 with
 * errno set. A short write fails with EIO.
 */
int write_file(const char *path, const char *str);

#endif
