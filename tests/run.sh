#!/bin/sh
# Test suite for room.
#
# Usage: tests/run.sh ROOM_BINARY [PATTERN]
#
# Runs as root inside a Linux container started with at least
# --cap-add SYS_ADMIN, or under systemd with room.service active. PATTERN
# (or $ROOM_TEST_FILTER) runs only the tests whose name contains it.
# $ROOM_TEST_HELPER points at a prebuilt helper, otherwise t in
# $ROOM_TEST_BUILD (default build/) is used, otherwise tests/t.c is
# compiled. seccomp_sim is taken from $ROOM_TEST_BUILD too, or compiled.
#
# Every test runs in a subshell. An assertion that fails calls fail(), which
# logs why and exits the subshell. skip() exits it with 77. A test that
# reaches its end passes. Each test gets its own box ids ($B and $B2), and
# the runner cleans them up afterwards.

if [ $# -lt 1 ]; then
    echo "usage: $0 ROOM_BINARY [PATTERN]" >&2
    exit 2
fi
ROOM=$1
case $ROOM in /*) ;; *) ROOM=$PWD/$ROOM ;; esac
FILTER=${2:-${ROOM_TEST_FILTER:-}}
TESTDIR=$(cd "$(dirname "$0")" && pwd)
BUILD=${ROOM_TEST_BUILD:-$TESTDIR/../build}
ROOT=${ROOM_BOX_ROOT:-/var/lib/room}

if [ "$(id -u)" != 0 ]; then
    echo "run.sh: must run as root" >&2
    exit 2
fi
if [ ! -x "$ROOM" ]; then
    echo "run.sh: $ROOM is not an executable" >&2
    exit 2
fi

WORK=$(mktemp -d /tmp/room-test.XXXXXX) || exit 2
chmod 755 "$WORK"
trap 'rm -rf "$WORK"' EXIT
trap 'exit 130' INT TERM

HELPER=${ROOM_TEST_HELPER:-$BUILD/t}
if [ ! -x "$HELPER" ]; then
    HELPER=$WORK/t
    if ! gcc -O2 -Wall -static -pthread -o "$HELPER" "$TESTDIR/t.c"; then
        echo "run.sh: cannot build the test helper" >&2
        exit 2
    fi
fi

# <base> of DESIGN.md, the cgroup that holds room/. Under systemd it is the
# cgroup of room.service, otherwise the one this script was started in.
# Room may have moved the script into <base>/room/main already.
if [ -d /run/systemd/system ]; then
    SYSTEMD=1
    CG_BASE=/sys/fs/cgroup/system.slice/room.service
else
    SYSTEMD=
    p=$(sed -n 's/^0:://p' /proc/self/cgroup)
    p=${p%/room/main}
    CG_BASE=/sys/fs/cgroup${p%/}
fi

OUT=$WORK/out
ERR=$WORK/err
META=$WORK/meta
T=/box/t

# ---- helpers used inside tests ----

fail() {
    printf 'FAIL: %s\n' "$*" >> "$LOG"
    exit 1
}

skip() {
    printf '%s\n' "$*" > "$WORK/skip"
    exit 77
}

note() {
    printf '%s\n' "$*" >> "$LOG"
}

# R ARGS: runs room with ARGS. Stdin comes from $IN (default /dev/null).
# $WRAP is an optional command that starts room. Sets RC and leaves the
# output in $OUT and $ERR. Everything goes into the log so a failure can be
# debugged from the output alone.
R() {
    rm -f "$META"
    timeout -k 5 "${TMO:-60}" ${WRAP:-} "$ROOM" "$@" < "${IN:-/dev/null}" > "$OUT" 2> "$ERR"
    RC=$?
    {
        printf '$ room %s\n' "$*"
        printf 'exit status: %s\n' "$RC"
        if [ "$RC" = 124 ] || [ "$RC" = 137 ]; then echo "(killed by the harness timeout)"; fi
        if [ -s "$OUT" ]; then echo "stdout:"; sed -n '1,30p' "$OUT"; fi
        if [ -s "$ERR" ]; then echo "stderr:"; sed -n '1,30p' "$ERR"; fi
        if [ -f "$META" ]; then echo "meta:"; sed 's/^/  /' "$META"; fi
    } >> "$LOG"
}

# rr ARGS: room in box $B with the meta file at $META.
rr() {
    R -b "$B" -M "$META" "$@"
}

# binit [ID]: initialises a box and copies the helper into it as /box/t.
binit() {
    id=${1:-$B}
    R -b "$id" --init
    [ "$RC" = 0 ] || fail "--init of box $id failed"
    cp "$HELPER" "$ROOT/$id/box/t" || fail "cannot copy the helper into box $id"
}

# use_box ID...: has the runner clean these boxes up after the test.
use_box() {
    echo "$@" >> "$WORK/boxes"
}

meta() {
    sed -n "s/^$1://p" "${2:-$META}" | head -n 1
}

has() {
    [ -f "${2:-$META}" ] && grep -q "^$1:" "${2:-$META}"
}

num_le() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 <= b + 0) }'
}

want_rc() {
    [ "$RC" = "$1" ] || fail "room exited with $RC, expected $1${2:+ ($2)}"
}

want_eq() {
    [ "$1" = "$2" ] || fail "${3:-value} is '$1', expected '$2'"
}

want_meta() {
    has "$1" || fail "meta has no $1 key, expected $1:$2"
    want_eq "$(meta "$1")" "$2" "meta $1"
}

want_key() {
    has "$1" || fail "meta has no $1 key"
}

want_nokey() {
    if has "$1"; then fail "meta has $1:$(meta "$1"), expected no $1 key"; fi
}

# want_range KEY LO HI: numeric meta value in [LO, HI].
want_range() {
    v=$(meta "$1")
    [ -n "$v" ] || fail "meta has no $1 key"
    num_le "$2" "$v" && num_le "$v" "$3" || fail "meta $1 is $v, expected between $2 and $3"
}

want_line() {
    grep -qxF -- "$1" "${2:-$OUT}" || fail "no line '$1' in ${2:-stdout}"
}

# field WORD: second field of the stdout line that starts with WORD.
field() {
    awk -v k="$1" '$1 == k { print $2; exit }' "$OUT"
}

# leftovers UID [STRING]: processes running as UID, or whose command line
# contains STRING.
leftovers() {
    ps -eo pid=,uid=,args= | awk -v u="$1" -v m="${2:-}" \
        '$2 == u || (m != "" && index($0, m) && $3 !~ /(awk|ps)$/) { print }'
}

wait_gone() {
    i=0
    while [ -n "$(leftovers "$@")" ]; do
        i=$((i + 1))
        [ $i -gt 30 ] && return 1
        sleep 0.1
    done
    return 0
}

wait_started() {
    i=0
    while [ -z "$(leftovers "$1")" ]; do
        i=$((i + 1))
        [ $i -gt 50 ] && return 1
        sleep 0.1
    done
    return 0
}

alive() {
    case $(ps -o stat= -p "$1" 2>/dev/null) in
        '' | Z*) return 1 ;;
    esac
    return 0
}

# timed COMMAND...: runs COMMAND and sets MS to its wall time in milliseconds.
timed() {
    t0=$(date +%s%N)
    "$@"
    MS=$((($(date +%s%N) - t0) / 1000000))
    note "that took $MS ms"
}

# have_peak: the box cgroup of $B reports peak memory, so meta has cg-mem.
# Call it after binit. Kernels 5.14 to 5.18 have no memory.peak.
have_peak() {
    [ -e "$(cg_box_dir "$B")/memory.peak" ]
}

has_net_admin() {
    cap=$(awk '/^CapEff:/ { print $2 }' /proc/self/status)
    [ $(((0x$cap >> 12) & 1)) = 1 ]
}

# The cgroup directory of box $1.
cg_box_dir() {
    if [ -n "${ROOM_CG_ROOT:-}" ]; then
        echo "$ROOM_CG_ROOT/box-$1"
    else
        echo "$CG_BASE/room/box-$1"
    fi
}

# ---- lifecycle ----

t_init_prints_dir() {
    R -b "$B" --init
    want_rc 0
    want_eq "$(cat "$OUT")" "$BD" "--init output"
    [ -d "$BD/box" ] || fail "$BD/box is missing"
    want_eq "$(stat -c '%u %g %a' "$BD/box")" "$U $U 700" "owner, group and mode of box/"
}

t_init_resets() {
    binit
    echo junk > "$BD/box/junk"
    mkdir "$BD/box/dir"
    echo x > "$BD/box/dir/file"
    R -b "$B" --init
    want_rc 0
    [ -e "$BD/box/junk" ] && fail "box/junk survived --init"
    [ -e "$BD/box/dir" ] && fail "box/dir survived --init"
    [ -d "$BD/box" ] || fail "box/ is missing after the second --init"
}

t_cleanup() {
    binit
    R -b "$B" --cleanup
    want_rc 0
    [ -e "$BD" ] && fail "$BD still exists after --cleanup"
    R -b "$B" --cleanup
    want_rc 0 "cleanup of a missing box"
    R -b "$B2" --cleanup
    want_rc 0 "cleanup of a box that never existed"
}

t_run_uninitialised() {
    rr --run -- $T exit 0
    want_rc 2
}

t_box_root_env() {
    export ROOM_BOX_ROOT="$WORK/alt"
    R -b "$B" --init
    want_rc 0
    want_eq "$(cat "$OUT")" "$WORK/alt/$B" "--init output"
    [ -d "$WORK/alt/$B/box" ] || fail "$WORK/alt/$B/box is missing"
    [ -e "$ROOT/$B" ] && fail "the box was also created in $ROOT"
    cp "$HELPER" "$WORK/alt/$B/box/t"
    rr --run -- $T exit 0
    want_rc 0
    R -b "$B" --cleanup
    want_rc 0
    [ -e "$WORK/alt/$B" ] && fail "$WORK/alt/$B still exists after --cleanup"
}

t_box_root_relative() {
    cd "$WORK" || fail "cannot change to $WORK"
    export ROOM_BOX_ROOT=relroot
    R -b "$B" --init
    want_rc 2 "--init with a relative ROOM_BOX_ROOT"
    [ -e "$WORK/relroot" ] && fail "--init created $WORK/relroot"
    R -b "$B" --cleanup
    want_rc 2 "--cleanup with a relative ROOM_BOX_ROOT"
}

t_usage_errors() {
    binit
    R
    want_rc 2 "no command"
    R -b "$B" --bogus --run -- $T exit 0
    want_rc 2 "unknown option"
    # too high a number that is valid is box_too_high
    for v in -1 99999999999 2147483648 abc; do
        R -b "$v" --init
        want_rc 2 "box id $v"
        [ -e "$ROOT/$v" ] && fail "box $v was created"
    done
    R -b "$B" -t abc --run -- $T exit 0
    want_rc 2 "time abc"
    R -b "$B" -w 1x --run -- $T exit 0
    want_rc 2 "wall time 1x"
    for v in " +3" +3 " 3" 3x; do
        R -b "$v" --cleanup
        want_rc 2 "box id '$v'"
    done
    for v in 0x1p-1 1e3 " 1" +1 -0 inf nan; do
        R -b "$B" -t "$v" --run -- $T exit 0
        want_rc 2 "time '$v'"
    done
    R -b "$B" -m " 100" --run -- $T exit 0
    want_rc 2 "memory limit ' 100'"
    R -b "$B" -t 0.5 -w 2 --run -- $T exit 0
    want_rc 0 "time 0.5 and wall time 2"
    R -b "$B" --run
    want_rc 2 "--run without a program"
    R -b "$B" --run --
    want_rc 2 "--run -- without a program"
}

t_version() {
    R --version
    want_rc 0
    want_eq "$(head -n 1 "$OUT")" "room 0.1.0" "first line of --version"
}

t_run_twice() {
    binit
    rr --run -- $T exit 0
    want_rc 0 "first run"
    rr --run -- $T exit 0
    want_rc 0 "second run"
}

# ---- results and limits ----

t_success() {
    binit
    rr --run -- $T exit 0
    want_rc 0
    want_meta exitcode 0
    want_nokey status
    want_nokey message
    want_nokey killed
    want_nokey exitsig
    grep -Eq '^time:[0-9]+\.[0-9]{3}$' "$META" || fail "time is not a number with three decimals"
    grep -Eq '^time-wall:[0-9]+\.[0-9]{3}$' "$META" || fail "time-wall is not a number with three decimals"
    grep -Eq '^max-rss:[0-9]+$' "$META" || fail "max-rss is not a number"
}

t_exit_code() {
    binit
    rr --run -- $T exit 3
    want_rc 1
    want_meta exitcode 3
    want_meta status RE
    want_meta message "Exited with error status 3"
    want_nokey exitsig
}

t_abort() {
    binit
    rr --run -- $T abort
    want_rc 1
    want_meta exitsig 6
    want_meta status SG
    want_meta message "Caught fatal signal 6"
    want_nokey exitcode
}

t_segv() {
    binit
    rr --run -- $T segv
    want_rc 1
    want_meta exitsig 11
    want_meta status SG
    want_meta message "Caught fatal signal 11"
}

t_not_found() {
    binit
    rr --run -- /box/missing
    want_rc 1
    want_meta exitcode 127
    want_meta status RE
}

t_cpu_limit() {
    binit
    TMO=20 rr -t 0.5 --run -- $T spin
    want_rc 1
    want_meta status TO
    want_meta killed 1
    want_meta message "Time limit exceeded"
    want_range time 0.5 1.0
}

t_extra_time() {
    binit
    TMO=20 rr -t 0.3 -x 0.6 --run -- $T spin
    want_rc 1
    want_meta status TO
    want_meta message "Time limit exceeded"
    want_range time 0.9 1.6
}

t_over_time_exits() {
    binit
    TMO=20 rr -t 0.3 -x 5 --run -- $T spin-ms 600
    want_rc 1
    want_meta status TO
    want_meta message "Time limit exceeded"
    want_nokey killed
    want_range time 0.6 5
}

t_wall_limit() {
    binit
    TMO=20 rr -w 0.5 --run -- $T sleep 10
    want_rc 1
    want_meta status TO
    want_meta killed 1
    want_meta message "Time limit exceeded (wall clock)"
    want_range time-wall 0.5 1.5
}

t_cpu_per_run() {
    binit
    rr --run -- $T spin-ms 400
    want_rc 0 "spin-ms 400"
    want_range time 0.4 10
    rr -t 0.3 --run -- $T exit 0
    want_rc 0 "second run"
    want_nokey status
    want_range time 0 0.2
}

# The keeper writes cgroup.kill. A run that reused that cgroup would be
# killed at birth on kernel 7.0, see DESIGN.md.
t_run_after_kill() {
    binit
    TMO=20 rr -t 0.3 --run -- $T spin
    want_rc 1 "spin"
    want_meta status TO
    want_meta killed 1
    rr --run -- $T exit 0
    want_rc 0 "the run after the killed one"
    want_meta exitcode 0
    want_range time 0 0.2
}

t_cg_mem_oom() {
    binit
    rr --cg-mem=65536 --run -- $T alloc 20
    want_rc 0 "alloc 20 before"
    want_nokey cg-oom-killed
    rr --cg-mem=65536 --run -- $T alloc 200
    want_rc 1 "alloc 200"
    want_meta exitsig 9
    want_meta status SG
    want_meta cg-oom-killed 1
    rr --cg-mem=65536 --run -- $T alloc 20
    want_rc 0 "alloc 20 after the OOM run"
    want_nokey cg-oom-killed
}

t_cg_mem_report() {
    binit
    have_peak || skip "the box cgroup has no memory.peak"
    rr --run -- $T alloc 100
    want_rc 0
    want_range cg-mem 100000 140000
}

t_cg_mem_per_run() {
    binit
    have_peak || skip "the box cgroup has no memory.peak"
    rr --run -- $T alloc 100
    want_rc 0 "alloc 100"
    want_range cg-mem 100000 140000
    rr --run -- $T alloc 5
    want_rc 0 "alloc 5"
    want_range cg-mem 0 50000
}

t_mem_as_limit() {
    binit
    rr -m 65536 --run -- $T alloc 200
    want_rc 1
    want_meta status RE
    want_meta exitcode 3
    want_nokey cg-oom-killed
    want_line "alloc failed"
}

t_procs_default() {
    binit
    rr --run -- $T fork 1
    want_rc 0
    want_line "created=0 err=EAGAIN"
}

t_procs_limit() {
    binit
    rr -p4 --run -- $T fork 10
    want_rc 0
    n=$(sed -n 's/^created=\([0-9]*\).*/\1/p' "$OUT")
    [ -n "$n" ] && [ "$n" -ge 1 ] && [ "$n" -le 3 ] || fail "created '$n' children with -p4, expected 1 to 3"
}

