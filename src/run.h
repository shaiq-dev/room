/* room - --run: the keeper, the proxy, the inside process and the meta file. */
#ifndef ROOM_RUN_H
#define ROOM_RUN_H

#include "room.h"

#include <stdbool.h>

struct run_opts {
    int time_ms;             /* -t  CPU time limit for the box, 0 = none */
    int wall_ms;             /* -w  wall clock limit, 0 = none. main.c fills in the default */
    int extra_ms;            /* -x  grace on top of time_ms before the box is killed */
    long long mem_kb;        /* -m  address space per process (RLIMIT_AS), 0 = none */
    long long cg_mem_kb;     /* --cg-mem  memory for the whole box, 0 = none */
    long long stack_kb;      /* -k  0 = unlimited */
    long long fsize_kb;      /* -f  0 = none */
    int open_files;          /* -n  default 1024, 0 = leave the limit alone */
    int max_procs;           /* -p  default 1, 0 = unlimited */
    const char *cpus;        /* --cpus  cpuset list, NULL = don't pin */
    const char *stdin_path;  /* -i  these three are opened inside the box */
    const char *stdout_path; /* -o */
    const char *stderr_path; /* -r */
    bool stderr_to_stdout;
    const char *chdir_to;  /* -c */
    const char *meta_path; /* -M  "-" means stdout */
    bool share_net;
    bool full_env; /* -e */
    bool silent;   /* -s */
};

/*
 * Parses one --env argument: VAR, VAR=value or VAR= . Returns false if it
 * is empty or starts with =. Keeps the pointer, so arg must stay valid
 * until run_box().
 */
bool env_rule_add(const char *arg);

/*
 * --run, first of all: opens the meta file (meta_path NULL means none, "-"
 * stdout) and makes every later die() write status XX and the message to
 * it. A failed run must not leave the previous result there for a judge
 * that reuses the path.
 */
void run_begin(const char *meta_path);

/*
 * --run. Call it after run_begin() and cg_init(), with the box lock held
 * and the box initialised. Runs argv in the box, writes the meta file and exits with
 * 0 (the program exited with 0), 1 (any other verdict, an interruption
 * included) or 2 (room itself failed). argv[0] goes to execve() as it is.
 */
void run_box(const struct run_opts *o, char **argv) NORETURN;

#endif
