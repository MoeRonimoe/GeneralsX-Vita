/*
**	Direct3D 8 on vitaGL
**	Part of the PS Vita port of Command & Conquer Generals.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// VitaD3D8Format.h ///////////////////////////////////////////////////////////
// Pixel formats and flexible vertex formats: sizes, layouts and the mapping of
// Direct3D pixel data onto what vitaGL stores without converting.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "VitaD3D8.h"

namespace VitaD3D8
{

/// How the pixels of a Direct3D format reach vitaGL.
enum PixelTransfer
{
	TRANSFER_NONE,			///< not a texture format this implementation can upload
	TRANSFER_DIRECT,		///< memory layout matches the GL format; uploaded as is
	TRANSFER_ARGB1555,		///< 16 bit, rotated to RGBA5551
	TRANSFER_ARGB1555_WIDE,	///< 16 bit with one bit of alpha, widened to 32 bit BGRA
	TRANSFER_XRGB1555,		///< 16 bit without alpha, widened to RGB565
	TRANSFER_ARGB4444,		///< 16 bit, rotated to RGBA4444
	TRANSFER_XRGB8888,		///< 32 bit BGRA with alpha forced to opaque
	TRANSFER_A8,			///< alpha only, expanded to luminance alpha with white luminance
	TRANSFER_A4L4,			///< 8 bit, expanded to luminance alpha
	TRANSFER_COMPRESSED		///< S3TC blocks, uploaded as is
};

struct FormatInfo
{
	D3DFORMAT format;
	UINT bytesPerPixel;		///< 0 for block compressed formats
	UINT blockBytes;		///< bytes per 4x4 block for compressed formats, else 0
	bool hasAlpha;
	PixelTransfer transfer;
	GLenum glInternalFormat;
	GLenum glFormat;
	GLenum glType;
	UINT uploadBytesPerPixel;	///< bytes per pixel of the data handed to vitaGL
};

/// Description of a format, or nullptr if the format is unknown to this implementation.
const FormatInfo *GetFormatInfo(D3DFORMAT format);

/// The description to upload a texture of the given format with. It differs from the
/// format's own for A1R5G5B5 textures with mip maps: vitaGL builds the lower levels of a
/// 5551 texture with the bit layout of 1555, which turns them into noise.
const FormatInfo &GetUploadFormat(const FormatInfo &info, bool mipMapped);

/// True if textures of this format can be created and sampled.
bool IsTextureFormatSupported(D3DFORMAT format);

/// True for the depth/stencil formats the display surface satisfies.
bool IsDepthFormatSupported(D3DFORMAT format);

/// Bytes between two rows of a surface. For compressed formats this is the size of a row of blocks.
UINT GetPitch(const FormatInfo &info, UINT width);

/// Bytes a surface of the given size occupies.
UINT GetSurfaceSize(const FormatInfo &info, UINT width, UINT height);

/// Convert rows of Direct3D pixels into the layout named by info.glFormat / info.glType.
/// source and destination may not overlap. For TRANSFER_DIRECT and TRANSFER_COMPRESSED
/// nothing has to be converted and the caller uploads the source directly.
void ConvertPixels(const FormatInfo &info, const void *source, UINT sourcePitch,
	void *destination, UINT destinationPitch, UINT width, UINT height);

//-----------------------------------------------------------------------------
/// Layout of the vertex described by a flexible vertex format code.
struct VertexLayout
{
	enum { MAX_TEXCOORDS = 8 };

	DWORD fvf;
	UINT stride;
	bool hasPosition;
	bool isTransformed;		///< D3DFVF_XYZRHW: screen space position, no transform or lighting
	int normalOffset;		///< -1 when the vertex has no normal
	int diffuseOffset;		///< -1 when the vertex has no diffuse color
	int specularOffset;		///< -1 when the vertex has no specular color
	UINT texCoordCount;
	UINT texCoordOffset[MAX_TEXCOORDS];
	UINT texCoordSize[MAX_TEXCOORDS];	///< floats per coordinate set
};

/// Decode a flexible vertex format code. The position is always at offset 0.
void GetVertexLayout(DWORD fvf, VertexLayout &layout);

/// Size in bytes of one vertex of the given flexible vertex format.
UINT GetVertexSize(DWORD fvf);

/// Direct3D stores vertex colors as B, G, R, A bytes; the vitaGL color array reads R, G, B, A.
/// Swap red and blue of the diffuse and specular colors of count vertices in place.
void SwapVertexColors(const VertexLayout &layout, void *vertices, UINT count);

} // namespace VitaD3D8
