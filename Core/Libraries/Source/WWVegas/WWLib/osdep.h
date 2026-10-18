#ifndef OSDEP_H
#define OSDEP_H

#ifdef _UNIX
#include <alloca.h>
#define _alloca alloca
#include <ctype.h>
#include <cstring>
#include <wchar.h>
typedef char TCHAR;
typedef wchar_t WCHAR; 
#define _tcslen strlen
#define _tcsclen strlen
#define _tcscmp strcmp
#define _tcsicmp strcasecmp
#define _wcsicmp wcscasecmp 
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define strcmpi strcasecmp

#define _vsnprintf vsnprintf
#define _snprintf snprintf
#define _strdup strdup      // GeneralsX @build BenderAI 10/02/2026 - String duplication
#define lstrcpy strcpy      // GeneralsX @build BenderAI 10/02/2026 - String copy
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
#define lstrcat strcat      // GeneralsX @build BenderAI 10/02/2026 - String concatenation

// GeneralsX @TheSuperHackers @build BenderAI 11/02/2026 Only define strupr/strrev if not already provided by compat headers
// Note: Check for macro definition, not function existence
#if !defined(strupr)
static char *strupr(char *str)
{
    for (int i = 0; i < strlen(str); i++)
        str[i] = toupper(str[i]);

    return str;
}
#endif

#if !defined(strrev)
static char *strrev(char *str)
{
    if (!str || ! *str)
        return str;

    int i = strlen(str) - 1, j = 0;

    char ch;
    while (i > j)
    {
        ch = str[i];
        str[i] = str[j];
        str[j] = ch;
        i--;
        j++;
    }
    return str;
}
#endif

#endif // _UNIX

#endif // OSDEP_H