t_procs_unlimited() {
    binit
    rr -p --run -- $T fork 10
    want_rc 0
    want_line "created=10 err=none"
}

t_threads() {
    binit
    rr --run -- $T threads 4
    grep -qxF "threads=4 err=none" "$OUT" && fail "4 threads were created with the default -p"
    rr -p8 --run -- $T threads 4
    want_rc 0 "-p8"
    want_line "threads=4 err=none"
}

t_forkbomb() {
    binit
    TMO=30 rr -p20 -w 1 --run -- $T forkbomb
    want_rc 1
    want_meta status TO
    want_meta message "Time limit exceeded (wall clock)"
    wait_gone "$U" "$T forkbomb" || fail "processes left after the run: $(leftovers "$U" "$T forkbomb")"
}

t_fsize() {
    binit
    rr -f 1024 --run -- $T bigfile 2048
    want_rc 1
    if [ "$(meta exitsig)" != 25 ] && ! grep -q EFBIG "$OUT"; then
        fail "expected exitsig:25 or EFBIG"
    fi
    size=$(stat -c %s "$BD/box/big" 2>/dev/null || echo 0)
    [ "$size" -le 1048576 ] || fail "box/big is $size bytes, over 1024 KB"
}

t_open_files() {
    binit
    rr -n 10 --run -- $T openmany 20
    want_rc 0
    grep -q "err=EMFILE" "$OUT" || fail "opening 20 files with -n 10 did not fail with EMFILE"
    rr --run -- $T openmany 500
    want_rc 0 "default limit"
    grep -q "^opened=500 err=none" "$OUT" || fail "could not open 500 files with the default limit of 1024"
    rr -n 4 -o out.txt --run -- $T echo hello
    want_rc 0 "-n 4 with -o"
    want_eq "$(cat "$BD/box/out.txt")" hello "out.txt with -n 4"
}

t_orphans() {
    binit
    TMO=20 rr -p4 --run -- $T daemon
    want_rc 0
    want_meta exitcode 0
    want_range time-wall 0 3
    wait_gone "$U" || fail "the background child is still running: $(leftovers "$U")"
}

t_rlimits() {
    binit
    rr --run -- $T limits
    want_rc 0
    want_line "stack unlimited"
    want_line "nofile 1024"
    want_line "core 0"
    want_line "memlock 0"
    rr -k 8192 -f 100 -m 100000 --run -- $T limits
    want_rc 0 "with -k -f -m"
    want_line "stack 8388608"
    want_line "fsize 102400"
    want_line "as 102400000"
}

# ---- isolation ----

t_identity() {
    binit
    rr --run -- $T ids
    want_rc 0
    want_line "uid=$U $U $U"
    want_line "gid=$U $U $U"
    want_line "groups="
    want_line "CapEff=0000000000000000"
    want_line "NoNewPrivs=1"
    want_line "Seccomp=2"
    want_line "session=leader"
}

t_no_leaked_fds() {
    binit
    exec 7< /dev/null 8> /dev/null
    rr --run -- $T fds
    want_rc 0
    want_line "fds=0 1 2"
}

t_fs_readonly() {
    binit
    rr --run -- $T write /x /usr/x /bin/x /dev/x
    want_rc 0
    for p in /x /usr/x /bin/x /dev/x; do
        want_eq "$(field "$p")" EROFS "writing $p"
    done
}

t_fs_writable() {
    binit
    rr --run -- $T write /box/new /tmp/new /dev/shm/new /dev/null
    want_rc 0
    for p in /box/new /tmp/new /dev/shm/new /dev/null; do
        want_eq "$(field "$p")" OK "writing $p"
    done
}

t_fs_hidden() {
    binit
    binit "$B2"
    echo secret > "$ROOT/$B2/box/secret"
    rr --run -- $T stat /etc /root /var "$ROOT" "$ROOT/$B2"
    want_rc 0
    for p in /etc /root /var "$ROOT" "$ROOT/$B2"; do
        want_eq "$(field "$p")" ENOENT "stat $p"
    done
    rr --run -- $T ls /
    for e in $(cat "$OUT"); do
        case $e in
            box | bin | dev | lib | lib64 | proc | tmp | usr) ;;
            *) fail "unexpected entry /$e in the box root" ;;
        esac
    done
}

t_proc_own() {
    binit
    rr --run -- $T procs
    want_rc 0
    n=$(sed -n 's/^count=//p' "$OUT")
    [ -n "$n" ] && [ "$n" -ge 1 ] && [ "$n" -le 3 ] || fail "/proc shows $n processes, expected 1 to 3"
    awk -v u="$U" '$1 == "proc" && $3 != u { bad = 1 } END { exit bad }' "$OUT" ||
        fail "/proc shows a process that isn't the box user's"
}

t_dev_contents() {
    binit
    rr --run -- $T ls /dev
    want_rc 0
    want_eq "$(cat "$OUT")" "fd full null random shm stderr stdin stdout urandom zero" "/dev"
}

t_net_default() {
    binit
    rr --run -- $T net
    want_rc 0
    # A new network namespace can also get the kernel's fallback tunnel
    # devices (tunl0, gre0, sit0 and so on). They are always down.
    grep -qx "if lo [a-z]*" "$OUT" || fail "no lo interface in the box"
    up=$(awk '$1 == "if" && $2 != "lo" && $3 != "down" { printf "%s ", $2 }' "$OUT")
    [ -z "$up" ] || fail "interfaces up in the box besides lo: $up"
    for f in /sys/class/net/*; do
        name=${f##*/}
        [ -d "$f" ] && [ "$name" != lo ] || continue
        [ $(($(cat "$f/flags") & 1)) = 1 ] || continue
        grep -q "^if $name " "$OUT" && fail "the container's interface $name is visible in the box"
    done
    c=$(field connect)
    [ -n "$c" ] && [ "$c" != OK ] || fail "connect to 1.1.1.1 gave '$c', expected an error"
}

t_net_loopback_up() {
    has_net_admin || skip "the container has no NET_ADMIN"
    binit
    rr --run -- $T net
    want_rc 0
    want_line "if lo up"
}

t_net_share() {
    binit
    rr --share-net --run -- $T net
    want_rc 0
    inside=$(awk '$1 == "if" { print $2 }' "$OUT" | sort | tr '\n' ' ')
    # Interfaces are the directories here. bonding_masters is a plain file.
    outside=$(for f in /sys/class/net/*; do [ -d "$f" ] && echo "${f##*/}"; done | sort | tr '\n' ' ')
    want_eq "$inside" "$outside" "interfaces with --share-net"
}

