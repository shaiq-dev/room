# Room design

This document describes how room works inside and what a change must preserve. The options, the meta file and the setup steps are in the [README](../README.md). This document links there and doesn't repeat them.

## Modules

Each source file in `src/` has a header that states its contract. The comments in the headers are the reference for each function.

| File | Responsibility |
| --- | --- |
| `room.h` | `ROOM_VERSION`, the compile-time defaults (`ROOM_BOX_ROOT`, `ROOM_CGROUP_FS`, `ROOM_FIRST_UID`) and the identity of the current box (`box_id`, `box_uid`, `box_gid`, `box_dir`). |
| `util.c`, `util.h` | `die()` and `die_hook`, `msg()` for `-v`, allocation wrappers that die on failure, `read_file()` and `write_file()` for small files such as cgroup files. |
| `main.c` | Option parsing, the box identity, the lock, `-b auto`, `--init` and `--cleanup`. It calls `run_box()` for `--run`. |
| `run.c`, `run.h` | `--run`: the keeper, the proxy, the inside process, the environment rules and the meta file. `struct run_opts` carries the options. |
| `cg.c`, `cg.h` | `cg_init()`, which finds or arranges a delegated cgroup subtree, and the per-box cgroup operations. |
| `fs.c`, `fs.h` | The directory rules, the sandbox root, the walks over the box tree and the disk quota. |
| `seccomp.c`, `seccomp.h` | The system call filter. |

Outside `src/`:

| File | Contents |
| --- | --- |
| `tests/run.sh` | The test suite, one shell function per test. |
| `tests/t.c` | The helper the suite runs in the box, one subcommand per behaviour. |
| `tests/seccomp_sim.c` | Includes `seccomp.c` and runs the filter through a small BPF interpreter against a table of expected results. |
| `systemd/room.service` | The unit that gives room a delegated cgroup under systemd. |
| `Dockerfile` | The stages `build` (static binary on Alpine), `release` (only `/room`), `tools` (format and lint tools), `test` (Debian, the default) and `systemd` (the test image booting systemd). |
| `Makefile` | Build, install, checks and the Docker targets. `make help` lists them. |
| `.github/workflows/` | `ci.yml` runs the checks and the suite, `release.yml` publishes a tagged version. |

Build output goes to `build/`, release binaries to `build/dist/`.

## The box on disk

```
/var/lib/room/            box root, $ROOM_BOX_ROOT
  .locks/                 one lock file per box number that was ever used
    0
  0/                      box directory, printed by --init
    box/                  the program's working directory, seen as /box. Box user, mode 0700
    tmp/                  backing directories of "tmp" rules, one per rule
      tmp                 backs /tmp. The name is IN with "/" replaced by ":"
    root/                 mount point of the sandbox root. Empty when seen from outside
```

`set_box()` in `main.c` fills in the box identity and the lock path. Every other module reads the globals from `room.h`.

### Box numbers and uids

Each box has its own user. The kernel's per-user accounting and permission checks then keep boxes apart where the namespaces don't.

`box_uid_of()` gives box N the uid and gid `ROOM_FIRST_UID` + N, which is 60000 + N. It skips the uids 65534 (`nobody`) and 65535, so from box 5534 the uid is 60002 + N. Every room process computes the same uid for a number, so two boxes never get the same user.

A box number is valid if its uid exists on the machine. `count_boxes()` finds the number of boxes:

1. It reads `/proc/self/uid_map` and `/proc/self/gid_map`.
2. In each map it takes the range that holds uid 60000.
3. It allows every box whose uid and gid are in both ranges and below 2^31. Some software treats a uid as a signed 32-bit number.

Outside a user namespace the maps cover all 32-bit uids, which gives 2147423646 boxes. A user namespace with 65536 uids gives 5534. Room refuses a higher box number, and the message gives the number of boxes. If uid 60000 isn't mapped, room has no uids for boxes and dies.

## Cgroup layout

```
<base>/                   the cgroup room arranges. No processes left here
  room/                   subtree_control: memory pids (cpuset if available)
    main/                 every process that was in <base>
    box-0/                from --init to --cleanup. Never holds a process
      run/                made new for every run. The program lives here
    box-1/
```

`box-N` is the claim on box number N. `box-N/run` holds the program, the limits and the counters. With `ROOM_CG_ROOT` that directory takes the place of `room/` and there is no `<base>` or `main/`.

### Setting up the subtree

`cg_init()` runs at the start of every command and every step is safe to repeat. In this order:

