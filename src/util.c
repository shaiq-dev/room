/* room - small helpers shared by every module. */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int verbose;
void (*die_hook)(const char *msg);

void die(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (die_hook)
        die_hook(buf);
    fprintf(stderr, "room: %s\n", buf);
    exit(2);
}

void msg(const char *fmt, ...)
{
    va_list ap;

    if (!verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void *xmalloc(size_t size)
{
    void *p = malloc(size);

    if (!p)
        die("Out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    char *p = strdup(s);

    if (!p)
        die("Out of memory");
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    char *out;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vasprintf(&out, fmt, ap);
    va_end(ap);
    if (n < 0)
        die("Out of memory");
    return out;
}

ssize_t read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    size_t len = 0;

    if (fd < 0)
        return -1;
    while ((n = read(fd, buf + len, size - len)) > 0)
        len += n;
    if (n < 0 || len == size) {
        int e = n < 0 ? errno : EFBIG;

        close(fd);
        errno = e;
        return -1;
    }
    close(fd);
    if (len > 0 && buf[len - 1] == '\n')
        len--;
    buf[len] = 0;
    return len;
}

int write_file(const char *path, const char *str)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    size_t len = strlen(str);
    ssize_t n;

    if (fd < 0)
        return -1;
    /* cgroup files want the whole value in one write() */
    n = write(fd, str, len);
    if (n < 0 || (size_t)n != len) {
        int e = n < 0 ? errno : EIO;

        close(fd);
        errno = e;
        return -1;
    }
    close(fd);
    return 0;
}