t_seccomp_table() {
    binit
    rr -p4 --run -- $T sys unshare-user unshare-0 setns clone-newuser clone-newns \
        clone3 add_key request_key keyctl io_uring_setup io_uring_enter \
        io_uring_register vsock packet alg netlink-diag socketpair-inet fanotify \
        tiocsti tioclinux bpf perf_event_open userfaultfd
    want_rc 0
    bad=
    for pair in unshare-user:EPERM unshare-0:EPERM setns:EPERM clone-newuser:EPERM \
        clone-newns:EPERM clone3:ENOSYS add_key:ENOSYS request_key:ENOSYS \
        keyctl:ENOSYS io_uring_setup:ENOSYS io_uring_enter:ENOSYS \
        io_uring_register:ENOSYS vsock:EAFNOSUPPORT packet:EAFNOSUPPORT \
        alg:EAFNOSUPPORT netlink-diag:EPROTONOSUPPORT socketpair-inet:EAFNOSUPPORT \
        fanotify:EPERM tiocsti:EPERM \
        tioclinux:EPERM bpf:EPERM perf_event_open:EPERM userfaultfd:EPERM; do
        name=${pair%%:*}
        want=${pair#*:}
        got=$(field "$name")
        [ "$got" = "$want" ] || bad="$bad $name=$got(want $want)"
    done
    [ -z "$bad" ] || fail "wrong results:$bad"
}

t_seccomp_arch() {
    [ "$(uname -m)" = x86_64 ] || skip "x32 and i386 checks are for x86_64 only"
    binit
    rr --run -- $T sys x32
    want_rc 0
    want_eq "$(field x32)" ENOSYS "x32 system call"
    "$HELPER" foreign > /dev/null 2>&1 || skip "this kernel has no i386 entry point"
    rr --run -- $T foreign
    want_rc 1
    want_meta status SG
    want_meta exitsig 31
}

t_seccomp_allowed() {
    binit
    rr -p8 --run -- $T threads 4
    want_rc 0 "threads with -p8"
    rr -p4 --run -- $T fork 3
    want_line "created=3 err=none"
    rr --run -- $T flock
    want_line "flock OK"
    want_line "fcntl OK"
    rr -p4 --run -- $T sys clone-plain socket-inet socket-inet6 socket-unix netlink-route \
        socketpair-unix getifaddrs
    want_line "clone-plain OK"
    want_line "socket-inet OK"
    want_line "socket-inet6 OK"
    want_line "socket-unix OK"
    want_line "netlink-route OK"
    want_line "socketpair-unix OK"
    want_line "getifaddrs OK"
}

t_special_files() {
    binit
    rr --run -- $T mklinks
    want_rc 0
    want_line "symlink OK"
    want_line "fifo OK"
    [ -L "$BD/box/link" ] && fail "the symlink is still in box/"
    [ -p "$BD/box/fifo" ] && fail "the fifo is still in box/"
    [ -f "$BD/box/regular" ] || fail "the regular file is gone from box/"
}

t_caller_file() {
    binit
    echo hello > "$BD/box/given"
    chmod 600 "$BD/box/given"
    mkdir "$BD/box/given-dir"
    chmod 700 "$BD/box/given-dir"
    rr --run -- $T read /box/given
    want_line "/box/given OK"
    rr --run -- $T write /box/given /box/given-dir/new
    want_line "/box/given OK"
    want_line "/box/given-dir/new OK"
}

# ---- options ----

t_redirect() {
    binit
    printf 'hello\n' > "$BD/box/in.txt"
    printf 'old content that is longer\n' > "$BD/box/out.txt"
    rr -i /box/in.txt -o /box/out.txt -r /box/err.txt --run -- $T stdin-echo ERRMSG
    want_rc 0
    want_eq "$(cat "$BD/box/out.txt")" hello "out.txt"
    want_eq "$(cat "$BD/box/err.txt")" ERRMSG "err.txt"
    want_eq "$(stat -c %u "$BD/box/out.txt")" "$U" "owner of out.txt"
    want_eq "$(stat -c %u "$BD/box/err.txt")" "$U" "owner of err.txt"
    [ -s "$OUT" ] && fail "room's stdout is not empty"
    grep -q ERRMSG "$ERR" && fail "the program's stderr reached room's stderr"
    return 0
}

t_stderr_to_stdout() {
    binit
    printf 'hello\n' > "$BD/box/in.txt"
    rr -i /box/in.txt -o /box/out.txt --stderr-to-stdout --run -- $T stdin-echo ERRMSG
    want_rc 0
    want_line hello "$BD/box/out.txt"
    want_line ERRMSG "$BD/box/out.txt"
}

t_inherit_std() {
    binit
    printf 'piped in\n' > "$WORK/input"
    IN=$WORK/input rr -s --run -- $T stdin-echo ERRMSG
    want_rc 0
    want_eq "$(cat "$OUT")" "piped in" "stdout"
    want_line ERRMSG "$ERR"
}

t_closed_stdio() {
    binit
    timeout -k 5 60 "$ROOM" -b "$B" -o out.txt --run -- $T echo hello <&- >&- 2>&-
    RC=$?
    note "-o out.txt with stdio closed: exit status $RC"
    want_rc 0
    want_eq "$(cat "$BD/box/out.txt" 2> /dev/null)" hello "out.txt"
    # -r must not replace the pipe that carries room's own errors out of the box
    timeout -k 5 60 "$ROOM" -b "$B" -r err.txt -c nope --run -- $T exit 0 <&- >&- 2>&-
    RC=$?
    note "-r err.txt -c nope with stdio closed: exit status $RC"
    note "box/err.txt: $(cat "$BD/box/err.txt" 2> /dev/null)"
    want_rc 2 "a missing -c directory with -r and stdio closed"
}

t_chdir() {
    binit
    mkdir "$BD/box/sub"
    rr -c sub --run -- $T cwd
    want_rc 0
    want_eq "$(cat "$OUT")" /box/sub "cwd with -c sub"
    rr -c nope --run -- $T cwd
    want_rc 2 "missing directory"
    want_meta status XX
}

t_xx_special_files() {
    binit
    ln -s /etc/passwd "$BD/box/link"
    rr -c nope --run -- $T exit 0
    want_rc 2
    want_meta status XX
    [ -L "$BD/box/link" ] && fail "the symlink is still in box/ after a run that ended in XX"
    want_eq "$(cut -d: -f1 "$META" | tr '\n' ' ')" "status message " "keys of an XX meta file"
}

t_xx_first_message() {
    binit
    # chown fails on a read-only mount in box/, and so does removing the
    # special files on it during the cleanup after that
    mkdir "$WORK/ro" "$BD/box/ro" && ln -s /etc/passwd "$WORK/ro/link" ||
        fail "cannot prepare $WORK/ro"
    mount --bind -o ro "$WORK/ro" "$BD/box/ro" || fail "cannot bind $WORK/ro read-only"
    rr --run -- $T exit 0
    umount "$BD/box/ro"
    want_rc 2 "read-only mount in box/"
    want_meta status XX
    case $(meta message) in
        "Cannot chown "*) ;;
        *) fail "message is '$(meta message)', expected the first failure, Cannot chown ..." ;;
    esac

    # Let room remove the run cgroup at the start of the run but not create
    # it again. Killing the missing cgroup during the cleanup must be quiet.
    parent=$(cg_box_dir "$B")
    parent=${parent%/*}
    n=$(sed -n 's/^nr_descendants //p' "$parent/cgroup.stat")
    echo $((n - 1)) > "$parent/cgroup.max.descendants" || fail "cannot limit $parent"
    rr --run -- $T exit 0
    echo max > "$parent/cgroup.max.descendants"
    want_rc 2 "box cgroup can't be created"
    want_meta status XX
    case $(meta message) in
        "Cannot create cgroup "*) ;;
        *) fail "message is '$(meta message)', expected the first failure, Cannot create cgroup ..." ;;
    esac
}

t_env_default() {
    binit
    rr --run -- $T env
    want_rc 0
    want_eq "$(cat "$OUT")" "LIBC_FATAL_STDERR_=1" "environment"
}

t_env_rules() {
    binit
    export ROOMTEST_IN=hello
    rr -E FOO=bar -E ROOMTEST_IN -E LIBC_FATAL_STDERR_= --run -- $T env
    want_rc 0
    want_eq "$(cat "$OUT" | tr '\n' ' ')" "FOO=bar ROOMTEST_IN=hello " "environment"
}

t_env_full() {
    binit
    export ROOMTEST_IN=hello ROOMTEST_GONE=x
    # Rules apply after --full-env even when they come first.
    rr -E ROOMTEST_GONE= -e --run -- $T env
    want_rc 0
    want_line "ROOMTEST_IN=hello"
    want_line "PATH=$PATH"
    grep -q '^ROOMTEST_GONE=' "$OUT" && fail "ROOMTEST_GONE was not removed"
    return 0
}

t_dir_ro() {
    binit
    mkdir "$WORK/data"
    echo data > "$WORK/data/f"
    chmod 777 "$WORK/data"
    chmod 666 "$WORK/data/f"
    rr -d data="$WORK/data" --run -- $T read /data/f
    want_line "/data/f OK"
    rr -d data="$WORK/data" --run -- $T write /data/f /data/new
    want_line "/data/f EROFS"
    want_line "/data/new EROFS"
}

t_dir_rw() {
    binit
    mkdir "$WORK/data"
    chmod 777 "$WORK/data"
    rr -d data="$WORK/data:rw" --run -- $T write /data/new
    want_line "/data/new OK"
    [ -f "$WORK/data/new" ] || fail "/data/new did not appear outside"
}

t_dir_noexec() {
    binit
    mkdir "$WORK/exe"
    cp "$HELPER" "$WORK/exe/t"
    chmod 755 "$WORK/exe" "$WORK/exe/t"
    rr -d exe="$WORK/exe" --run -- /exe/t exit 0
    want_rc 0 "without noexec"
    rr -d exe="$WORK/exe:noexec" --run -- /exe/t exit 0
    want_rc 1 "with noexec"
    want_meta exitcode 127
}

t_dir_maybe() {
    binit
    rr -d nothere="$WORK/missing:maybe" --run -- $T stat /nothere
    want_rc 0 "with maybe"
    want_line "/nothere ENOENT"
    rr -d nothere="$WORK/missing" --run -- $T exit 0
    want_rc 2 "missing path without maybe"
    want_meta status XX
}

t_dir_file() {
    binit
    echo single > "$WORK/single.txt"
    chmod 666 "$WORK/single.txt"
    rr -d single.txt="$WORK/single.txt" --run -- $T cat /single.txt
    want_rc 0
    want_eq "$(cat "$OUT")" single "content of /single.txt"
    rr -d single.txt="$WORK/single.txt" --run -- $T write /single.txt
    want_line "/single.txt EROFS"
}

t_dir_remove_tmp() {
    binit
    rr -d tmp= --run -- $T stat /tmp
    want_rc 0
    want_line "/tmp ENOENT"
}

t_dir_etc() {
    binit
    # Make the files world-writable for a moment, so only a read-only
    # mount can stop the box user.
    hosts_mode=$(stat -c %a /etc/hosts)
    probe=/etc/room-test-$B
    : > "$probe"
    chmod 666 "$probe"
    hosts_rw=
    chmod 666 /etc/hosts 2> /dev/null && hosts_rw=1
    rr -d etc --run -- $T write /etc/hosts "$probe"
    chmod "$hosts_mode" /etc/hosts 2> /dev/null
    rm -f "$probe"
    want_rc 0
    if [ -n "$hosts_rw" ]; then
        want_eq "$(field /etc/hosts)" EROFS "writing /etc/hosts"
    else
        note "could not chmod /etc/hosts, only checking that writing fails"
        r=$(field /etc/hosts)
        [ -n "$r" ] && [ "$r" != OK ] || fail "writing /etc/hosts gave '$r'"
    fi
    want_eq "$(field "$probe")" EROFS "writing $probe"
    rr -d etc --run -- $T read /etc/passwd
    want_line "/etc/passwd OK"
}

t_dir_dotdot() {
    binit
    rr -d foo/../bar=/tmp --run -- $T exit 0
    want_rc 2 "foo/../bar"
    rr -d ../x=/tmp --run -- $T exit 0
    want_rc 2 "../x"
    # rejected while parsing the options, so no run and no meta file
    for r in a//b=/tmp ./x=/tmp x/./y=/tmp x/.=/tmp; do
        rr -d "$r" --run -- $T exit 0
        want_rc 2 "$r"
        [ -f "$META" ] && fail "rule $r was not refused as a usage error"
    done
    return 0
}

t_no_default_dirs() {
    binit
    rr -D -d box=./box:rw --run -- $T exit 0
    want_rc 0
    rr -D -d box=./box:rw --run -- $T ls /
    want_rc 0 "ls /"
    want_eq "$(cat "$OUT")" box "box root with -D"
}

t_relative_program() {
    binit
    rr --run -- t exit 0
    want_rc 0
    want_meta exitcode 0
}

t_cg_flag() {
    binit
    rr --cg --run -- $T exit 0
    want_rc 0
}

t_silent() {
    binit
    rr -s --run -- $T exit 0
    want_rc 0
    [ -s "$ERR" ] && fail "stderr is not empty with -s"
    rr --run -- $T exit 0
    want_rc 0
    case $(head -n 1 "$ERR") in
        "OK ("*) ;;
        *) fail "the status line doesn't start with 'OK ('" ;;
    esac
}

t_meta_stdout() {
    binit
    R -b "$B" -M - --run -- $T exit 0
    want_rc 0
    want_line "exitcode:0"
    grep -q '^time:' "$OUT" || fail "no time: line on stdout"
}

# The meta files below are $M and not $META, which R deletes before every
# command. A caller reuses the path, so the old result must not survive.
# want_xx_meta TEXT: $M is an XX result whose message contains TEXT.
want_xx_meta() {
    note "meta file $M: $(cat "$M" 2> /dev/null)"
    want_eq "$(meta status "$M")" XX "status in $M"
    case $(meta message "$M") in
        *"$1"*) ;;
        *) fail "message in $M is '$(meta message "$M")', expected one with '$1'" ;;
    esac
    if has exitcode "$M"; then fail "$M still has exitcode:$(meta exitcode "$M")"; fi
}

t_meta_uninitialised() {
    M=$WORK/meta.reused
    binit
    R -b "$B" -M "$M" --run -- $T exit 0
    want_rc 0 "the run that writes $M"
    R -b "$B2" -M "$M" --run -- $T exit 0
    want_rc 2 "--run on box $B2, which was never initialised"
    want_xx_meta "run room --init"
}

# Before the lock is held the box is the other command's. Failing must not
# kill its run or touch its box/.
t_meta_box_in_use() {
    M=$WORK/meta.reused
    binit
    R -b "$B" -M "$M" --run -- $T exit 0
    want_rc 0 "the run that writes $M"
    ("$ROOM" -b "$B" -M "$WORK/meta.first" -w 20 --run -- $T sleep 3 < /dev/null > /dev/null 2>&1
        echo $? > "$WORK/rc.first") &
    if ! wait_started "$U"; then
        wait
        fail "the first run never started"
    fi
    # the keeper's cleanup would delete this
    ln -s /etc/passwd "$BD/box/link" || fail "cannot create a symlink in box/"
    R -b "$B" -M "$M" --run -- $T exit 0
    want_rc 2 "--run on a busy box"
    want_xx_meta "in use"
    [ -n "$(leftovers "$U")" ] || fail "the first run's program is gone after the second command"
    [ -L "$BD/box/link" ] || fail "the second command removed a symlink from box/"
    wait
    note "first run: exit status $(cat "$WORK/rc.first"), meta: $(cat "$WORK/meta.first")"
    want_eq "$(cat "$WORK/rc.first")" 0 "exit status of the first run"
    want_eq "$(meta exitcode "$WORK/meta.first")" 0 "exitcode of the first run"
    if has killed "$WORK/meta.first"; then fail "the first run was killed"; fi
    return 0
}

t_meta_usage_error() {
    M=$WORK/meta.reused
    binit
    R -b "$B" -M "$M" --run -- $T exit 0
    want_rc 0 "the run that writes $M"
    cp "$M" "$WORK/meta.before" || fail "cannot copy $M"
    for bad in "-t 1x" "--no-such-option" "-b 2147483648"; do
        R -b "$B" -M "$M" $bad --run -- $T exit 0
        want_rc 2 "--run with $bad"
        cmp -s "$M" "$WORK/meta.before" || fail "--run with $bad changed $M: $(cat "$M")"
    done
}

# ---- robustness ----

t_lock() {
    binit
    binit "$B2"
    ("$ROOM" -b "$B" -M "$WORK/meta.bg" -w 20 --run -- $T sleep 3 < /dev/null > /dev/null 2>&1
        echo $? > "$WORK/rc.bg") &
    if ! wait_started "$U"; then
        wait
        fail "the first run never started"
    fi
    rr --run -- $T exit 0
    want_rc 2 "--run on a busy box"
    R -b "$B2" -M "$WORK/meta.other" --run -- $T exit 0
    want_rc 0 "--run on another box at the same time"
    R -b "$B" --init
    want_rc 2 "--init on a busy box"
    R -b "$B" --cleanup
    want_rc 2 "--cleanup on a busy box"
    wait
    want_eq "$(cat "$WORK/rc.bg")" 0 "exit status of the first run"
}

t_signals() {
    binit
    # The numbers are the same on x86_64 and aarch64
    for s in INT:2 HUP:1 QUIT:3 TERM:15; do
        name=${s%:*}
        rm -f "$META"
        "$ROOM" -b "$B" -M "$META" --run -- $T sleep 30 < /dev/null > "$OUT" 2> "$ERR" &
        pid=$!
        if ! wait_started "$U"; then
            kill -9 $pid
            wait $pid
            fail "the program never started"
        fi
        kill -"$name" $pid
        i=0
        while alive $pid && [ $i -lt 100 ]; do
            sleep 0.1
            i=$((i + 1))
        done
        if alive $pid; then
            kill -9 $pid
            wait $pid
            fail "room still running 10 s after SIG$name"
        fi
        wait $pid
        RC=$?
        note "SIG$name: room exit status $RC"
        [ -f "$META" ] && note "meta:" && note "$(cat "$META")"
        want_rc 1 "SIG$name"
        want_meta exitsig "${s#*:}"
        want_meta status SG
        want_meta message Interrupted
        wait_gone "$U" "$META" || fail "processes left after SIG$name: $(leftovers "$U" "$META")"
    done
}

t_sigkill() {
    binit
    "$ROOM" -b "$B" -M "$META" --run -- $T sleep 30 < /dev/null > /dev/null 2>&1 &
    pid=$!
    if ! wait_started "$U"; then
        kill -9 $pid
        wait $pid
        fail "the program never started"
    fi
    kill -KILL $pid
    wait $pid
    wait_gone "$U" "$META" || fail "processes left 3 s after SIGKILL: $(leftovers "$U" "$META")"
    rr --run -- $T exit 0
    want_rc 0 "a run in the same box afterwards"
}

# A program can nest directories far deeper than room has descriptors. The
# low limit shows that none of room's walks depends on the depth.
t_deep_box() {
    ulimit -n 128
    binit
    rr --run -- $T deep 3000
    want_rc 0
    want_meta exitcode 0
    [ -z "$(find "$BD/box" -mindepth 66 -print -quit)" ] ||
        fail "box/ has entries nested more than 65 levels deep"
    R -b "$B" --cleanup
    want_rc 0 "--cleanup"
    [ -e "$BD" ] && fail "$BD still exists after --cleanup"
    binit
    rr --run -- $T exit 0
    want_rc 0 "a run after --init"
}

t_deep_tmp() {
    ulimit -n 128
    binit
    rr --run -- $T deep 3000 /tmp
    want_rc 0
    want_meta exitcode 0
    R -b "$B" --init
    want_rc 0 "--init over a deep /tmp"
    cp "$HELPER" "$BD/box/t"
    rr --run -- $T deep 3000 /tmp
    want_rc 0 "the second deep run"
    R -b "$B" --cleanup
    want_rc 0 "--cleanup"
    [ -e "$BD" ] && fail "$BD still exists after --cleanup"
    binit
    rr --run -- $T exit 0
    want_rc 0 "a run after --init"
}

t_deep_rerun() {
    ulimit -n 128
    binit
    rr --run -- $T deep 3000
    want_rc 0 "the run that builds the tree"
    rr --run -- $T exit 0
    want_rc 0 "the next run in the same box"
    # A tree the caller leaves in box/ is chowned before the run
    mkdir "$BD/box/caller" && (cd "$BD/box/caller" && "$HELPER" deep 3000) ||
        fail "cannot build a deep tree in box/caller"
    rr --run -- $T exit 0
    want_rc 0 "a run after the caller built a deep tree"
    [ -z "$(find "$BD/box" -mindepth 66 -print -quit)" ] ||
        fail "box/ has entries nested more than 65 levels deep"
}

t_overhead() {
    case $(date +%N) in *N*) skip "date has no %N" ;; esac
    binit
    best=
    for i in 1 2 3; do
        t0=$(date +%s%N)
        R -b "$B" -s --run -- t exit 0
        t1=$(date +%s%N)
        want_rc 0
        ms=$(((t1 - t0) / 1000000))
        note "run $i took $ms ms"
        if [ -z "$best" ] || [ $ms -lt $best ]; then best=$ms; fi
    done
    [ "$best" -lt 500 ] || fail "the fastest run took $best ms"
}

t_concurrency() {
    ids="900 901 902 903 904 905 906 907"
    use_box $ids
    for id in $ids; do binit $id; done
    for id in $ids; do
        (timeout 60 "$ROOM" -b $id -M "$WORK/meta.$id" --run -- $T spin-ms 200 \
            < /dev/null > /dev/null 2> "$WORK/err.$id"
            echo $? > "$WORK/rc.$id") &
    done
    wait
    for id in $ids; do
        rc=$(cat "$WORK/rc.$id")
        if [ "$rc" != 0 ] || [ "$(meta exitcode "$WORK/meta.$id")" != 0 ]; then
            note "box $id: exit status $rc, stderr: $(cat "$WORK/err.$id")"
            note "meta: $(cat "$WORK/meta.$id" 2> /dev/null)"
            fail "the run in box $id failed"
        fi
    done
}

t_cgroup_layout() {
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is set"
    R -b "$B" --cleanup
    want_rc 0
    # a process that was in <base> before room ran: the container's PID 1 or the unit's sleep
    pid=1
    [ -n "$SYSTEMD" ] && pid=$(systemctl show -p MainPID --value room.service)
    want_eq "$(sed -n 's/^0:://p' "/proc/$pid/cgroup")" "${CG_BASE#/sys/fs/cgroup}/room/main" \
        "the cgroup of PID $pid"
    [ -e "$CG_BASE/cgroup.events" ] || skip "the container's cgroup is the machine's root cgroup"
    procs=$(cat "$CG_BASE/cgroup.procs")
    [ -z "$procs" ] || fail "$CG_BASE/cgroup.procs is not empty: $procs"
    sc=$(cat "$CG_BASE/room/cgroup.subtree_control")
    case " $sc " in *" memory "*) ;; *) fail "room/cgroup.subtree_control is '$sc', no memory" ;; esac
    case " $sc " in *" pids "*) ;; *) fail "room/cgroup.subtree_control is '$sc', no pids" ;; esac
}

t_cleanup_cgroup() {
    binit
    dir=$(cg_box_dir "$B")
    [ -d "$dir" ] || fail "$dir is missing after --init"
    R -b "$B" --cleanup
    want_rc 0
    [ -e "$dir" ] && fail "$dir still exists after --cleanup"
    return 0
}

# ---- more limits and results ----

t_time_overflow() {
    binit
    rr -t 2000000 -x 2000000 --run -- $T exit 0
    want_rc 0
    want_meta exitcode 0
}

# Room's own setup can't run in 16 KB. That is room's failure, not a
# verdict. With 64 KB, kernel 7.0 gets through the setup and kills the box
# inside execve(), after the start marker, which counts as the program.
t_cg_mem_tiny() {
    binit
    rr --cg-mem=16 --run -- $T exit 0
    want_rc 2
    want_meta status XX
    want_nokey exitsig
}

t_oom_group() {
    binit
    rr -p4 --cg-mem=65536 --run -- $T oomchild 200
    want_rc 1
    want_meta exitsig 9
    want_meta status SG
    want_meta cg-oom-killed 1
    grep -q "parent survived" "$OUT" && fail "the parent survived its child's OOM kill"
    return 0
}

t_meta_newlines() {
    binit
    rr -o 'nodir/x
status:OK
exitcode:0' --run -- $T exit 0
    want_rc 2
    want_eq "$(grep -c '^status:' "$META")" 1 "number of status lines"
    want_meta status XX
    want_nokey exitcode
}

# Room fails after the program ended. The meta file must not also carry the
# program's result.
t_xx_after_run() {
    binit
    "$ROOM" -b "$B" -M "$META" -w 10 --run -- $T sleep 1 < /dev/null > "$OUT" 2> "$ERR" &
    pid=$!
    if ! wait_started "$U"; then
        kill -9 $pid
        wait $pid
        fail "the program never started"
    fi
    rm -rf "$BD/box"
    wait $pid
    RC=$?
    note "exit status $RC, meta: $(cat "$META" 2> /dev/null)"
    want_rc 2
    want_eq "$(cut -d: -f1 "$META" | tr '\n' ' ')" "status message " "keys of an XX meta file"
}

# ---- mounts, namespaces and files in box/ ----

# opts MOUNTPOINT: the per-mount options of the last mount on MOUNTPOINT in
# the mountinfo in $OUT.
opts() {
    awk -v m="$1" '$5 == m { o = $6 } END { print o }' "$OUT"
}

# want_opts MOUNTPOINT OPTION...: every OPTION is set on MOUNTPOINT.
want_opts() {
    mp=$1
    shift
    o=$(opts "$mp")
    [ -n "$o" ] || fail "nothing is mounted on $mp"
    for x in "$@"; do
        case ",$o," in
            *",$x,"*) ;;
            *) fail "the options of $mp are '$o', no $x" ;;
        esac
    done
}

t_mount_flags() {
    binit
    rr --run -- $T cat /proc/self/mountinfo
    want_rc 0
    want_opts / ro nosuid nodev
    want_opts /box rw nosuid nodev
    want_opts /tmp rw nosuid nodev
    for m in /bin /lib /usr; do
        want_opts $m ro nosuid nodev
    done
    want_opts /proc ro nosuid nodev noexec
    want_opts /dev ro nosuid nodev noexec
    for d in null zero full random urandom; do
        want_opts /dev/$d ro nosuid noexec
    done
    want_opts /dev/shm rw nosuid nodev
}

# pivot_root also moves the root of the proxy, which shares the box's mount
# namespace. After the chroot fallback the proxy still sees the container's
# own mounts, for example /sys. Docker's default seccomp profile is what
# refuses pivot_root, so a container without a filter must get pivot_root.
t_root_switch() {
    binit
    "$ROOM" -b "$B" -M "$META" -w 10 --run -- $T sleep 1 < /dev/null > /dev/null 2>&1 &
    pid=$!
    if ! wait_started "$U"; then
        kill -9 $pid
        wait $pid
        fail "the program never started"
    fi
    proxy=$(pgrep -P $pid)
    old=$(awk '$5 == "/sys"' "/proc/$proxy/mountinfo")
    wait $pid
    RC=$?
    want_rc 0
    if [ "$(awk '/^Seccomp:/ { print $2 }' /proc/self/status)" = 0 ]; then
        [ -z "$old" ] || fail "no seccomp filter here, but the box namespace still holds the old root"
    else
        [ -n "$old" ] || fail "pivot_root is refused here, but the old root is gone"
    fi
}

t_namespaces() {
    binit
    host=$(cat /proc/sys/kernel/hostname)
    rr --run -- $T cat /proc/self/cgroup
    want_rc 0
    want_eq "$(cat "$OUT")" "0::/" "/proc/self/cgroup in the box"
    rr --run -- $T cat /proc/sys/kernel/hostname
    want_rc 0
    want_eq "$(cat "$OUT")" room "the hostname in the box"
    want_eq "$(cat /proc/sys/kernel/hostname)" "$host" "the container's hostname"
}

t_special_subdir() {
    binit
    mkdir -p "$BD/box/a/b"
    rr -c a/b --run -- $T mklinks
    want_rc 0
    want_line "symlink OK"
    [ -L "$BD/box/a/b/link" ] && fail "the symlink is still in box/a/b"
    [ -p "$BD/box/a/b/fifo" ] && fail "the fifo is still in box/a/b"
    [ -f "$BD/box/a/b/regular" ] || fail "the regular file is gone from box/a/b"
}

t_hardlink() {
    binit
    echo data > "$WORK/linked"
    chmod 644 "$WORK/linked"
    ln "$WORK/linked" "$BD/box/linked" || fail "cannot hard-link $WORK/linked into box/"
    rr --run -- $T write /box/linked
    want_rc 0
    want_eq "$(stat -c %u:%g "$WORK/linked")" 0:0 "owner of the original"
    want_line "/box/linked EACCES"
}

# ---- directory rules ----

t_dir_dev() {
    binit
    mkdir "$WORK/nodes"
    mknod "$WORK/nodes/null" c 1 3 && chmod 666 "$WORK/nodes/null" || fail "cannot create a device node"
    rr -d nodes="$WORK/nodes" --run -- $T write /nodes/null
    want_line "/nodes/null EACCES"
    rr -d nodes="$WORK/nodes:dev" --run -- $T write /nodes/null
    want_line "/nodes/null OK"
}

t_dir_norec() {
    binit
    mkdir -p "$WORK/outer/inner"
    mount -t tmpfs tmpfs "$WORK/outer/inner" || fail "cannot mount a tmpfs"
    echo x > "$WORK/outer/inner/f"
    rr -d o="$WORK/outer" --run -- $T stat /o/inner/f
    with=$(field /o/inner/f)
    rr -d o="$WORK/outer:norec" --run -- $T stat /o/inner/f
    without=$(field /o/inner/f)
    umount "$WORK/outer/inner"
    want_eq "$with" OK "/o/inner/f without norec"
    want_eq "$without" ENOENT "/o/inner/f with norec"
}

t_dir_fs_tmpfs() {
    binit
    rr -d scratch=tmpfs:fs:rw --run -- $T write /scratch/new
    want_line "/scratch/new OK"
    rr -d scratch=tmpfs:fs --run -- $T write /scratch/new
    want_line "/scratch/new EROFS"
}

t_dir_tmp_rule() {
    binit
    rr -d work:tmp --run -- $T write /work/new
    want_line "/work/new OK"
    want_eq "$(stat -c %u "$BD/tmp/work" 2> /dev/null)" "$U" "owner of tmp/work"
    rr -d work:tmp --run -- $T stat /work/new
    want_line "/work/new OK"
}

# x/y is mounted on top of x. If the replaced x rule moved to the end, x
# would be mounted last and hide x/y.
t_dir_replace_order() {
    binit
    mkdir -p "$WORK/a/y" "$WORK/a2/y" "$WORK/b"
    echo b > "$WORK/b/f"
    rr -d x="$WORK/a" -d x/y="$WORK/b" -d x="$WORK/a2" --run -- $T stat /x/y/f
    want_rc 0
    want_line "/x/y/f OK"
}

t_dir_dev_rules() {
    binit
    rr --run -- $T stat /dev/ptmx
    want_line "/dev/ptmx ENOENT"
    rr -d dev=/dev:dev --run -- $T stat /dev/ptmx
    want_line "/dev/ptmx OK"
    rr -d dev= --run -- $T stat /dev
    want_line "/dev ENOENT"
}

# ---- signals ----

# A signal while room still walks box/ must stop room from starting the box.
t_signal_early() {
    binit
    (cd "$BD/box" && "$HELPER" wide 0 50000 > /dev/null) || fail "cannot fill box/"
    cg=$(cg_box_dir "$B")/run
    ino=$(stat -c %i "$cg" 2> /dev/null)
    "$ROOM" -b "$B" -M "$META" --run -- $T write /box/ran < /dev/null > "$OUT" 2> "$ERR" &
    pid=$!
    # A new run cgroup means room is past cg_box_prepare() and walks box/ now
    i=0
    while [ "$(stat -c %i "$cg" 2> /dev/null)" = "$ino" ] && [ $i -lt 1000 ]; do
        i=$((i + 1))
        sleep 0.01
    done
    kill -TERM $pid
    wait $pid
    RC=$?
    note "exit status $RC, meta: $(cat "$META" 2> /dev/null)"
    want_rc 1
    want_meta exitsig 15
    want_meta status SG
    want_meta message Interrupted
    [ -e "$BD/box/ran" ] && fail "the program ran"
    return 0
}

# Signal numbers here are the same on x86_64 and aarch64: CHLD 17, USR1 10, ALRM 14.
t_sigchld_ignored() {
    binit
    WRAP="$HELPER ignore 17" rr --run -- $T exit 0
    want_rc 0
    want_meta exitcode 0
}

t_sig_inherit() {
    binit
    WRAP="$HELPER ignore 10,14" rr --run -- $T cat /proc/self/status
    want_rc 0
    want_eq "$(awk '$1 == "SigIgn:" { print $2 }' "$OUT")" 0000000000000000 "SigIgn of the program"
    want_eq "$(awk '$1 == "SigBlk:" { print $2 }' "$OUT")" 0000000000000000 "SigBlk of the program"
}

# ---- wide trees ----

# Room walks box/ as root around every run. A wide directory must cost time
# in proportion to its entries, also below the 64 levels the walks recurse.
t_wide_deep() {
    binit
    timed rr --run -- $T wide 70 4000
    want_rc 0 "the run that builds the tree"
    [ "$MS" -lt 5000 ] || fail "the run took $MS ms"
    rr -c /tmp --run -- $T wide 70 4000
    want_rc 0 "building the tree in /tmp"
    timed R -b "$B" --init
    want_rc 0 "--init"
    [ "$MS" -lt 5000 ] || fail "--init took $MS ms"
    cp "$HELPER" "$BD/box/t"
    rr -c /tmp --run -- $T wide 70 4000
    want_rc 0 "building the tree in /tmp again"
    timed R -b "$B" --cleanup
    want_rc 0 "--cleanup"
    [ "$MS" -lt 5000 ] || fail "--cleanup took $MS ms"
}

t_wide_shallow() {
    binit
    timed rr --run -- $T wide 0 4000
    want_rc 0 "the run that builds the tree"
    [ "$MS" -lt 5000 ] || fail "the run took $MS ms"
    timed rr --run -- $T exit 0
    want_rc 0 "the next run"
    [ "$MS" -lt 5000 ] || fail "the next run took $MS ms"
    rr -c /tmp --run -- $T wide 0 4000
    want_rc 0 "building the tree in /tmp"
    timed R -b "$B" --cleanup
    want_rc 0 "--cleanup"
    [ "$MS" -lt 5000 ] || fail "--cleanup took $MS ms"
}

# ---- cgroup setups ----

t_cg_root() {
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is already set"
    R -b "$B" --cleanup
    cg=$(cg_box_dir "$B")
    cg=${cg%/*}/t-root-$B
    mkdir "$cg" || fail "cannot create $cg"
    export ROOM_CG_ROOT=$cg
    binit
    rr --run -- $T exit 0
    run_rc=$RC
    [ -d "$cg/box-$B" ] && in_root=1
    R -b "$B" --cleanup
    cleanup_rc=$RC
    rmdir "$cg"
    RC=$run_rc
    want_rc 0 "a run with ROOM_CG_ROOT"
    [ -n "${in_root:-}" ] || fail "the box cgroup is not under ROOM_CG_ROOT"
    RC=$cleanup_rc
    want_rc 0 "--cleanup with ROOM_CG_ROOT"
}

# The kernel kills the program at birth when its caller's cgroup has seen
# more cgroup.kill writes than the run cgroup, see DESIGN.md. Room must
# report that as its own failure.
t_cg_root_kill_history() {
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is already set"
    R -b "$B" --cleanup
    room=$(cg_box_dir "$B")
    room=${room%/*}
    caller=$room/t-caller-$B
    root=$room/t-root-$B
    mkdir "$caller" "$root" || fail "cannot create the cgroups"
    echo 1 > "$caller/cgroup.kill"
    export ROOM_CG_ROOT=$root
    binit
    rm -f "$META"
    sh -c 'echo $$ > "$1/cgroup.procs" && exec "$2" -b "$3" -M "$4" --run -- /box/t exit 0' \
        sh "$caller" "$ROOM" "$B" "$META" < /dev/null > "$OUT" 2> "$ERR"
    RC=$?
    note "exit status $RC, stderr: $(cat "$ERR"), meta: $(cat "$META" 2> /dev/null)"
    run_rc=$RC
    cp "$META" "$WORK/meta.keep" 2> /dev/null
    R -b "$B" --cleanup
    rmdir "$root" "$caller"
    mv "$WORK/meta.keep" "$META" 2> /dev/null
    RC=$run_rc
    want_rc 2
    want_meta status XX
    want_nokey exitsig
}

# want_unit_named: room's message tells the user to start room.service.
want_unit_named() {
    grep -qF "systemctl enable --now room.service" "$ERR" ||
        fail "stderr '$(cat "$ERR")' doesn't say to start room.service"
}

# The real systemd is tested by systemd_stopped. Without it there is no unit.
t_systemd_refusal() {
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is set"
    [ -e /run/systemd ] && skip "/run/systemd exists"
    trap 'rm -rf /run/systemd' EXIT
    mkdir -p /run/systemd/system || fail "cannot create /run/systemd/system"
    R -b "$B" --init
    rm -rf /run/systemd
    want_rc 2
    want_unit_named
    [ -e "$BD" ] && fail "--init created $BD"
    return 0
}

# ---- under systemd, with room.service ----

# start_unit: starts room.service and waits until it is active.
start_unit() {
    timeout 20 systemctl start room.service || fail "cannot start room.service"
    want_eq "$(systemctl is-active room.service)" active "the state of room.service"
}

need_systemd() {
    [ -n "$SYSTEMD" ] || skip "systemd is not running"
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is set"
    return 0
}

# want_unit_kept WHEN: room.service is active and its process is in room/main.
want_unit_kept() {
    want_eq "$(systemctl is-active room.service)" active "the state of room.service $1"
    pid=$(systemctl show -p MainPID --value room.service)
    want_eq "$(sed -n 's/^0:://p' "/proc/$pid/cgroup")" "${CG_BASE#/sys/fs/cgroup}/room/main" \
        "the cgroup of the unit's process $pid $1"
    sc=" $(cat "$CG_BASE/cgroup.subtree_control") "
    for c in memory pids; do
        case $sc in *" $c "*) ;; *) fail "the unit's subtree_control is '$sc' $1, no $c" ;; esac
    done
}

t_systemd_unit() {
    need_systemd
    binit
    rr --run -- $T exit 0
    want_rc 0
    want_unit_kept "after room moved its process"
    timeout 20 systemctl daemon-reload || fail "systemctl daemon-reload failed"
    want_unit_kept "after daemon-reload"
    rr --run -- $T exit 0
    want_rc 0 "a run after daemon-reload"
}

t_systemd_stopped() {
    need_systemd
    trap 'timeout 20 systemctl start room.service' EXIT
    timeout 20 systemctl stop room.service || fail "cannot stop room.service"
    [ -e "$CG_BASE" ] && fail "$CG_BASE is still there after systemctl stop"
    cg=$(cat /proc/self/cgroup)
    locks=$(ls -A "$ROOT/.locks" 2> /dev/null)
    for cmd in "-b $B --init" "-b auto --init" "-b $B --run -- $T exit 0" "-b $B --cleanup"; do
        R $cmd
        want_rc 2 "room $cmd with room.service stopped"
        want_unit_named
    done
    want_eq "$(cat /proc/self/cgroup)" "$cg" "the cgroup of this script"
    [ -e "$CG_BASE" ] && fail "room created $CG_BASE"
    [ -e "$BD" ] && fail "room created $BD"
    want_eq "$(ls -A "$ROOT/.locks" 2> /dev/null)" "$locks" "the lock files"
    start_unit
    binit
    rr --run -- $T exit 0
    want_rc 0 "a run after systemctl start"
}

# Stopping the unit kills every box and removes room's cgroups.
t_systemd_stop_box() {
    need_systemd
    trap 'timeout 20 systemctl start room.service' EXIT
    binit
    timeout -k 5 60 "$ROOM" -b "$B" -M "$META" -w 30 --run -- $T sleep 30 \
        < /dev/null > /dev/null 2> "$ERR" &
    pid=$!
    if ! wait_started "$U"; then
        kill -9 $pid
        wait $pid
        fail "the program never started"
    fi
    timeout 20 systemctl stop room.service || fail "cannot stop room.service"
    wait $pid
    RC=$?
    note "exit status $RC, stderr: $(cat "$ERR"), meta: $(cat "$META" 2> /dev/null)"
    [ "$RC" = 1 ] || [ "$RC" = 2 ] || fail "room exited with $RC, expected 1 or 2"
    wait_gone "$U" || fail "processes left after systemctl stop: $(leftovers "$U")"
    [ -e "$CG_BASE" ] && fail "$CG_BASE is still there after systemctl stop"
    start_unit
    R -b "$B" --run -- $T exit 0
    want_rc 2 "a run in a box whose cgroup systemd removed"
    binit
    rr --run -- $T exit 0
    want_rc 0 "the first run after systemctl start"
    # see many_runs. The cgroups are new, but systemd may have written cgroup.kill above them
    for i in 1 2 3; do
        TMO=20 rr -t 0.1 --run -- $T spin
        want_rc 1 "CPU limit, round $i"
        want_meta killed 1
        rr --run -- $T exit 0
        want_rc 0 "the run after the CPU kill, round $i"
        rr --cg-mem=16384 --run -- $T alloc 100
        want_rc 1 "OOM, round $i"
        rr --run -- $T exit 0
        want_rc 0 "the run after the OOM kill, round $i"
    done
}

# ---- the filter in a BPF interpreter ----

t_seccomp_sim() {
    sim=$BUILD/seccomp_sim
    if [ ! -x "$sim" ]; then
        sim=$WORK/seccomp_sim
        gcc -O2 -Wall -D_GNU_SOURCE -o "$sim" "$TESTDIR/seccomp_sim.c" "$TESTDIR/../src/util.c" ||
            fail "cannot build the simulator"
    fi
    "$sim" > "$OUT" 2>&1
    RC=$?
    grep -v '^ok ' "$OUT" >> "$LOG"
    want_rc 0
}

# ---- how a caller uses room ----

# J COMMAND: runs a command line through sh -c, the way a caller does. Sets RC.
J() {
    rm -f "$META"
    timeout -k 5 "${TMO:-60}" sh -c "$1" 2> "$ERR"
    RC=$?
    {
        printf '$ %s\n' "$1"
        printf 'exit status: %s\n' "$RC"
        if [ -s "$ERR" ]; then echo "stderr:"; sed -n '1,30p' "$ERR"; fi
        if [ -f "$META" ]; then echo "meta:"; sed 's/^/  /' "$META"; fi
    } >> "$LOG"
}

t_caller_flow() {
    binit
    echo 'gcc -O2 -o a.out main.c' > "$BD/box/compile.sh"
    cat > "$BD/box/main.c" << 'EOF'
#include <stdio.h>

int main(void)
{
    long a, b;

    if (scanf("%ld %ld", &a, &b) != 2)
        return 1;
    printf("%ld\n", a + b);
    return 0;
}
EOF
    printf '2 40\n' > "$WORK/stdin.txt"
    env="-E HOME=/tmp -E PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    compile="$ROOM --cg -s -b $B -M $META --stderr-to-stdout -i /dev/null -t 10 -x 1 -w 20 -k 128000 -p60 --cg-mem=512000 -f 8192 -n 256 $env -d /etc:noexec --run -- /bin/sh compile.sh > $WORK/compile.out"

    J "$compile"
    note "compile output: $(cat "$WORK/compile.out")"
    want_rc 0 "compile"
    want_meta exitcode 0
    have_peak && want_key cg-mem
    compile_mem=$(meta cg-mem)
    [ -f "$BD/box/a.out" ] || fail "the compiler left no a.out"

    J "$ROOM --cg -s -b $B -M $META -t 2 -x 0.5 -w 10 -k 128000 -p1 --cg-mem=262144 -f 8192 -n 64 $env -d /etc:noexec --run -- ./a.out < $WORK/stdin.txt > $WORK/stdout.txt 2> $WORK/stderr.txt"
    note "program stdout: $(cat "$WORK/stdout.txt")"
    note "program stderr: $(cat "$WORK/stderr.txt")"
    want_rc 0 "run"
    want_eq "$(cat "$WORK/stdout.txt")" 42 "the program's output"
    want_meta exitcode 0
    want_key time
    want_key time-wall
    if have_peak; then
        want_key cg-mem
        run_mem=$(meta cg-mem)
        [ $((run_mem * 4)) -lt "$compile_mem" ] ||
            fail "the run's cg-mem $run_mem is not much smaller than the compile's $compile_mem"
    fi

    printf 'int main(void)\n{\n    return x;\n}\n' > "$BD/box/main.c"
    J "$compile"
    note "compile output: $(cat "$WORK/compile.out")"
    want_rc 1 "compile error"
    want_meta status RE
    grep -q 'main\.c:.*error' "$WORK/compile.out" || fail "the compiler's error is not in the compile output"
}

# ---- disk quota ----

# mounts DIR: "TYPE SOURCE" of each mount on DIR in this mount namespace,
# the lowest first.
mounts() {
    awk -v d="$1" '$5 == d { for (i = 7; $i != "-"; i++) ; print $(i + 1), $(i + 2) }' /proc/self/mountinfo
}

# qinit KB,INODES: --init with a quota, then copies the helper in like binit.
qinit() {
    R -b "$B" -q "$1" --init
    [ "$RC" = 0 ] || fail "--init -q $1 failed"
    cp "$HELPER" "$BD/box/t" || fail "cannot copy the helper into the quota box"
}

t_quota_size() {
    qinit 4096,1000
    want_eq "$(mounts "$BD")" "tmpfs room-quota" "mounts on $BD"
    cp /proc/self/mountinfo "$OUT"
    want_opts "$BD" nosuid nodev
    rr --run -- $T bigfile 8192
    want_rc 1
    grep -q '^bigfile ENOSPC' "$OUT" || fail "writing 8 MB into a 4 MB quota did not fail with ENOSPC"
    rr -c /tmp --run -- $T bigfile 8192
    want_rc 1 "writing to /tmp"
    grep -q '^bigfile ENOSPC' "$OUT" || fail "writing 8 MB to /tmp with a 4 MB quota did not fail with ENOSPC"
}

t_quota_inodes() {
    qinit 102400,100
    rr --run -- $T wide 0 100
    want_rc 1
    grep -q 'ENOSPC' "$OUT" || fail "creating 200 inodes with a quota of 100 did not fail with ENOSPC"
}

t_quota_compile() {
    qinit 102400,10000
    printf '#include <stdio.h>\n\nint main(void)\n{\n    puts("hi");\n    return 0;\n}\n' > "$BD/box/main.c"
    rr -p --cg-mem=524288 -t 10 -E PATH=/usr/bin:/bin -d /etc:noexec --run -- /usr/bin/gcc -O2 -o a.out main.c
    want_rc 0 "compile"
    rr --run -- ./a.out
    want_rc 0 "run"
    want_eq "$(cat "$OUT")" hi "the program's output"
}

t_quota_cleanup() {
    qinit 4096,1000
    want_eq "$(mounts "$BD")" "tmpfs room-quota" "mounts on $BD after --init"
    R -b "$B" --cleanup
    want_rc 0
    want_eq "$(mounts "$BD")" "" "mounts on $BD after --cleanup"
    [ -e "$BD" ] && fail "$BD still exists after --cleanup"
    return 0
}

t_quota_reinit() {
    qinit 4096,1000
    echo x > "$BD/box/old"
    qinit 4096,1000
    want_eq "$(mounts "$BD")" "tmpfs room-quota" "mounts on $BD after --init -q twice"
    [ -e "$BD/box/old" ] && fail "box/old survived the second --init"
    binit
    want_eq "$(mounts "$BD")" "" "mounts on $BD after --init without --quota"
    rr --run -- $T bigfile 8192
    want_rc 0 "an 8 MB file in the box without a quota"
}

# A mount on a box directory that room didn't make belongs to the user.
t_quota_foreign() {
    binit
    trap 'while [ -n "$(mounts "$BD")" ] && umount "$BD"; do :; done' EXIT
    mount -t tmpfs -o mode=755 foreign "$BD" || fail "cannot mount a tmpfs on $BD"
    R -b "$B" --init
    want_rc 0 "--init on the foreign mount"
    want_eq "$(mounts "$BD")" "tmpfs foreign" "mounts on $BD after --init"
    R -b "$B" -q 4096,100 --init
    want_rc 0 "--init -q on the foreign mount"
    want_eq "$(mounts "$BD" | tr '\n' ' ')" "tmpfs foreign tmpfs room-quota " "mounts on $BD after --init -q"
    R -b "$B" --init
    want_rc 0 "--init without -q on the foreign mount"
    want_eq "$(mounts "$BD")" "tmpfs foreign" "mounts on $BD after --init without -q"
    # removing the directory fails on a mount point, that's expected
    R -b "$B" --cleanup
    want_eq "$(mounts "$BD")" "tmpfs foreign" "mounts on $BD after --cleanup"
}

# A quota mount is recognised by type and source. Another filesystem with
# the same source name isn't room's.
t_quota_ramfs() {
    binit
    trap 'while [ -n "$(mounts "$BD")" ] && umount "$BD"; do :; done' EXIT
    mount -t ramfs room-quota "$BD" || fail "cannot mount a ramfs on $BD"
    R -b "$B" --init
    want_rc 0 "--init on the ramfs"
    want_eq "$(mounts "$BD")" "ramfs room-quota" "mounts on $BD after --init"
    R -b "$B" -q 4096,100 --init
    want_rc 0 "--init -q on the ramfs"
    R -b "$B" --init
    want_rc 0 "--init without -q on the ramfs"
    want_eq "$(mounts "$BD")" "ramfs room-quota" "mounts on $BD after --init without -q"
    R -b "$B" --cleanup
    want_eq "$(mounts "$BD")" "ramfs room-quota" "mounts on $BD after --cleanup"
}

t_quota_usage() {
    binit
    rr -q 4096,100 --run -- $T exit 0
    want_rc 2 "--quota with --run"
    [ -f "$META" ] && fail "--quota with --run was not refused as a usage error"
    R -b "$B" -q 4096,100 --cleanup
    want_rc 2 "--quota with --cleanup"
    [ -d "$BD/box" ] || fail "--cleanup with --quota removed the box"
    for v in 4096 4096, ,100 a,1 1,a 1,2,3 -1,1 " 1,1" 1,+1 1,1x 0,100 4096,0 0,0 4096,15 \
        4096,2147483648 4096,9223372036854775807 9007199254740992,16; do
        R -b "$B2" -q "$v" --init
        want_rc 2 "--quota '$v'"
        [ -e "$ROOT/$B2" ] && fail "--init -q '$v' created the box"
    done
    # checked before anything in an existing box is touched
    qinit 4096,100
    echo keep > "$BD/box/keep"
    for v in 0,100 4096,0 4096,15 4096,2147483648 9007199254740992,16; do
        R -b "$B" -q "$v" --init
        want_rc 2 "--quota '$v' on an existing box"
        [ -f "$BD/box/keep" ] || fail "--init -q '$v' emptied the existing box"
        want_eq "$(mounts "$BD")" "tmpfs room-quota" "mounts on $BD after --init -q '$v'"
    done
    R -b "$B2" --quota=1,16 --init
    want_rc 0 "--quota=1,16"
    want_eq "$(mounts "$ROOT/$B2")" "tmpfs room-quota" "mounts with --quota=1,16"
    case ,$(awk -v d="$ROOT/$B2" '$5 == d { print $NF }' /proc/self/mountinfo), in
        *,nr_inodes=16,*) ;;
        *) fail "the tmpfs with --quota=1,16 doesn't have nr_inodes=16" ;;
    esac
    R -b "$B2" --quota=9007199254740991,2147483647 --init
    want_rc 0 "the largest quota"
}

# Detaching throws the files away. Walking them would take far longer.
t_quota_fast_cleanup() {
    qinit 1048576,200000
    (cd "$BD/box" && "$HELPER" wide 0 50000 > /dev/null) || fail "cannot fill box/"
    timed R -b "$B" --cleanup
    want_rc 0
    [ -e "$BD" ] && fail "$BD still exists after --cleanup"
    [ "$MS" -lt 500 ] || fail "--cleanup of a quota box with 100000 entries took $MS ms"
}

# ---- CPUs ----

# have_cpuset: the cgroup that holds room's cgroups offers cpuset.
have_cpuset() {
    if [ -n "${ROOM_CG_ROOT:-}" ]; then
        c=$ROOM_CG_ROOT
    else
        c=$CG_BASE
    fi
    grep -qw cpuset "$c/cgroup.controllers"
}

t_cpus() {
    binit
    have_cpuset || skip "the container has no cpuset controller"
    all=$("$HELPER" cpus)
    rr --run -- $T cpus
    want_rc 0 "without --cpus"
    want_eq "$(cat "$OUT")" "$all" "CPUs without --cpus"
    set -- $all
    first=$2
    rr --cpus="$first" --run -- $T cpus
    want_rc 0 "--cpus=$first"
    want_eq "$(cat "$OUT")" "cpus $first" "CPUs with --cpus=$first"
    [ "${3:-}" = $((first + 1)) ] || skip "the container has no two CPUs in a row"
    rr --cpus="$first-$3" --run -- $T cpus
    want_rc 0 "--cpus=$first-$3"
    want_eq "$(cat "$OUT")" "cpus $first $3" "CPUs with --cpus=$first-$3"
    rr --cpus="$3,$first" --run -- $T cpus
    want_rc 0 "--cpus=$3,$first"
    want_eq "$(cat "$OUT")" "cpus $first $3" "CPUs with --cpus=$3,$first"
}

t_cpus_missing() {
    binit
    have_cpuset || skip "the container has no cpuset controller"
    room=$(cg_box_dir "$B")
    avail=$(cat "${room%/*}/cpuset.cpus.effective") || fail "cannot read the CPUs of ${room%/*}"
    rr --cpus=4000 --run -- $T exit 0
    want_rc 2
    want_meta status XX
    case $(meta message) in
        *" $avail"*) ;;
        *) fail "the message '$(meta message)' doesn't name the available CPUs $avail" ;;
    esac
}

# A CPU that exists but the box's parent cgroup doesn't have. The kernel
# would quietly run the box on the parent's CPUs instead.
t_cpus_unavailable() {
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is already set"
    have_cpuset || skip "the container has no cpuset controller"
    R -b "$B" --cleanup
    set -- $("$HELPER" cpus)
    [ $# -ge 3 ] || skip "the container has only one CPU"
    cg=$(cg_box_dir "$B")
    cg=${cg%/*}/t-cpus-$B
    mkdir "$cg" || fail "cannot create $cg"
    trap '"$ROOM" -b "$B" --cleanup > /dev/null 2>&1; rmdir "$cg"' EXIT
    echo "$2" > "$cg/cpuset.cpus" || fail "cannot limit $cg to CPU $2"
    export ROOM_CG_ROOT=$cg
    binit
    rr --cpus="$3" --run -- $T exit 0
    want_rc 2 "--cpus=$3 under a cgroup that only has CPU $2"
    want_meta status XX
    case $(meta message) in
        *"available CPUs are $2") ;;
        *) fail "the message '$(meta message)' doesn't name CPU $2 as the available one" ;;
    esac
    rr --cpus="$2" --run -- $T cpus
    want_rc 0 "--cpus=$2 under the same cgroup"
    want_eq "$(cat "$OUT")" "cpus $2" "CPUs with --cpus=$2"
}

# Room enables cpuset even where memory and pids are already delegated and
# cpuset isn't.
t_cpuset_enable() {
    [ -n "${ROOM_CG_ROOT:-}" ] && skip "ROOM_CG_ROOT is set"
    have_cpuset || skip "the container has no cpuset controller"
    R -b "$B" --cleanup
    room=$(cg_box_dir "$B")
    room=${room%/*}
    base=${room%/*}
    echo -cpuset > "$room/cgroup.subtree_control" && echo -cpuset > "$base/cgroup.subtree_control" ||
        fail "cannot disable cpuset in $base and $room"
    binit
    for d in "$base" "$room"; do
        grep -qw cpuset "$d/cgroup.subtree_control" ||
            fail "$d/cgroup.subtree_control is '$(cat "$d/cgroup.subtree_control")', no cpuset"
    done
    set -- $("$HELPER" cpus)
    rr --cpus="$2" --run -- $T cpus
    want_rc 0
    want_eq "$(cat "$OUT")" "cpus $2" "CPUs with --cpus=$2"
}

t_cpus_usage() {
    binit
    for v in "" a "0 1" 1.5 "0;1" +1 0x1 , 0, ,0 - -1 1- 0-1,; do
        rr --cpus="$v" --run -- $T exit 0
        want_rc 2 "--cpus='$v'"
        [ -f "$META" ] && fail "--cpus='$v' was not refused as a usage error"
    done
    return 0
}

# ---- the default wall clock limit ----

t_wall_default() {
    binit
    rr -v -t 1 --run -- $T exit 0
    want_rc 0 "-t 1"
    want_line "Wall clock limit: 30.000 sec (default)" "$ERR"
    rr -v -t 5 -x 1 --run -- $T exit 0
    want_line "Wall clock limit: 60.000 sec (default)" "$ERR"
    rr -v -t 0.5 -x 0.2 --run -- $T exit 0
    want_line "Wall clock limit: 30.000 sec (default)" "$ERR"
    rr -v -t 300000 --run -- $T exit 0
    want_line "Wall clock limit: 2147483.647 sec (default)" "$ERR"
    rr -v -t 1 -w 0 --run -- $T exit 0
    want_line "Wall clock limit: none" "$ERR"
    rr -v -w 0 -t 1 --run -- $T exit 0
    want_line "Wall clock limit: none" "$ERR"
    rr -v -t 1 -w 2 --run -- $T exit 0
    want_line "Wall clock limit: 2.000 sec" "$ERR"
    rr -v -w 3 --run -- $T exit 0
    want_line "Wall clock limit: 3.000 sec" "$ERR"
    rr -v --run -- $T exit 0
    want_line "Wall clock limit: none" "$ERR"
    rr -v -t 0 --run -- $T exit 0
    want_line "Wall clock limit: none" "$ERR"
}

# ---- --root ----

# make_rootfs DIR: a small root filesystem. bin is an absolute link as in a
# merged /usr, there is no lib64, and some links try to lead out of DIR.
make_rootfs() {
    mkdir -p "$1/usr/bin" "$1/lib" "$1/etc" || fail "cannot create $1"
    cp "$HELPER" "$1/usr/bin/t" || fail "cannot copy the helper into $1"
    echo marker > "$1/etc/marker"
    ln -s /usr/bin "$1/bin" &&
        ln -s ../../../../../../etc "$1/up" &&
        ln -s /etc "$1/abs" &&
        ln -s /usr/share "$1/share" || fail "cannot create the links in $1"
}

t_root_dir() {
    binit
    rf=$WORK/rootfs.$B
    make_rootfs "$rf"
    [ -e /usr/bin/env ] || fail "the container has no /usr/bin/env to compare with"
    rr --root="$rf" --run -- /bin/t stat /usr/bin/t /usr/bin/env /lib64
    want_rc 0 "/bin/t through the absolute link bin"
    want_line "/usr/bin/t OK"
    want_line "/usr/bin/env ENOENT"
    want_line "/lib64 ENOENT"
    rr --root="$rf" --run -- /bin/t ls /
    want_eq "$(cat "$OUT")" "bin box dev lib proc tmp usr" "the box root"
    rr --root="$rf" --run -- /bin/t write /box/new /tmp/new /dev/null /usr/new /bin/new
    want_line "/box/new OK"
    want_line "/tmp/new OK"
    want_line "/dev/null OK"
    want_line "/usr/new EROFS"
    want_line "/bin/new EROFS"
    rr --root="$rf" --run -- /bin/t ls /dev
    want_eq "$(cat "$OUT")" "fd full null random shm stderr stdin stdout urandom zero" "/dev"
    rr --root="$rf" --run -- /bin/t procs
    want_rc 0 "procs"
    grep -q '^count=[123]$' "$OUT" || fail "/proc doesn't show only the box's processes"
}

t_root_rules() {
    binit
    rf=$WORK/rootfs.$B
    make_rootfs "$rf"
    for order in before after; do
        if [ $order = before ]; then set -- --root="$rf" -d etc; else set -- -d etc --root="$rf"; fi
        rr "$@" --run -- /bin/t stat /etc/marker /etc/hostname
        want_rc 0 "-d etc with --root $order it"
        want_line "/etc/marker OK"
        want_line "/etc/hostname ENOENT"
    done
    rr --root="$rf" -d etc=/etc --run -- /bin/t stat /etc/marker /etc/hostname
    want_rc 0 "-d etc=/etc"
    want_line "/etc/marker ENOENT"
    want_line "/etc/hostname OK"
    rr --root="$rf" -d up -d abs --run -- /bin/t stat /up/marker /up/hostname /abs/marker /abs/hostname
    want_rc 0 "links that lead out of the root"
    want_line "/up/marker OK"
    want_line "/up/hostname ENOENT"
    want_line "/abs/marker OK"
    want_line "/abs/hostname ENOENT"
    rr --root="$rf" -d share --run -- /bin/t exit 0
    want_rc 2 "a link to a directory only the container has"
    want_meta status XX
    rr --root="$rf" -d share:maybe --run -- /bin/t stat /share
    want_rc 0 "the same with maybe"
    want_line "/share ENOENT"
}

t_root_usage() {
    binit
    : > "$WORK/file"
    for d in "$WORK/nothere" rootfs "$WORK/file" ""; do
        rr --root="$d" --run -- $T exit 0
        want_rc 2 "--root='$d'"
    done
    return 0
}

# ---- -b auto ----

# ainit [ARGS]: --init -b auto. Sets A to the number of the box it picked
# and has the runner clean that box up.
ainit() {
    R -b auto "$@" --init
    [ "$RC" = 0 ] || fail "--init -b auto failed"
    A=$(cat "$OUT")
    A=${A##*/}
    case $A in '' | *[!0-9]*) fail "--init -b auto printed '$(cat "$OUT")'" ;; esac
    use_box "$A"
    want_eq "$(cat "$OUT")" "$ROOT/$A" "--init -b auto output"
}

