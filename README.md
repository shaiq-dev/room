# room

Room is a sandbox for running untrusted code. It runs a program in a box made from Linux namespaces, cgroup v2 and a seccomp filter. It limits the program's time, memory and processes, and it reports how the program ended.

Room uses the command line and the meta file format of [isolate](https://github.com/ioi/isolate). It needs no daemon and no libraries. It arranges its own cgroups, so it runs in a Docker container without systemd and without `--privileged`.

## Requirements

- Linux on x86_64 or little-endian aarch64.
- Cgroup v2 at `/sys/fs/cgroup` with the `memory` and `pids` controllers. `--cpus` also needs `cpuset`. Room refuses cgroup v1 and hybrid setups.
- Kernel 5.14 or later, going by the kernel's documentation. The `cg-mem` result needs 5.19. [docs/DESIGN.md](docs/DESIGN.md#platform-dependencies) lists the features.
- Root. Room isn't setuid, so a caller that isn't root uses `sudo`.
- In Docker: `--cap-add SYS_ADMIN` with `--security-opt apparmor=unconfined`, or `--privileged`. See [In a Docker image](#in-a-docker-image). Add `--cap-add NET_ADMIN` if the box needs a loopback interface that is up.
- To build: a C compiler, `make` and the Linux kernel headers. Room links only against libc, glibc or musl.

The test suite passes in two environments:

- GitHub's Ubuntu 24.04 runners on kernel 7.0, x86_64 and arm64. CI runs the suite there on every push.
- Docker 29.9.3 on kernel 7.0.14 (linuxkit, aarch64).

In both, the suite runs in a container with `--cap-add SYS_ADMIN --security-opt apparmor=unconfined`, in a container with `--privileged`, and under systemd 257 with `room.service`. A build with AddressSanitizer and UndefinedBehaviorSanitizer also passes, with no reports.

Without systemd, only the cgroup logic is tested, from the root cgroup of Docker's VM.

Not tested:

- Room on a machine or VM outside a container.
- Kernels other than 7.0.
- Other systemd versions and other init systems.
- SELinux, rootless Docker, podman and Kubernetes.

## Install

### Build

```sh
make                 # builds build/room
sudo make install    # installs /usr/local/bin/room
make STATIC=1        # a static PIE that you can copy into any image
```

Room only builds on Linux. `PREFIX` and `DESTDIR` change where `install` puts the binary. `make help` lists every target.

On another system, use Docker. `make image` builds the release image. `make dist` builds static binaries for linux/amd64 and linux/arm64 into `build/dist`.

### In a Docker image

`make image` builds the image `room-release`, which holds only the static binary at `/room`. Copy the binary into your own image:

```dockerfile
FROM python:3.13-slim
COPY --from=room-release /room /usr/local/bin/room
```

Start the container with these two options:

```console
$ docker run --rm --cap-add SYS_ADMIN --security-opt apparmor=unconfined my-image sh -c 'room --init && echo "print(6 * 7)" > /var/lib/room/0/box/main.py && room --run --cg-mem=262144 -t 2 -- /usr/local/bin/python3 main.py'
/var/lib/room/0
42
OK (0.011 sec real, 0.018 sec wall)
```

- `--cap-add SYS_ADMIN` lets room create namespaces and mount filesystems.
- `--security-opt apparmor=unconfined` is for a host with AppArmor, such as Ubuntu. Docker's default AppArmor profile denies every mount, also with `SYS_ADMIN`. On a host without AppArmor the option has no effect.

In Compose:

```yaml
services:
  sandbox:
    image: my-image
    cap_add: [SYS_ADMIN]
    security_opt: [apparmor=unconfined]
```

The first room command in a container changes the container's cgroups:

- It remounts `/sys/fs/cgroup` read-write if Docker mounted it read-only.
- It moves every process of the container, PID 1 too, into a new cgroup `room/main`.
- It creates one cgroup per box next to `room/main`.

Your processes continue to run and `docker exec` still works. The container's limits, such as `--memory` and `--pids-limit`, still apply to everything, boxes included. [docs/DESIGN.md](docs/DESIGN.md#cgroup-layout) gives the reason for the move.

### On a machine with systemd

```sh
make
sudo make install install-systemd
sudo systemctl daemon-reload
sudo systemctl enable --now room.service
```

Under systemd, room only uses the cgroup that systemd delegates to `room.service`. Without the unit, room refuses to run. If you stop the unit, systemd kills every box and removes room's cgroups. Each box then needs `--init` again.

To use another delegated cgroup, set [`ROOM_CG_ROOT`](#environment-variables).

### On a machine without systemd

```sh
make
sudo make install
```

Room then arranges the cgroup it's called from, as it does in a container:

- From the machine's root cgroup, it moves no process. If the root cgroup doesn't pass `memory`, `pids` and `cpuset` to its children, room enables them. That is a machine-wide setting.
- From any other cgroup, it moves every process of that cgroup into `room/main` below it.

To keep room away from your cgroups, give it its own cgroup with [`ROOM_CG_ROOT`](#environment-variables).

## Quick start

Run all commands as root. `main.c` reads two numbers and prints their sum.

Create a box and copy the files into it:

```console
$ BOX=$(room --init)
$ echo $BOX
/var/lib/room/0
$ cp main.c input.txt $BOX/box/
```

Compile in the box. The compiler starts other programs, so it needs `-p` and a `PATH`:

```console
$ room --run -p --cg-mem=524288 -t 10 -E PATH=/usr/bin:/bin -- /usr/bin/gcc -O2 -o prog main.c
OK (0.017 sec real, 0.019 sec wall)
```

Run the result with a 1 second CPU limit and 64 MB of memory. Then read the output and the meta file:

```console
$ room --run --cg-mem=65536 -t 1 -x 0.5 -w 5 -i input.txt -o output.txt -M run.meta -- ./prog
OK (0.000 sec real, 0.001 sec wall)
$ cat $BOX/box/output.txt
42
$ cat run.meta
exitcode:0
time:0.000
time-wall:0.001
max-rss:1252
csw-voluntary:4
csw-forced:1
cg-mem:256
```

A program that never ends gets a verdict:

```console
$ room --run -t 1 -w 5 -M loop.meta -- ./loop
Time limit exceeded
$ cat loop.meta
time:1.048
time-wall:1.049
max-rss:0
csw-voluntary:3
csw-forced:0
cg-mem:256
killed:1
status:TO
message:Time limit exceeded
```

Remove the box:

```console
$ room --cleanup
```

## Boxes

A box is a numbered sandbox. Box N has:

- A directory, `/var/lib/room/N`. Its `box` subdirectory is the program's working directory, which the program sees as `/box`.
- Its own user, with uid and gid 60000 + N. From box 5534 the uid is 60002 + N, because room skips uids 65534 and 65535.
- A cgroup, `room/box-N`, from `--init` to `--cleanup`. Each run gets a new cgroup inside it, so the limits and the results cover that run only.

Use a box in this order:

1. `room -b N --init` creates the box, or resets it if it exists, and prints its directory.
2. Copy the program's files into `box/`.
3. `room -b N --run -- program` runs a program in the box. You can run several, for example a compiler and then its output.
4. Read the output from `box/`.
5. `room -b N --cleanup` removes the box.

Before each run, room gives everything in `box/` to the box user. A regular file with more than one hard link keeps its owner. After each run, room deletes everything in `box/` that isn't a regular file or a directory. Root can then read the output safely. Both steps remove a directory that is nested more than 64 levels deep.

Only one room command can use a box at a time. A second command fails immediately with exit status 2. To run programs at the same time, use one box for each. `-b auto` picks the lowest free number and prints the box directory, which ends in the number:

```console
$ room --init -b auto
/var/lib/room/0
$ room --init -b auto
/var/lib/room/1
$ room -b 1 --run -- ./prog
$ room -b 1 --cleanup
```

Several callers can do this at the same time. A number stays taken from `--init` until `--cleanup`.

### How many boxes

Room has no fixed number of boxes. Each box needs its own uid, so the machine's uids above 60000 set the number. Room reads it from `/proc/self/uid_map` and `/proc/self/gid_map`:

- Outside a user namespace: 2147423646 boxes, numbered from 0.
- In a user namespace with 65536 uids: 5534 boxes.

A box number that is too high is an error. The message gives the number of boxes on the machine. In a test, one container ran 2000 boxes at the same time. [Limits](#limits) gives the cost of a running box.

## Command reference

```
room [options] --init
room [options] --run -- program [arguments]
room [options] --cleanup
room --version | --help
```

Give exactly one command.

| Command | What it does |
| --- | --- |
| `--init` | Creates the box and prints its directory. If the box exists, room first kills what still runs in it and deletes its files. |
| `--run -- program [arguments]` | Runs the program in the box and waits for it. The box must be initialised. |
| `--cleanup` | Kills what still runs in the box and removes the box. Succeeds if the box doesn't exist. |
| `--version` | Prints `room 0.1.0`. |
| `--help` | Prints the options. |

Rules for every command:

- Options go before the program. Room stops reading options at the first argument that isn't an option. The `--` is optional.
- Room passes the program path to `execve()` unchanged and doesn't search `PATH`. Give an absolute path, or a path relative to the working directory.
- If `execve()` fails, room writes the reason to the program's stderr and the run counts as exit code 127.
- `-b N` works with every command. `--quota` and `-b auto` work only with `--init`. All other options are for `--run`, and the other commands ignore them.
- Times are seconds with an optional fraction, such as `2` or `0.5`. Room rounds them to milliseconds and refuses forms such as `.5`, `1e3` and `+1`.
- Sizes are kilobytes of 1024 bytes. Counts and sizes are plain decimal digits.
- For `-t`, `-w`, `-m`, `--cg-mem`, `-k`, `-f` and `-p`, 0 means no limit.
- A usage error exits with status 2.

### Box

| Option | Meaning |
| --- | --- |
| `-b N`, `--box-id=N` | The box number, from 0. The default is 0. Use the same number for `--init`, `--run` and `--cleanup`. |
| `-b auto`, `--box-id=auto` | Picks the lowest free number. See [Boxes](#boxes). |
| `-q KB,INODES`, `--quota=KB,INODES` | Puts the box on an in-memory filesystem (tmpfs) that holds at most KB kilobytes and INODES files and directories. KB is at least 1 and INODES at least 16. |

With a quota, everything in `/box` and `/tmp` counts, and so do the files you copy into the box. Room's own directories use 5 inodes after the first run. For example, `room --init --quota=16384,200` gives the box 16 MB and 200 inodes.

- The files are memory. They count toward `--cg-mem` of the run that writes them, and toward the container's memory.
- You can't hard-link files into a quota box. Copy them.
- `--init` without `--quota` makes a quota box a plain directory again.

### Time

| Option | Meaning |
| --- | --- |
| `-t SEC`, `--time=SEC` | CPU time limit for the whole box: user and system time of all its processes and threads. A run that used more counts as timed out, even if the program exited by itself. No limit by default. |
| `-x SEC`, `--extra-time=SEC` | A grace period on top of `-t`. Room kills the box when its CPU time passes `-t` plus `-x`. The run counts as timed out as soon as it passes `-t`. You then get the real time of a program that is slightly over the limit. The default is 0. |
| `-w SEC`, `--wall-time=SEC` | Wall clock limit from the start of the run. It stops a program that sleeps or waits for input, which uses no CPU time. |

With `-t` and no `-w`, the wall limit is ten times `-t` plus `-x`, and at least 30 seconds. `-w 0` disables the wall limit. With neither `-t` nor `-w`, there is no wall limit. `-v` prints the limit that room uses.

### Memory

| Option | Meaning |
| --- | --- |
| `--cg-mem=KB` | Memory limit for the whole box, through the cgroup's `memory.max`. It covers all processes together, in-memory filesystems such as `/dev/shm`, files on a quota box and the page cache. Swap is off for the box. If the box exceeds the limit, the kernel's OOM killer kills everything in it. No limit by default. |
| `-m KB`, `--mem=KB` | Address space limit for each process (`RLIMIT_AS`). At the limit, allocations fail. Nothing is killed, and the meta file doesn't record it. No limit by default. |
| `-k KB`, `--stack=KB` | Stack size limit (`RLIMIT_STACK`). No limit by default. |

Room's own setup inside the box also runs under `--cg-mem`. A limit that is too small for it, such as 16 KB, ends in an internal error.

### Files, processes and CPUs

| Option | Meaning |
| --- | --- |
| `-f KB`, `--fsize=KB` | The largest file the program may write (`RLIMIT_FSIZE`). A write past it fails and the program gets `SIGXFSZ`, which kills it unless it handles the signal. It limits each file, not the total. No limit by default. |
| `-n N`, `--open-files=N` | The most files a process may have open (`RLIMIT_NOFILE`). The default is 1024. 0 keeps the limit that room started with. Room lowers a value above its own hard limit to that limit. |
| `-p[N]`, `--processes[=N]` | The most processes and threads the box may have at once, through the cgroup's `pids.max`. The default is 1, so `fork` and thread creation fail with `EAGAIN`. `-p` alone removes the limit. Write a number as `-p8` or `--processes=8`. `-p 8` sets no limit and takes `8` as the program. |
| `--cpus=LIST` | The CPUs the box may run on, as numbers and ranges such as `2`, `0-3` or `0-3,6`. It's a list, not a count. Room fails if `cpuset` isn't available or the container can't use a CPU in the list. |

### Input and output

| Option | Meaning |
| --- | --- |
| `-i FILE`, `--stdin=FILE` | Redirects the program's stdin. |
| `-o FILE`, `--stdout=FILE` | Redirects the program's stdout. Room creates the file, or empties it if it exists. |
| `-r FILE`, `--stderr=FILE` | Redirects the program's stderr, in the same way as `-o`. |
| `--stderr-to-stdout` | Sends the program's stderr to the same place as its stdout. |
| `-c DIR`, `--chdir=DIR` | The working directory inside the box. A relative DIR is relative to `/box`. |
| `-M FILE`, `--meta=FILE` | Writes the result of the run to FILE, see [Results](#results). FILE is outside the box, relative to room's own working directory. `-M -` writes to room's stdout. |

The paths for `-i`, `-o` and `-r` are inside the box. A relative path is relative to `/box`, even with `-c`. Room opens the files as the box user. A file that room can't open, such as a missing input, is an internal error.

Without these options the program inherits room's stdin, stdout and stderr, so shell redirection works too.

### Environment

The program's environment starts with one variable, `LIBC_FATAL_STDERR_=1`. There is no `PATH`. A compiler or a shell usually needs one.

| Option | Meaning |
| --- | --- |
| `-E VAR`, `--env=VAR` | Passes VAR with the value it has in room's environment. Does nothing if VAR isn't set there. |
| `-E VAR=VALUE`, `--env=VAR=VALUE` | Sets VAR. |
| `-E VAR=`, `--env=VAR=` | Removes VAR. That also works for `LIBC_FATAL_STDERR_`. |
| `-e`, `--full-env` | Passes room's whole environment. |

`-E` rules apply in the order given, after `-e`.

### Filesystem and network

| Option | Meaning |
| --- | --- |
| `-d RULE`, `--dir=RULE` | Adds, replaces or removes a directory that the program can see. See [Directory rules](#directory-rules). |
| `-D`, `--no-default-dirs` | Omits the default rules. You then need at least a rule for `box`, such as `-D -d box=./box:rw`. |
| `--root=DIR` | Takes the box's system directories from DIR, not from `/`. DIR is an absolute path to an existing directory. See [A root filesystem per language](#a-root-filesystem-per-language). |
| `--share-net` | Keeps the caller's network namespace. By default the box has its own network namespace, where nothing is reachable. Its loopback interface is up only if room has `NET_ADMIN`. |

### Room's own output

| Option | Meaning |
| --- | --- |
| `-s`, `--silent` | Doesn't print the status line. Room still prints its own errors. |
| `-v`, `--verbose` | Prints room's steps to stderr. |
| `--cg` | Ignored. Room always uses cgroups. The option lets command lines written for isolate work. |

## Directory rules

The program sees an empty, read-only root that holds only what the rules put there. A rule has this form:

```
IN[=OUT][:OPTION]...
```

| Form | Meaning |
| --- | --- |
| `IN` | Binds `/IN` from outside at `/IN` in the box. |
| `IN=OUT` | Binds OUT at `/IN`. OUT is an absolute path, or starts with `./` for a path relative to the box directory. OUT can be a single file. |
| `IN=` | Removes the rule for IN, a default rule too. |

IN can't be empty or have a `..`, `.` or empty component. IN and OUT can't contain `:`, and IN can't contain `=`. A rule for an IN that already has a rule replaces it.

Everything is read-only, nosuid and nodev unless an option changes that. Setuid bits never work in the box.

| Option | Meaning |
| --- | --- |
| `rw` | The program can write to it. |
| `noexec` | Files in it can't be executed. |
| `dev` | Device nodes in it work. |
| `maybe` | Skips the rule if OUT doesn't exist. Without it, a missing OUT is an internal error. |
| `norec` | Omits what is mounted below OUT. Without it, the flags also apply to every mount below OUT. |
| `fs` | OUT is a filesystem to mount: `proc`, `sysfs`, `tmpfs` or `devpts`. Room passes no size to a `tmpfs`. Every `proc` shows only the box's own processes and is noexec. |
| `tmp` | A private writable directory that the box user owns. It takes no OUT and keeps its files until the next `--init` or `--cleanup`. |

Examples:

```sh
-d /etc:noexec                 # the container's /etc, read-only, nothing executable
-d /data=/srv/inputs/42        # input data at /data, read-only
-d /out=/srv/results/42:rw     # a directory the program can write to
-d /hosts=/etc/hosts           # a single file
-d /scratch=tmpfs:fs:rw        # an empty in-memory filesystem
-d /work:tmp                   # a private writable directory
-d /opt:maybe                  # the container's /opt, if it has one
-d /tmp=                       # no /tmp at all
-d dev=/dev:dev                # the container's /dev, not the private one
```

The default rules, in this order:

| In the box | What it is |
| --- | --- |
| `/box` | `box=./box:rw`, the box's `box` directory. |
| `/bin` | The container's, read-only. |
| `/dev` | A private one with `null`, `zero`, `full`, `random`, `urandom`, the links `fd`, `stdin`, `stdout` and `stderr`, and a writable `/dev/shm`. |
| `/lib` | The container's, read-only. |
| `/lib64` | The container's, read-only, if it has one. |
| `/proc` | `proc=proc:fs`, a new one. |
| `/tmp` | `tmp:tmp`, a private writable directory. |
| `/usr` | The container's, read-only. |

There's no `/etc`, `/home`, `/root`, `/var` or `/sys` unless you add them.

Rules apply in order: the defaults first, then yours. A rule that replaces a default keeps the default's place, and it stays when you give `-D`. Room creates a mount point that doesn't exist. It can't do that inside a read-only directory, so a rule such as `/usr/x` needs an existing mount point.

### A root filesystem per language

By default the program sees the container's own `/bin`, `/lib` and `/usr`, which includes everything installed there. `--root=DIR` makes every rule without an OUT bind from DIR. That includes the defaults `bin`, `lib`, `lib64` and `usr`. With `--root`, `-d etc` means the `/etc` of DIR.

Room follows symlinks inside DIR as if DIR were `/`, so they can't lead out of it. Rules with an OUT, `fs` rules, `tmp` rules, `/box` and `/dev` don't change.

A Dockerfile can copy a whole image into a directory. That gives one image with a root filesystem per language:

```dockerfile
FROM debian:trixie-slim
COPY --from=room-release /room /usr/local/bin/room
COPY --from=python:3.13-slim / /opt/roots/python
```

```console
$ room --run --root=/opt/roots/python --cg-mem=262144 -t 2 -- /usr/local/bin/python3 main.py
```

The container has no Python, and the program can't see the room binary.

## Environment variables

These configure room itself.

| Variable | Meaning |
| --- | --- |
| `ROOM_BOX_ROOT` | The directory that holds the box directories. The default is `/var/lib/room`. It must be an absolute path. Room keeps its lock files there, in `.locks`. |
| `ROOM_CG_ROOT` | The cgroup in which room creates the box cgroups. Set it when room must leave every other cgroup alone. Room then remounts nothing, moves no process and doesn't look for systemd. |

The box user and the box cgroup depend only on the box number. Never use the same number under two box roots at the same time. `-b auto` never picks a number that is in use under another root.

`ROOM_CG_ROOT` must be an existing cgroup with no processes, whose parent passes `memory` and `pids` to it. A cgroup from a systemd unit with `Delegate=yes` is one example. Room only claims box numbers among room processes that use the same `ROOM_CG_ROOT`.

On kernel 7.0, a run can fail with an internal error if the caller's cgroup had a `cgroup.kill` write before. [docs/DESIGN.md](docs/DESIGN.md#per-box-cgroups) has the details.

## Results

### Exit status

| Status | Meaning |
| --- | --- |
| 0 | The program exited with 0. |
| 1 | The program failed: non-zero exit, killed by a signal, timed out, or room was interrupted. |
| 2 | Room itself failed or was used wrongly. It says nothing about the program. |

### Status line

Unless you give `-s`, room prints one line to stderr. On success it's `OK (0.017 sec real, 0.019 sec wall)`, where "real" is the CPU time. Otherwise it's the message, for example `Time limit exceeded`. Room's own errors start with `room: `.

### Meta file

With `-M`, room writes one `key:value` per line, in this order. Not every key is always there.

| Key | Value |
| --- | --- |
| `exitcode` | The exit code, when the program exited by itself and the verdict isn't a timeout or `Interrupted`. |
| `exitsig` | The signal number, when the program died on a signal or room was interrupted. |
| `time` | CPU time of the box, in seconds with three decimals. |
| `time-wall` | Wall clock time, in seconds with three decimals. |
| `max-rss` | Largest resident set of a single process, in KB. When room kills the box, it doesn't cover the program. Use `cg-mem`. |
| `csw-voluntary`, `csw-forced` | Context switch counts, with the same gap as `max-rss`. |
| `cg-mem` | Peak memory of the box during this run, in KB. Missing before kernel 5.19. |
| `cg-oom-killed` | `1` if the OOM killer fired in the box during this run. |
| `killed` | `1` if room killed the box for a limit or a signal. |
| `status` | `RE` non-zero exit, `SG` died on a signal, `TO` timed out, `XX` room failed. Missing on success. |
| `message` | The reason in words. Missing on success. |

Give each run its own meta file. Two commands that write the same file at the same time overwrite each other.

### How the verdict is decided

The first rule that matches wins:

1. Room got `SIGINT`, `SIGTERM`, `SIGHUP` or `SIGQUIT` before the program ended: `status:SG`, `message:Interrupted`, and `exitsig` is that signal.
2. CPU time over `-t`: `status:TO`, `message:Time limit exceeded`.
3. Wall time over `-w`: `status:TO`, `message:Time limit exceeded (wall clock)`.
4. The program exited with code N: `exitcode:N`. If N isn't 0, `status:RE` and `message:Exited with error status N`.
5. The program died on signal N: `exitsig:N`, `status:SG`, `message:Caught fatal signal N`.

If a signal comes before the program starts, room doesn't start it. A signal after the program ended doesn't change the result.

If room itself fails, it exits with 2 and the meta file has only `status:XX` and `message`. Examples: a box in use, a box that isn't initialised, a box that dies before the program starts, and a redirect that room can't open.

`--run` empties the meta file before it does anything else, so a failed run never leaves an earlier result in it. Room finds three errors before that and leaves the file unchanged: a usage error, a bad `ROOM_BOX_ROOT` and a box number that the machine doesn't have.

### Reading the result

| What happened | What you see |
| --- | --- |
| Success | Exit status 0, no `status` key |
| Time limit exceeded | `status:TO` |
| Memory limit exceeded | `cg-oom-killed:1`, with `status:SG` and `exitsig:9` |
| Crash | `status:SG` and `exitsig` (11 is a segfault, 6 an abort, 8 an arithmetic error) |
| Non-zero exit | `status:RE` and `exitcode` |
| File size limit exceeded, with `-f` | `status:SG` and `exitsig:25` |
| Internal error, not the program's fault | Exit status 2 and `status:XX` |

Time used is `time`. Memory used is `cg-mem`.

## What the program can and can't do

The program runs:

- As the box user, with no supplementary groups, no capabilities and `no_new_privs` set, so setuid programs gain nothing.
- In its own PID, mount, IPC, UTS, network and cgroup namespaces. It sees only its own processes, and its hostname is `room`.
- With only the directories from the rules.
- With stdin, stdout and stderr open and every other descriptor closed.
- In its own session, with no controlling terminal.
- With every signal at its default and none blocked.
- With `RLIMIT_CORE` and `RLIMIT_MEMLOCK` at 0.
- Behind a system call filter.

The filter refuses:

- New namespaces: `unshare`, `setns`, `clone` with a namespace flag, and `clone3`, which libc replaces with `clone`.
- Sockets other than UNIX, IPv4, IPv6 and route netlink. `socketpair` works only for UNIX.
- Kernel keyrings, io_uring, `bpf`, `perf_event_open`, `userfaultfd` and `fanotify_init`.
- The terminal ioctls `TIOCSTI` and `TIOCLINUX`.
- Any system call of a foreign architecture, and on x86_64 the x32 calls.

The filter allows everything else, including threads, `fork` and file locks. [docs/DESIGN.md](docs/DESIGN.md#system-call-filter) has the table.

When the program's main process ends, room kills everything else in the box. Nothing that the program started outlives the run, also when room itself is killed.

## Differences from isolate

- Room isn't setuid and must run as root.
- Cgroups are always on. `--cg` is ignored.
- `--extra-time` is added on top of `--time`.
- `--time` without `--wall-time` gets a default wall limit.
- `--processes` counts every process and thread in the box.
- `--quota=KB,INODES` puts the box on a tmpfs.
- There's no fixed number of boxes.
- `-b auto`, `--cpus` and `--root` are room's own.
- `time`, `cg-mem` and `cg-oom-killed` cover one run, so you can compile and run in the same box.
- `/dev` is a private one with a few devices.
- Room has no `--tty-hack`, `--inherit-fds`, `--special-files`, `--wait`, `--as-uid`, `--as-gid` or `--core`, and no config file. An unknown option is a usage error.

## Limits

- Without `--quota`, a box has no disk limit. `-f` limits a single file, not the total.
- Room walks `box/` before and after every run. That time isn't limited or reported, so a program that leaves very many files slows every later command on its box. With `--quota`, `--cleanup` and the next `--init` discard the filesystem without a walk.
- The memory charged to a box includes the page cache of the files it reads and writes.
- Room checks the CPU limit at most 50 ms apart. A program with many threads can use more than `-t` plus `-x` before room kills it. The reported time is still the real time.
- File locks are allowed, because compilers and runtimes need them. Two cooperating programs in different boxes can signal each other through locks on a shared read-only file.
- With `--share-net`, the program can reach whatever the container can reach.
- Docker's default seccomp profile refuses `pivot_root`, even with `SYS_ADMIN`. Room then moves the new root over the old one and uses `chroot`. The old root stays in the box's mount namespace, but no path leads to it. With `--privileged` or `--security-opt seccomp=unconfined`, room uses `pivot_root` and detaches the old root.
- Box uids start at 60000, which is fixed at build time. Make sure nothing else on the machine uses the uids of your boxes.
- A running box is three processes: the program and two of room's. It also costs memory outside its own limit. With 2000 boxes running at once, the whole container used about 1.5 MB per box. The container's process limit and the caller's open file limit must allow for that.
- No test ran a box in a user namespace with 65536 uids. A test covers only the box count there.

## Development

```sh
make docker-check          # format check, lint, -Werror builds with gcc and clang
make docker-test           # the suite in Docker, with SYS_ADMIN and with --privileged
make docker-test-systemd   # the suite under systemd, with room.service
make docker-test-san       # the suite against a build with ASan and UBSan
```

- [docs/DESIGN.md](docs/DESIGN.md): how room works inside.
- [SECURITY.md](SECURITY.md): how to report a security problem.

Room is released under the MIT licence, see [LICENSE](LICENSE). It's new code, not copied from isolate.
