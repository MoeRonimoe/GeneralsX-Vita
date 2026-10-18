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

// VitaD3D8Resources.h ////////////////////////////////////////////////////////
// Surfaces, textures, vertex buffers and index buffers.
//
// Memory is the scarce resource on the Vita, so the system memory copy Direct3D
// keeps of every managed resource is only kept where the engine reads it back:
//
//   textures        block compressed pixels are released after the upload;
//                   uncompressed pixels are kept because the engine relocks them
//   static buffers  write-only buffers have no copy; Lock() hands out a scratch
//                   block that Unlock() uploads and frees
//   dynamic buffers live in system memory only and are drawn from there, because
//                   vitaGL copies a whole buffer object on every partial update.
//                   A draw call copies the vertices it uses for the thread that
//                   draws, unless the buffer's writer keeps the rules of dynamic
//                   buffers and has said so (VitaD3D8Tags.h)
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "VitaD3D8.h"
#include "VitaD3D8Format.h"

namespace VitaD3D8
{

//-----------------------------------------------------------------------------
/// IDirect3DResource8 members shared by buffers and textures.
template <class Interface>
class Resource : public Unknown<Interface>
{
public:
	Resource() : m_priority(0) {}

	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **ppDevice) override;
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags) override { return D3DERR_INVALIDCALL; }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return D3DERR_NOTFOUND; }
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return D3DERR_NOTFOUND; }
	DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { const DWORD old = m_priority; m_priority = PriorityNew; return old; }
	DWORD STDMETHODCALLTYPE GetPriority() override { return m_priority; }
	void STDMETHODCALLTYPE PreLoad() override {}

private:
	DWORD m_priority;
};

/// AddRef'd interface of the device, for the GetDevice() members.
HRESULT GetDeviceInterface(IDirect3DDevice8 **ppDevice);

/// System memory this layer currently holds for the engine's textures and buffers: the
/// copies Direct3D lets an application lock and read back. Reported in the log's memory lines.
struct ResourceMemory
{
	size_t textureBytes;
	size_t bufferBytes;
};
ResourceMemory GetResourceMemory();

template <class Interface>
HRESULT STDMETHODCALLTYPE Resource<Interface>::GetDevice(IDirect3DDevice8 **ppDevice)
{
	return GetDeviceInterface(ppDevice);
}

//-----------------------------------------------------------------------------
/// A two dimensional texture with its mip chain.
class Texture : public Resource<IDirect3DTexture8>
{
public:
	/// nullptr if the format cannot be used or the memory is not available.
	static Texture *Create(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);

	// IDirect3DResource8
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }

	// IDirect3DBaseTexture8
	DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return 0; }
	DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
	DWORD STDMETHODCALLTYPE GetLevelCount() override { return m_levelCount; }

	// IDirect3DTexture8
	HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
	HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface8 **ppSurfaceLevel) override;
	HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override;
	HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override;
	HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *dirty_rect) override;

	// Implementation
	const FormatInfo &GetFormat() const { return *m_format; }
	UINT GetWidth() const { return m_levels[0].width; }
	UINT GetHeight() const { return m_levels[0].height; }
	bool HasMipMaps() const { return m_levelCount > 1; }

	/// True if the texture was written since it was last uploaded.
	bool IsDirty() const { return m_dirty; }

	/// Upload what changed since the last call and return the GL texture, or 0 if the texture
	/// has never been written. Called by the game's thread before a draw call with the
	/// texture. An upload leaves the texture bound to whichever GL texture unit is active,
	/// and tells the device so.
	GLuint Realize();

	/// Sampler state last applied to the GL texture object. GL keeps the filter and address
	/// modes per texture where Direct3D keeps them per stage.
	///
	/// This belongs to the thread that makes the calls to vitaGL, which is behind the game's
	/// thread: it may have draw calls with this texture before it when the engine lets go of
	/// the texture. So it is a block of its own, and that thread frees it when it gets there.
	struct SamplerState
	{
		GLint minFilter;
		GLint magFilter;
		GLint wrapU;
		GLint wrapV;
	};
	SamplerState *GetAppliedSamplerState() { return m_sampler; }

protected:
	virtual ~Texture() override;