t_auto_pick() {
    ainit
    a=$A
    ainit
    b=$A
    ainit
    c=$A
    [ "$a" -lt "$b" ] && [ "$b" -lt "$c" ] || fail "got boxes $a $b $c, expected increasing numbers"
    R -b "$b" --cleanup
    want_rc 0 "--cleanup of box $b"
    ainit
    want_eq "$A" "$b" "the box picked after box $b was cleaned up"
    # a directory without a cgroup, as after a restart of the container
    echo junk > "$ROOT/$b/box/junk"
    rmdir "$(cg_box_dir "$b")" || fail "cannot remove the cgroup of box $b"
    ainit
    want_eq "$A" "$b" "the box picked when box $b has no cgroup"
    [ -e "$ROOT/$b/box/junk" ] && fail "--init did not reset box $b"
    cp "$HELPER" "$ROOT/$c/box/t"
    R -b "$c" -M "$META" --run -- $T exit 0
    want_rc 0 "--run in box $c"
    R -b "$c" --cleanup
    want_rc 0 "--cleanup of box $c"
    [ -e "$ROOT/$c" ] && fail "$ROOT/$c still exists after --cleanup"
    return 0
}

t_auto_concurrent() {
    for i in 1 2 3 4 5 6 7 8; do
        (timeout 60 "$ROOM" -b auto --init < /dev/null > "$WORK/auto.$i" 2> "$WORK/auto-err.$i"
            echo $? > "$WORK/auto-rc.$i") &
    done
    wait
    for i in 1 2 3 4 5 6 7 8; do
        d=$(cat "$WORK/auto.$i")
        [ -n "$d" ] && use_box "${d##*/}"
    done
    for i in 1 2 3 4 5 6 7 8; do
        note "init $i: exit status $(cat "$WORK/auto-rc.$i"), $(cat "$WORK/auto.$i") $(cat "$WORK/auto-err.$i")"
        want_eq "$(cat "$WORK/auto-rc.$i")" 0 "exit status of init $i"
        [ -d "$(cat "$WORK/auto.$i")/box" ] || fail "init $i printed a box without box/"
    done
    want_eq "$(cat "$WORK"/auto.[1-8] | sort -u | wc -l | tr -d ' ')" 8 "number of different boxes"
}

