/*
** PS Vita POSIX shim: <net/if.h>
**
** Interface flags used together with the getifaddrs() shim.
*/
#ifndef VITA_COMPAT_NET_IF_H
#define VITA_COMPAT_NET_IF_H

#define IFNAMSIZ 16

#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_LOOPBACK    0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_RUNNING     0x0040
#define IFF_MULTICAST   0x1000

#endif
