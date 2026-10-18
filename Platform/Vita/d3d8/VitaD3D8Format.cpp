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

// VitaD3D8Format.cpp /////////////////////////////////////////////////////////
// Pixel formats and flexible vertex formats, see VitaD3D8Format.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaD3D8Format.h"

namespace VitaD3D8
{

namespace
{

// The GL formats are chosen so vitaGL takes its "fast store" path: the data is copied
// into the texture without a per pixel conversion and without widening 16 bit formats.
//
// Direct3D names formats by the bit order inside a little endian word, so A8R8G8B8 is
// the byte sequence B, G, R, A, which is GL_BGRA with unsigned bytes.
const FormatInfo s_formats[] =
{
	//  format           bpp blk alpha  transfer             internal             format               type                          upload bpp
	{ D3DFMT_A8R8G8B8,   4,  0,  true,  TRANSFER_DIRECT,     GL_BGRA,             GL_BGRA,             GL_UNSIGNED_BYTE,             4 },
	{ D3DFMT_X8R8G8B8,   4,  0,  false, TRANSFER_XRGB8888,   GL_BGRA,             GL_BGRA,             GL_UNSIGNED_BYTE,             4 },
	{ D3DFMT_R8G8B8,     3,  0,  false, TRANSFER_DIRECT,     GL_BGR,              GL_BGR,              GL_UNSIGNED_BYTE,             3 },
	{ D3DFMT_R5G6B5,     2,  0,  false, TRANSFER_DIRECT,     GL_RGB,              GL_RGB,              GL_UNSIGNED_SHORT_5_6_5,      2 },
	{ D3DFMT_A1R5G5B5,   2,  0,  true,  TRANSFER_ARGB1555,   GL_RGBA,             GL_RGBA,             GL_UNSIGNED_SHORT_5_5_5_1,    2 },
	{ D3DFMT_X1R5G5B5,   2,  0,  false, TRANSFER_XRGB1555,   GL_RGB,              GL_RGB,              GL_UNSIGNED_SHORT_5_6_5,      2 },
	{ D3DFMT_A4R4G4B4,   2,  0,  true,  TRANSFER_ARGB4444,   GL_RGBA,             GL_RGBA,             GL_UNSIGNED_SHORT_4_4_4_4,    2 },
	{ D3DFMT_A8,         1,  0,  true,  TRANSFER_A8,         GL_LUMINANCE_ALPHA,  GL_LUMINANCE_ALPHA,  GL_UNSIGNED_BYTE,             2 },
	{ D3DFMT_L8,         1,  0,  false, TRANSFER_DIRECT,     GL_LUMINANCE,        GL_LUMINANCE,        GL_UNSIGNED_BYTE,             1 },
	{ D3DFMT_A8L8,       2,  0,  true,  TRANSFER_DIRECT,     GL_LUMINANCE_ALPHA,  GL_LUMINANCE_ALPHA,  GL_UNSIGNED_BYTE,             2 },
	{ D3DFMT_A4L4,       1,  0,  true,  TRANSFER_A4L4,       GL_LUMINANCE_ALPHA,  GL_LUMINANCE_ALPHA,  GL_UNSIGNED_BYTE,             2 },
	// DXT2 and DXT4 are DXT3 and DXT5 with premultiplied alpha; the blocks have the same layout.
	{ D3DFMT_DXT1,       0,  8,  true,  TRANSFER_COMPRESSED, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0 },
	{ D3DFMT_DXT2,       0,  16, true,  TRANSFER_COMPRESSED, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0 },
	{ D3DFMT_DXT3,       0,  16, true,  TRANSFER_COMPRESSED, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0 },
	{ D3DFMT_DXT4,       0,  16, true,  TRANSFER_COMPRESSED, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0 },
	{ D3DFMT_DXT5,       0,  16, true,  TRANSFER_COMPRESSED, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0 },

	// Known, but only as system memory surfaces or for the device surfaces; never uploaded.
	{ D3DFMT_P8,         1,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_V8U8,       2,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_L6V5U5,     2,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_X8L8V8U8,   4,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_Q8W8V8U8,   4,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D16,        2,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D16_LOCKABLE, 2, 0, false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D15S1,      2,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D24S8,      4,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D24X8,      4,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D24X4S4,    4,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
	{ D3DFMT_D32,        4,  0,  false, TRANSFER_NONE,       0, 0, 0, 0 },
};

// A1R5G5B5 with mip maps, see GetUploadFormat().
const FormatInfo s_argb1555Wide =
	{ D3DFMT_A1R5G5B5,   2,  0,  true,  TRANSFER_ARGB1555_WIDE, GL_BGRA,          GL_BGRA,             GL_UNSIGNED_BYTE,             4 };

/// Spread a five bit color channel over eight bits, repeating its top bits at the bottom
/// so that full intensity stays full intensity.
uint32_t Expand5(uint32_t value)
{
	return (value << 3) | (value >> 2);
}

} // namespace

//-----------------------------------------------------------------------------
const FormatInfo *GetFormatInfo(D3DFORMAT format)
{
	for (const FormatInfo &info : s_formats)
	{
		if (info.format == format)
			return &info;
	}

	return nullptr;
}

//-----------------------------------------------------------------------------
const FormatInfo &GetUploadFormat(const FormatInfo &info, bool mipMapped)
{
	if (mipMapped && info.format == D3DFMT_A1R5G5B5)
		return s_argb1555Wide;

	return info;
}

//-----------------------------------------------------------------------------
bool IsTextureFormatSupported(D3DFORMAT format)
{
	const FormatInfo *info = GetFormatInfo(format);
	return info != nullptr && info->transfer != TRANSFER_NONE;
}

//-----------------------------------------------------------------------------
bool IsDepthFormatSupported(D3DFORMAT format)
{
	// vitaGL creates the display with a 24 bit depth and 8 bit stencil surface. The engine
	// only asks whether a format can be used with the display, never for its exact layout.
	switch (format)
	{
		case D3DFMT_D16:
		case D3DFMT_D24S8:
		case D3DFMT_D24X8:
		case D3DFMT_D24X4S4:
			return true;

		default:
			return false;
	}
}

//-----------------------------------------------------------------------------
UINT GetPitch(const FormatInfo &info, UINT width)
{
	if (info.blockBytes != 0)
	{
		UINT blocks = (width + 3) / 4;
		if (blocks == 0)
			blocks = 1;
		return blocks * info.blockBytes;
	}

	return width * info.bytesPerPixel;
}

//-----------------------------------------------------------------------------
UINT GetSurfaceSize(const FormatInfo &info, UINT width, UINT height)
{
	if (info.blockBytes != 0)
	{
		UINT rows = (height + 3) / 4;
		if (rows == 0)
			rows = 1;
		return GetPitch(info, width) * rows;
	}

	return GetPitch(info, width) * height;
}

//-----------------------------------------------------------------------------
void ConvertPixels(const FormatInfo &info, const void *source, UINT sourcePitch,
	void *destination, UINT destinationPitch, UINT width, UINT height)
{
	const uint8_t *sourceRow = static_cast<const uint8_t *>(source);
	uint8_t *destinationRow = static_cast<uint8_t *>(destination);

	for (UINT y = 0; y < height; ++y)
	{
		switch (info.transfer)
		{
			case TRANSFER_ARGB1555:
			{
				// ARRRRRGG GGGBBBBB -> RRRRRGGG GGBBBBBA
				const uint16_t *in = reinterpret_cast<const uint16_t *>(sourceRow);
				uint16_t *out = reinterpret_cast<uint16_t *>(destinationRow);
				for (UINT x = 0; x < width; ++x)
					out[x] = static_cast<uint16_t>((in[x] << 1) | (in[x] >> 15));
				break;
			}

			case TRANSFER_ARGB1555_WIDE:
			{
				// ARRRRRGG GGGBBBBB -> the bytes B, G, R, A
				const uint16_t *in = reinterpret_cast<const uint16_t *>(sourceRow);
				uint32_t *out = reinterpret_cast<uint32_t *>(destinationRow);
				for (UINT x = 0; x < width; ++x)
				{
					const uint32_t pixel = in[x];
					out[x] = Expand5(pixel & 0x1F)
						| (Expand5((pixel >> 5) & 0x1F) << 8)
						| (Expand5((pixel >> 10) & 0x1F) << 16)
						| ((pixel & 0x8000) != 0 ? 0xFF000000u : 0u);
				}
				break;
			}

			case TRANSFER_XRGB1555:
			{
				// XRRRRRGG GGGBBBBB -> RRRRRGGG GGGBBBBB; the sixth green bit repeats the first.
				const uint16_t *in = reinterpret_cast<const uint16_t *>(sourceRow);
				uint16_t *out = reinterpret_cast<uint16_t *>(destinationRow);
				for (UINT x = 0; x < width; ++x)
				{
					const uint32_t pixel = in[x];
					const uint32_t green = (pixel >> 5) & 0x1F;
					out[x] = static_cast<uint16_t>(((pixel << 1) & 0xF800) | (((green << 1) | (green >> 4)) << 5) | (pixel & 0x1F));
				}
				break;
			}

			case TRANSFER_ARGB4444:
			{
				// AAAARRRR GGGGBBBB -> RRRRGGGG BBBBAAAA
				const uint16_t *in = reinterpret_cast<const uint16_t *>(sourceRow);
				uint16_t *out = reinterpret_cast<uint16_t *>(destinationRow);
				for (UINT x = 0; x < width; ++x)
					out[x] = static_cast<uint16_t>((in[x] << 4) | (in[x] >> 12));
				break;
			}

			case TRANSFER_XRGB8888:
			{
				// The unused byte is undefined in Direct3D and read as opaque.
				const uint32_t *in = reinterpret_cast<const uint32_t *>(sourceRow);
				uint32_t *out = reinterpret_cast<uint32_t *>(destinationRow);
				for (UINT x = 0; x < width; ++x)
					out[x] = in[x] | 0xFF000000u;
				break;
			}

			case TRANSFER_A8:
			{
				// Direct3D reads the color of an alpha only texture as black; the engine uses
				// these for masks that are modulated with a color, where white is what works.
				for (UINT x = 0; x < width; ++x)
				{
					destinationRow[x * 2] = 0xFF;
					destinationRow[x * 2 + 1] = sourceRow[x];
				}
				break;
			}

			case TRANSFER_A4L4:
			{
				for (UINT x = 0; x < width; ++x)
				{
					const uint8_t luminance = sourceRow[x] & 0x0F;
					const uint8_t alpha = sourceRow[x] >> 4;
					destinationRow[x * 2] = static_cast<uint8_t>((luminance << 4) | luminance);
					destinationRow[x * 2 + 1] = static_cast<uint8_t>((alpha << 4) | alpha);
				}
				break;
			}

			default:
				memcpy(destinationRow, sourceRow, width * info.uploadBytesPerPixel);
				break;
		}

		sourceRow += sourcePitch;
		destinationRow += destinationPitch;
	}
}

//-----------------------------------------------------------------------------
void GetVertexLayout(DWORD fvf, VertexLayout &layout)
{
	memset(&layout, 0, sizeof(layout));
	layout.fvf = fvf;
	layout.normalOffset = -1;
	layout.diffuseOffset = -1;
	layout.specularOffset = -1;

	UINT offset = 0;

	switch (fvf & D3DFVF_POSITION_MASK)
	{
		case D3DFVF_XYZ:
			layout.hasPosition = true;
			offset = 3 * sizeof(float);
			break;

		case D3DFVF_XYZRHW:
			layout.hasPosition = true;
			layout.isTransformed = true;
			offset = 4 * sizeof(float);
			break;

		case D3DFVF_XYZB1:
		case D3DFVF_XYZB2:
		case D3DFVF_XYZB3:
		case D3DFVF_XYZB4:
		case D3DFVF_XYZB5:
		{
			// Position followed by blend weights. Vertex blending is not implemented; the
			// weights are skipped so the remaining components are found where they are.
			const UINT weights = ((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZ) / 2;
			layout.hasPosition = true;
			offset = (3 + weights) * sizeof(float);
			break;
		}

		default:
			break;
	}

	if (fvf & D3DFVF_NORMAL)
	{
		layout.normalOffset = static_cast<int>(offset);
		offset += 3 * sizeof(float);
	}

	if (fvf & D3DFVF_PSIZE)
		offset += sizeof(float);

	if (fvf & D3DFVF_DIFFUSE)
	{
		layout.diffuseOffset = static_cast<int>(offset);
		offset += sizeof(DWORD);
	}

	if (fvf & D3DFVF_SPECULAR)
	{
		layout.specularOffset = static_cast<int>(offset);
		offset += sizeof(DWORD);
	}

	layout.texCoordCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (layout.texCoordCount > VertexLayout::MAX_TEXCOORDS)
		layout.texCoordCount = VertexLayout::MAX_TEXCOORDS;

	for (UINT i = 0; i < layout.texCoordCount; ++i)
	{
		// Two bits per coordinate set: 0 = two floats, 1 = three, 2 = four, 3 = one.
		static const UINT sizes[4] = { 2, 3, 4, 1 };
		const UINT code = (fvf >> (16 + i * 2)) & 0x3;

		layout.texCoordOffset[i] = offset;
		layout.texCoordSize[i] = sizes[code];
		offset += sizes[code] * sizeof(float);
	}

	layout.stride = offset;
}

//-----------------------------------------------------------------------------
UINT GetVertexSize(DWORD fvf)
{
	VertexLayout layout;
	GetVertexLayout(fvf, layout);
	return layout.stride;
}

//-----------------------------------------------------------------------------
void SwapVertexColors(const VertexLayout &layout, void *vertices, UINT count)
{
	if ((layout.diffuseOffset < 0 && layout.specularOffset < 0) || layout.stride == 0)
		return;

	uint8_t *vertex = static_cast<uint8_t *>(vertices);
	for (UINT i = 0; i < count; ++i)
	{
		if (layout.diffuseOffset >= 0)
		{
			uint8_t *color = vertex + layout.diffuseOffset;
			const uint8_t blue = color[0];
			color[0] = color[2];
			color[2] = blue;
		}

		if (layout.specularOffset >= 0)
		{
			uint8_t *color = vertex + layout.specularOffset;
			const uint8_t blue = color[0];
			color[0] = color[2];
			color[2] = blue;
		}

		vertex += layout.stride;
	}
}

} // namespace VitaD3D8