t_auto_usage() {
    binit
    R -b auto -M "$META" --run -- $T exit 0
    want_rc 2 "-b auto with --run"
    R -b auto --cleanup
    want_rc 2 "-b auto with --cleanup"
    ainit -q 4096,100
    want_eq "$(mounts "$ROOT/$A")" "tmpfs room-quota" "mounts on the box -b auto picked"
    cp "$HELPER" "$ROOT/$A/box/t"
    R -b "$A" -M "$META" --run -- $T exit 0
    want_rc 0 "a run in the quota box"
    R -b "$A" --cleanup
    want_rc 0 "--cleanup of the quota box"
    want_eq "$(mounts "$ROOT/$A")" "" "mounts after --cleanup"
    [ -e "$ROOT/$A" ] && fail "$ROOT/$A still exists after --cleanup"
    return 0
}

# ---- concurrent room commands ----

# The lock is a file of its own, taken before anything else.
t_lock_file() {
    binit
    lock=$ROOT/.locks/$B
    [ -f "$lock" ] || fail "$lock is missing after --init"
    for cmd in --init "--run -- $T exit 0" --cleanup; do
        flock -n "$lock" "$ROOM" -b "$B" -M "$META" $cmd < /dev/null > "$OUT" 2> "$ERR"
        RC=$?
        note "room $cmd while $lock is held: exit status $RC, $(cat "$ERR")"
        want_rc 2 "$cmd while the lock file is held"
        grep -q 'is in use by another room process' "$ERR" || fail "$cmd did not say the box is in use"
    done
    [ -f "$BD/box/t" ] || fail "a command touched the box while the lock was held"
    R -b "$B" --cleanup
    want_rc 0
    [ -f "$lock" ] || fail "--cleanup removed $lock"
}

