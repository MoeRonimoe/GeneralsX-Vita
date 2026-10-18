#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <string.h>  // GeneralsX @build fbraz 10/02/2026 - strlen, memcpy for strlcpy/strlcat

typedef const char* LPCSTR;
typedef char* LPSTR;

extern "C"
{
char* itoa(int value, char* str, int base);

int _vsnwprintf(wchar_t* buffer, size_t count, const wchar_t* format, va_list args);
// GeneralsX @build fbraz 11/02/2026 BenderAI - Removed _strlwr declaration (conflicts with Utility inline definition)
// GeneralsX @build fbraz 11/02/2026 BenderAI - _strupr implemented in string_compat.cpp as weak symbol
char* _strupr(char* str);
}
#define _vsnprintf vsnprintf
#define _snprintf snprintf
#define strlwr _strlwr
#define strupr _strupr

#define lstrcat strcat
#define lstrcpy strcpy
// GeneralsX @bugfix Claude 12/10/2026 lstrcpyn() is not strncpy(): the Windows function copies at most
// count - 1 characters and always ends the string. Mapped to strncpy(), the code that makes a file name
// of "Model.Part" (lstrcpyn up to the dot, then lstrcat ".w3d") kept the dot, got no end of string and
// appended to whatever followed in the buffer. The file was never found, and every request for the
// part went to the disk again with another name.
#ifndef GENERALSX_LSTRCPYN_DEFINED
#define GENERALSX_LSTRCPYN_DEFINED
static inline char *lstrcpyn(char *destination, const char *source, int count)
{
	if (count > 0)
	{
		strncpy(destination, source, count - 1);
		destination[count - 1] = 0;
	}
	return destination;
}
#endif
#define lstrcmpi strcasecmp
#define lstrlen strlen
#define _stricmp strcasecmp
#define stricmp strcasecmp
#define _strnicmp strncasecmp
#define strnicmp strncasecmp
#define _strdup strdup

// GeneralsX @build fbraz 10/02/2026 Bender
// NOTE: strlcpy/strlcat originally added here for WWDownload/FTP.cpp but WWLib/stringex.h
// already defines them (lines 137/140). Since WWCommon.h includes stringex.h first,
// we don't need to redefine. Keeping commented code for reference.
/*
static inline size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copylen = (srclen < size - 1) ? srclen : size - 1;
        if (copylen > 0) {
            memcpy(dst, src, copylen);
        }
        dst[copylen] = '\0';
    }
    return srclen;
}

static inline size_t strlcat(char* dst, const char* src, size_t size) {
    size_t dstlen = strlen(dst);
    size_t srclen = strlen(src);
    if (dstlen >= size) {
        return dstlen + srclen;
    }
    size_t copylen = size - dstlen - 1;
    if (srclen < copylen) {
        copylen = srclen;
    }
    if (copylen > 0) {
        memcpy(dst + dstlen, src, copylen);
    }
    dst[dstlen + copylen] = '\0';
    return dstlen + srclen;
}
*/