1. `/sys/fs/cgroup` must be a cgroup2 filesystem.
2. With `ROOM_CG_ROOT`, room writes `+memory +pids` to its `cgroup.subtree_control`, adds `+cpuset` if the cgroup has that controller, and stops. It doesn't remount, move processes or look for systemd.
3. Room picks `<base>`. If `/run/systemd/system` exists, `<base>` is the cgroup of `room.service`, `/sys/fs/cgroup/system.slice/room.service`, and room dies if it doesn't exist. Otherwise `find_base()` takes room's own cgroup from the `0::` line of `/proc/self/cgroup`, minus a trailing `/room/main` left by an earlier run.
4. If `<base>` isn't writable because of `EROFS`, `remount_rw()` bind-remounts `/sys/fs/cgroup` read-write. Docker mounts it read-only per mount while the superblock is writable. The remount repeats the per-mount flags it keeps, or the kernel would clear them.
5. `memory` and `pids` must be in `<base>/cgroup.controllers`.
6. Room creates `room/` and `room/main/`.
7. If `<base>/cgroup.subtree_control` lacks `memory` or `pids`, `delegate()` moves every process in `<base>` into `room/main` and then enables the controllers. A cgroup that holds processes can't hand controllers to its children. The machine's root cgroup is exempt from that rule. Room recognises it because it has no `cgroup.events` file, and skips the move.
8. Room enables `memory` and `pids` in `room/`, and `cpuset` in `<base>` and `room/` if `<base>` has it. Only `--cpus` needs `cpuset`, so a failure there is only reported with `-v`. This step is outside `delegate()`, which is skipped once `<base>` passes on `memory` and `pids`.

`move_procs()` writes one PID per `write()`, because the kernel takes one per write. A process that exited meanwhile (`ESRCH`) is fine. `docker exec` can add processes while room moves them, so it reads `cgroup.procs` again until a pass finds it empty, at most 100 passes. If enabling the controllers then fails with `EBUSY`, a process slipped in, and `delegate()` moves again, at most 10 times.

The three environments differ only in step 3 and in what step 7 finds:

- **In a container**, `<base>` is the container's cgroup. Step 4 usually remounts. Step 7 moves the container's processes, PID 1 too, into `room/main`. Later calls find `/room/main` in `/proc/self/cgroup` and get the same `<base>`.
- **Under systemd**, `<base>` is the unit's cgroup. systemd leaves what is below it alone because of `Delegate=yes`. The unit runs `sleep infinity` because systemd 257 removes the cgroup of a unit that has no process, even while the unit is active. Step 7 moves the sleep into `room/main` on the first call. The caller of room stays in its own cgroup, and only the program goes into the box cgroup. Stopping the unit kills every box and removes all of room's cgroups.
- **Without systemd**, `<base>` is the cgroup room is called from. From the root cgroup step 7 moves nothing, but it may enable `memory` and `pids` at the root, and step 8 may enable `cpuset` there.

### Per-box cgroups

- `cg_box_claim()` creates `box-N` with one `mkdir()`. `EEXIST` means the number is taken. On success it enables the controllers in `box-N`, which can pass them on because it never holds a process.
- `cg_box_create()`, for `--init` with a number, claims `box-N` or, if it exists, kills and removes `run/` in it and enables the controllers again in case an earlier `--init` died between the two. It never removes `box-N`, so the claim is never dropped, even for a moment.
- `cg_box_prepare()`, at the start of every run, removes `run/` (killing what is left in it) and creates it again. It then writes `memory.max`, `memory.swap.max` (0, with a memory limit), `memory.oom.group` (1, so the OOM killer takes the whole box and not one process the program might survive), `pids.max` and, with `--cpus`, `cpuset.cpus`. The last two of the memory files are optional and skipped if missing.
- `set_cpus()` reads `cpuset.cpus` and `cpuset.cpus.effective` back after writing. The kernel quietly drops CPUs the parent doesn't have, or gives the box all of the parent's when none are left, so a difference means some CPUs aren't available.
- `cg_box_kill()` writes `1` to `run/cgroup.kill` and polls `cgroup.events` until `populated 0`, at most about 5 seconds. `cgroup.kill` only sends the signals, the tasks still have to exit. A missing `run/` is fine. That is its state before a box's first run.
- `cg_box_remove()`, for `--cleanup`, kills and removes `run/` and then `box-N`. `remove_cg()` retries `rmdir()` on `EBUSY` for up to a second, because a cgroup can stay busy for a moment after its last task is reaped.
- `cg_box_cpu_ms()` reads `usage_usec` from `run/cpu.stat`. Room doesn't enable the cpu controller. `cg_box_stats()` reads `memory.peak` (missing before 5.19, reported as -1) and `oom_kill` from `memory.events`.