# two_cleanup: cleans up the boxes t_auto_two_roots got.
two_cleanup() {
    for f in "$WORK"/two.*; do
        [ -f "$f" ] || continue
        d=$(cat "$f")
        case $d in */[0-9]*) ROOM_BOX_ROOT=${d%/*} "$ROOM" -b "${d##*/}" --cleanup > /dev/null 2>&1 ;; esac
        rm -f "$f"
    done
}

# The cgroups and the box users depend only on the number, so two box roots
# must never get the same one.
t_auto_two_roots() {
    trap two_cleanup EXIT
    for round in $(seq 1 25); do
        for i in 1 2 3 4 5 6 7 8; do
            for r in a b; do
                (ROOM_BOX_ROOT=$WORK/r$r.$B timeout 60 "$ROOM" -b auto --init < /dev/null \
                    > "$WORK/two.$r$i" 2> "$WORK/two-err.$r$i"
                    echo $? > "$WORK/two-rc.$r$i") &
            done
        done
        wait
        for f in "$WORK"/two-rc.*; do
            [ "$(cat "$f")" = 0 ] ||
                fail "round $round: --init -b auto exited with $(cat "$f"): $(cat "$WORK/two-err.${f##*.}")"
        done
        shared=$(cat "$WORK"/two.* | sed 's,.*/,,' | sort | uniq -d | tr '\n' ' ')
        [ -z "$shared" ] || fail "round $round: numbers handed out twice: $shared"
        want_eq "$(cat "$WORK"/two.* | sort -u | wc -l | tr -d ' ')" 16 "round $round: number of boxes"
        two_cleanup
    done
}

t_quota_concurrent_init() {
    trap 'while [ -n "$(mounts "$BD")" ] && umount "$BD"; do :; done' EXIT
    qinit 1024,100
    for round in $(seq 1 150); do
        for i in 1 2 3 4; do
            ("$ROOM" -b "$B" -q 1024,100 --init < /dev/null > /dev/null 2> "$WORK/qi-err.$i"
                echo $? > "$WORK/qi-rc.$i") &
        done
        wait
        n=$(mounts "$BD" | wc -l | tr -d ' ')
        [ "$n" = 1 ] || fail "round $round: $n mounts on $BD: $(mounts "$BD" | tr '\n' ' ')"
        for i in 1 2 3 4; do
            rc=$(cat "$WORK/qi-rc.$i")
            [ "$rc" = 0 ] || { [ "$rc" = 2 ] && grep -q 'is in use by another room process' "$WORK/qi-err.$i"; } ||
                fail "round $round: --init exited with $rc: $(cat "$WORK/qi-err.$i")"
        done
    done
    R -b "$B" --cleanup
    want_rc 0 "the first --cleanup"
    want_eq "$(mounts "$BD")" "" "mounts on $BD after --cleanup"
}

# Every command on a busy box works or says the box is in use. A --run that
# comes after a --cleanup may also say the box isn't initialised.
t_concurrent_commands() {
    trap 'while [ -n "$(mounts "$BD")" ] && umount "$BD"; do :; done' EXIT
    : > "$WORK/cc.log"
    for round in $(seq 1 30); do
        R -b "$B" -q 4096,100 --init
        want_rc 0 "round $round: the first --init"
        for i in 1 2 3; do
            ("$ROOM" -b "$B" -q 4096,100 --init < /dev/null > /dev/null 2> "$WORK/cc.init$i"
                echo "init $? $(tr '\n' ' ' < "$WORK/cc.init$i")" >> "$WORK/cc.log") &
            ("$ROOM" -b "$B" -s --run -- /usr/bin/true < /dev/null > /dev/null 2> "$WORK/cc.run$i"
                echo "run $? $(tr '\n' ' ' < "$WORK/cc.run$i")" >> "$WORK/cc.log") &
        done
        for i in 1 2; do
            ("$ROOM" -b "$B" --cleanup < /dev/null > /dev/null 2> "$WORK/cc.cleanup$i"
                echo "cleanup $? $(tr '\n' ' ' < "$WORK/cc.cleanup$i")" >> "$WORK/cc.log") &
        done
        wait
        n=$(mounts "$BD" | wc -l | tr -d ' ')
        [ "$n" -le 1 ] || fail "round $round: $n mounts on $BD"
    done
    note "results: $(sed 's/[0-9][0-9]*/N/g' "$WORK/cc.log" | sort | uniq -c)"
    bad=$(grep -v -e '^[a-z]* 0 $' \
        -e '^[a-z]* 2 room: Box [0-9]* is in use by another room process $' \
        -e '^run 2 room: Box [0-9]* is not initialised, run room --init first $' "$WORK/cc.log")
    [ -z "$bad" ] || fail "unexpected results: $bad"
}

# ---- more of -b auto, --root, the quota and the cgroups ----

# A failed --init -b auto must give back the number it claimed. The mount
# point in the box directory makes it fail after the claim.
t_auto_failed_init() {
    n=0
    while [ -d "$(cg_box_dir $n)" ]; do n=$((n + 1)); done
    use_box "$n"
    fr=$WORK/fr.$B
    mkdir -p "$fr/$n/sub" || fail "cannot create $fr/$n/sub"
    trap 'umount "$fr/$n/sub" 2> /dev/null' EXIT
    mount -t tmpfs tmpfs "$fr/$n/sub" || fail "cannot mount a tmpfs on $fr/$n/sub"
    ROOM_BOX_ROOT=$fr "$ROOM" -b auto --init < /dev/null > "$OUT" 2> "$ERR"
    RC=$?
    note "--init -b auto with box $n failing: exit status $RC, $(cat "$OUT") $(cat "$ERR")"
    want_rc 2
    [ -d "$(cg_box_dir "$n")" ] && fail "the failed --init left the cgroup of box $n"
    ainit
    want_eq "$A" "$n" "the box picked after the failed --init"
}

# A magic link in a procfs below DIR points out of DIR, so --root must
# refuse it.
t_root_magiclink() {
    binit
    rf=$WORK/rootfs.$B
    make_rootfs "$rf"
    mkdir "$rf/proc" || fail "cannot create $rf/proc"
    trap 'umount "$rf/proc" 2> /dev/null' EXIT
    mount --bind /proc "$rf/proc" || fail "cannot bind /proc into $rf"
    ln -s /proc/self/root/etc "$rf/esc" && ln -s /proc/1/root "$rf/esc2" ||
        fail "cannot create the links in $rf"
    rr --root="$rf" -d esc --run -- /bin/t stat /esc/hostname
    want_rc 2 "a link through /proc/self/root"
    want_meta status XX
    rr --root="$rf" -d esc2:maybe --run -- /bin/t stat /esc2/etc/hostname
    want_rc 2 "a link to /proc/1/root with maybe"
    want_meta status XX
}

# File data on the quota tmpfs is memory of the run that writes it.
t_quota_cg_mem() {
    qinit 204800,100
    rr --cg-mem=32768 --run -- $T bigfile 65536
    want_rc 1
    size=$(stat -c %s "$BD/box/big" 2> /dev/null || echo 0)
    [ "$size" -lt 67108864 ] || fail "the program wrote all of a 64 MB file with a 32 MB memory limit"
    has cg-oom-killed || grep -q ENOMEM "$OUT" ||
        fail "writing 64 MB into /box with a 32 MB memory limit gave neither an OOM kill nor ENOMEM"
}

# Kernel 7.0 kills a clone3 into a cgroup whose cgroup.kill count differs
# from its caller's. Every run gets a fresh run cgroup below box-N, so runs
# right after killed ones must still start.
t_many_runs() {
    binit
    for i in 1 2 3 4 5; do
        TMO=20 rr -t 0.1 --run -- $T spin
        want_rc 1 "CPU limit, round $i"
        want_meta killed 1
        rr --run -- $T exit 0
        want_rc 0 "the run after the CPU kill, round $i"
        TMO=20 rr -w 0.2 --run -- $T sleep 5
        want_rc 1 "wall limit, round $i"
        want_meta killed 1
        rr --run -- $T exit 0
        want_rc 0 "the run after the wall kill, round $i"
        rr --cg-mem=16384 --run -- $T alloc 100
        want_rc 1 "OOM, round $i"
        rr --run -- $T exit 0
        want_rc 0 "the run after the OOM kill, round $i"
    done
    for i in $(seq 1 20); do
        rr --run -- $T exit 0
        want_rc 0 "plain run $i"
    done
}

t_cgroup_nested() {
    binit
    cg=$(cg_box_dir "$B")
    [ -d "$cg" ] || fail "$cg is missing after --init"
    [ -e "$cg/run" ] && fail "$cg/run exists right after --init"
    sc=" $(cat "$cg/cgroup.subtree_control") "
    for c in memory pids; do
        case $sc in *" $c "*) ;; *) fail "$cg/cgroup.subtree_control is '$sc', no $c" ;; esac
    done
    if have_cpuset; then
        case $sc in *" cpuset "*) ;; *) fail "$cg/cgroup.subtree_control is '$sc', no cpuset" ;; esac
    fi
    "$ROOM" -b "$B" -M "$META" -w 10 --run -- $T sleep 1 < /dev/null > /dev/null 2>&1 &
    pid=$!
    if ! wait_started "$U"; then
        kill -9 $pid
        wait $pid
        fail "the program never started"
    fi
    box_procs=$(cat "$cg/cgroup.procs")
    run_procs=$(cat "$cg/run/cgroup.procs")
    wait $pid
    RC=$?
    want_rc 0
    [ -z "$box_procs" ] || fail "$cg holds processes during the run: $box_procs"
    [ -n "$run_procs" ] || fail "$cg/run holds no processes during the run"
    # box-N is the claim on the number. Neither a run nor --init may drop it, even for a moment
    ino=$(stat -c %i "$cg")
    rr --run -- $T exit 0
    want_rc 0 "a second run"
    binit
    want_eq "$(stat -c %i "$cg" 2> /dev/null)" "$ino" "inode of $cg after a run and another --init"
    [ -e "$cg/run" ] && fail "$cg/run is still there after --init"
    R -b "$B" --cleanup
    want_rc 0
    [ -e "$cg" ] && fail "$cg still exists after --cleanup"
    return 0
}

