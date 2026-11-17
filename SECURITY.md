# Security policy

Room exists to run programs you don't trust. A way for such a program to get out of its box is the most serious kind of bug this project can have.

## Reporting a problem

Please don't open a public issue for a security problem. Report it privately through GitHub: on this repository open the **Security** tab and choose **Report a vulnerability**.

Include what you can of:

- The room version (`room --version`), the kernel version and the architecture.
- How room was run: the container's flags (`--cap-add SYS_ADMIN`, `--privileged`, anything else), or the machine and whether it uses systemd and `room.service`.
- The room command line.
- The program that shows the problem, or a description of what it does.

## What counts

The caller of room is root and is trusted. The program in the box is not. These are security problems:

- The program reads or writes something outside what the directory rules give it.
- The program gains root, a capability or a namespace it shouldn't have.
- The program reaches the network without `--share-net`, or reaches processes or devices outside the box.
- A process of the box is still alive after `room --run` has returned.
- The program makes a box impossible to clean up, or makes room hang or crash.
- The program uses clearly more time, memory, processes or disk than its limits allow, or makes the meta file report less than it used.
- The program gets the caller to read something through what it left in the box.

These are known and described in the README under "Limits", so they aren't reports:

- Without `--quota` a box has no disk limit.
- The memory charged to a box includes the page cache of the files it uses.
- File locks are allowed, so two programs that cooperate can signal each other through a shared read-only file.
- With `--share-net` the program can reach whatever the container can reach.
- Under Docker's default seccomp profile room switches the root with `chroot`, not `pivot_root`.

A bug in the Linux kernel itself should go to the kernel's security team. If room could reasonably have kept the program away from the affected kernel code, that part is welcome here.

## Supported versions

Fixes go into the latest release.
