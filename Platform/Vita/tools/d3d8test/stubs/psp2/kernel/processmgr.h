// Stands in for the PS Vita's header when the Direct3D 8 layer is built on a PC
// (see ../../../driver.cpp). mockgl.cpp has the function.
#pragma once

#include <stdint.h>

typedef uint64_t SceUInt64;

#ifdef __cplusplus
extern "C" {
#endif

SceUInt64 sceKernelGetProcessTimeWide(void);

#ifdef __cplusplus
}
#endif