# A program running in another box holds a copy of this box's quota mount
# in its mount namespace. --init must drop it, or its memory stays in use.
t_quota_pin() {
    qinit 102400,100
    binit "$B2"
    "$ROOM" -b "$B2" -w 20 --run -- $T sleep 5 < /dev/null > /dev/null 2>&1 &
    pid=$!
    if ! wait_started $((60000 + B2)); then
        kill -9 $pid
        wait $pid
        fail "the program in box $B2 never started"
    fi
    proxy=$(pgrep -P $pid)
    before=$(awk -v d="$BD" '$5 == d' "/proc/$proxy/mountinfo" | wc -l | tr -d ' ')
    R -b "$B" --init
    init_rc=$RC
    after=$(awk -v d="$BD" '$5 == d' "/proc/$proxy/mountinfo" | wc -l | tr -d ' ')
    kill -TERM $pid
    wait $pid
    RC=$init_rc
    want_rc 0 "--init of box $B"
    note "copies of $BD in the namespace of box $B2: $before before --init, $after after"
    want_eq "$after" 0 "copies of the quota mount left in the namespace of box $B2"
}

# ---- box numbers and uids ----

# box_uid N: the uid and gid of box N. 65534 and 65535 are skipped.
box_uid() {
    if [ "$1" -lt 5534 ]; then echo $((60000 + $1)); else echo $((60002 + $1)); fi
}