private:
	enum { MAX_LEVELS = 13 };	///< enough for MAX_TEXTURE_SIZE

	struct Level
	{
		UINT width;
		UINT height;
		UINT pitch;
		UINT size;
		uint8_t *data;		///< system memory pixels, nullptr until locked and after release
		bool locked;
	};

	Texture();

	bool EnsureLevelData(UINT level);
	void UploadCompressed();
	void UploadUncompressed();

	/// Hands the pixels of the top level to the render thread without waiting for it.
	/// False if this upload has to be waited for.
	bool ReplacePixelsLater();

	const FormatInfo *m_format;
	D3DFORMAT m_d3dFormat;
	D3DPOOL m_pool;
	DWORD m_usage;
	UINT m_levelCount;
	Level m_levels[MAX_LEVELS];
	Surface *m_surfaces[MAX_LEVELS];	///< the surface of each level, created when first asked for

	GLuint m_glTexture;
	bool m_dirty;			///< system memory differs from the GL texture
	SamplerState *m_sampler;
};

//-----------------------------------------------------------------------------
/// A surface: one level of a texture, a system memory image, or one of the two display
/// surfaces (back buffer, depth/stencil), which exist only as descriptions.
///
/// The surface of a texture level is part of its texture, as in Direct3D: the texture owns
/// it, hands out the same object every time, and references to the surface count as
/// references to the texture. Code written against Direct3D relies on this, for example
/// by using a level's surface after releasing it while it still holds the texture.
class Surface : public Unknown<IDirect3DSurface8>
{
public:
	enum Kind
	{
		KIND_IMAGE,			///< CreateImageSurface: system memory pixels owned by the surface
		KIND_TEXTURE_LEVEL,	///< a level of a texture, owned by that texture
		KIND_BACK_BUFFER,
		KIND_DEPTH_STENCIL
	};

	static Surface *CreateImage(UINT width, UINT height, D3DFORMAT format);
	static Surface *CreateTextureLevel(Texture *texture, UINT level);
	static Surface *CreateDisplaySurface(Kind kind, UINT width, UINT height, D3DFORMAT format);

	/// For the texture that owns a level's surface; everyone else uses Release().
	static void DestroyTextureLevel(Surface *surface) { delete surface; }

	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;

	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **ppDevice) override { return GetDeviceInterface(ppDevice); }
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags) override { return D3DERR_INVALIDCALL; }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return D3DERR_NOTFOUND; }
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return D3DERR_NOTFOUND; }
	HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) override;
	HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override;
	HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override;
	HRESULT STDMETHODCALLTYPE UnlockRect() override;

	Kind GetKind() const { return m_kind; }
	UINT GetWidth() const { return m_width; }
	UINT GetHeight() const { return m_height; }
	D3DFORMAT GetD3DFormat() const { return m_d3dFormat; }

protected:
	virtual ~Surface() override;

private:
	Surface();

	Kind m_kind;
	UINT m_width;
	UINT m_height;
	D3DFORMAT m_d3dFormat;
	const FormatInfo *m_format;

	Texture *m_texture;		///< KIND_TEXTURE_LEVEL
	UINT m_level;

	uint8_t *m_data;		///< KIND_IMAGE
	bool m_locked;
};

//-----------------------------------------------------------------------------
/// Storage shared by vertex and index buffers.
///
/// Every buffer has a copy in system memory, and a lock hands out a pointer into it. The
/// engine locks a whole buffer to change a few of its vertices and counts on the others
/// staying as they were, also in buffers it declared write-only (the terrain does this
/// whenever the view scrolls or a light moves).
///
/// Buffers with D3DUSAGE_DYNAMIC are drawn straight from that copy. The others have a GL
/// buffer object, which receives what a lock changed.
class BufferStorage
{
public:
	BufferStorage(GLenum target);
	~BufferStorage();

	bool Init(UINT length, DWORD usage);

	HRESULT Lock(UINT offset, UINT size, BYTE **data, DWORD flags);

	/// End the lock. True if it was a lock for writing; the locked range is reported then.
	bool Unlock(UINT &offset, UINT &size);

	/// Replace a range of the GL buffer object. source holds the bytes of that range: a
	/// pointer into the system copy, or a converted copy of that part of it.
	void Upload(UINT offset, UINT size, const uint8_t *source);

	UINT GetLength() const { return m_length; }
	DWORD GetUsage() const { return m_usage; }

	/// True if the buffer is drawn from system memory instead of a GL buffer object.
	bool IsClientSide() const { return m_clientSide; }
	bool IsLocked() const { return m_locked; }

	/// For a buffer that is drawn straight from its memory by a thread that is behind the one
	/// that writes it. Makes the system copy a block that nothing written to the stream of
	/// the render thread points into: the block it is, one it was before, a new one, or,
	/// when there are as many as there may be, whichever it is after waiting for the render
	/// thread. What the buffer held is not in the block.
	void TakeUnusedBlock();

