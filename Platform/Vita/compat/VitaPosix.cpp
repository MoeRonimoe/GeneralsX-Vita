/*
** PS Vita POSIX shim: the few libc entry points newlib does not provide on this platform.
*/

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <fnmatch.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <dlfcn.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

namespace
{

// One allocation per getifaddrs() result, so freeifaddrs() is a single free().
struct VitaInterface
{
	ifaddrs entry;
	sockaddr_in address;
	sockaddr_in netmask;
	sockaddr_in broadcast;
	char name[IFNAMSIZ];
};

bool queryAddress(int code, in_addr &out)
{
	SceNetCtlInfo info;
	memset(&info, 0, sizeof(info));
	if (sceNetCtlInetGetInfo(code, &info) < 0)
		return false;

	// ip_address and netmask share the leading bytes of the info union as dotted strings.
	return inet_pton(AF_INET, info.ip_address, &out) == 1;
}

void fillAddress(sockaddr_in &out, in_addr value)
{
	memset(&out, 0, sizeof(out));
	out.sin_family = AF_INET;
	out.sin_addr = value;
}

} // namespace

extern "C" {

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	va_start(args, request);
	int *argument = va_arg(args, int *);
	va_end(args);

	switch (request)
	{
		case FIONBIO:
		{
			int nonBlocking = (argument != nullptr && *argument != 0) ? 1 : 0;
			return setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &nonBlocking, sizeof(nonBlocking));
		}

		case FIONREAD:
		{
			// The Vita network stack has no pending-byte query. Peeking into a datagram
			// sized buffer gives the size of the next packet, which is how the callers use it.
			char scratch[2048];
			const int pending = recv(fd, scratch, sizeof(scratch), MSG_PEEK | MSG_DONTWAIT);
			if (argument != nullptr)
				*argument = pending > 0 ? pending : 0;
			return 0;
		}

		default:
			errno = ENOSYS;
			return -1;
	}
}

int getifaddrs(struct ifaddrs **ifap)
{
	if (ifap == nullptr)
	{
		errno = EINVAL;
		return -1;
	}

	*ifap = nullptr;

	in_addr address;
	in_addr netmask;
	if (!queryAddress(SCE_NETCTL_INFO_GET_IP_ADDRESS, address))
		return 0; // not connected: an empty list, as on a machine without interfaces

	if (!queryAddress(SCE_NETCTL_INFO_GET_NETMASK, netmask))
		netmask.s_addr = htonl(0xFFFFFF00u);

	VitaInterface *iface = static_cast<VitaInterface *>(calloc(1, sizeof(VitaInterface)));
	if (iface == nullptr)
	{
		errno = ENOMEM;
		return -1;
	}

	in_addr broadcast;
	broadcast.s_addr = address.s_addr | ~netmask.s_addr;

	fillAddress(iface->address, address);
	fillAddress(iface->netmask, netmask);
	fillAddress(iface->broadcast, broadcast);
	strcpy(iface->name, "wlan0");

	iface->entry.ifa_name = iface->name;
	iface->entry.ifa_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
	iface->entry.ifa_addr = reinterpret_cast<sockaddr *>(&iface->address);
	iface->entry.ifa_netmask = reinterpret_cast<sockaddr *>(&iface->netmask);
	iface->entry.ifa_broadaddr = reinterpret_cast<sockaddr *>(&iface->broadcast);

	*ifap = &iface->entry;
	return 0;
}

void freeifaddrs(struct ifaddrs *ifa)
{
	// entry is the first member of VitaInterface
	free(ifa);
}

void *dlopen(const char *, int)
{
	return nullptr;
}

void *dlsym(void *, const char *)
{
	return nullptr;
}

int dlclose(void *)
{
	return 0;
}

char *dlerror(void)
{
	static char message[] = "dynamic loading is not available on PS Vita";
	return message;
}

} // extern "C"

//-----------------------------------------------------------------------------
// The functions below are declared by newlib's headers but not implemented for the Vita.

extern "C" {

/// Shell style pattern matching with '*', '?' and backslash escapes. Bracket expressions
/// are not supported; the engine only matches file names against wildcards.
int fnmatch(const char *pattern, const char *string, int flags)
{
	const char *starPattern = nullptr;
	const char *starString = nullptr;

	while (*string != '\0')
	{
		if (*pattern == '*')
		{
			// Remember where the star was so the match can back up to it.
			starPattern = ++pattern;
			starString = string;
			continue;
		}

		char expected = *pattern;
		if (expected == '\\' && (flags & FNM_NOESCAPE) == 0 && pattern[1] != '\0')
			expected = *++pattern;

		if (*pattern == '?' || expected == *string)
		{
			++pattern;
			++string;
			continue;
		}

		if (starPattern == nullptr)
			return FNM_NOMATCH;

		// Let the last star swallow one more character and try again.
		pattern = starPattern;
		string = ++starString;
	}

	while (*pattern == '*')
		++pattern;

	return *pattern == '\0' ? 0 : FNM_NOMATCH;
}

/// There are no symbolic links on the memory card.
ssize_t readlink(const char *, char *, size_t)
{
	errno = EINVAL;
	return -1;
}

} // extern "C"

/// newlib's <sys/utsname.h> declares uname() without C linkage.
int uname(struct utsname *name)
{
	if (name == nullptr)
	{
		errno = EFAULT;
		return -1;
	}

	memset(name, 0, sizeof(*name));
	strcpy(name->sysname, "PS Vita");
	strcpy(name->nodename, "vita");
	strcpy(name->release, "3.60+");
	strcpy(name->version, "VitaSDK");
	strcpy(name->machine, "armv7");
	return 0;
}
