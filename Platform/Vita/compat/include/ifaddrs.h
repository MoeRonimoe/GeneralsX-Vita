/*
** PS Vita POSIX shim: <ifaddrs.h>
**
** Reports the single Wi-Fi interface of the console as returned by SceNetCtl.
** Implemented in Platform/Vita/compat/VitaPosix.cpp.
*/
#ifndef VITA_COMPAT_IFADDRS_H
#define VITA_COMPAT_IFADDRS_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs
{
	struct ifaddrs  *ifa_next;
	char            *ifa_name;
	unsigned int     ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	struct sockaddr *ifa_broadaddr;
	void            *ifa_data;
};

#define ifa_dstaddr ifa_broadaddr

int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif

#endif