	/// Says that something written to the stream points into the system copy.
	void NoteUse();

	/// Called for a draw call from the buffer. True if the buffer has not been locked for
	/// writing since it was drawn from in an earlier frame: what is drawn is what was drawn.
	bool NoteDraw();
	const uint8_t *GetSystemCopy() const { return m_systemCopy; }
	uint8_t *GetSystemCopy() { return m_systemCopy; }
	GLuint GetGLBuffer() const { return m_glBuffer; }

private:
	GLenum m_target;
	UINT m_length;
	DWORD m_usage;
	bool m_clientSide;
	uint8_t *m_systemCopy;
	GLuint m_glBuffer;
	bool m_glAllocated;		///< the GL buffer object has storage

	bool m_locked;
	UINT m_lockOffset;
	UINT m_lockSize;

	// The blocks the system copy has been, see TakeUnusedBlock(). A block that was pointed
	// into is free again when the frame that was being written then has been drawn. The
	// game is at most a frame ahead, so a buffer that starts over once per frame goes round
	// three of them.
	struct Block
	{
		uint8_t *memory;
		uint32_t usedInFrame;
		bool used;
	};
	enum { MAX_BLOCKS = 6 };
	Block m_blocks[MAX_BLOCKS];
	UINT m_blockCount;
	UINT m_currentBlock;

	uint32_t m_writes;				///< locks for writing so far
	uint32_t m_writesWhenDrawn;		///< that number at the last draw call
	uint32_t m_frameWhenDrawn;		///< g_frameNumber at the last draw call
	bool m_unchangedThisFrame;		///< what NoteDraw() found at the first draw call of that frame
};

//-----------------------------------------------------------------------------
class VertexBuffer : public Resource<IDirect3DVertexBuffer8>
{
public:
	static VertexBuffer *Create(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);

	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }

	HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) override;
	HRESULT STDMETHODCALLTYPE Unlock() override;
	HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override;

	/// Takes VITAD3D8_KEEPS_DYNAMIC_BUFFER_RULES, see VitaD3D8Tags.h.
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags) override;

	BufferStorage &GetStorage() { return m_storage; }

	/// Called before a draw with the format the vertices are drawn as. A buffer created
	/// without a format is sent to vitaGL here, on first use.
	void PrepareForDraw(const VertexLayout &layout);

	/// Called for a draw call from a buffer in system memory. True if the call is to take
	/// its vertices from the buffer's memory as it is: that memory stays as it is until the
	/// render thread has made the call, and the colors in it are in the order vitaGL reads
	/// them by then.
	bool DrawFromMemory(const VertexLayout &layout);

protected:
	virtual ~VertexBuffer() override {}

private:
	VertexBuffer();

	/// Make a written range of the system copy what gets drawn, with the vertex colors
	/// in the byte order vitaGL reads them.
	void CommitRange(UINT offset, UINT size);

	// A buffer whose writer keeps the rules of dynamic buffers is drawn from directly: from
	// a lock that discards everything, which gives it a block of memory nothing is drawn
	// from any more, until a lock that does not fit the rules.
	void PrepareLock(UINT offset, DWORD flags);
	void CommitDirect(UINT offset, UINT size);
	void LeaveDirect();

	BufferStorage m_storage;
	DWORD m_fvf;				///< format given at creation, 0 if none
	D3DPOOL m_pool;
	VertexLayout m_colorLayout;	///< layout the vertex colors are reordered with
	bool m_hasColorLayout;

	bool m_keepsRules;			///< the engine said so
	bool m_direct;				///< draw calls take their vertices from the buffer's memory
	UINT m_directEnd;			///< the end of what was written since the buffer was discarded, in bytes
	UINT m_swappedFirst;		///< the vertices whose colors the render thread was told to reorder since then
	UINT m_swappedEnd;
};

//-----------------------------------------------------------------------------
class IndexBuffer : public Resource<IDirect3DIndexBuffer8>
{
public:
	static IndexBuffer *Create(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);

	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }

	HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) override;
	HRESULT STDMETHODCALLTYPE Unlock() override;
	HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) override;

	BufferStorage &GetStorage() { return m_storage; }
	D3DFORMAT GetD3DFormat() const { return m_d3dFormat; }
	UINT GetIndexSize() const { return m_d3dFormat == D3DFMT_INDEX32 ? 4 : 2; }

protected:
	virtual ~IndexBuffer() override {}

private:
	IndexBuffer();

	BufferStorage m_storage;
	D3DFORMAT m_d3dFormat;
	D3DPOOL m_pool;
};

} // namespace VitaD3D8
