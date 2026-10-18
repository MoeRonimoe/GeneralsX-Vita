/*
** PS Vita POSIX shim: <dlfcn.h>
**
** The game binary is linked statically; there are no shared objects to load. The functions
** exist so the Win32 LoadLibrary/GetProcAddress emulation links, and always fail.
** Implemented in Platform/Vita/compat/VitaPosix.cpp.
*/
#ifndef VITA_COMPAT_DLFCN_H
#define VITA_COMPAT_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
