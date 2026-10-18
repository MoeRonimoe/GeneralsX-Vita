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

// VitaD3D8Resources.cpp //////////////////////////////////////////////////////
// Surfaces, textures, vertex buffers and index buffers, see VitaD3D8Resources.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaD3D8Resources.h"

#include <VitaD3D8Tags.h>
#include <VitaDiagnostics.h>

#include <psp2/gxm.h>

#include "VitaD3D8Device.h"

// Last: from here on the calls to vitaGL go through the render thread.
#include "VitaD3D8GL.h"

namespace VitaD3D8
{

namespace
{

ResourceMemory s_resourceMemory = { 0, 0 };

/// Counts a buffer upload and the time it takes for the frame statistics.
class BufferUploadTimer
{
public:
	BufferUploadTimer() : m_start(VitaDiagnostics::profileStart()) { ++g_frameStatistics.bufferUploads; }
	~BufferUploadTimer()
	{
		if (m_start != 0)
			g_frameStatistics.bufferUploadTime += VitaDiagnostics::now() - m_start;
	}

private:
	uint64_t m_start;
};

void FreeLevelData(uint8_t *&data, UINT size)
{
	if (data == nullptr)
		return;

	s_resourceMemory.textureBytes -= size;
	free(data);
	data = nullptr;
}

// Scratch block for conversions on the way to vitaGL: texture pixels and vertex colors.
// It grows to what an upload needs and is kept for the next one, unless it became large.
uint8_t *s_uploadScratch = nullptr;
UINT s_uploadScratchSize = 0;

// A scratch block up to this size is kept between uploads.
const UINT UPLOAD_SCRATCH_KEEP = 1024 * 1024;

// New pixels for a texture are copied into the stream of the render thread up to this
// size; an upload of more is waited for. 512 x 512 pixels of four bytes fit.
const UINT MAX_PIXELS_SENT_LATER = 1024 * 1024;

uint8_t *GetUploadScratch(UINT size)
{
	if (size > s_uploadScratchSize)
	{
		free(s_uploadScratch);
		s_uploadScratch = static_cast<uint8_t *>(malloc(size));
		s_uploadScratchSize = s_uploadScratch != nullptr ? size : 0;
	}

	return s_uploadScratch;
}

/// Spread the low 16 bits of a value so that a zero bit follows each of them.
uint32_t SpreadBits(uint32_t value)
{
	value &= 0x0000FFFF;
	value = (value | (value << 8)) & 0x00FF00FF;
	value = (value | (value << 4)) & 0x0F0F0F0F;
	value = (value | (value << 2)) & 0x33333333;
	value = (value | (value << 1)) & 0x55555555;
	return value;
}

/// Copy the blocks of a compressed level from rows into the order the GPU reads them in:
/// the bits of row and column interleaved, the row in the even positions. In a level that
/// is not square this holds for the square part; the remaining bits of the longer side
/// count whole squares. Both sides have to be powers of two.
void CopyBlocksSwizzled(uint8_t *destination, const uint8_t *source, UINT blocksWide, UINT blocksHigh, UINT blockBytes)
{
	const UINT square = blocksWide < blocksHigh ? blocksWide : blocksHigh;
	UINT shift = 0;
	while ((1u << shift) < square)
		++shift;
	const UINT mask = square - 1;

	for (UINT y = 0; y < blocksHigh; ++y)
	{
		const uint32_t rowBits = SpreadBits(y & mask) | ((y >> shift) << (2 * shift));
		for (UINT x = 0; x < blocksWide; ++x)
		{
			const uint32_t index = rowBits | (SpreadBits(x & mask) << 1) | ((x >> shift) << (2 * shift));
			memcpy(destination + index * blockBytes, source, blockBytes);
			source += blockBytes;
		}
	}
}

// Names for textures, taken from vitaGL a handful at a time. Asking for one means waiting
// for the render thread, which a texture that is otherwise sent without waiting should not
// have to do. Used by the game's thread only.
enum { TEXTURE_NAMES_TAKEN = 32 };
GLuint s_textureNames[TEXTURE_NAMES_TAKEN];
UINT s_textureNameCount = 0;

GLuint TakeTextureName()
{
	if (s_textureNameCount == 0)
	{
		glGenTextures(TEXTURE_NAMES_TAKEN, s_textureNames);
		s_textureNameCount = TEXTURE_NAMES_TAKEN;
	}

	return s_textureNames[--s_textureNameCount];
}

/// Give a large scratch block back after the upload that needed it, so that one big
/// texture does not hold megabytes for the rest of the session.
void TrimUploadScratch()
{
	if (s_uploadScratchSize > UPLOAD_SCRATCH_KEEP)
	{
		free(s_uploadScratch);
		s_uploadScratch = nullptr;
		s_uploadScratchSize = 0;
	}
}

/// Offset of a pixel inside a surface. For block compressed formats the position is
/// rounded down to the block that contains it.
UINT GetPixelOffset(const FormatInfo &info, UINT pitch, UINT x, UINT y)
{
	if (info.blockBytes != 0)
		return (y / 4) * pitch + (x / 4) * info.blockBytes;

	return y * pitch + x * info.bytesPerPixel;
}

} // namespace

//-----------------------------------------------------------------------------
ResourceMemory GetResourceMemory()
{
	return s_resourceMemory;
}

///////////////////////////////////////////////////////////////////////////////
// Texture
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
Texture::Texture() :
	m_format(nullptr),
	m_d3dFormat(D3DFMT_UNKNOWN),
	m_pool(D3DPOOL_MANAGED),
	m_usage(0),
	m_levelCount(0),
	m_glTexture(0),
	m_dirty(false),
	m_sampler(static_cast<SamplerState *>(malloc(sizeof(SamplerState))))
{
	memset(m_levels, 0, sizeof(m_levels));
	memset(m_surfaces, 0, sizeof(m_surfaces));

	if (m_sampler != nullptr)
	{
		m_sampler->minFilter = -1;
		m_sampler->magFilter = -1;
		m_sampler->wrapU = -1;
		m_sampler->wrapV = -1;
	}
}

//-----------------------------------------------------------------------------
Texture::~Texture()
{
	if (m_glTexture != 0)
		glDeleteTextures(1, &m_glTexture);

	// The render thread may have draw calls with this texture before it, and those look at
	// the sampler state. It frees the block once it has made them, which is when it gets
	// to this.
	SamplerState *sampler = m_sampler;
	RenderThread::Defer([sampler] { free(sampler); });

	for (UINT i = 0; i < m_levelCount; ++i)
	{
		if (m_surfaces[i] != nullptr)
			Surface::DestroyTextureLevel(m_surfaces[i]);

		FreeLevelData(m_levels[i].data, m_levels[i].size);
	}
}

//-----------------------------------------------------------------------------
Texture *Texture::Create(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
{
	const FormatInfo *info = GetFormatInfo(format);
	if (info == nullptr || width == 0 || height == 0)
		return nullptr;

	// The engine does not check whether its locks succeed, so a texture that can never get
	// its memory must not come into existence.
	if (width > MAX_TEXTURE_SIZE || height > MAX_TEXTURE_SIZE)
		return nullptr;

	// System memory textures are only ever sources for copies and need no GL counterpart,
	// so any known format will do for them.
	if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH && info->transfer == TRANSFER_NONE)
		return nullptr;

	Texture *texture = new Texture;
	if (texture->m_sampler == nullptr)
	{
		texture->Release();
		return nullptr;
	}

	texture->m_format = info;
	texture->m_d3dFormat = format;
	texture->m_pool = pool;
	texture->m_usage = usage;

	// Zero levels asks for the whole chain down to 1 x 1.
	UINT levelWidth = width;
	UINT levelHeight = height;
	UINT count = 0;
	while (count < MAX_LEVELS)
	{
		Level &level = texture->m_levels[count];
		level.width = levelWidth;
		level.height = levelHeight;
		level.pitch = GetPitch(*info, levelWidth);
		level.size = GetSurfaceSize(*info, levelWidth, levelHeight);
		++count;

		if ((levels != 0 && count == levels) || (levelWidth == 1 && levelHeight == 1))
			break;

		levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
		levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
	}
	texture->m_levelCount = count;

	return texture;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
{
	if (Level >= m_levelCount || pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	pDesc->Format = m_d3dFormat;
	pDesc->Type = D3DRTYPE_SURFACE;
	pDesc->Usage = m_usage;
	pDesc->Pool = m_pool;
	pDesc->Size = m_levels[Level].size;
	pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
	pDesc->Width = m_levels[Level].width;
	pDesc->Height = m_levels[Level].height;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface8 **ppSurfaceLevel)
{
	if (ppSurfaceLevel == nullptr)
		return D3DERR_INVALIDCALL;

	*ppSurfaceLevel = nullptr;
	if (Level >= m_levelCount)
		return D3DERR_INVALIDCALL;

	if (m_surfaces[Level] == nullptr)
		m_surfaces[Level] = Surface::CreateTextureLevel(this, Level);

	m_surfaces[Level]->AddRef();
	*ppSurfaceLevel = m_surfaces[Level];
	return D3D_OK;
}

//-----------------------------------------------------------------------------
bool Texture::EnsureLevelData(UINT level)
{
	Level &target = m_levels[level];
	if (target.data != nullptr)
		return true;

	// The pixels of an uploaded compressed texture are gone. A lock that expects to read
	// them back gets black; say so once instead of failing silently.
	if (m_glTexture != 0 && m_format->transfer == TRANSFER_COMPRESSED)
		LogUnsupportedOnce("a compressed texture was locked again after its system memory copy was released");

	target.data = static_cast<uint8_t *>(calloc(1, target.size));
	if (target.data == nullptr)
		return false;

	s_resourceMemory.textureBytes += target.size;
	return true;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Texture::LockRect(UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
	if (level >= m_levelCount || locked_rect == nullptr)
		return D3DERR_INVALIDCALL;

	Level &target = m_levels[level];
	if (target.locked)
		return D3DERR_INVALIDCALL;

	if (!EnsureLevelData(level))
	{
		Log("LockRect failed: no memory for level %u (%ux%u, %u bytes) of a texture",
			static_cast<unsigned>(level), static_cast<unsigned>(target.width), static_cast<unsigned>(target.height),
			static_cast<unsigned>(target.size));
		return D3DERR_OUTOFVIDEOMEMORY;
	}

	UINT x = 0;
	UINT y = 0;
	if (rect != nullptr)
	{
		x = static_cast<UINT>(rect->left);
		y = static_cast<UINT>(rect->top);
	}

	locked_rect->Pitch = static_cast<INT>(target.pitch);
	locked_rect->pBits = target.data + GetPixelOffset(*m_format, target.pitch, x, y);
	target.locked = true;

	if ((flags & D3DLOCK_READONLY) == 0)
	{
		m_dirty = true;
		++g_textureWrites;
	}

	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Texture::UnlockRect(UINT Level)
{
	if (Level >= m_levelCount || !m_levels[Level].locked)
		return D3DERR_INVALIDCALL;

	m_levels[Level].locked = false;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Texture::AddDirtyRect(const RECT *dirty_rect)
{
	m_dirty = true;
	++g_textureWrites;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
GLuint Texture::Realize()
{
	if (m_dirty && m_levels[0].data != nullptr && !ReplacePixelsLater())
	{
		// For the log: which uploads are still waited for.
		FrameStatistics &statistics = g_frameStatistics;
		++statistics.textureUploadsWaited;
		if (m_levels[0].width * m_levels[0].height >= statistics.waitedUploadWidth * statistics.waitedUploadHeight)
		{
			statistics.waitedUploadWidth = m_levels[0].width;
			statistics.waitedUploadHeight = m_levels[0].height;
			statistics.waitedUploadFormat = static_cast<uint32_t>(m_d3dFormat);
			statistics.waitedUploadWasFirst = m_glTexture == 0;
		}

		// An upload reads the pixels where the engine left them, asks vitaGL where it put
		// the texture and writes there. With a render thread, that thread does all of it
		// while this one waits.
		RenderThread::Sync([this]
		{
			const uint64_t start = VitaDiagnostics::profileStart();

			if (m_format->transfer == TRANSFER_COMPRESSED)
				UploadCompressed();
			else if (m_format->transfer != TRANSFER_NONE)
				UploadUncompressed();

			++g_frameStatistics.textureUploads;
			if (start != 0)
				g_frameStatistics.textureUploadTime += VitaDiagnostics::now() - start;

			// The texture is now bound to the texture unit that happened to be active, in
			// place of the one the device had put there.
			Device *device = VitaD3D8::GetDevice();
			if (device != nullptr)
				device->NoteTextureUpload();
		});
	}

	m_dirty = false;
	return m_glTexture;
}

//-----------------------------------------------------------------------------
/// The textures the game writes to again and again are small ones: the shroud, the radar,
/// text. About three of them change per frame. Waiting for each upload meant waiting until
/// the render thread had made every call written before it, a millisecond at a time. So
/// the new pixels of a texture vitaGL already has are copied into the stream.
bool Texture::ReplacePixelsLater()
{
	// A compressed texture gets a new name with every upload, and asks vitaGL where it put
	// the pixels. Those are waited for.
	if (m_format->transfer == TRANSFER_COMPRESSED || m_format->transfer == TRANSFER_NONE)
		return false;

	Device *device = VitaD3D8::GetDevice();
	if (device == nullptr)
		return false;

	const Level &level = m_levels[0];
	const FormatInfo &upload = GetUploadFormat(*m_format, m_levelCount > 1);
	const UINT uploadPitch = level.width * upload.uploadBytesPerPixel;
	const UINT size = uploadPitch * level.height;
	if (size > MAX_PIXELS_SENT_LATER)
		return false;

	// A texture vitaGL does not have yet needs a name. The game makes two new textures per
	// frame for text that changed; waiting for the name each time was most of what the
	// game's thread still waited for.
	if (m_glTexture == 0)
		m_glTexture = TakeTextureName();

	const uint64_t start = VitaDiagnostics::profileStart();

	const void *pixels = level.data;
	if (upload.transfer != TRANSFER_DIRECT)
	{
		uint8_t *scratch = GetUploadScratch(size);
		if (scratch == nullptr)
			return false;

		ConvertPixels(upload, level.data, level.pitch, scratch, uploadPitch, level.width, level.height);
		pixels = scratch;
	}

	// The engine may write to the texture again before the render thread gets to this.
	pixels = RenderThread::Keep(pixels, size);

	const GLuint glTexture = m_glTexture;
	const GLint internalFormat = static_cast<GLint>(upload.glInternalFormat);
	const GLsizei width = static_cast<GLsizei>(level.width);
	const GLsizei height = static_cast<GLsizei>(level.height);
	const GLenum format = upload.glFormat;
	const GLenum type = upload.glType;
	const bool mipMapped = m_levelCount > 1;
	RenderThread::Defer([device, glTexture, internalFormat, width, height, format, type, pixels, mipMapped]
	{
		device->ReplaceTexturePixels(glTexture, internalFormat, width, height, format, type, pixels, mipMapped);
	});

	++g_frameStatistics.textureUploads;
	if (start != 0)
		g_frameStatistics.textureUploadTime += VitaDiagnostics::now() - start;
	return true;
}

//-----------------------------------------------------------------------------
void Texture::UploadCompressed()
{
	// vitaGL lays a compressed mip chain out as it receives the levels in ascending order
	// and cannot replace one in place, so a changed texture is uploaded as a new one.
	if (m_glTexture != 0)
		glDeleteTextures(1, &m_glTexture);

	glGenTextures(1, &m_glTexture);
	glBindTexture(GL_TEXTURE_2D, m_glTexture);
	m_sampler->minFilter = -1;
	m_sampler->magFilter = -1;
	m_sampler->wrapU = -1;
	m_sampler->wrapV = -1;

	// Switch for an experiment on the console (see "+NAME=VALUE" in VitaMain.cpp): hand
	// vitaGL only the top level, to tell wrong mip levels from wrong textures.
	static const bool topLevelOnly = getenv("VITA_D3D8_TOP_MIP_ONLY") != nullptr;

	UINT levels = 0;
	while (levels < m_levelCount && m_levels[levels].data != nullptr && !(topLevelOnly && levels > 0))
		++levels;

	// vitaGL has the GPU reorder the blocks of a level it is given. That happens some time
	// after the call, lands in freed memory when the next level makes the texture's block
	// move, and on the console left small levels (the far end of the mip chain, and whole
	// small textures) black or filled with noise. So vitaGL only gets to reserve the
	// memory, and the blocks are put in the GPU's order here.
	bool powerOfTwo = levels > 0;
	for (UINT i = 0; i < levels; ++i)
	{
		const UINT blocksWide = (m_levels[i].width + 3) / 4;
		const UINT blocksHigh = (m_levels[i].height + 3) / 4;
		if ((blocksWide & (blocksWide - 1)) != 0 || (blocksHigh & (blocksHigh - 1)) != 0)
			powerOfTwo = false;
	}

	if (!powerOfTwo)
	{
		// The reordering below is defined for power of two sizes, which is all the game
		// ships. Anything else goes through vitaGL, one level at a time; waiting for the
		// GPU's copy keeps it from landing in memory the next level has made vitaGL free.
		for (UINT i = 0; i < levels; ++i)
		{
			const Level &level = m_levels[i];
			if (i > 0 && (level.width < 4 || level.height < 4))
				break;

			glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i), m_format->glInternalFormat,
				static_cast<GLsizei>(level.width), static_cast<GLsizei>(level.height), 0,
				static_cast<GLsizei>(level.size), level.data);
			sceGxmTransferFinish();
		}
	}
	else
	{
		for (UINT i = 0; i < levels; ++i)
		{
			const Level &level = m_levels[i];
			glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i), m_format->glInternalFormat,
				static_cast<GLsizei>(level.width), static_cast<GLsizei>(level.height), 0,
				static_cast<GLsizei>(level.size), nullptr);
		}

		// The levels follow each other in the texture's memory, each as large as Direct3D
		// makes it. The address is only final once the last level has been reserved.
		uint8_t *destination = static_cast<uint8_t *>(vglGetTexDataPointer(GL_TEXTURE_2D));
		for (UINT i = 0; i < levels && destination != nullptr; ++i)
		{
			const Level &level = m_levels[i];
			CopyBlocksSwizzled(destination, level.data, (level.width + 3) / 4, (level.height + 3) / 4, m_format->blockBytes);
			destination += level.size;
		}
	}

	// The engine loads compressed textures once and never reads them back.
	if ((m_usage & D3DUSAGE_DYNAMIC) == 0)
	{
		for (UINT i = 0; i < m_levelCount; ++i)
			FreeLevelData(m_levels[i].data, m_levels[i].size);
	}
}

//-----------------------------------------------------------------------------
void Texture::UploadUncompressed()
{
	if (m_glTexture == 0)
		glGenTextures(1, &m_glTexture);

	glBindTexture(GL_TEXTURE_2D, m_glTexture);

	const Level &level = m_levels[0];
	const void *pixels = level.data;
	const FormatInfo &upload = GetUploadFormat(*m_format, m_levelCount > 1);

	if (upload.transfer != TRANSFER_DIRECT)
	{
		const UINT uploadPitch = level.width * upload.uploadBytesPerPixel;
		uint8_t *scratch = GetUploadScratch(uploadPitch * level.height);
		if (scratch == nullptr)
			return;

		ConvertPixels(upload, level.data, level.pitch, scratch, uploadPitch, level.width, level.height);
		pixels = scratch;
	}

	glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(upload.glInternalFormat),
		static_cast<GLsizei>(level.width), static_cast<GLsizei>(level.height), 0,
		upload.glFormat, upload.glType, pixels);

	// vitaGL builds the lower levels of uncompressed textures itself and ignores level data
	// handed to it, so the chain is generated from the top level.
	if (m_levelCount > 1)
		glGenerateMipmap(GL_TEXTURE_2D);

	TrimUploadScratch();
}

///////////////////////////////////////////////////////////////////////////////
// Surface
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
Surface::Surface() :
	m_kind(KIND_IMAGE),
	m_width(0),
	m_height(0),
	m_d3dFormat(D3DFMT_UNKNOWN),
	m_format(nullptr),
	m_texture(nullptr),
	m_level(0),
	m_data(nullptr),
	m_locked(false)
{
}

//-----------------------------------------------------------------------------
Surface::~Surface()
{
	free(m_data);
}

//-----------------------------------------------------------------------------
ULONG STDMETHODCALLTYPE Surface::AddRef()
{
	if (m_kind == KIND_TEXTURE_LEVEL)
		return m_texture->AddRef();

	return Unknown<IDirect3DSurface8>::AddRef();
}

//-----------------------------------------------------------------------------
ULONG STDMETHODCALLTYPE Surface::Release()
{
	// The last reference to the texture takes this surface with it.
	if (m_kind == KIND_TEXTURE_LEVEL)
		return m_texture->Release();

	return Unknown<IDirect3DSurface8>::Release();
}

//-----------------------------------------------------------------------------
Surface *Surface::CreateImage(UINT width, UINT height, D3DFORMAT format)
{
	const FormatInfo *info = GetFormatInfo(format);
	if (info == nullptr || width == 0 || height == 0)
		return nullptr;

	uint8_t *data = static_cast<uint8_t *>(calloc(1, GetSurfaceSize(*info, width, height)));
	if (data == nullptr)
		return nullptr;

	Surface *surface = new Surface;
	surface->m_kind = KIND_IMAGE;
	surface->m_width = width;
	surface->m_height = height;
	surface->m_d3dFormat = format;
	surface->m_format = info;
	surface->m_data = data;
	return surface;
}

//-----------------------------------------------------------------------------
Surface *Surface::CreateTextureLevel(Texture *texture, UINT level)
{
	D3DSURFACE_DESC desc;
	texture->GetLevelDesc(level, &desc);

	Surface *surface = new Surface;
	surface->m_kind = KIND_TEXTURE_LEVEL;
	surface->m_width = desc.Width;
	surface->m_height = desc.Height;
	surface->m_d3dFormat = desc.Format;
	surface->m_format = &texture->GetFormat();
	surface->m_texture = texture;
	surface->m_level = level;
	return surface;
}

//-----------------------------------------------------------------------------
Surface *Surface::CreateDisplaySurface(Kind kind, UINT width, UINT height, D3DFORMAT format)
{
	Surface *surface = new Surface;
	surface->m_kind = kind;
	surface->m_width = width;
	surface->m_height = height;
	surface->m_d3dFormat = format;
	surface->m_format = GetFormatInfo(format);
	return surface;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Surface::GetContainer(REFIID riid, void **ppContainer)
{
	if (ppContainer == nullptr)
		return D3DERR_INVALIDCALL;

	if (m_texture == nullptr)
	{
		*ppContainer = nullptr;
		return D3DERR_INVALIDCALL;
	}

	m_texture->AddRef();
	*ppContainer = static_cast<IDirect3DTexture8 *>(m_texture);
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Surface::GetDesc(D3DSURFACE_DESC *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	if (m_kind == KIND_TEXTURE_LEVEL)
		return m_texture->GetLevelDesc(m_level, pDesc);

	pDesc->Format = m_d3dFormat;
	pDesc->Type = D3DRTYPE_SURFACE;
	pDesc->Usage = 0;
	if (m_kind == KIND_BACK_BUFFER)
		pDesc->Usage = D3DUSAGE_RENDERTARGET;
	else if (m_kind == KIND_DEPTH_STENCIL)
		pDesc->Usage = D3DUSAGE_DEPTHSTENCIL;
	pDesc->Pool = m_kind == KIND_IMAGE ? D3DPOOL_SYSTEMMEM : D3DPOOL_DEFAULT;
	pDesc->Size = m_format != nullptr ? GetSurfaceSize(*m_format, m_width, m_height) : 0;
	pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
	pDesc->Width = m_width;
	pDesc->Height = m_height;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Surface::LockRect(D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
	if (locked_rect == nullptr)
		return D3DERR_INVALIDCALL;

	switch (m_kind)
	{
		case KIND_TEXTURE_LEVEL:
			return m_texture->LockRect(m_level, locked_rect, rect, flags);

		case KIND_IMAGE:
		{
			if (m_locked)
				return D3DERR_INVALIDCALL;

			UINT x = 0;
			UINT y = 0;
			if (rect != nullptr)
			{
				x = static_cast<UINT>(rect->left);
				y = static_cast<UINT>(rect->top);
			}

			const UINT pitch = GetPitch(*m_format, m_width);
			locked_rect->Pitch = static_cast<INT>(pitch);
			locked_rect->pBits = m_data + GetPixelOffset(*m_format, pitch, x, y);
			m_locked = true;
			return D3D_OK;
		}

		default:
			// The display surfaces live in GPU memory managed by vitaGL.
			LogUnsupportedOnce("LockRect on the back buffer or depth buffer");
			locked_rect->Pitch = 0;
			locked_rect->pBits = nullptr;
			return D3DERR_INVALIDCALL;
	}
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Surface::UnlockRect()
{
	switch (m_kind)
	{
		case KIND_TEXTURE_LEVEL:
			return m_texture->UnlockRect(m_level);

		case KIND_IMAGE:
			if (!m_locked)
				return D3DERR_INVALIDCALL;
			m_locked = false;
			return D3D_OK;

		default:
			return D3DERR_INVALIDCALL;
	}
}

///////////////////////////////////////////////////////////////////////////////
// BufferStorage
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
BufferStorage::BufferStorage(GLenum target) :
	m_target(target),
	m_length(0),
	m_usage(0),
	m_clientSide(false),
	m_systemCopy(nullptr),
	m_glBuffer(0),
	m_glAllocated(false),
	m_locked(false),
	m_lockOffset(0),
	m_lockSize(0),
	m_blockCount(0),
	m_currentBlock(0),
	m_writes(0),
	m_writesWhenDrawn(0),
	m_frameWhenDrawn(0xFFFFFFFFu),
	m_unchangedThisFrame(false)
{
}

//-----------------------------------------------------------------------------
BufferStorage::~BufferStorage()
{
	if (m_glBuffer != 0)
		glDeleteBuffers(1, &m_glBuffer);

	s_resourceMemory.bufferBytes -= m_length * m_blockCount;

	// What was written to the stream may still point into a block.
	for (UINT i = 0; i < m_blockCount; ++i)
	{
		uint8_t *memory = m_blocks[i].memory;
		if (m_blocks[i].used)
			RenderThread::Defer([memory] { free(memory); });
		else
			free(memory);
	}
}

//-----------------------------------------------------------------------------
bool BufferStorage::Init(UINT length, DWORD usage)
{
	m_length = length;
	m_usage = usage;
	m_clientSide = (usage & D3DUSAGE_DYNAMIC) != 0;

	m_systemCopy = static_cast<uint8_t *>(calloc(1, length));
	if (m_systemCopy == nullptr)
		return false;

	s_resourceMemory.bufferBytes += length;
	m_blocks[0].memory = m_systemCopy;
	m_blocks[0].usedInFrame = 0;
	m_blocks[0].used = false;
	m_blockCount = 1;
	m_currentBlock = 0;

	if (!m_clientSide)
		glGenBuffers(1, &m_glBuffer);

	return true;
}

//-----------------------------------------------------------------------------
void BufferStorage::TakeUnusedBlock()
{
	// The block it is, if that will do, then the others.
	for (UINT tried = 0; tried < m_blockCount; ++tried)
	{
		const UINT index = (m_currentBlock + tried) % m_blockCount;
		Block &block = m_blocks[index];
		if (block.used && !RenderThread::IsFrameDone(block.usedInFrame))
			continue;

		block.used = false;
		m_currentBlock = index;
		m_systemCopy = block.memory;
		return;
	}

	if (m_blockCount < MAX_BLOCKS)
	{
		uint8_t *memory = static_cast<uint8_t *>(malloc(m_length));
		if (memory != nullptr)
		{
			s_resourceMemory.bufferBytes += m_length;
			m_currentBlock = m_blockCount++;
			m_blocks[m_currentBlock].memory = memory;
			m_blocks[m_currentBlock].usedInFrame = 0;
			m_blocks[m_currentBlock].used = false;
			m_systemCopy = memory;
			return;
		}
	}

	// As many blocks as there may be, or no memory for another, and all are waiting to be
	// drawn from. After this none is.
	RenderThread::Sync([] {});
	for (UINT i = 0; i < m_blockCount; ++i)
		m_blocks[i].used = false;
}

//-----------------------------------------------------------------------------
void BufferStorage::NoteUse()
{
	Block &block = m_blocks[m_currentBlock];
	block.used = true;
	block.usedInFrame = RenderThread::FrameBeingWritten();
}

//-----------------------------------------------------------------------------
HRESULT BufferStorage::Lock(UINT offset, UINT size, BYTE **data, DWORD flags)
{
	if (data == nullptr || m_locked)
		return D3DERR_INVALIDCALL;

	// A size of zero locks everything from the offset on.
	if (size == 0)
		size = offset < m_length ? m_length - offset : 0;

	if (size == 0 || offset + size > m_length)
		return D3DERR_INVALIDCALL;

	*data = m_systemCopy + offset;

	m_locked = true;
	m_lockOffset = offset;
	// A read-only lock changes nothing and is not committed.
	m_lockSize = (flags & D3DLOCK_READONLY) != 0 ? 0 : size;
	if (m_lockSize != 0)
		++m_writes;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
bool BufferStorage::NoteDraw()
{
	if (m_frameWhenDrawn != g_frameNumber)
	{
		m_unchangedThisFrame = m_frameWhenDrawn != 0xFFFFFFFFu && m_writes == m_writesWhenDrawn;
		m_frameWhenDrawn = g_frameNumber;
	}

	m_writesWhenDrawn = m_writes;
	return m_unchangedThisFrame;
}

//-----------------------------------------------------------------------------
bool BufferStorage::Unlock(UINT &offset, UINT &size)
{
	if (!m_locked)
		return false;

	m_locked = false;
	offset = m_lockOffset;
	size = m_lockSize;
	return size != 0;
}

//-----------------------------------------------------------------------------
void BufferStorage::Upload(UINT offset, UINT size, const uint8_t *source)
{
	if (m_clientSide || size == 0)
		return;

	const BufferUploadTimer timer;
	glBindBuffer(m_target, m_glBuffer);

	if (offset == 0 && size == m_length)
	{
		glBufferData(m_target, static_cast<GLsizei>(m_length), source, GL_STATIC_DRAW);
		m_glAllocated = true;
		return;
	}

	if (!m_glAllocated)
	{
		// The first write is partial: give the buffer object its full storage first. Nothing
		// outside the range has been written yet, so the system copy will do for it.
		glBufferData(m_target, static_cast<GLsizei>(m_length), m_systemCopy, GL_STATIC_DRAW);
		m_glAllocated = true;
	}

	glBufferSubData(m_target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), source);
}

///////////////////////////////////////////////////////////////////////////////
// VertexBuffer
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
VertexBuffer::VertexBuffer() :
	m_storage(GL_ARRAY_BUFFER),
	m_fvf(0),
	m_pool(D3DPOOL_DEFAULT),
	m_hasColorLayout(false),
	m_keepsRules(false),
	m_direct(false),
	m_directEnd(0),
	m_swappedFirst(0),
	m_swappedEnd(0)
{
	memset(&m_colorLayout, 0, sizeof(m_colorLayout));
}

//-----------------------------------------------------------------------------
VertexBuffer *VertexBuffer::Create(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
{
	if (length == 0)
		return nullptr;

	VertexBuffer *buffer = new VertexBuffer;
	buffer->m_fvf = fvf;
	buffer->m_pool = pool;

	// Without a format the layout of the vertices is only known once the buffer is drawn.
	if (fvf != 0)
	{
		GetVertexLayout(fvf, buffer->m_colorLayout);
		buffer->m_hasColorLayout = true;
	}

	if (!buffer->m_storage.Init(length, usage))
	{
		buffer->Release();
		return nullptr;
	}

	return buffer;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VertexBuffer::SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags)
{
	if (memcmp(&refguid, &VITAD3D8_KEEPS_DYNAMIC_BUFFER_RULES, sizeof(GUID)) != 0 || data == nullptr || data_size != sizeof(DWORD))
		return Resource<IDirect3DVertexBuffer8>::SetPrivateData(refguid, data, data_size, flags);

	DWORD keepsRules = 0;
	memcpy(&keepsRules, data, sizeof(keepsRules));
	if (keepsRules == 0 && m_direct)
		LeaveDirect();

	m_keepsRules = keepsRules != 0;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags)
{
	if ((m_keepsRules || m_direct) && !m_storage.IsLocked())
		PrepareLock(OffsetToLock, Flags);

	return m_storage.Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VertexBuffer::Unlock()
{
	UINT offset = 0;
	UINT size = 0;

	// Until the layout is known nothing can be sent; PrepareForDraw() sends everything then.
	if (m_storage.Unlock(offset, size) && m_hasColorLayout)
	{
		if (m_direct)
			CommitDirect(offset, size);
		else
			CommitRange(offset, size);
	}

	return D3D_OK;
}

//-----------------------------------------------------------------------------
/// Before a lock of a buffer whose writer keeps the rules of dynamic buffers, or that is
/// being drawn from directly.
void VertexBuffer::PrepareLock(UINT offset, DWORD flags)
{
	const bool writes = (flags & D3DLOCK_READONLY) == 0;

	if (writes && (flags & D3DLOCK_DISCARD) != 0)
	{
		// What the buffer held is given up. Draw calls that have not been made yet may point
		// into its memory, so it goes on in memory that none points into, and what is
		// written there can be drawn from where it is in turn. That takes a render thread to
		// be behind, and knowing where the colors are.
		const bool direct = m_keepsRules && m_hasColorLayout && m_storage.IsClientSide() && RenderThread::g_queueing;
		if (direct)
			m_storage.TakeUnusedBlock();
		else if (m_direct)
			LeaveDirect();

		m_direct = direct;
		m_directEnd = 0;
		m_swappedFirst = 0;
		m_swappedEnd = 0;
		return;
	}

	if (!m_direct)
		return;

	if (writes && (flags & D3DLOCK_NOOVERWRITE) != 0)
	{
		// The promise is kept by writing behind what was written. The engine also locks the
		// same vertices a second time, for the skins that did not fit the first time: then
		// the draw calls for the first lot have to be made before it writes.
		if (offset < m_directEnd)
			RenderThread::Sync([] {});
		return;
	}

	// A lock to read, or one that promises nothing.
	LeaveDirect();
}

//-----------------------------------------------------------------------------
/// After a lock for writing of a buffer that is drawn from directly.
void VertexBuffer::CommitDirect(UINT offset, UINT size)
{
	if (offset + size > m_directEnd)
		m_directEnd = offset + size;

	const UINT stride = m_colorLayout.stride;
	const int diffuseOffset = m_colorLayout.diffuseOffset;
	const int specularOffset = m_colorLayout.specularOffset;
	if (stride == 0 || (diffuseOffset < 0 && specularOffset < 0))
		return;

	const UINT firstVertex = offset / stride;
	UINT endVertex = (offset + size + stride - 1) / stride;
	if (endVertex > m_storage.GetLength() / stride)
		endVertex = m_storage.GetLength() / stride;
	if (endVertex <= firstVertex)
		return;

	if (m_swappedEnd == m_swappedFirst)
	{
		m_swappedFirst = firstVertex;
		m_swappedEnd = endVertex;
	}
	else
	{
		if (firstVertex < m_swappedFirst)
			m_swappedFirst = firstVertex;
		if (endVertex > m_swappedEnd)
			m_swappedEnd = endVertex;
	}

	// The colors are put in vitaGL's order where they are, once for what was locked, by the
	// render thread: it gets to this before it gets to the draw calls that follow, and the
	// game's thread does not touch these vertices again before the buffer is discarded.
	uint8_t *vertices = m_storage.GetSystemCopy() + firstVertex * stride;
	const UINT count = endVertex - firstVertex;
	m_storage.NoteUse();
	RenderThread::Defer([vertices, count, stride, diffuseOffset, specularOffset]
	{
		VertexLayout layout = {};
		layout.stride = stride;
		layout.diffuseOffset = diffuseOffset;
		layout.specularOffset = specularOffset;
		SwapVertexColors(layout, vertices, count);
	});
}

//-----------------------------------------------------------------------------
/// Back to draw calls that copy what they draw, with the buffer as Direct3D has it.
void VertexBuffer::LeaveDirect()
{
	// The render thread first makes every draw call that was written, and reorders every
	// color it was told to.
	RenderThread::Sync([] {});

	if (m_swappedEnd > m_swappedFirst)
	{
		SwapVertexColors(m_colorLayout, m_storage.GetSystemCopy() + m_swappedFirst * m_colorLayout.stride,
			m_swappedEnd - m_swappedFirst);
	}

	m_direct = false;
	m_swappedFirst = 0;
	m_swappedEnd = 0;
}

//-----------------------------------------------------------------------------
bool VertexBuffer::DrawFromMemory(const VertexLayout &layout)
{
	if (!m_direct)
		return false;

	// The colors were reordered where the buffer's own format has them.
	if (layout.stride != m_colorLayout.stride || layout.diffuseOffset != m_colorLayout.diffuseOffset
		|| layout.specularOffset != m_colorLayout.specularOffset)
		LogUnsupportedOnce("a dynamic vertex buffer that is drawn with another vertex format than it was created with has the wrong colors");

	m_storage.NoteUse();
	return true;
}

//-----------------------------------------------------------------------------
void VertexBuffer::CommitRange(UINT offset, UINT size)
{
	// A dynamic buffer is drawn from its system copy. Device::Draw*() copy what a draw call
	// uses, and the colors are put in vitaGL's order in that copy, by the render thread. They
	// used to be reordered here, in the buffer: for everything that was locked, which for the
	// tread marks on the ground is 20000 vertices on every frame for the few that are drawn.
	if (m_storage.IsClientSide())
		return;

	uint8_t *vertices = m_storage.GetSystemCopy();
	const UINT stride = m_colorLayout.stride;
	const bool hasColors = stride != 0 && (m_colorLayout.diffuseOffset >= 0 || m_colorLayout.specularOffset >= 0);

	if (!hasColors)
	{
		m_storage.Upload(offset, size, vertices + offset);
		return;
	}

	// The system copy keeps the colors as Direct3D orders them, because the engine changes
	// single vertices of a locked buffer and leaves the others alone. The GL buffer object
	// gets a reordered copy of the vertices the range touches.
	const UINT firstVertex = offset / stride;
	UINT endVertex = (offset + size + stride - 1) / stride;
	if (endVertex > m_storage.GetLength() / stride)
		endVertex = m_storage.GetLength() / stride;
	if (endVertex <= firstVertex)
		return;

	const UINT byteOffset = firstVertex * stride;
	const UINT byteSize = (endVertex - firstVertex) * stride;

	uint8_t *scratch = GetUploadScratch(byteSize);
	if (scratch == nullptr)
		return;

	memcpy(scratch, vertices + byteOffset, byteSize);
	SwapVertexColors(m_colorLayout, scratch, endVertex - firstVertex);
	m_storage.Upload(byteOffset, byteSize, scratch);
	TrimUploadScratch();
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	pDesc->Format = D3DFMT_VERTEXDATA;
	pDesc->Type = D3DRTYPE_VERTEXBUFFER;
	pDesc->Usage = m_storage.GetUsage();
	pDesc->Pool = m_pool;
	pDesc->Size = m_storage.GetLength();
	pDesc->FVF = m_fvf;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
void VertexBuffer::PrepareForDraw(const VertexLayout &layout)
{
	if (m_hasColorLayout || layout.stride == 0)
		return;

	// First draw of a buffer created without a format: nothing written so far has been
	// reordered or sent yet.
	m_colorLayout = layout;
	m_hasColorLayout = true;
	CommitRange(0, m_storage.GetLength());
}

///////////////////////////////////////////////////////////////////////////////
// IndexBuffer
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
IndexBuffer::IndexBuffer() :
	m_storage(GL_ELEMENT_ARRAY_BUFFER),
	m_d3dFormat(D3DFMT_INDEX16),
	m_pool(D3DPOOL_DEFAULT)
{
}

//-----------------------------------------------------------------------------
IndexBuffer *IndexBuffer::Create(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
{
	if (length == 0 || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32))
		return nullptr;

	IndexBuffer *buffer = new IndexBuffer;
	buffer->m_d3dFormat = format;
	buffer->m_pool = pool;

	if (!buffer->m_storage.Init(length, usage))
	{
		buffer->Release();
		return nullptr;
	}

	return buffer;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags)
{
	return m_storage.Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE IndexBuffer::Unlock()
{
	UINT offset = 0;
	UINT size = 0;

	if (m_storage.Unlock(offset, size))
		m_storage.Upload(offset, size, m_storage.GetSystemCopy() + offset);

	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	pDesc->Format = m_d3dFormat;
	pDesc->Type = D3DRTYPE_INDEXBUFFER;
	pDesc->Usage = m_storage.GetUsage();
	pDesc->Pool = m_pool;
	pDesc->Size = m_storage.GetLength();
	return D3D_OK;
}

} // namespace VitaD3D8
