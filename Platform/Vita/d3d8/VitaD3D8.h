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

// VitaD3D8.h /////////////////////////////////////////////////////////////////
// Shared declarations of the Direct3D 8 implementation for the PS Vita.
//
// The engine keeps talking to IDirect3D8 / IDirect3DDevice8 exactly as it does
// on Windows. This library implements those interfaces on top of vitaGL's
// fixed function pipeline, the way DXVK implements them on Vulkan for the
// Linux and macOS ports. Only what the engine uses is implemented; everything
// else fails with D3DERR_INVALIDCALL and a line in the log.
//
// Not supported, and reported as such through the device caps so the engine
// takes its fixed function fallbacks: vertex and pixel shaders, cube and volume
// textures, render target textures, more than two simultaneous textures.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <d3d8.h>

#include <vitaGL.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace VitaD3D8
{

/// Physical size of the display. The back buffer always has this size.
enum
{
	DISPLAY_WIDTH = 960,
	DISPLAY_HEIGHT = 544,
	DISPLAY_REFRESH_RATE = 60
};

/// Limits of vitaGL's fixed function pipeline.
enum
{
	MAX_TEXTURE_STAGES = 2,		///< texture coordinate sets the fixed function shaders take
	MAX_D3D_TEXTURE_STAGES = 8,	///< stages Direct3D 8 lets an application address
	MAX_LIGHTS = 8,
	MAX_CLIP_PLANES = 6,
	MAX_STREAMS = 1
};

/// Texture sizes. The device advertises the limit of the graphics cards the game was made
/// for, which keeps the engine's own choices modest; the GPU itself takes twice that.
enum
{
	ADVERTISED_TEXTURE_SIZE = 2048,
	MAX_TEXTURE_SIZE = 4096
};

/// What the frames of the current reporting period cost, summed up. Device::Present()
/// writes the averages to the log every few seconds and starts over.
struct FrameStatistics
{
	uint64_t periodStart;	///< microseconds, 0 before the first frame
	uint32_t frames;
	uint32_t drawCalls;
	uint32_t primitives;
	uint64_t drawTime;		///< inside the draw calls, on the game's thread
	uint64_t clearTime;		///< inside the clears; the first one of a frame starts the GPU scene
	uint64_t presentTime;	///< inside vglSwapBuffers
	uint32_t textureUploads;
	uint64_t textureUploadTime;
	uint32_t textureUploadsWaited;	///< of those, the ones the game's thread waited for
	uint32_t waitedUploadWidth;		///< the largest of them, and what it was
	uint32_t waitedUploadHeight;
	uint32_t waitedUploadFormat;	///< its D3DFORMAT
	bool waitedUploadWasFirst;		///< vitaGL did not have that texture yet
	uint32_t bufferUploads;
	uint64_t bufferUploadTime;

	// Draw calls with vertices from system memory (dynamic buffers and Draw*UP), and what
	// they copy for the thread that draws later.
	uint32_t copyingDrawCalls;
	uint64_t copiedVertexBytes;
	uint64_t copiedIndexBytes;
	uint64_t copiedUnchangedVertexBytes;	///< from buffers not written to since the frame before

	// Draw calls that took their vertices from a dynamic buffer as they were, without a copy.
	uint32_t directDrawCalls;
	uint64_t directVertexBytes;
};
extern FrameStatistics g_frameStatistics;

/// Counts the frames that were presented.
extern uint32_t g_frameNumber;

/// Counts the times a texture was written to: a lock for writing, or a rectangle declared
/// changed. The device looks at the textures of the stages again when this has moved.
extern uint32_t g_textureWrites;

/// Two parts of a draw call, in microseconds, summed up over the same period: translating
/// the recorded state (ApplyState) and handing the vertex arrays to vitaGL. They are counted
/// by the thread that makes the calls to vitaGL and read by the game's thread.
extern std::atomic<uint32_t> g_stateTime;
extern std::atomic<uint32_t> g_arrayTime;

/// printf style line in the game's log (stderr is redirected to a file on the memory card).
void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/// Log a call the engine made that this implementation does not provide. Each distinct
/// message is written once so a per-frame call does not flood the log.
void LogUnsupportedOnce(const char *what);

//-----------------------------------------------------------------------------
/// Reference counting and QueryInterface for the COM style interfaces. Direct3D 8 objects
/// are only ever addressed through the interface they were created as, so QueryInterface
/// hands back the object itself.
template <class Interface>
class Unknown : public Interface
{
public:
	Unknown() : m_refCount(1) {}
	virtual ~Unknown() {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
	{
		if (ppvObject == nullptr)
			return E_POINTER;

		*ppvObject = this;
		AddRef();
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++m_refCount;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --m_refCount;
		if (remaining == 0)
			delete this;

		return remaining;
	}

protected:
	ULONG m_refCount;
};

class Device;
class Texture;
class Surface;
class VertexBuffer;
class IndexBuffer;

/// The one device of the process, or nullptr before it was created. vitaGL owns process
/// lifetime GPU state, so there is never more than one.
Device *GetDevice();

} // namespace VitaD3D8