# box_count: the number of boxes DESIGN.md gives for this machine's uid and gid maps.
box_count() {
    awk 'FNR == 1 { e[++n] = 0 }
        $1 <= 60000 && 60000 < $1 + $3 { e[n] = $1 + $3 }
        END {
            m = e[1] < e[2] ? e[1] : e[2]
            if (m > 2147483648) m = 2147483648
            c = m - 60000 - (m > 65535 ? 2 : m > 65534 ? 1 : 0)
            printf "%.0f\n", c < 0 ? 0 : c
        }' /proc/self/uid_map /proc/self/gid_map
}

# want_box_ids N...: --init, a run as the right user and --cleanup in each box.
want_box_ids() {
    use_box "$@"
    for id in "$@"; do
        binit "$id"
        u=$(box_uid "$id")
        R -b "$id" -M "$META" --run -- $T ids
        want_rc 0 "a run in box $id"
        want_line "uid=$u $u $u"
        want_line "gid=$u $u $u"
        R -b "$id" --cleanup
        want_rc 0 "--cleanup of box $id"
        [ -e "$ROOT/$id" ] && fail "$ROOT/$id still exists after --cleanup"
        [ -e "$(cg_box_dir "$id")" ] && fail "the cgroup of box $id still exists after --cleanup"
    done
    return 0
}

# The highest box runs as uid 2147483647 when the maps cover every uid.
t_box_large() {
    want_box_ids 1000 1234567 $(($(box_count) - 1))
}

t_box_uid_skip() {
    want_box_ids 5533 5534 5535
}

t_box_too_high() {
    n=$(box_count)
    for cmd in --init "--run -- $T exit 0" --cleanup; do
        R -b "$n" $cmd
        want_rc 2 "-b $n $cmd"
        grep -qF "has $n boxes" "$ERR" || fail "stderr '$(cat "$ERR")' doesn't say there are $n boxes"
    done
    [ -e "$ROOT/$n" ] && fail "$ROOT/$n was created"
    [ -e "$ROOT/.locks/$n" ] && fail "$ROOT/.locks/$n was created"
    [ -e "$(cg_box_dir "$n")" ] && fail "the cgroup of box $n was created"
    return 0
}

# With 65536 uids, uids 60000 to 65533 are left for boxes. What happens
# after the check in a user namespace (cgroups, mounts) isn't this test's.
t_box_userns() {
    use_box 5533 5534
    ns="unshare --user --map-users=0:0:65536 --map-groups=0:0:65536"
    timeout 10 $ns true 2> "$ERR" || skip "cannot create a user namespace with 65536 uids: $(cat "$ERR")"
    WRAP=$ns R -b 5534 --init
    want_rc 2 "box 5534 with 65536 uids"
    grep -qF "has 5534 boxes" "$ERR" || fail "stderr '$(cat "$ERR")' doesn't say there are 5534 boxes"
    WRAP=$ns R -b 5533 --init
    if grep -q "boxes" "$ERR"; then fail "box 5533 was refused: $(cat "$ERR")"; fi
    WRAP="unshare --user --map-users=0:0:1000 --map-groups=0:0:1000" R -b "$B" --init
    want_rc 2 "a user namespace without uid 60000"
    grep -qF "no uids for boxes" "$ERR" || fail "stderr '$(cat "$ERR")' doesn't say there are no uids"
}

# auto_inits N: N --init -b auto, 100 at a time. The box directories go to $WORK/picked.
auto_inits() {
    : > "$WORK/picked"
    : > "$WORK/init.err"
    i=0
    while [ $i -lt "$1" ]; do
        timeout 60 "$ROOM" -b auto --init < /dev/null >> "$WORK/picked" 2>> "$WORK/init.err" &
        i=$((i + 1))
        [ $((i % 100)) = 0 ] && wait
    done
    wait
    [ -s "$WORK/init.err" ] && fail "--init -b auto failed: $(head -n 3 "$WORK/init.err")"
    sed 's,.*/,,' "$WORK/picked" | sort -n > "$WORK/got"
    seq 0 $(($1 - 1)) > "$WORK/want"
    cmp -s "$WORK/got" "$WORK/want" ||
        fail "the boxes handed out are not 0 to $(($1 - 1)) once each: $(diff "$WORK/want" "$WORK/got" | head -n 5)"
}

# auto_cleanups: --cleanup of every box in $WORK/picked, 100 at a time.
auto_cleanups() {
    i=0
    for a in $(sed 's,.*/,,' "$WORK/picked"); do
        "$ROOM" -b "$a" --cleanup < /dev/null > /dev/null 2>&1 &
        i=$((i + 1))
        [ $((i % 100)) = 0 ] && wait
    done
    wait
}

# want_no_probe NEXT: a pick with an unused box root creates one lock file,
# for NEXT. Trying the taken numbers one by one would lock every one of them.
want_no_probe() {
    fresh=$(mktemp -d "$WORK/fresh.XXXXXX") || fail "cannot create a new box root"
    export ROOM_BOX_ROOT=$fresh
    R -b auto --init
    want_rc 0 "--init -b auto in a new box root"
    want_eq "$(cat "$OUT")" "$fresh/$1" "the box picked in a new box root"
    want_eq "$(ls -A "$fresh/.locks" | tr '\n' ' ')" "$1 " "the lock files in the new box root"
    R -b "$1" --cleanup
    want_rc 0 "--cleanup of box $1 in the new box root"
    unset ROOM_BOX_ROOT
}

t_auto_no_probe() {
    trap auto_cleanups EXIT
    auto_inits 50
    want_no_probe 50
}

t_auto_many() {
    trap auto_cleanups EXIT
    auto_inits 1100
    cp "$HELPER" "$ROOT/1099/box/t" || fail "cannot copy the helper into box 1099"
    R -b 1099 -M "$META" --run -- $T ids
    want_rc 0 "a run in box 1099"
    want_line "uid=61099 61099 61099"
    R -b 550 --cleanup
    want_rc 0 "--cleanup of box 550"
    R -b auto --init
    want_rc 0 "--init -b auto after box 550 was cleaned up"
    want_eq "$(cat "$OUT")" "$ROOT/550" "the box picked after box 550 was cleaned up"
    want_no_probe 1100
    auto_cleanups
    trap - EXIT
    for a in $(cat "$WORK/want"); do
        [ -e "$ROOT/$a" ] && fail "$ROOT/$a still exists after --cleanup"
        [ -e "$(cg_box_dir "$a")" ] && fail "the cgroup of box $a still exists after --cleanup"
    done
    return 0
}

# Names in room/ that aren't box-N claims don't count as taken numbers.
t_auto_names() {
    room=$(cg_box_dir 0)
    room=${room%/*}
    names="box-x boxes box- box-01 box-1x box--1 box-99999999999"
    trap 'for d in $names; do rmdir "$room/$d" 2> /dev/null; done' EXIT
    for d in $names; do mkdir "$room/$d" || fail "cannot create $room/$d"; done
    want=0
    while [ -d "$room/box-$want" ]; do want=$((want + 1)); done
    ainit
    want_eq "$A" "$want" "the box picked next to $names"
}

# ---- runner ----

PASS=0
FAILED=0
SKIPPED=0
N=0

run() {
    name=$1
    N=$((N + 1))
    if [ -n "$FILTER" ]; then
        case $name in *"$FILTER"*) ;; *) return ;; esac
    fi
    # Box ids are tied to the position in the list so they don't change
    # when a filter is used.
    B=$((N + 10))
    B2=$((B + 400))
    BD=$ROOT/$B
    U=$((60000 + B))
    LOG=$WORK/log
    : > "$LOG"
    rm -f "$WORK/skip"
    echo "$B $B2" > "$WORK/boxes"

    # Only fail() and skip() end a test early, so reaching the end is a pass.
    ("t_$name"; exit 0) >> "$LOG" 2>&1
    rc=$?

    for id in $(cat "$WORK/boxes"); do
        "$ROOM" -b "$id" --cleanup > /dev/null 2>&1 ||
            echo "cleanup of box $id failed" >> "$LOG"
        if [ -d "$WORK/alt" ]; then
            ROOM_BOX_ROOT=$WORK/alt "$ROOM" -b "$id" --cleanup > /dev/null 2>&1
        fi
    done

    case $rc in
        0)
            echo "ok - $name"
            PASS=$((PASS + 1))
            ;;
        77)
            echo "ok - $name # SKIP $(cat "$WORK/skip")"
            SKIPPED=$((SKIPPED + 1))
            ;;
        *)
            echo "not ok - $name"
            sed 's/^/    # /' "$LOG"
            FAILED=$((FAILED + 1))
            ;;
    esac
}

echo "# room tests: $ROOM, kernel $(uname -r) $(uname -m), CapEff $(awk '/^CapEff:/ { print $2 }' /proc/self/status)"

for t in \
    init_prints_dir init_resets cleanup run_uninitialised box_root_env \
    box_root_relative usage_errors version run_twice \
    success exit_code abort segv not_found cpu_limit extra_time \
    over_time_exits wall_limit cpu_per_run run_after_kill cg_mem_oom cg_mem_report \
    cg_mem_per_run mem_as_limit procs_default procs_limit procs_unlimited \
    threads forkbomb fsize open_files orphans rlimits \
    identity no_leaked_fds fs_readonly fs_writable fs_hidden proc_own \
    dev_contents net_default net_loopback_up net_share seccomp_table \
    seccomp_arch seccomp_allowed special_files caller_file \
    redirect stderr_to_stdout inherit_std closed_stdio chdir xx_special_files \
    xx_first_message env_default env_rules \
    env_full dir_ro dir_rw dir_noexec dir_maybe dir_file dir_remove_tmp \
    dir_etc dir_dotdot no_default_dirs relative_program cg_flag silent \
    meta_stdout meta_uninitialised meta_box_in_use meta_usage_error \
    lock signals sigkill deep_box deep_tmp deep_rerun overhead concurrency \
    cgroup_layout cleanup_cgroup caller_flow \
    time_overflow cg_mem_tiny oom_group meta_newlines xx_after_run \
    mount_flags root_switch namespaces special_subdir hardlink \
    dir_dev dir_norec dir_fs_tmpfs dir_tmp_rule dir_replace_order dir_dev_rules \
    signal_early sigchld_ignored sig_inherit wide_deep wide_shallow \
    cg_root cg_root_kill_history systemd_refusal systemd_unit systemd_stopped systemd_stop_box \
    seccomp_sim \
    quota_size quota_inodes quota_compile quota_cleanup quota_reinit quota_foreign \
    quota_usage quota_fast_cleanup cpus cpus_missing cpus_unavailable cpuset_enable \
    cpus_usage wall_default \
    root_dir root_rules root_usage auto_pick auto_concurrent auto_usage \
    quota_ramfs lock_file auto_two_roots quota_concurrent_init concurrent_commands \
    auto_failed_init root_magiclink quota_cg_mem many_runs cgroup_nested quota_pin \
    box_large box_uid_skip box_too_high box_userns auto_no_probe auto_many auto_names
do
    run "$t"
done

echo "$PASS passed, $FAILED failed, $SKIPPED skipped"
[ "$FAILED" = 0 ]