A new run cgroup for every run has two reasons. Its counters start at zero, so the numbers cover one run without baselines. And on kernel 7.0 a `clone3()` with `CLONE_INTO_CGROUP` is killed at birth when the target cgroup has had a different number of `cgroup.kill` writes than the caller's cgroup. The keeper kills `run/` after every run, so a reused one would break the next run. `room/main` never gets a `cgroup.kill` write. With `ROOM_CG_ROOT` the caller's cgroup might, and then the box dies before the program starts and the run ends in `XX`.

The per-box functions cope with a cgroup that disappears while they work, for example because `room.service` was stopped.

## Commands

`main()` resets `SIGCHLD` to its default (a caller that ignores it would make every wait fail) and opens `/dev/null` on any of descriptors 0, 1 and 2 that are closed (otherwise room's own pipes could land there and a redirect in the box would replace them). It then installs the default directory rules, parses the options, fills in the default wall limit, checks for root, reads `ROOM_BOX_ROOT` and runs the command. Every usage error exits with 2 before anything is touched.

### Locking

Every command takes `flock(LOCK_EX | LOCK_NB)` on `<box root>/.locks/N` before it looks at the box, and holds it until it exits. A held lock means the command dies with `Box N is in use by another room process`. Nothing waits.

The lock is a file of its own because the box directory gets mounted over, emptied and removed. The file is never removed, or two processes could each hold "the" lock on a different file. It is opened with `O_NOFOLLOW` and `O_CLOEXEC`. A lock file appears for every number a command looks at. `-b auto` only looks at the numbers the cgroup listing suggests, normally one.

During `--run` the proxy and the inside process inherit the locked descriptor, and the inside process keeps it until `execve()`. The lock is released once the keeper and the proxy are gone.

### Picking a box

`pick_box()` asks `cg_box_first_free()` for the lowest number that has no `box-N` cgroup. That function reads the names in `room/` once, so a pick costs one directory listing, whatever the number of boxes. A scan of the numbers from 0 would cost a lock attempt and a failed `mkdir()` for each taken box.

The listing is only a hint. For the number it suggests, `pick_box()` tries the lock without waiting. If it gets the lock, it calls `cg_box_claim()`. If both succeed, that is the box, and room keeps its lock. If either fails, another command took the number first. `pick_box()` then asks for the lowest free number above that one. It dies when the machine has no box number left.

The lock alone isn't enough, because it belongs to one box root. The claim is a `mkdir()` in the cgroup filesystem. Two room processes that share the same `room/` or `ROOM_CG_ROOT` therefore never get the same number, whatever their box roots. An initialised box keeps `box-N` until `--cleanup`, so `pick_box()` never picks its number. A box directory left from a restarted container has no cgroup, so its number is free and `--init` resets the directory.

After a successful claim, `pick_box()` sets `die_hook` to `unclaim()`. If `--init` then fails, the caller never learns the number and nobody would free it. `unclaim()` therefore removes the cgroups again.

### --init

1. `cg_init()`. Create the box root.
2. Settle the number: `pick_box()` for `-b auto`, otherwise the lock and `cg_box_create()`.
3. Create the box directory.
4. If it holds a quota mount from an earlier `--init`, detach it, empty and remove the directory and create it again. Programs running in other boxes hold copies of the mount in their mount namespaces, which keep its memory in use. Removing the directory drops those copies. `EBUSY` means a mount of the user's is below it, and the directory stays a plain one.
5. Remove everything in the box directory.
6. With `--quota`, mount the tmpfs on the box directory.
7. Create `box/` with mode 0700 for the box user. `chmod()` follows `mkdir()` because `mkdir()` is subject to the umask. Print the box directory.

### --cleanup

`cg_init()` and the lock. Then `cg_box_remove()`, and if the box directory exists, detach a quota mount (which throws the files away without walking them), remove the contents and remove the directory. A box that doesn't exist is a success.

### --run

`do_run()` first calls `run_begin()`, which opens the meta file and makes every later `die()` write `status:XX` to it. A failed run must not leave an earlier result there for a caller that reuses the path. It then calls `cg_init()`, takes the lock and checks that `box/` and `box-N` exist, in that order, so a concurrent `--init` or `--cleanup` can't change the box between the check and the run. Then `run_box()` takes over and never returns.

## A run

### Three processes

- **The keeper** is the `room --run` process. It stays outside the box. It enforces the time limits, collects the numbers and writes the meta file.
- **The proxy** is created by the keeper with `clone3()` in new mount, PID, IPC, UTS and (without `--share-net`) network namespaces. It is PID 1 of the new PID namespace and stays in the caller's cgroup. It sets `PR_SET_PDEATHSIG` to `SIGKILL` and ignores the signals the keeper handles, because the keeper decides what a signal means. It starts the inside process, reaps every child (orphans of the program are reparented to it), and writes the inside process's wait status to the keeper. It exits as soon as the inside process has ended.
- **The inside process** is created by the proxy with `clone3()` and `CLONE_INTO_CGROUP`, straight into `box-N/run`. It builds the sandbox, drops privileges and calls `execve()`. It never runs outside the run cgroup, so the limits and the counters cover room's setup in the box as well as the program.

The proxy exists so that the program isn't PID 1 of its PID namespace. A PID 1 doesn't get a signal sent from inside its namespace unless it has a handler for it. A program that was PID 1 couldn't end itself with `abort()`, and room couldn't report its fatal signals.

When PID 1 of a PID namespace exits, the kernel kills everything left in it. So the rest of the box dies when the program's main process ends, and also when the keeper dies, through the proxy's death signal.

### Pipes and the start marker

Two pipes connect the box to the keeper, both close-on-exec.

- The error pipe carries the message of `die()` in the proxy or the inside process. Their `die_hook` is `child_die()`, which writes the message and exits with 2.
- Just before `execve()` the inside process writes a single byte, `START_MARKER` (`0x01`), to the error pipe. No message and no marker means the box died during room's setup. The proxy can still fail after the marker, so the keeper treats any byte after it as a message.
- The status pipe carries the wait status of the inside process.

The proxy checks the write end of the status pipe for `POLLERR` right after it sets its death signal. An error there means the keeper died before the death signal was armed, and the proxy exits.

### The keeper, start to verdict

1. Set `die_hook` to `keeper_die()`. Install the handlers for `SIGINT`, `SIGTERM`, `SIGHUP` and `SIGQUIT`, which only set a flag. Ignore `SIGPIPE`.
2. `cg_box_prepare()`. It comes first because it also kills what a killed keeper left, so nothing is alive while room walks `box/`.
3. `tree_chown()` gives `box/` to the box user.
4. Create the pipes and take the start time. If a signal already arrived, skip to the verdict without starting the program.
5. Start the proxy, with `CLONE_PIDFD`.
6. Poll the pidfd. The timeout is at most 50 ms, shorter when the wall limit is closer. On every wakeup check for a signal, CPU time over `-t` plus `-x` and wall time over `-w`. Any of them kills the proxy with `SIGKILL`, kills the run cgroup and sets `killed`.
7. Take the end time. Reap the proxy with `wait4()`, which gives `max-rss` and the context switches. The proxy only collected the program's usage if the program ended by itself. Kill the run cgroup again so nothing survives.
8. Read the error pipe and the status pipe. Every writer is gone, so the reads can't block.
9. `tree_remove_special()` on `box/`.
10. A message on the error pipe becomes `die()`. No message, no marker and no kill by room also becomes `die()`, with a hint if the OOM killer fired.
11. Read the CPU time and the memory numbers, decide the verdict and write all the keys at once, then the status line, and exit with 0 or 1.

The verdict rules are in the [README](../README.md#how-the-verdict-is-decided). Only a signal that made room kill the box counts, so one that arrives after the program ended changes nothing.

### Inside the box, before execve

The order matters. In this order:

1. `unshare(CLONE_NEWCGROUP)`, so `/proc/self/cgroup` in the box shows `/`. Set the hostname to `room`.
2. `fs_setup_root()`, see below.
3. Without `--share-net`, bring up `lo`. `EPERM` (no `NET_ADMIN`) is only reported with `-v`.
4. Resource limits: `RLIMIT_AS`, `RLIMIT_FSIZE`, `RLIMIT_STACK` (unlimited by default), `RLIMIT_MEMLOCK` 0 and `RLIMIT_CORE` 0. `set_limit()` falls back to the hard limit when raising past it fails with `EPERM`, which needs `CAP_SYS_RESOURCE`.
5. `setsid()`, so the program has no controlling terminal.
6. `setgroups(0, NULL)`, `setresgid()` and `setresuid()` to the box user. From here the process has no capabilities.
7. Open the `-i`, `-o` and `-r` files as the box user, so the program can't reach a file it couldn't open itself. Then `--stderr-to-stdout`.
8. `chdir()` for `-c`. This comes after the redirects, which is why their relative paths stay relative to `/box`.
9. `RLIMIT_NOFILE`. Only now, because room's own descriptors would count against a small limit and block the redirects. The process is no longer root here, so the limit can't go above the hard one.
10. Build the environment.
11. Set every signal to its default and unblock all of them. Ignored signals survive `execve()`, and the program must not inherit what room or its caller ignored.
12. `close_range(3, ~0U, CLOSE_RANGE_CLOEXEC)`, so no descriptor of room's reaches the program.
13. `seccomp_install()`.
14. Write the start marker.
15. `execve()`. If it fails, write the error to the program's stderr and exit with 127. That is a result about the program, because the marker is already written.

### Environment

`env_rule_add()` stores each `-E` argument as given. `build_env()` starts from room's environment with `-e`, or from nothing, sets `LIBC_FATAL_STDERR_=1`, and then applies the rules in order: `VAR` copies room's value if there is one, `VAR=` removes, `VAR=VALUE` sets. A variable that is set again keeps its position.

## Directory rules and the sandbox root

The rules are a singly linked list of `struct rule` in `fs.c`, in the order they apply. `dir_rules_init()` adds the defaults and marks them with `R_DEFAULT`. The `dev` default gets `R_BUILTIN_DEV` and no OUT. `dir_rule_add()` parses one `-d` argument:

- A rule for an IN that is in the list replaces that entry in place, so it keeps its position. Its flags are the new ones, so it loses `R_DEFAULT` and survives `-D`.
- `IN=` unlinks the entry.
- A new IN is appended.
- A rule without OUT keeps `out` as `NULL`. Where it binds from depends on `--root`, which may come later on the command line, so `apply_rule()` decides that.

`fs_setup_root()` runs in the inside process, as root, in the new mount namespace:

1. Make every mount private, so nothing below propagates back to the caller's namespace.
2. Mount a tmpfs on `<box dir>/root`.
3. Apply the rules in order, skipping defaults when `-D` is given.
4. Make the root tmpfs read-only. Not recursively, the rules set their own flags.
5. `chdir()` into it and `pivot_root(".", ".")`, then detach the old root that now sits on top. If `pivot_root` fails with `EPERM`, which Docker's default seccomp profile causes even with `CAP_SYS_ADMIN`, move the new root over `/` with `MS_MOVE` and `chroot(".")`. The old root then stays in the namespace, but no path from the new root leads there and the box user has no capability to `chroot()` out.
6. `chdir("/box")`.

`apply_rule()` handles three kinds of rule:

- The built-in `/dev`: `make_dev()` mounts a tmpfs, binds `null`, `zero`, `full`, `random` and `urandom` from the caller's `/dev` one by one (read-only and noexec, writing to a device node works on a read-only mount), adds the links into `/proc/self/fd` and a tmpfs for `shm`, then makes the `/dev` tmpfs read-only without recursion, so `shm` stays writable. It doesn't expose the caller's other devices, which matters in a privileged container.
- `fs` rules mount a new filesystem of type OUT. Every `proc` gets `hidepid=2` and noexec.
- Everything else is a bind mount, recursive unless `norec`. The source is a `tmp` backing directory, a path inside `--root`, `/IN`, a path relative to the box directory, or OUT.

Afterwards `set_attr()` sets the flags with `mount_setattr()`, recursively unless `norec`, and always with nosuid. `nodev` comes from there too, because `set_attr()` couldn't clear it again for `devpts` with `dev`.

`make_mount_point()` creates the mount point under the new root one component at a time, relative to a directory descriptor, and refuses symlinks. A link that an earlier bind brought in therefore can't make room create or mount anything outside the root. It sets mode 0755 explicitly so room's umask can't make a parent untraversable for the box user.

With `--root`, `open_in_root()` opens IN with `openat2()`, `RESOLVE_IN_ROOT` and `RESOLVE_NO_MAGICLINKS`, so every link on the way resolves as if DIR were `/`. The bind then uses `/proc/self/fd/N` as its source, not the path, which would follow the links in DIR again from `/`.

## Tree walks

Room walks `box/` as root around every run: `tree_chown()` before, `tree_remove_special()` after, and `rm_contents()` for `--init` and `--cleanup`. The program that filled the tree is dead or not yet started, but it may have left traps. All three walks share `walk()`:

- Every step is relative to a directory descriptor and nothing is ever followed. Directories are opened with `O_NOFOLLOW`, entries are examined with `AT_SYMLINK_NOFOLLOW`.
- `walk()` keeps each directory open while it is below it, so the cost is proportional to the number of entries. That needs a descriptor per level, so it recurses for 64 levels (`WALK_MAX_DEPTH`) and no deeper.
- A directory nested deeper is handed to `rm_tree()` and removed, whatever the walk was for. Without that a program could nest directories deep enough to exhaust room's descriptors and leave a box that can't be cleaned up.
- Removal dies at a mount point, detected by a device number that differs from the top's.
- `tree_chown()` keeps the owner of a regular file with more than one hard link unless it already belongs to the box user, so a file linked in from outside `box/` isn't handed to the program.
- `tree_remove_special()` deletes everything that isn't a regular file or a directory, so the caller can read the box afterwards without being tricked by a symlink.

`rm_tree()` holds one descriptor at a time. It deletes what it meets, enters the first directory that isn't empty as soon as it finds one, and climbs back with `..` once a listing finds the current directory empty. Nothing should change the tree meanwhile, but in case something does it recognises the directory it started in by device and inode and never climbs above it. Every climb lists the parent again from its start, so a wide directory below level 64 costs up to the square of its width. Messages show the top and the name with `...` in between, because the full path can be longer than `PATH_MAX`.

## Disk quota

`quota_mount()` mounts a tmpfs on the box directory with the source name `room-quota`, `size=KBk`, `nr_inodes=INODES`, mode 755, nosuid and nodev. `main.c` accepts neither number as 0, which tmpfs would take as no limit. A box without one of the two limits could use up the container's memory. The mount lives in the caller's mount namespace until `--cleanup` or the next `--init` detaches it with `MNT_DETACH`.

`quota_mounted()` recognises the mount exactly. The box directory's device must differ from its parent's, and `/proc/self/mountinfo` must have a line for that device with type `tmpfs` and source `room-quota`. A different mount on a box directory, such as a volume, belongs to the user and is treated as a plain directory.

## System call filter

`seccomp.c` holds the filter as a static array of classic BPF instructions. `seccomp_install()` sets `PR_SET_NO_NEW_PRIVS` and loads it. The process is no longer root at that point, and the kernel only takes a filter from it with that flag set. Everything not listed is allowed.

The program starts with a fixed header:

1. Load the architecture. Anything but the native one kills the process.
2. Load the call number. On x86_64, a number with the x32 bit set gets `ENOSYS`.

Then come the rules. Each one is a self-contained block that leaves the call number in the accumulator, so blocks can be added, removed and reordered freely:

- `DENY(sys, e)`: two instructions. Not the call, skip to the next block.
- `DENY_ARG(sys, i, test, k, e)`: five instructions. Load argument `i`, test it with `JEQ` or `JSET`, deny on a match. A different call and an argument that fails the test both land on the block's final reload of the call number.
- `ONLY_ARG(sys, i, k, e)`: the same shape, denying unless the argument equals `k`.
- `SOCKET_RULE`: eleven instructions for `socket()`, checking the family and, for netlink, the protocol. Every jump stays inside the block and lands on its final reload.

The last instruction allows the call. The filter has 62 instructions on aarch64 and 64 on x86_64. `tests/seccomp_sim.c` runs the same array through an interpreter against 70 cases, which also checks the other architecture's filter when it's cross-compiled.

The argument checks read the low 32 bits of an argument from its first four bytes, so only little-endian 64-bit targets are supported and an x32 build is refused at compile time. Call numbers from 424 up are the same on every architecture, so `seccomp.c` fills them in when the headers are too old to know them. Dropping the rule instead would silently open a hole.

The namespaces do most of the isolation. The filter covers what they don't: kernel objects that aren't namespaced, and kernel interfaces the program in the box has no use for.

| System call | Result | Why |
| --- | --- | --- |
| Any call from a foreign architecture | Process killed | The rules hold the native architecture's call numbers |
| x86_64: a call number with the x32 bit (`0x40000000`) | `ENOSYS` | The same |
| `unshare`, `setns` | `EPERM` | New namespaces. A user namespace gives a process capabilities inside it, and with them kernel code an unprivileged process can't otherwise reach |
| `clone` with any `CLONE_NEW*` flag (mask `0x7E020000`) | `EPERM` | The same |
| `clone3` | `ENOSYS` | Its flags are behind a pointer the filter can't read. libc falls back to `clone`, where they can be checked |
| `add_key`, `request_key`, `keyctl` | `ENOSYS` | Keyrings aren't namespaced, so two runs could pass data through them |
| `io_uring_setup`, `io_uring_enter`, `io_uring_register` | `ENOSYS` | io_uring can create sockets without the `socket` call, past the rule below |
| `socket` with a family other than `AF_UNIX`, `AF_INET`, `AF_INET6` and `AF_NETLINK` | `EAFNOSUPPORT` | Asking for a family can make the kernel load its module. `AF_VSOCK` isn't namespaced |
| `socket` with `AF_NETLINK` and a protocol other than `NETLINK_ROUTE` | `EPROTONOSUPPORT` | libc lists the interfaces over `NETLINK_ROUTE`. Nothing needs the other protocols |
| `socketpair` with a family other than `AF_UNIX` | `EAFNOSUPPORT` | The same as `socket` |
| `ioctl` with `TIOCSTI` or `TIOCLINUX` | `EPERM` | They put input into a terminal, which could be the caller's |
| `fanotify_init`, `bpf`, `perf_event_open`, `userfaultfd` | `EPERM` | Kernel interfaces the program in the box doesn't need |

File locks are allowed on purpose, see [Decisions](#decisions).

## Errors and partial failure

`die()` formats the message into a 1024-byte buffer. If `die_hook` is set, the hook gets the message and must not return. Otherwise `die()` prints `room: <message>` and exits with 2. Usage errors go through `usage_error()` instead, which also exits with 2.

| Process or phase | `die_hook` | What happens |
| --- | --- | --- |
| `main.c`, before and outside a run | none | Message on stderr, exit 2 |
| `--init -b auto`, after the claim | `unclaim()` | Message, `cg_box_remove()`, exit 2. The hook clears itself first, so a failure inside it is a plain `die()` |
| `--run`, from `run_begin()` until `run_box()` | `report_failure()` | `XX` in the meta file, message, exit 2. No cleanup, because until the lock is held the box may belong to another command |
| Keeper, from `run_box()` on | `keeper_die()` | Cleanup, then `report_failure()` |
| Proxy and inside process | `child_die()` | Message on the error pipe, `_exit(2)`. The keeper then dies with that message |

`keeper_die()` runs three cleanup steps in order: kill and reap the proxy, kill the run cgroup, delete the special files in `box/`. A step can die too. That re-enters `keeper_die()`, whose static step counter makes it go on with the next step, so every step runs and the first message, the cause, is the one reported. Control characters in the message become `?`, so it can't add lines to the meta file. The keeper writes every key of a normal result at the end, so a failure leaves only `status` and `message`.

What a failure leaves behind:

- **A failed `--init` with a number** keeps `box-N` and may leave the directory partly emptied, with or without a quota mount. `--run` then says the box isn't initialised if `box/` is missing. Another `--init` or a `--cleanup` repairs it.
- **A failed `--init -b auto`** removes the cgroups it claimed. The directory and any quota mount stay until the next `--init` or `--cleanup` of that number.
- **A failed `--cleanup`** has already removed the cgroups, so `--run` refuses the box. Another `--cleanup` retries the rest.
- **A failed `--run`** leaves an empty run cgroup, which the next `cg_box_prepare()` or `cg_box_remove()` replaces. The box stays usable.
- **A killed keeper** (`SIGKILL`) takes the proxy and the box with it through the death signal. Nothing deletes the special files in `box/` or writes the meta file. The next `--run` kills what is left in the run cgroup before it walks `box/`, and the next `--run` or `--init` cleans `box/`.
- **A usage error, a bad `ROOM_BOX_ROOT` or a box number the machine doesn't have** is found before `run_begin()`, so it leaves an existing meta file as it was. Every later failure of `--run` writes `XX` to it.
- **A failure to write the meta file** at the end is reported by `meta_close()` after the keys were written, so the file may hold a full result while room exits with 2.

## Memory and descriptors

Room is a short-lived process that exits or execs, and much of it allocates without freeing on purpose:

- The rule list, the environment rules and the strings `dir_rule_add()` returns live until the process exits.
- `fs_setup_root()` and everything it calls run in the inside process, which execs soon after, so nothing there is freed. The same holds for the environment that `build_env()` builds.
- The keeper's strings, such as paths in `keeper_die()` and messages from `xasprintf()`, are released by `exit()`.
- Every `die()` exits, so nothing is freed on an error path. That is why `env_rule_add()` reassigns the result of `realloc()` directly.
- Code that runs in a loop frees what it allocates: `set_box()` and `set_paths()` in `cg.c` replace their strings when `-b auto` moves to the next number, and `walk()` frees its path per entry.

The sanitizer build (`make test SAN=1`) runs with `detect_leaks=0`.

Descriptors are opened with `O_CLOEXEC`. A few are held until the process ends: the lock descriptor, the keeper's pidfd and the root descriptor of `open_in_root()`. The inside process marks everything from 3 up close-on-exec before `execve()`.

## Concurrency

Any number of room processes may run at once. Commands on different boxes are independent apart from `cg_init()`, which every command runs. Its steps tolerate another room doing the same at the same time: `mkdir()` accepts `EEXIST`, moving processes is repeated until the cgroup stays empty, and enabling controllers retries on `EBUSY`.

The lock serialises every command on one box number under one box root. It covers the box directory, its quota mount and its cgroups for as long as the command runs. Nothing waits for a lock: the second command fails at once.

The claim (`box-N` in the cgroup filesystem) covers the box number across box roots for the whole time from `--init` to `--cleanup`. It is the only thing that keeps two box roots apart, and only for numbers picked with `-b auto`. With explicit numbers two box roots can use the same number, and then they share the box user and the cgroups.

Signals in the keeper only set a flag. The keeper checks it before it starts the proxy and on every pass of its poll loop, so it acts on a signal within 50 ms. The proxy ignores the same signals, so a signal sent to the whole process group, such as Ctrl-C, reaches only the keeper. The program runs in its own session and doesn't get them.

## Platform dependencies

- **Kernel.** Going by the kernel's documentation: `openat2()` 5.6, `clone3()` with `CLONE_INTO_CGROUP` 5.7, `close_range()` with `CLOSE_RANGE_CLOEXEC` 5.11, `mount_setattr()` 5.12, `cgroup.kill` 5.14 and `memory.peak` 5.19. Room has been tested on 7.0 only. Where libc may lack a wrapper or a constant, room calls `syscall()` and defines the constant itself (`fs.c`, `seccomp.c`), so it builds with older glibc and with musl.
- **Kernel 7.0** kills a `CLONE_INTO_CGROUP` child whose target cgroup has a different `cgroup.kill` count than the caller's cgroup. Hence the run cgroup per run.
- **Cgroup v2.** A cgroup with processes can't enable controllers for its children, except the root. Writes to `cgroup.procs` take one PID. `cpuset.cpus` quietly drops CPUs the parent lacks.
- **Docker.** The cgroup filesystem is mounted read-only per mount and needs `CAP_SYS_ADMIN` to remount. The default seccomp profile refuses `pivot_root`. The box's `lo` only comes up with `NET_ADMIN`.
- **systemd** owns the cgroup tree when `/run/systemd/system` exists. Room then only uses the cgroup of `room.service`, which needs `Delegate=yes` and a process, because systemd 257 removes the cgroup of a unit that has none.
- **Architectures.** The filter supports x86_64 and little-endian aarch64 only.

## Decisions

- **Root only, not setuid.** The caller is trusted, the program isn't. A caller that isn't root uses `sudo`.
- **No libraries.** Room links only against libc. The static release binary is copied into other images as it is.
- **Cgroups always on.** There's no mode without cgroup v2, and `--cg` is ignored.
- **No fixed number of boxes.** A box needs its own uid, so the uid map sets the number of boxes. See [Box numbers and uids](#box-numbers-and-uids).
- **One uid per box, not one for all.** A shared uid would need no uid range. But boxes would then share the kernel's per-user accounting, and one box could use up what another needs.
- **One mkdir() as the claim** on a box number, because it is atomic for every room process that shares the cgroup tree, whatever its box root.
- **A lock file outside the box directory** that is never removed, see [Locking](#locking).
- **A new run cgroup per run**, for per-run numbers and because of the kernel 7.0 behaviour above.
- **`--extra-time` on top of `--time`.** isolate treats it differently.
- **A default wall limit** when only `-t` is given, so a program that sleeps can't hold the box forever.
- **Both quota numbers required,** because a box without one of them can use up the container's memory.
- **File locks allowed,** because blocking them breaks compilers and runtimes, and the leak only matters between programs that cooperate.
- **Strict number parsing.** `strtoll()` and `strtod()` alone would take spaces, signs, exponents and hex.
- **A checked `--cpus` list,** so nothing odd reaches the cgroup file. The kernel checks the rest.
- **No man page.** The README is the option reference.
- **New code under the MIT licence,** nothing copied from isolate.

## Limits

The limits that affect use are in the [README](../README.md#limits). In the implementation:

- `ROOM_FIRST_UID` (60000) is a compile-time constant. The number of boxes follows from it and from the machine's uid map, see [Box numbers and uids](#box-numbers-and-uids).
- A running box costs memory outside its own limit: its two cgroups, its namespaces, the keeper and the proxy. The [README](../README.md#limits) has a measurement.
- `ROOM_CG_ROOT` may be at most `PATH_MAX - 64` characters, because room appends paths to it in `PATH_MAX` buffers.
- A message from the box is cut at 1023 bytes.
- `rm_tree()` costs up to the square of the width of a directory nested more than 64 levels deep.
- The proxy stays in the caller's cgroup, so its own memory isn't charged to the box.
