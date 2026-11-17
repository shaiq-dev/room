/* room - the system call filter. */
#ifndef ROOM_SECCOMP_H
#define ROOM_SECCOMP_H

/*
 * Sets no_new_privs and installs the syscall filter. Call it right before
 * execve(), once room needs none of the calls it blocks. Dies on failure.
 */
void seccomp_install(void);

#endif
