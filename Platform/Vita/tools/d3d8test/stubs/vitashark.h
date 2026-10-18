// What vitaGL.h needs of the PS Vita's headers, for building the Direct3D 8 layer on a PC
// (see ../driver.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct SceGxmTexture SceGxmTexture;

typedef enum
{
	SCE_GXM_MULTISAMPLE_NONE,
	SCE_GXM_MULTISAMPLE_2X,
	SCE_GXM_MULTISAMPLE_4X
} SceGxmMultisampleMode;

typedef enum
{
	SHARK_OPT_SLOW,
	SHARK_OPT_SAFE,
	SHARK_OPT_DEFAULT,
	SHARK_OPT_FAST,
	SHARK_OPT_UNSAFE
} shark_opt;
