/*
** PS Vita POSIX shim: <sys/ioctl.h>
**
** newlib on the Vita provides BSD sockets but no ioctl(). The engine and the GameSpy SDK
** use it only to switch a socket to non-blocking mode and to ask how many bytes are pending.
** Implemented in Platform/Vita/compat/VitaPosix.cpp.
*/
#ifndef VITA_COMPAT_SYS_IOCTL_H
#define VITA_COMPAT_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define FIONBIO  0x5421
#define FIONREAD 0x541B

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
