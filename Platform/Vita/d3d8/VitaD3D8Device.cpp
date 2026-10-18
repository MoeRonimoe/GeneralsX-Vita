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

// VitaD3D8Device.cpp /////////////////////////////////////////////////////////
// IDirect3DDevice8 on vitaGL: device lifetime, resources, state recording and
// draw calls. The translation of the recorded state is in VitaD3D8State.cpp.
///////////////////////////////////////////////////////////////////////////////

#include "VitaD3D8Device.h"

#include <VitaDiagnostics.h>

#include <psp2/kernel/processmgr.h>

// Last: from here on the calls to vitaGL go through the render thread.
#include "VitaD3D8GL.h"

namespace VitaD3D8
{

FrameStatistics g_frameStatistics = {};
uint32_t g_frameNumber = 0;
uint32_t g_textureWrites = 0;
std::atomic<uint32_t> g_stateTime(0);
std::atomic<uint32_t> g_arrayTime(0);

namespace
{

// Time between two lines of frame statistics in the log.
const uint64_t STATISTICS_PERIOD_US = 10 * 1000 * 1000;

//-----------------------------------------------------------------------------
/// Counts a draw call and the time it takes for the frame statistics.
class DrawTimer
{
public:
	explicit DrawTimer(UINT primitives) : m_start(VitaDiagnostics::profileStart())
	{
		++VitaDiagnostics::g_drawCalls;
		++g_frameStatistics.drawCalls;
		g_frameStatistics.primitives += primitives;
	}

	~DrawTimer()
	{
		RenderThread::Publish();
		if (m_start != 0)
			g_frameStatistics.drawTime += VitaDiagnostics::now() - m_start;
	}

private:
	uint64_t m_start;
};

//-----------------------------------------------------------------------------
/// Adds the time of a clear to the frame statistics.
class ClearTimer
{
public:
	ClearTimer() : m_start(VitaDiagnostics::profileStart()) {}
	~ClearTimer()
	{
		RenderThread::Publish();
		if (m_start != 0)
			g_frameStatistics.clearTime += VitaDiagnostics::now() - m_start;
	}

private:
	uint64_t m_start;
};

//-----------------------------------------------------------------------------
/// What the render thread did per frame, and what the game's thread waited for it.
void LogRenderThread(double frames, double stateMs, double arrayMs)
{
	// Among the calls that are waited for are texture uploads; which ones says what could
	// still be sent without waiting.
	const FrameStatistics &statistics = g_frameStatistics;
	if (statistics.textureUploadsWaited != 0)
	{
		Log("%.1f texture uploads per frame were waited for; the largest was %ux%u of format %u, %s",
			statistics.textureUploadsWaited / frames, static_cast<unsigned>(statistics.waitedUploadWidth),
			static_cast<unsigned>(statistics.waitedUploadHeight), static_cast<unsigned>(statistics.waitedUploadFormat),
			statistics.waitedUploadWasFirst ? "its first upload" : "an upload again");
	}

	// The parts are only timed while a frame profile is taken.
	char parts[96] = "";
	if (VitaDiagnostics::g_profiling)
		snprintf(parts, sizeof(parts), ", of that %.1f translating state and %.1f setting up vertex arrays", stateMs, arrayMs);

	const RenderThread::Waits waits = RenderThread::TakeWaits();
	Log("render thread: %.1f ms per frame%s; the game waited %.1f ms per frame for %.1f calls and %.1f ms for the frame before; the thread slept %.1f times per frame",
		RenderThread::TakeBusyTime() / 1000.0 / frames, parts, waits.callTime / 1000.0 / frames, waits.calls / frames,
		waits.frameTime / 1000.0 / frames, waits.sleeps / frames);
}

//-----------------------------------------------------------------------------
/// Called once per presented frame. Every few seconds the log gets the frame rate and
/// what an average frame of that time was made of. Loading screens count as frames, so
/// the first line after a mission has loaded says little.
/// What the draw calls from system memory copied per frame. Every byte of it is written by
/// the game's thread and read again by the render thread.
void LogCopies(double frames)
{
	const FrameStatistics &statistics = g_frameStatistics;
	if (statistics.copyingDrawCalls == 0 && statistics.directDrawCalls == 0)
		return;

	Log("%.0f draw calls per frame had their vertices in system memory: %.0f KiB of vertices and %.0f KiB of indices were copied per frame, "
		"%.0f KiB of the vertices from buffers that had not changed since the frame before; "
		"%.0f more draw calls took %.0f KiB of vertices from their buffers without a copy",
		statistics.copyingDrawCalls / frames, statistics.copiedVertexBytes / 1024.0 / frames,
		statistics.copiedIndexBytes / 1024.0 / frames, statistics.copiedUnchangedVertexBytes / 1024.0 / frames,
		statistics.directDrawCalls / frames, statistics.directVertexBytes / 1024.0 / frames);
}

//-----------------------------------------------------------------------------
void LogFrameStatistics()
{
	FrameStatistics &statistics = g_frameStatistics;
	const uint64_t now = VitaDiagnostics::now();
	++g_frameNumber;

	if (statistics.periodStart == 0)
	{
		statistics = FrameStatistics();
		statistics.periodStart = now;
		VitaDiagnostics::takeTime(VitaDiagnostics::TIME_GAME_LOGIC);
		return;
	}

	++statistics.frames;
	const uint64_t elapsed = now - statistics.periodStart;
	if (elapsed < STATISTICS_PERIOD_US)
		return;

	const double frames = statistics.frames;
	const double frameMs = elapsed / 1000.0 / frames;

	if (!VitaDiagnostics::g_profiling)
	{
		// Without a frame profile only what costs nothing to count is known.
		Log("%.1f frames per second; average frame %.1f ms with %.0f draw calls (%.0f primitives), %.1f buffer and %.1f texture uploads",
			frames * 1000000.0 / elapsed, frameMs, statistics.drawCalls / frames, statistics.primitives / frames,
			statistics.bufferUploads / frames, statistics.textureUploads / frames);
		if (RenderThread::IsRunning())
			LogRenderThread(frames, 0.0, 0.0);
		LogCopies(frames);

		statistics = FrameStatistics();
		statistics.periodStart = now;
		return;
	}

	const double drawMs = statistics.drawTime / 1000.0 / frames;
	const double clearMs = statistics.clearTime / 1000.0 / frames;
	const double presentMs = statistics.presentTime / 1000.0 / frames;
	const double logicMs = VitaDiagnostics::takeTime(VitaDiagnostics::TIME_GAME_LOGIC) / 1000.0 / frames;
	const double bufferMs = statistics.bufferUploadTime / 1000.0 / frames;

	// With a render thread the draw calls cost the game's thread what writing them down
	// costs. Translating the state and setting up the arrays is the other thread's time
	// then, and is in the line about that thread.
	const bool threaded = RenderThread::IsRunning();
	const double stateMs = g_stateTime.exchange(0) / 1000.0 / frames;
	const double arrayMs = g_arrayTime.exchange(0) / 1000.0 / frames;
	const double ownStateMs = threaded ? 0.0 : stateMs;
	const double ownArrayMs = threaded ? 0.0 : arrayMs;

	Log("%.1f frames per second; average frame %.1f ms: %.1f in %.0f draw calls (%.0f primitives; state %.1f, arrays %.1f, vitaGL %.1f), "
		"%.1f clearing, %.1f presenting, %.1f game logic, %.1f in %.1f buffer uploads, %.1f other engine work; "
		"texture uploads in the draw calls: %.1f ms for %.1f",
		frames * 1000000.0 / elapsed, frameMs,
		drawMs, statistics.drawCalls / frames, statistics.primitives / frames, ownStateMs, ownArrayMs, drawMs - ownStateMs - ownArrayMs,
		clearMs, presentMs, logicMs, bufferMs, statistics.bufferUploads / frames,
		frameMs - drawMs - clearMs - presentMs - logicMs - bufferMs,
		statistics.textureUploadTime / 1000.0 / frames, statistics.textureUploads / frames);

	if (threaded)
		LogRenderThread(frames, stateMs, arrayMs);
	LogCopies(frames);

	VitaDiagnostics::logNamedTimers(statistics.frames);

	statistics = FrameStatistics();
	statistics.periodStart = now;
}

// Memory vitaGL leaves to the rest of the process when it claims the remaining user RAM
// for its own pools: thread stacks, the network stack and anything allocated behind
// newlib's back.
const int VITAGL_RAM_RESERVE = 16 * 1024 * 1024;

// vitaGL's pool for glBegin/glEnd style drawing, which this implementation never uses.
const int VITAGL_LEGACY_POOL = 64 * 1024;

// Collecting the heap statistics walks the allocator's free lists, which is too slow to
// do on every frame.
const SceUInt64 MEMORY_CHECK_INTERVAL_US = 1000 * 1000;

// Growth or shrinkage that is worth a line in the log.
const size_t MEMORY_LOG_STEP = 8 * 1024 * 1024;

//-----------------------------------------------------------------------------
/// The C heap and vitaGL's pools side by side: together they are all the memory the
/// application has, and what one of them holds the other cannot use.
void LogMemory(const char *label)
{
	const double megabyte = 1024.0 * 1024.0;
	const VitaDiagnostics::MemoryState state = VitaDiagnostics::getMemoryState();
	const ResourceMemory resources = GetResourceMemory();

	Log("memory %s: heap %.1f of %.1f MB in use, of that texture copies %.1f MB and buffer copies %.1f MB; vitaGL free: vram %.1f of %.1f MB, ram %.1f of %.1f MB, phycont %.1f of %.1f MB",
		label, state.heapUsed / megabyte, state.heapLimit / megabyte,
		resources.textureBytes / megabyte, resources.bufferBytes / megabyte,
		vglMemFree(VGL_MEM_VRAM) / megabyte, vglMemTotal(VGL_MEM_VRAM) / megabyte,
		vglMemFree(VGL_MEM_RAM) / megabyte, vglMemTotal(VGL_MEM_RAM) / megabyte,
		vglMemFree(VGL_MEM_PHYCONT) / megabyte, vglMemTotal(VGL_MEM_PHYCONT) / megabyte);
}

//-----------------------------------------------------------------------------
/// Called once per presented frame, which includes the steps of a loading screen. The
/// lines it writes show between which other log lines the memory went.
void LogMemoryWhenChanged()
{
	static SceUInt64 nextCheck = 0;
	static size_t loggedHeap = 0;
	static size_t loggedGraphics = 0;

	const SceUInt64 now = sceKernelGetProcessTimeWide();
	if (now < nextCheck)
		return;
	nextCheck = now + MEMORY_CHECK_INTERVAL_US;

	// The render thread is asked to look at vitaGL's pools, and what it found the last time
	// is used: this is for a line in the log, and waiting for the answer stopped the game for
	// as long as the render thread was behind, once a second.
	static std::atomic<size_t> graphicsFree(0);
	RenderThread::Defer([]
	{
		graphicsFree.store((::vglMemFree)(VGL_MEM_VRAM) + (::vglMemFree)(VGL_MEM_RAM) + (::vglMemFree)(VGL_MEM_PHYCONT));
	});

	const size_t heap = VitaDiagnostics::getMemoryState().heapUsed;
	const size_t graphics = graphicsFree.load();

	const size_t heapChange = heap > loggedHeap ? heap - loggedHeap : loggedHeap - heap;
	const size_t graphicsChange = graphics > loggedGraphics ? graphics - loggedGraphics : loggedGraphics - graphics;
	if (heapChange < MEMORY_LOG_STEP && graphicsChange < MEMORY_LOG_STEP)
		return;

	loggedHeap = heap;
	loggedGraphics = graphics;
	LogMemory("now");
}

DWORD FloatBits(float value)
{
	DWORD bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

bool HasColors(const VertexLayout &layout)
{
	return layout.diffuseOffset >= 0 || layout.specularOffset >= 0;
}

/// The layout of the vertices of a format, with the stride the application gave if that is
/// not 0. A draw call used to work the layout out and copy all of it into the stream of the
/// render thread, a thousand times per frame for the same handful of formats. Now it writes
/// where the layout is: what this returns stays where it is and as it is.
const VertexLayout *GetCachedLayout(DWORD fvf, UINT stride)
{
	struct Entry
	{
		DWORD fvf;
		UINT stride;
		VertexLayout layout;
	};
	enum { MAX_LAYOUTS = 64 };
	static Entry entries[MAX_LAYOUTS];
	static UINT count = 0;
	static UINT last = 0;

	if (count != 0 && entries[last].fvf == fvf && entries[last].stride == stride)
		return &entries[last].layout;

	for (UINT i = 0; i < count; ++i)
	{
		if (entries[i].fvf == fvf && entries[i].stride == stride)
		{
			last = i;
			return &entries[i].layout;
		}
	}

	if (count == MAX_LAYOUTS)
	{
		// More formats than a game has. The table starts over once nothing written to the
		// stream points into it any more.
		RenderThread::Sync([] {});
		count = 0;
	}

	Entry &entry = entries[count];
	entry.fvf = fvf;
	entry.stride = stride;
	GetVertexLayout(fvf, entry.layout);
	if (stride != 0)
		entry.layout.stride = stride;

	last = count++;
	return &entry.layout;
}

/// Copy a rectangle between two locked surfaces of the same format.
void CopyPixels(const FormatInfo &info, const D3DLOCKED_RECT &source, UINT sourceX, UINT sourceY,
	const D3DLOCKED_RECT &destination, UINT destinationX, UINT destinationY, UINT width, UINT height)
{
	UINT rowBytes;
	UINT rows;
	UINT sourceOffset;
	UINT destinationOffset;

	if (info.blockBytes != 0)
	{
		// Whole blocks; the engine only copies block aligned regions of compressed surfaces.
		rowBytes = ((width + 3) / 4) * info.blockBytes;
		rows = (height + 3) / 4;
		sourceOffset = (sourceY / 4) * source.Pitch + (sourceX / 4) * info.blockBytes;
		destinationOffset = (destinationY / 4) * destination.Pitch + (destinationX / 4) * info.blockBytes;
	}
	else
	{
		rowBytes = width * info.bytesPerPixel;
		rows = height;
		sourceOffset = sourceY * source.Pitch + sourceX * info.bytesPerPixel;
		destinationOffset = destinationY * destination.Pitch + destinationX * info.bytesPerPixel;
	}

	const uint8_t *from = static_cast<const uint8_t *>(source.pBits) + sourceOffset;
	uint8_t *to = static_cast<uint8_t *>(destination.pBits) + destinationOffset;
	for (UINT row = 0; row < rows; ++row)
	{
		memcpy(to, from, rowBytes);
		from += source.Pitch;
		to += destination.Pitch;
	}
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// Lifetime
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
Device::Device(IDirect3D8 *parent, const D3DDEVICE_CREATION_PARAMETERS &creation, const D3DPRESENT_PARAMETERS &presentation) :
	m_parent(parent),
	m_creation(creation),
	m_backBuffer(nullptr),
	m_depthStencil(nullptr),
	m_displayScaleX(1.0f),
	m_displayScaleY(1.0f),
	m_texturesToSend(0),
	m_textureWritesSeen(0),
	m_colorOpChanged(true),
	m_streamBuffer(nullptr),
	m_streamStride(0),
	m_indexBuffer(nullptr),
	m_baseVertexIndex(0),
	m_vertexShader(0),
	m_pixelShader(0),
	m_texGenStages(0),
	m_userVertices(nullptr),
	m_userVerticesSize(0),
	m_indexScratch(nullptr),
	m_indexScratchSize(0),
	m_cursorVisible(false),
	m_dirty(DIRTY_EVERYTHING),
	m_whiteTexture(0)
{
	m_parent->AddRef();
	memset(m_textures, 0, sizeof(m_textures));
	memset(m_sentTextures, 0, sizeof(m_sentTextures));
	memset(m_boundTextures, 0, sizeof(m_boundTextures));

	InitGraphics();
	ApplyPresentationParameters(presentation);
	SetDefaultState();

	Log("device created: back buffer %ux%u on a %dx%d display",
		static_cast<unsigned>(m_presentation.BackBufferWidth), static_cast<unsigned>(m_presentation.BackBufferHeight),
		static_cast<int>(DISPLAY_WIDTH), static_cast<int>(DISPLAY_HEIGHT));
}

//-----------------------------------------------------------------------------
Device::~Device()
{
	for (UINT stage = 0; stage < MAX_D3D_TEXTURE_STAGES; ++stage)
	{
		if (m_textures[stage] != nullptr)
			m_textures[stage]->Release();
	}

	if (m_streamBuffer != nullptr)
		m_streamBuffer->Release();
	if (m_indexBuffer != nullptr)
		m_indexBuffer->Release();
	if (m_backBuffer != nullptr)
		m_backBuffer->Release();
	if (m_depthStencil != nullptr)
		m_depthStencil->Release();

	// The render thread makes the calls that are still in the stream, which refer to this
	// device, and lets go of what it made for it.
	RenderThread::Sync([this] { ReleaseGLObjects(); });

	free(m_userVertices);
	free(m_indexScratch);

	ClearDevice(this);
	m_parent->Release();
}

//-----------------------------------------------------------------------------
void Device::InitGraphics()
{
	// vitaGL initializes the GPU once per process and offers no way to shut it down again.
	static bool initialized = false;
	if (initialized)
		return;

	// With a render thread, vitaGL is that thread's from its first call on.
	const bool threaded = RenderThread::Start();

	RenderThread::Sync([]
	{
		// The engine paces its frames itself. Waiting for the vertical blank on top of that
		// rounds every frame up to a multiple of 16.7 ms, which costs frames at low frame rates.
		vglWaitVblankStart(GL_FALSE);

#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
		// The vitaGL of tools/build-vitagl.sh compiles its fixed function shaders from
		// changed sources and keeps them with the game's files.
		vglSetFfpShaderCachePath("ux0:data/Generals/shader_cache");
#endif

		// The return value only says whether vitaGL fell back to another resolution; the
		// native one requested here always exists.
		vglInitExtended(VITAGL_LEGACY_POOL, DISPLAY_WIDTH, DISPLAY_HEIGHT, VITAGL_RAM_RESERVE, SCE_GXM_MULTISAMPLE_NONE);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		// vitaGL shows its splash screen until the first clear of a frame.
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		vglSwapBuffers(GL_FALSE);
	});

	initialized = true;
	Log(threaded ? "vitaGL initialized, on a render thread" : "vitaGL initialized");
	LogMemory("after vitaGL start");
}

//-----------------------------------------------------------------------------
void Device::ApplyPresentationParameters(const D3DPRESENT_PARAMETERS &presentation)
{
	m_presentation = presentation;

	if (m_presentation.BackBufferWidth == 0)
		m_presentation.BackBufferWidth = DISPLAY_WIDTH;
	if (m_presentation.BackBufferHeight == 0)
		m_presentation.BackBufferHeight = DISPLAY_HEIGHT;
	if (m_presentation.BackBufferFormat == D3DFMT_UNKNOWN)
		m_presentation.BackBufferFormat = D3DFMT_X8R8G8B8;
	if (m_presentation.BackBufferCount == 0)
		m_presentation.BackBufferCount = 1;

	m_displayScaleX = static_cast<float>(DISPLAY_WIDTH) / m_presentation.BackBufferWidth;
	m_displayScaleY = static_cast<float>(DISPLAY_HEIGHT) / m_presentation.BackBufferHeight;

	if (m_backBuffer != nullptr)
		m_backBuffer->Release();
	if (m_depthStencil != nullptr)
		m_depthStencil->Release();

	m_backBuffer = Surface::CreateDisplaySurface(Surface::KIND_BACK_BUFFER,
		m_presentation.BackBufferWidth, m_presentation.BackBufferHeight, m_presentation.BackBufferFormat);
	m_depthStencil = Surface::CreateDisplaySurface(Surface::KIND_DEPTH_STENCIL,
		m_presentation.BackBufferWidth, m_presentation.BackBufferHeight, D3DFMT_D24S8);
}

//-----------------------------------------------------------------------------
void Device::SetDefaultState()
{
	memset(m_front.renderStates, 0, sizeof(m_front.renderStates));
	memset(m_front.textureStageStates, 0, sizeof(m_front.textureStageStates));
	memset(&m_front.material, 0, sizeof(m_front.material));
	memset(m_front.lights, 0, sizeof(m_front.lights));
	memset(m_front.lightEnabled, 0, sizeof(m_front.lightEnabled));
	memset(m_front.clipPlanes, 0, sizeof(m_front.clipPlanes));

	m_front.world = GetIdentityMatrix();
	m_front.view = GetIdentityMatrix();
	m_front.projection = GetIdentityMatrix();
	for (D3DMATRIX &matrix : m_front.textureMatrix)
		matrix = GetIdentityMatrix();

	m_front.viewport.X = 0;
	m_front.viewport.Y = 0;
	m_front.viewport.Width = m_presentation.BackBufferWidth;
	m_front.viewport.Height = m_presentation.BackBufferHeight;
	m_front.viewport.MinZ = 0.0f;
	m_front.viewport.MaxZ = 1.0f;

	// The documented initial values of Direct3D 8.
	m_front.renderStates[D3DRS_ZENABLE] = D3DZB_TRUE;
	m_front.renderStates[D3DRS_FILLMODE] = D3DFILL_SOLID;
	m_front.renderStates[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
	m_front.renderStates[D3DRS_ZWRITEENABLE] = TRUE;
	m_front.renderStates[D3DRS_LASTPIXEL] = TRUE;
	m_front.renderStates[D3DRS_SRCBLEND] = D3DBLEND_ONE;
	m_front.renderStates[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
	m_front.renderStates[D3DRS_CULLMODE] = D3DCULL_CCW;
	m_front.renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	m_front.renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
	m_front.renderStates[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
	m_front.renderStates[D3DRS_FOGSTART] = FloatBits(0.0f);
	m_front.renderStates[D3DRS_FOGEND] = FloatBits(1.0f);
	m_front.renderStates[D3DRS_FOGDENSITY] = FloatBits(1.0f);
	m_front.renderStates[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
	m_front.renderStates[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
	m_front.renderStates[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
	m_front.renderStates[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
	m_front.renderStates[D3DRS_STENCILMASK] = 0xFFFFFFFF;
	m_front.renderStates[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
	m_front.renderStates[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
	m_front.renderStates[D3DRS_CLIPPING] = TRUE;
	m_front.renderStates[D3DRS_LIGHTING] = TRUE;
	m_front.renderStates[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
	m_front.renderStates[D3DRS_COLORVERTEX] = TRUE;
	m_front.renderStates[D3DRS_LOCALVIEWER] = TRUE;
	m_front.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
	m_front.renderStates[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
	m_front.renderStates[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_COLOR2;
	m_front.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
	m_front.renderStates[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
	m_front.renderStates[D3DRS_POINTSIZE] = FloatBits(1.0f);
	m_front.renderStates[D3DRS_POINTSCALE_A] = FloatBits(1.0f);
	m_front.renderStates[D3DRS_POINTSIZE_MAX] = FloatBits(64.0f);
	m_front.renderStates[D3DRS_COLORWRITEENABLE] = 0x0000000F;
	m_front.renderStates[D3DRS_BLENDOP] = D3DBLENDOP_ADD;

	for (UINT stage = 0; stage < MAX_D3D_TEXTURE_STAGES; ++stage)
	{
		DWORD *states = m_front.textureStageStates[stage];
		states[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
		states[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
		states[D3DTSS_COLORARG2] = D3DTA_CURRENT;
		states[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
		states[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
		states[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
		states[D3DTSS_TEXCOORDINDEX] = stage;
		states[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
		states[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
		states[D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
		states[D3DTSS_MAGFILTER] = D3DTEXF_POINT;
		states[D3DTSS_MINFILTER] = D3DTEXF_POINT;
		states[D3DTSS_MIPFILTER] = D3DTEXF_NONE;
		states[D3DTSS_MAXANISOTROPY] = 1;
		states[D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
		states[D3DTSS_COLORARG0] = D3DTA_CURRENT;
		states[D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
		states[D3DTSS_RESULTARG] = D3DTA_CURRENT;
	}

	m_texGenStages = 0;

	// No stage's texture has been named to the render thread.
	m_texturesToSend = (1u << MAX_TEXTURE_STAGES) - 1;
	memset(m_sentTextures, 0, sizeof(m_sentTextures));

	// The render thread starts from the same state, knowing nothing of what vitaGL has.
	RenderThread::Sync([this]
	{
		m_back = m_front;
		memset(m_boundTextures, 0, sizeof(m_boundTextures));
		InvalidateStateCache();
	});
}

///////////////////////////////////////////////////////////////////////////////
// Device management
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::TestCooperativeLevel()
{
	// The Vita never takes the GPU away from a running application.
	return D3D_OK;
}

//-----------------------------------------------------------------------------
UINT STDMETHODCALLTYPE Device::GetAvailableTextureMem()
{
	// The engine only uses this to decide whether textures have to be reduced.
	return 96 * 1024 * 1024;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::ResourceManagerDiscardBytes(DWORD Bytes)
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetDirect3D(IDirect3D8 **ppD3D8)
{
	if (ppD3D8 == nullptr)
		return D3DERR_INVALIDCALL;

	m_parent->AddRef();
	*ppD3D8 = m_parent;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetDeviceCaps(D3DCAPS8 *pCaps)
{
	if (pCaps == nullptr)
		return D3DERR_INVALIDCALL;

	FillDeviceCaps(*pCaps);
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetDisplayMode(D3DDISPLAYMODE *pMode)
{
	if (pMode == nullptr)
		return D3DERR_INVALIDCALL;

	pMode->Width = m_presentation.BackBufferWidth;
	pMode->Height = m_presentation.BackBufferHeight;
	pMode->RefreshRate = DISPLAY_REFRESH_RATE;
	pMode->Format = m_presentation.BackBufferFormat;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
	if (pParameters == nullptr)
		return D3DERR_INVALIDCALL;

	*pParameters = m_creation;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap)
{
	// There is no hardware cursor; the game draws its pointer itself (VitaMouse).
	return D3D_OK;
}

//-----------------------------------------------------------------------------
void STDMETHODCALLTYPE Device::SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags)
{
}

//-----------------------------------------------------------------------------
WINBOOL STDMETHODCALLTYPE Device::ShowCursor(WINBOOL bShow)
{
	const WINBOOL previous = m_cursorVisible ? TRUE : FALSE;
	m_cursorVisible = bShow != FALSE;
	return previous;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain8 **pSwapChain)
{
	LogUnsupportedOnce("CreateAdditionalSwapChain");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
	if (pPresentationParameters == nullptr)
		return D3DERR_INVALIDCALL;

	// Reset returns every state to its default, and the engine sets its own again afterwards.
	for (UINT stage = 0; stage < MAX_D3D_TEXTURE_STAGES; ++stage)
		SetTexture(stage, nullptr);
	SetStreamSource(0, nullptr, 0);
	SetIndices(nullptr, 0);

	// The render thread reads the size of the back buffer, and has to be through with
	// everything that was drawn at the old one.
	RenderThread::Sync([] {});

	ApplyPresentationParameters(*pPresentationParameters);
	*pPresentationParameters = m_presentation;
	SetDefaultState();

	Log("device reset: back buffer %ux%u",
		static_cast<unsigned>(m_presentation.BackBufferWidth), static_cast<unsigned>(m_presentation.BackBufferHeight));
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region)
{
	const uint64_t start = VitaDiagnostics::now();
	vglSwapBuffers(GL_FALSE);
	g_frameStatistics.presentTime += VitaDiagnostics::now() - start;

	LogFrameStatistics();
	LogMemoryWhenChanged();
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer)
{
	if (ppBackBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	m_backBuffer->AddRef();
	*ppBackBuffer = m_backBuffer;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus)
{
	if (pRasterStatus == nullptr)
		return D3DERR_INVALIDCALL;

	pRasterStatus->InVBlank = FALSE;
	pRasterStatus->ScanLine = 0;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
void STDMETHODCALLTYPE Device::SetGammaRamp(DWORD flags, const D3DGAMMARAMP *ramp)
{
	// The caps do not advertise gamma control.
}

//-----------------------------------------------------------------------------
void STDMETHODCALLTYPE Device::GetGammaRamp(D3DGAMMARAMP *pRamp)
{
	if (pRamp == nullptr)
		return;

	for (int i = 0; i < 256; ++i)
	{
		const WORD value = static_cast<WORD>(i * 257);
		pRamp->red[i] = value;
		pRamp->green[i] = value;
		pRamp->blue[i] = value;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Resources
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
	if (ppTexture == nullptr)
		return D3DERR_INVALIDCALL;

	*ppTexture = nullptr;

	if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
	{
		LogUnsupportedOnce("render target and depth textures");
		return D3DERR_NOTAVAILABLE;
	}

	Texture *texture = Texture::Create(Width, Height, Levels, Usage, Format, Pool);
	if (texture == nullptr)
	{
		Log("CreateTexture failed: %ux%u, format %u", static_cast<unsigned>(Width), static_cast<unsigned>(Height), static_cast<unsigned>(Format));
		return D3DERR_NOTAVAILABLE;
	}

	*ppTexture = texture;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture)
{
	if (ppVolumeTexture != nullptr)
		*ppVolumeTexture = nullptr;

	LogUnsupportedOnce("volume textures");
	return D3DERR_NOTAVAILABLE;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture)
{
	if (ppCubeTexture != nullptr)
		*ppCubeTexture = nullptr;

	LogUnsupportedOnce("cube textures");
	return D3DERR_NOTAVAILABLE;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
	if (ppVertexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppVertexBuffer = VertexBuffer::Create(Length, Usage, FVF, Pool);
	return *ppVertexBuffer != nullptr ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
	if (ppIndexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppIndexBuffer = IndexBuffer::Create(Length, Usage, Format, Pool);
	return *ppIndexBuffer != nullptr ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, WINBOOL Lockable, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface != nullptr)
		*ppSurface = nullptr;

	LogUnsupportedOnce("CreateRenderTarget");
	return D3DERR_NOTAVAILABLE;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	*ppSurface = nullptr;
	if (!IsDepthFormatSupported(Format))
		return D3DERR_NOTAVAILABLE;

	// Only the depth buffer of the display exists. The surface is a description the engine
	// can hold on to and pass back to SetRenderTarget together with the back buffer.
	*ppSurface = Surface::CreateDisplaySurface(Surface::KIND_DEPTH_STENCIL, Width, Height, Format);
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	*ppSurface = Surface::CreateImage(Width, Height, Format);
	return *ppSurface != nullptr ? D3D_OK : D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CopyRects(IDirect3DSurface8 *src_surface, const RECT *src_rects, UINT rect_count, IDirect3DSurface8 *dst_surface, const POINT *dst_points)
{
	if (src_surface == nullptr || dst_surface == nullptr)
		return D3DERR_INVALIDCALL;

	Surface *source = static_cast<Surface *>(src_surface);
	Surface *destination = static_cast<Surface *>(dst_surface);

	if (source->GetKind() == Surface::KIND_BACK_BUFFER || source->GetKind() == Surface::KIND_DEPTH_STENCIL
		|| destination->GetKind() == Surface::KIND_BACK_BUFFER || destination->GetKind() == Surface::KIND_DEPTH_STENCIL)
	{
		LogUnsupportedOnce("CopyRects from or to the display surfaces");
		return D3DERR_INVALIDCALL;
	}

	// Direct3D 8 does not convert in CopyRects either.
	if (source->GetD3DFormat() != destination->GetD3DFormat())
		return D3DERR_INVALIDCALL;

	const FormatInfo *info = GetFormatInfo(source->GetD3DFormat());
	if (info == nullptr)
		return D3DERR_INVALIDCALL;

	D3DLOCKED_RECT from;
	D3DLOCKED_RECT to;
	if (FAILED(source->LockRect(&from, nullptr, D3DLOCK_READONLY)))
		return D3DERR_INVALIDCALL;
	if (FAILED(destination->LockRect(&to, nullptr, 0)))
	{
		source->UnlockRect();
		return D3DERR_INVALIDCALL;
	}

	if (src_rects == nullptr || rect_count == 0)
	{
		UINT width = source->GetWidth() < destination->GetWidth() ? source->GetWidth() : destination->GetWidth();
		UINT height = source->GetHeight() < destination->GetHeight() ? source->GetHeight() : destination->GetHeight();
		CopyPixels(*info, from, 0, 0, to, 0, 0, width, height);
	}
	else
	{
		for (UINT i = 0; i < rect_count; ++i)
		{
			const RECT &rect = src_rects[i];
			const UINT targetX = dst_points != nullptr ? static_cast<UINT>(dst_points[i].x) : static_cast<UINT>(rect.left);
			const UINT targetY = dst_points != nullptr ? static_cast<UINT>(dst_points[i].y) : static_cast<UINT>(rect.top);
			CopyPixels(*info, from, static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), to, targetX, targetY,
				static_cast<UINT>(rect.right - rect.left), static_cast<UINT>(rect.bottom - rect.top));
		}
	}

	destination->UnlockRect();
	source->UnlockRect();
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture)
{
	if (pSourceTexture == nullptr || pDestinationTexture == nullptr)
		return D3DERR_INVALIDCALL;

	// Only two dimensional textures exist in this implementation.
	Texture *source = static_cast<Texture *>(pSourceTexture);
	Texture *destination = static_cast<Texture *>(pDestinationTexture);

	const UINT levels = source->GetLevelCount() < destination->GetLevelCount() ? source->GetLevelCount() : destination->GetLevelCount();
	for (UINT level = 0; level < levels; ++level)
	{
		D3DSURFACE_DESC fromDesc;
		D3DSURFACE_DESC toDesc;
		source->GetLevelDesc(level, &fromDesc);
		destination->GetLevelDesc(level, &toDesc);
		if (fromDesc.Format != toDesc.Format || fromDesc.Width != toDesc.Width || fromDesc.Height != toDesc.Height)
			return D3DERR_INVALIDCALL;

		D3DLOCKED_RECT from;
		D3DLOCKED_RECT to;
		if (FAILED(source->LockRect(level, &from, nullptr, D3DLOCK_READONLY)))
			return D3DERR_INVALIDCALL;
		if (FAILED(destination->LockRect(level, &to, nullptr, 0)))
		{
			source->UnlockRect(level);
			return D3DERR_INVALIDCALL;
		}

		CopyPixels(source->GetFormat(), from, 0, 0, to, 0, 0, fromDesc.Width, fromDesc.Height);

		destination->UnlockRect(level);
		source->UnlockRect(level);
	}

	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetFrontBuffer(IDirect3DSurface8 *pDestSurface)
{
	LogUnsupportedOnce("GetFrontBuffer (screenshots)");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetRenderTarget(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil)
{
	// A null render target keeps the current one and only changes the depth buffer.
	if (pRenderTarget == nullptr || pRenderTarget == m_backBuffer)
		return D3D_OK;

	LogUnsupportedOnce("SetRenderTarget with a surface other than the back buffer");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetRenderTarget(IDirect3DSurface8 **ppRenderTarget)
{
	if (ppRenderTarget == nullptr)
		return D3DERR_INVALIDCALL;

	m_backBuffer->AddRef();
	*ppRenderTarget = m_backBuffer;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface)
{
	if (ppZStencilSurface == nullptr)
		return D3DERR_INVALIDCALL;

	m_depthStencil->AddRef();
	*ppZStencilSurface = m_depthStencil;
	return D3D_OK;
}

///////////////////////////////////////////////////////////////////////////////
// Scene
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::BeginScene()
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::EndScene()
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::Clear(DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	GLbitfield mask = 0;
	if (flags & D3DCLEAR_TARGET)
		mask |= GL_COLOR_BUFFER_BIT;
	if (flags & D3DCLEAR_ZBUFFER)
		mask |= GL_DEPTH_BUFFER_BIT;
	if (flags & D3DCLEAR_STENCIL)
		mask |= GL_STENCIL_BUFFER_BIT;
	if (mask == 0)
		return D3D_OK;

	const ClearTimer timer;

	const UINT targetHeight = m_presentation.BackBufferHeight;

	// The clear is confined to the viewport, and to the rectangles if any are given.
	const D3DVIEWPORT8 &viewport = m_front.viewport;
	D3DRECT viewportRect;
	viewportRect.x1 = static_cast<LONG>(viewport.X);
	viewportRect.y1 = static_cast<LONG>(viewport.Y);
	viewportRect.x2 = static_cast<LONG>(viewport.X + viewport.Width);
	viewportRect.y2 = static_cast<LONG>(viewport.Y + viewport.Height);

	const bool wholeTarget = viewportRect.x1 == 0 && viewportRect.y1 == 0
		&& static_cast<UINT>(viewportRect.x2) == m_presentation.BackBufferWidth
		&& static_cast<UINT>(viewportRect.y2) == targetHeight;

	if ((rects == nullptr || rect_count == 0) && wholeTarget)
	{
		RenderThread::Defer([this, mask, color, z, stencil] { ClearTarget(mask, color, z, stencil, nullptr); });
		return D3D_OK;
	}

	const D3DRECT *list = rects;
	UINT count = rect_count;
	if (rects == nullptr || rect_count == 0)
	{
		list = &viewportRect;
		count = 1;
	}

	for (UINT i = 0; i < count; ++i)
	{
		LONG x1 = list[i].x1 > viewportRect.x1 ? list[i].x1 : viewportRect.x1;
		LONG y1 = list[i].y1 > viewportRect.y1 ? list[i].y1 : viewportRect.y1;
		LONG x2 = list[i].x2 < viewportRect.x2 ? list[i].x2 : viewportRect.x2;
		LONG y2 = list[i].y2 < viewportRect.y2 ? list[i].y2 : viewportRect.y2;
		if (x2 <= x1 || y2 <= y1)
			continue;

		// Direct3D rectangles have their origin at the top, OpenGL's scissor at the bottom.
		ClearBox box;
		box.x = static_cast<GLint>(x1 * m_displayScaleX);
		box.y = static_cast<GLint>((static_cast<LONG>(targetHeight) - y2) * m_displayScaleY);
		box.width = static_cast<GLsizei>((x2 - x1) * m_displayScaleX + 0.5f);
		box.height = static_cast<GLsizei>((y2 - y1) * m_displayScaleY + 0.5f);
		RenderThread::Defer([this, mask, color, z, stencil, box] { ClearTarget(mask, color, z, stencil, &box); });
	}

	return D3D_OK;
}

///////////////////////////////////////////////////////////////////////////////
// State recording
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{
	if (matrix == nullptr)
		return D3DERR_INVALIDCALL;

	// The other world matrices belong to vertex blending, which is not offered.
	D3DMATRIX *current = m_front.GetMatrix(state);
	if (current == nullptr)
		return D3D_OK;

	// The engine sets the same matrices again for every object it draws. Setting one that
	// did not change must not cost anything: each dirty group is paid for per draw call.
	if (memcmp(current, matrix, sizeof(*current)) == 0)
		return D3D_OK;

	const D3DMATRIX copy = *matrix;
	*current = copy;

	// Where vitaGL does not generate texture coordinates itself, the texture matrices of the
	// stages that have them generated contain the world and view matrices (see
	// ApplyTextureMatrix).
#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
	const uint32_t texGenDirty = 0;
#else
	const uint32_t texGenDirty = m_texGenStages << 16;
#endif

	uint32_t dirty;
	if (state == D3DTS_WORLD)
	{
		dirty = DIRTY_TRANSFORM | texGenDirty;
	}
	else if (state == D3DTS_VIEW)
	{
		// Light sources and clip planes are given in world space and stored by OpenGL in
		// eye space, so they follow the view.
		dirty = DIRTY_TRANSFORM | DIRTY_LIGHT_SOURCES | DIRTY_CLIP_PLANES | texGenDirty;
	}
	else if (state == D3DTS_PROJECTION)
	{
		dirty = DIRTY_TRANSFORM;
	}
	else
	{
		dirty = DIRTY_TEXTURE_0 << (static_cast<UINT>(state) - D3DTS_TEXTURE0);
	}

	RenderThread::Defer([this, state, copy, dirty]
	{
		*m_back.GetMatrix(state) = copy;
		m_dirty |= dirty;
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
	if (pMatrix == nullptr)
		return D3DERR_INVALIDCALL;

	const D3DMATRIX *matrix = m_front.GetMatrix(State);
	*pMatrix = matrix != nullptr ? *matrix : GetIdentityMatrix();
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{
	if (matrix == nullptr)
		return D3DERR_INVALIDCALL;

	D3DMATRIX current;
	GetTransform(state, &current);

	D3DMATRIX product;
	MultiplyMatrix(product, *matrix, current);
	return SetTransform(state, &product);
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetViewport(const D3DVIEWPORT8 *viewport)
{
	if (viewport == nullptr)
		return D3DERR_INVALIDCALL;

	const D3DVIEWPORT8 copy = *viewport;
	m_front.viewport = copy;

	// The projection carries a half pixel offset that depends on the viewport size.
	RenderThread::Defer([this, copy]
	{
		m_back.viewport = copy;
		m_dirty |= DIRTY_VIEWPORT | DIRTY_TRANSFORM;
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetViewport(D3DVIEWPORT8 *pViewport)
{
	if (pViewport == nullptr)
		return D3DERR_INVALIDCALL;

	*pViewport = m_front.viewport;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetMaterial(const D3DMATERIAL8 *material)
{
	if (material == nullptr)
		return D3DERR_INVALIDCALL;

	if (memcmp(&m_front.material, material, sizeof(m_front.material)) == 0)
		return D3D_OK;

	const D3DMATERIAL8 copy = *material;
	m_front.material = copy;

	RenderThread::Defer([this, copy]
	{
		m_back.material = copy;
		m_dirty |= DIRTY_LIGHTING;
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetMaterial(D3DMATERIAL8 *pMaterial)
{
	if (pMaterial == nullptr)
		return D3DERR_INVALIDCALL;

	*pMaterial = m_front.material;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetLight(DWORD index, const D3DLIGHT8 *light)
{
	if (light == nullptr)
		return D3DERR_INVALIDCALL;

	if (index >= MAX_LIGHTS)
	{
		LogUnsupportedOnce("more than eight light sources");
		return D3D_OK;
	}

	// The engine sets its light sources again for every object, mostly to the same sun.
	if (memcmp(&m_front.lights[index], light, sizeof(m_front.lights[index])) == 0)
		return D3D_OK;

	const D3DLIGHT8 copy = *light;
	m_front.lights[index] = copy;

	RenderThread::Defer([this, index, copy]
	{
		m_back.lights[index] = copy;
		m_dirty |= DIRTY_LIGHT_SOURCES;
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetLight(DWORD Index, D3DLIGHT8 *pLight)
{
	if (pLight == nullptr || Index >= MAX_LIGHTS)
		return D3DERR_INVALIDCALL;

	*pLight = m_front.lights[Index];
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::LightEnable(DWORD Index, WINBOOL Enable)
{
	if (Index >= MAX_LIGHTS)
		return D3D_OK;

	const bool enable = Enable != FALSE;
	if (m_front.lightEnabled[Index] != enable)
	{
		m_front.lightEnabled[Index] = enable;

		RenderThread::Defer([this, Index, enable]
		{
			m_back.lightEnabled[Index] = enable;
			m_dirty |= DIRTY_LIGHT_SOURCES;
		});
	}
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetLightEnable(DWORD Index, WINBOOL *pEnable)
{
	if (pEnable == nullptr || Index >= MAX_LIGHTS)
		return D3DERR_INVALIDCALL;

	*pEnable = m_front.lightEnabled[Index] ? TRUE : FALSE;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetClipPlane(DWORD index, const float *plane)
{
	if (plane == nullptr || index >= MAX_CLIP_PLANES)
		return D3DERR_INVALIDCALL;

	struct Plane
	{
		float equation[4];
	};
	Plane copy;
	memcpy(copy.equation, plane, sizeof(copy.equation));
	memcpy(m_front.clipPlanes[index], copy.equation, sizeof(copy.equation));

	RenderThread::Defer([this, index, copy]
	{
		memcpy(m_back.clipPlanes[index], copy.equation, sizeof(copy.equation));
		m_dirty |= DIRTY_CLIP_PLANES;
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetClipPlane(DWORD Index, float *pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
		return D3DERR_INVALIDCALL;

	memcpy(pPlane, m_front.clipPlanes[Index], sizeof(m_front.clipPlanes[Index]));
	return D3D_OK;
}

//-----------------------------------------------------------------------------
uint32_t Device::GetRenderStateDirtyFlags(D3DRENDERSTATETYPE state) const
{
	switch (state)
	{
		case D3DRS_ZENABLE:
		case D3DRS_ZWRITEENABLE:
		case D3DRS_ZFUNC:
		case D3DRS_ZBIAS:
			return DIRTY_DEPTH;

		case D3DRS_ALPHABLENDENABLE:
		case D3DRS_SRCBLEND:
		case D3DRS_DESTBLEND:
		case D3DRS_BLENDOP:
			return DIRTY_BLEND;

		case D3DRS_ALPHATESTENABLE:
		case D3DRS_ALPHAREF:
		case D3DRS_ALPHAFUNC:
			return DIRTY_ALPHA_TEST;

		case D3DRS_CULLMODE:
		case D3DRS_FILLMODE:
		case D3DRS_SHADEMODE:
		case D3DRS_COLORWRITEENABLE:
			return DIRTY_RASTER;

		case D3DRS_STENCILENABLE:
		case D3DRS_STENCILFAIL:
		case D3DRS_STENCILZFAIL:
		case D3DRS_STENCILPASS:
		case D3DRS_STENCILFUNC:
		case D3DRS_STENCILREF:
		case D3DRS_STENCILMASK:
		case D3DRS_STENCILWRITEMASK:
			return DIRTY_STENCIL;

		case D3DRS_FOGENABLE:
		case D3DRS_FOGCOLOR:
		case D3DRS_FOGTABLEMODE:
		case D3DRS_FOGVERTEXMODE:
		case D3DRS_FOGSTART:
		case D3DRS_FOGEND:
		case D3DRS_FOGDENSITY:
			return DIRTY_FOG;

		case D3DRS_LIGHTING:
		case D3DRS_AMBIENT:
		case D3DRS_COLORVERTEX:
		case D3DRS_SPECULARENABLE:
		case D3DRS_NORMALIZENORMALS:
		case D3DRS_DIFFUSEMATERIALSOURCE:
		case D3DRS_AMBIENTMATERIALSOURCE:
		case D3DRS_SPECULARMATERIALSOURCE:
		case D3DRS_EMISSIVEMATERIALSOURCE:
			return DIRTY_LIGHTING;

		case D3DRS_CLIPPLANEENABLE:
			return DIRTY_CLIP_PLANES;

		case D3DRS_TEXTUREFACTOR:
			// The factor is the constant color of every texture stage.
			return DIRTY_ALL_TEXTURES;

		default:
			return 0;
	}
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	if (static_cast<UINT>(State) >= RENDER_STATE_COUNT)
		return D3DERR_INVALIDCALL;

	if (m_front.renderStates[State] != Value)
	{
		m_front.renderStates[State] = Value;

		const uint32_t dirty = GetRenderStateDirtyFlags(State);
		RenderThread::Defer([this, State, Value, dirty]
		{
			m_back.renderStates[State] = Value;
			m_dirty |= dirty;
		});
	}
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
	if (pValue == nullptr || static_cast<UINT>(State) >= RENDER_STATE_COUNT)
		return D3DERR_INVALIDCALL;

	*pValue = m_front.renderStates[State];
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::BeginStateBlock()
{
	LogUnsupportedOnce("state blocks");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::EndStateBlock(DWORD *pToken)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::ApplyStateBlock(DWORD Token)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CaptureStateBlock(DWORD Token)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DeleteStateBlock(DWORD Token)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD *pToken)
{
	LogUnsupportedOnce("state blocks");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetClipStatus(const D3DCLIPSTATUS8 *clip_status)
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetClipStatus(D3DCLIPSTATUS8 *pClipStatus)
{
	if (pClipStatus == nullptr)
		return D3DERR_INVALIDCALL;

	memset(pClipStatus, 0, sizeof(*pClipStatus));
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
	if (ppTexture == nullptr || Stage >= MAX_D3D_TEXTURE_STAGES)
		return D3DERR_INVALIDCALL;

	*ppTexture = m_textures[Stage];
	if (m_textures[Stage] != nullptr)
		m_textures[Stage]->AddRef();
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
	if (Stage >= MAX_D3D_TEXTURE_STAGES)
		return D3DERR_INVALIDCALL;

	// Only two dimensional textures can be created, so that is what every base texture is.
	Texture *texture = static_cast<Texture *>(pTexture);
	if (m_textures[Stage] == texture)
		return D3D_OK;

	if (texture != nullptr)
		texture->AddRef();
	if (m_textures[Stage] != nullptr)
		m_textures[Stage]->Release();

	m_textures[Stage] = texture;

	// The render thread is told with the next draw call that uses the stage: only then is
	// it known which GL texture this one is (see SendTextures).
	if (Stage < MAX_TEXTURE_STAGES)
		m_texturesToSend |= 1u << Stage;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
	if (pValue == nullptr || Stage >= MAX_D3D_TEXTURE_STAGES || static_cast<UINT>(Type) >= TEXTURE_STAGE_STATE_COUNT)
		return D3DERR_INVALIDCALL;

	*pValue = m_front.textureStageStates[Stage][Type];
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	if (Stage >= MAX_D3D_TEXTURE_STAGES || static_cast<UINT>(Type) >= TEXTURE_STAGE_STATE_COUNT)
		return D3DERR_INVALIDCALL;

	if (m_front.textureStageStates[Stage][Type] == Value)
		return D3D_OK;

	m_front.textureStageStates[Stage][Type] = Value;

	if (Type == D3DTSS_COLOROP)
		m_colorOpChanged = true;

	if (Type == D3DTSS_TEXCOORDINDEX)
	{
		// The upper word selects coordinates generated from the position or the normal.
		if ((Value & 0xFFFF0000u) != 0)
			m_texGenStages |= 1u << Stage;
		else
			m_texGenStages &= ~(1u << Stage);
	}

	// Disabling a stage's color operation ends the cascade, which changes what the
	// stages after it do as well.
	const uint32_t dirty = Type == D3DTSS_COLOROP ? static_cast<uint32_t>(DIRTY_ALL_TEXTURES) : DIRTY_TEXTURE_0 << Stage;
	RenderThread::Defer([this, Stage, Type, Value, dirty]
	{
		m_back.textureStageStates[Stage][Type] = Value;
		m_dirty |= dirty;
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::ValidateDevice(DWORD *pNumPasses)
{
	if (pNumPasses != nullptr)
		*pNumPasses = 1;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetInfo(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize)
{
	return S_FALSE;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetCurrentTexturePalette(UINT PaletteNumber)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetCurrentTexturePalette(UINT *PaletteNumber)
{
	return D3DERR_INVALIDCALL;
}

///////////////////////////////////////////////////////////////////////////////
// Drawing
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
bool Device::GetPrimitiveMode(D3DPRIMITIVETYPE type, UINT primitiveCount, GLenum &mode, UINT &vertexCount)
{
	switch (type)
	{
		case D3DPT_POINTLIST:
			mode = GL_POINTS;
			vertexCount = primitiveCount;
			return true;

		case D3DPT_LINELIST:
			mode = GL_LINES;
			vertexCount = primitiveCount * 2;
			return true;

		case D3DPT_LINESTRIP:
			mode = GL_LINE_STRIP;
			vertexCount = primitiveCount + 1;
			return true;

		case D3DPT_TRIANGLELIST:
			mode = GL_TRIANGLES;
			vertexCount = primitiveCount * 3;
			return true;

		case D3DPT_TRIANGLESTRIP:
			mode = GL_TRIANGLE_STRIP;
			vertexCount = primitiveCount + 2;
			return true;

		case D3DPT_TRIANGLEFAN:
			mode = GL_TRIANGLE_FAN;
			vertexCount = primitiveCount + 2;
			return true;

		default:
			return false;
	}
}

//-----------------------------------------------------------------------------
bool Device::PrepareStreamDraw(const VertexLayout *&layout, const uint8_t *&vertices, GLuint &glBuffer, UINT firstVertex)
{
	if (m_streamBuffer == nullptr)
		return false;

	if (m_vertexShader & D3DFVF_RESERVED0)
	{
		// Shader handles are odd numbers; flexible vertex format codes are even.
		LogUnsupportedOnce("drawing with a programmable vertex shader");
		return false;
	}

	// The stride of the stream is what the application says it is; it may pad its vertices.
	layout = GetCachedLayout(m_vertexShader, m_streamStride);
	if (!layout->hasPosition)
		return false;

	m_streamBuffer->PrepareForDraw(*layout);

	BufferStorage &storage = m_streamBuffer->GetStorage();
	const UINT offset = firstVertex * layout->stride;
	if (storage.IsClientSide())
	{
		glBuffer = 0;
		vertices = storage.GetSystemCopy() + offset;
	}
	else
	{
		glBuffer = storage.GetGLBuffer();
		vertices = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(offset));
	}

	return true;
}

//-----------------------------------------------------------------------------
bool Device::PrepareUserVertices(const VertexLayout *&layout, const void *data, UINT stride, UINT vertexCount)
{
	if (m_vertexShader & D3DFVF_RESERVED0)
	{
		LogUnsupportedOnce("drawing with a programmable vertex shader");
		return false;
	}

	layout = GetCachedLayout(m_vertexShader, stride);
	return layout->hasPosition && data != nullptr && vertexCount != 0;
}

//-----------------------------------------------------------------------------
/// The vertices of a draw call from system memory, copied to where their colors can be put
/// in the order vitaGL reads them: into the stream of the render thread, which does that
/// before it draws, or without a render thread into a block of the device's own. Where they
/// came from stays as it is; the engine draws the same vertices more than once.
const uint8_t *Device::KeepVertices(const uint8_t *source, UINT size)
{
	if (RenderThread::g_queueing)
		return static_cast<const uint8_t *>(RenderThread::Keep(source, size));

	if (size > m_userVerticesSize)
	{
		free(m_userVertices);
		m_userVertices = static_cast<uint8_t *>(malloc(size));
		m_userVerticesSize = m_userVertices != nullptr ? size : 0;
		if (m_userVertices == nullptr)
			return nullptr;
	}

	memcpy(m_userVertices, source, size);
	return m_userVertices;
}

//-----------------------------------------------------------------------------
/// Copies indices so that the lowest one becomes 0, and reports the range they covered.
const void *Device::RebaseIndices(const uint8_t *source, UINT indexCount, UINT indexSize, UINT &lowest, UINT &highest)
{
	const UINT size = indexCount * indexSize;
	if (size > m_indexScratchSize)
	{
		free(m_indexScratch);
		m_indexScratch = static_cast<uint8_t *>(malloc(size));
		m_indexScratchSize = m_indexScratch != nullptr ? size : 0;
		if (m_indexScratch == nullptr)
			return nullptr;
	}

	lowest = 0xFFFFFFFFu;
	highest = 0;

	if (indexSize == 2)
	{
		const uint16_t *from = reinterpret_cast<const uint16_t *>(source);
		uint16_t *to = reinterpret_cast<uint16_t *>(m_indexScratch);
		for (UINT i = 0; i < indexCount; ++i)
		{
			if (from[i] < lowest)
				lowest = from[i];
			if (from[i] > highest)
				highest = from[i];
		}
		for (UINT i = 0; i < indexCount; ++i)
			to[i] = static_cast<uint16_t>(from[i] - lowest);
	}
	else
	{
		const uint32_t *from = reinterpret_cast<const uint32_t *>(source);
		uint32_t *to = reinterpret_cast<uint32_t *>(m_indexScratch);
		for (UINT i = 0; i < indexCount; ++i)
		{
			if (from[i] < lowest)
				lowest = from[i];
			if (from[i] > highest)
				highest = from[i];
		}
		for (UINT i = 0; i < indexCount; ++i)
			to[i] = from[i] - lowest;
	}

	return m_indexScratch;
}

//-----------------------------------------------------------------------------
void Device::SendTextures()
{
	// Nearly every draw call finds the stages as the one before left them: no texture was
	// set, none was written to, and the same stages take part.
	if (m_texturesToSend == 0 && !m_colorOpChanged && m_textureWritesSeen == g_textureWrites)
		return;

	m_colorOpChanged = false;
	m_textureWritesSeen = g_textureWrites;

	for (UINT stage = 0; stage < MAX_TEXTURE_STAGES; ++stage)
	{
		// A disabled color operation ends the cascade, and the render thread does not look
		// at the textures of the stages from there on.
		if (m_front.textureStageStates[stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
			break;

		// A bound texture may have been locked and rewritten since it was last drawn with.
		// It is uploaded then, which can give it another GL texture, and the stage is set
		// up again as for a texture that was just set.
		Texture *texture = m_textures[stage];
		const bool written = texture != nullptr && texture->IsDirty();
		const GLuint glTexture = texture != nullptr ? texture->Realize() : 0;

		const uint32_t bit = 1u << stage;
		if (!written && (m_texturesToSend & bit) == 0 && glTexture == m_sentTextures[stage])
			continue;

		// A texture that was never written to is drawn as if there were none.
		BoundTexture bound = {};
		if (glTexture != 0)
		{
			bound.glTexture = glTexture;
			bound.sampler = texture->GetAppliedSamplerState();
			bound.hasMipMaps = texture->HasMipMaps();
			bound.format = static_cast<DWORD>(texture->GetFormat().format);
		}

		m_texturesToSend &= ~bit;
		m_sentTextures[stage] = glTexture;

		RenderThread::Defer([this, stage, bound]
		{
			m_boundTextures[stage] = bound;
			m_dirty |= DIRTY_TEXTURE_0 << stage;
		});
	}
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	GLenum mode;
	UINT vertexCount;
	if (PrimitiveCount == 0 || !GetPrimitiveMode(PrimitiveType, PrimitiveCount, mode, vertexCount))
		return D3D_OK;

	const DrawTimer timer(PrimitiveCount);

	const VertexLayout *cachedLayout = nullptr;
	const uint8_t *vertices;
	GLuint glBuffer;
	if (!PrepareStreamDraw(cachedLayout, vertices, glBuffer, StartVertex))
		return D3DERR_INVALIDCALL;
	const VertexLayout &layout = *cachedLayout;

	SendTextures();

	// The engine goes on to write the next vertices into the same buffer.
	bool swapColors = false;
	if (glBuffer == 0 && m_streamBuffer->DrawFromMemory(layout))
	{
		++g_frameStatistics.directDrawCalls;
		g_frameStatistics.directVertexBytes += vertexCount * layout.stride;
	}
	else if (glBuffer == 0)
	{
		++g_frameStatistics.copyingDrawCalls;
		g_frameStatistics.copiedVertexBytes += vertexCount * layout.stride;
		if (m_streamBuffer->GetStorage().NoteDraw())
			g_frameStatistics.copiedUnchangedVertexBytes += vertexCount * layout.stride;

		vertices = KeepVertices(vertices, vertexCount * layout.stride);
		if (vertices == nullptr)
			return E_OUTOFMEMORY;
		swapColors = HasColors(layout);
	}

	const GLsizei count = static_cast<GLsizei>(vertexCount);
	RenderThread::Defer([this, cachedLayout, vertices, glBuffer, mode, count, swapColors]
	{
		DrawArrays(*cachedLayout, vertices, glBuffer, mode, count, swapColors);
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices, UINT startIndex, UINT primCount)
{
	GLenum mode;
	UINT indexCount;
	if (primCount == 0 || NumVertices == 0 || !GetPrimitiveMode(PrimitiveType, primCount, mode, indexCount))
		return D3D_OK;

	if (m_indexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	const DrawTimer timer(primCount);

	// The base vertex index is folded into the array pointers.
	const VertexLayout *cachedLayout = nullptr;
	const uint8_t *vertices;
	GLuint glBuffer;
	if (!PrepareStreamDraw(cachedLayout, vertices, glBuffer, m_baseVertexIndex))
		return D3DERR_INVALIDCALL;
	const VertexLayout &layout = *cachedLayout;

	BufferStorage &indexStorage = m_indexBuffer->GetStorage();
	const UINT indexSize = m_indexBuffer->GetIndexSize();
	const GLenum indexType = indexSize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
	const UINT indexOffset = startIndex * indexSize;
	const GLsizei count = static_cast<GLsizei>(indexCount);

	SendTextures();

	if (glBuffer == 0)
	{
		// For vertices in system memory vitaGL copies everything from the start of the
		// arrays up to the highest index of the draw call, once per vertex attribute. The
		// engine draws its sorted and translucent geometry (effects, particles) as many
		// small ranges of one large buffer, which made vitaGL copy that buffer over and
		// over: more than 100 ms per frame during an explosion. So the arrays start at the
		// lowest vertex the call uses, and the indices are moved down to match. The range
		// is taken from the indices themselves; the caller's numbers are only a promise.
		UINT lowest;
		UINT highest;
		const void *rebased = RebaseIndices(indexStorage.GetSystemCopy() + indexOffset, indexCount, indexSize, lowest, highest);
		if (rebased == nullptr)
			return E_OUTOFMEMORY;

		// The engine goes on to write the next vertices into the same buffers, and the
		// indices are in a block that the next draw call reuses. If it writes the vertices
		// behind these, the vertices can stay where they are.
		const UINT usedBytes = (highest - lowest + 1) * layout.stride;
		const uint8_t *used = vertices + lowest * layout.stride;
		bool swapColors = false;
		if (m_streamBuffer->DrawFromMemory(layout))
		{
			++g_frameStatistics.directDrawCalls;
			g_frameStatistics.directVertexBytes += usedBytes;
		}
		else
		{
			++g_frameStatistics.copyingDrawCalls;
			g_frameStatistics.copiedVertexBytes += usedBytes;
			if (m_streamBuffer->GetStorage().NoteDraw())
				g_frameStatistics.copiedUnchangedVertexBytes += usedBytes;

			used = KeepVertices(used, usedBytes);
			if (used == nullptr)
				return E_OUTOFMEMORY;
			swapColors = HasColors(layout);
		}

		g_frameStatistics.copiedIndexBytes += indexCount * indexSize;
		rebased = RenderThread::Keep(rebased, indexCount * indexSize);

		const GLuint last = highest - lowest;
		RenderThread::Defer([this, cachedLayout, used, mode, last, count, indexType, rebased, swapColors]
		{
			DrawElements(*cachedLayout, used, 0, 0, mode, 0, last, count, indexType, rebased, swapColors);
		});
		return D3D_OK;
	}

	GLuint glIndexBuffer = 0;
	const void *indices;
	if (indexStorage.IsClientSide())
	{
		g_frameStatistics.copiedIndexBytes += indexCount * indexSize;
		indices = RenderThread::Keep(indexStorage.GetSystemCopy() + indexOffset, indexCount * indexSize);
	}
	else
	{
		glIndexBuffer = indexStorage.GetGLBuffer();
		indices = reinterpret_cast<const void *>(static_cast<uintptr_t>(indexOffset));
	}

	const GLuint first = minIndex;
	const GLuint last = minIndex + NumVertices - 1;
	RenderThread::Defer([this, cachedLayout, vertices, glBuffer, glIndexBuffer, mode, first, last, count, indexType, indices]
	{
		DrawElements(*cachedLayout, vertices, glBuffer, glIndexBuffer, mode, first, last, count, indexType, indices, false);
	});
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride)
{
	GLenum mode;
	UINT vertexCount;
	if (primitive_count == 0 || !GetPrimitiveMode(primitive_type, primitive_count, mode, vertexCount))
		return D3D_OK;

	const DrawTimer timer(primitive_count);

	const VertexLayout *cachedLayout = nullptr;
	if (!PrepareUserVertices(cachedLayout, data, stride, vertexCount))
		return D3DERR_INVALIDCALL;
	const VertexLayout &layout = *cachedLayout;

	SendTextures();

	++g_frameStatistics.copyingDrawCalls;
	g_frameStatistics.copiedVertexBytes += vertexCount * layout.stride;

	const uint8_t *vertices = KeepVertices(static_cast<const uint8_t *>(data), vertexCount * layout.stride);
	if (vertices == nullptr)
		return E_OUTOFMEMORY;

	const GLsizei count = static_cast<GLsizei>(vertexCount);
	const bool swapColors = HasColors(layout);
	RenderThread::Defer([this, cachedLayout, vertices, mode, count, swapColors]
	{
		DrawArrays(*cachedLayout, vertices, 0, mode, count, swapColors);
	});

	// Direct3D unsets the stream source after a draw from user memory.
	SetStreamSource(0, nullptr, 0);
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, D3DFORMAT index_format, const void *data, UINT stride)
{
	GLenum mode;
	UINT indexCount;
	if (primitive_count == 0 || vertex_count == 0 || index_data == nullptr
		|| !GetPrimitiveMode(primitive_type, primitive_count, mode, indexCount))
		return D3D_OK;

	const DrawTimer timer(primitive_count);

	// The indices address the vertices from the start of the array.
	const VertexLayout *cachedLayout = nullptr;
	if (!PrepareUserVertices(cachedLayout, data, stride, min_vertex_idx + vertex_count))
		return D3DERR_INVALIDCALL;
	const VertexLayout &layout = *cachedLayout;

	SendTextures();

	const UINT indexSize = index_format == D3DFMT_INDEX32 ? 4 : 2;
	const GLenum indexType = indexSize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
	++g_frameStatistics.copyingDrawCalls;
	g_frameStatistics.copiedVertexBytes += (min_vertex_idx + vertex_count) * layout.stride;
	g_frameStatistics.copiedIndexBytes += indexCount * indexSize;

	const uint8_t *vertices = KeepVertices(static_cast<const uint8_t *>(data), (min_vertex_idx + vertex_count) * layout.stride);
	if (vertices == nullptr)
		return E_OUTOFMEMORY;
	const void *indices = RenderThread::Keep(index_data, indexCount * indexSize);

	const GLuint first = min_vertex_idx;
	const GLuint last = min_vertex_idx + vertex_count - 1;
	const GLsizei count = static_cast<GLsizei>(indexCount);
	const bool swapColors = HasColors(layout);
	RenderThread::Defer([this, cachedLayout, vertices, mode, first, last, count, indexType, indices, swapColors]
	{
		DrawElements(*cachedLayout, vertices, 0, 0, mode, first, last, count, indexType, indices, swapColors);
	});

	SetStreamSource(0, nullptr, 0);
	SetIndices(nullptr, 0);
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags)
{
	LogUnsupportedOnce("ProcessVertices");
	return D3DERR_INVALIDCALL;
}

///////////////////////////////////////////////////////////////////////////////
// Shaders and streams
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const DWORD *declaration, const DWORD *byte_code, DWORD *shader, DWORD usage)
{
	if (shader != nullptr)
		*shader = 0;

	LogUnsupportedOnce("vertex shaders");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetVertexShader(DWORD Handle)
{
	m_vertexShader = Handle;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetVertexShader(DWORD *pHandle)
{
	if (pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = m_vertexShader;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DeleteVertexShader(DWORD Handle)
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstant(DWORD reg_idx, const void *data, DWORD count)
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
	if (StreamNumber >= MAX_STREAMS)
	{
		if (pStreamData != nullptr)
			LogUnsupportedOnce("more than one vertex stream");
		return D3D_OK;
	}

	VertexBuffer *buffer = static_cast<VertexBuffer *>(pStreamData);
	if (buffer != m_streamBuffer)
	{
		if (buffer != nullptr)
			buffer->AddRef();
		if (m_streamBuffer != nullptr)
			m_streamBuffer->Release();
		m_streamBuffer = buffer;
	}

	m_streamStride = Stride;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
	if (ppStreamData == nullptr || StreamNumber >= MAX_STREAMS)
		return D3DERR_INVALIDCALL;

	*ppStreamData = m_streamBuffer;
	if (m_streamBuffer != nullptr)
		m_streamBuffer->AddRef();
	if (pStride != nullptr)
		*pStride = m_streamStride;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
	IndexBuffer *buffer = static_cast<IndexBuffer *>(pIndexData);
	if (buffer != m_indexBuffer)
	{
		if (buffer != nullptr)
			buffer->AddRef();
		if (m_indexBuffer != nullptr)
			m_indexBuffer->Release();
		m_indexBuffer = buffer;
	}

	m_baseVertexIndex = BaseVertexIndex;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetIndices(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
	if (ppIndexData == nullptr)
		return D3DERR_INVALIDCALL;

	*ppIndexData = m_indexBuffer;
	if (m_indexBuffer != nullptr)
		m_indexBuffer->AddRef();
	if (pBaseVertexIndex != nullptr)
		*pBaseVertexIndex = m_baseVertexIndex;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const DWORD *byte_code, DWORD *shader)
{
	if (shader != nullptr)
		*shader = 0;

	LogUnsupportedOnce("pixel shaders");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetPixelShader(DWORD Handle)
{
	if (Handle != 0)
		LogUnsupportedOnce("SetPixelShader with a shader");

	m_pixelShader = Handle;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetPixelShader(DWORD *pHandle)
{
	if (pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = m_pixelShader;
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DeletePixelShader(DWORD Handle)
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstant(DWORD reg_idx, const void *data, DWORD count)
{
	return D3D_OK;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DrawRectPatch(UINT handle, const float *segment_count, const D3DRECTPATCH_INFO *patch_info)
{
	LogUnsupportedOnce("higher order patches");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DrawTriPatch(UINT handle, const float *segment_count, const D3DTRIPATCH_INFO *patch_info)
{
	LogUnsupportedOnce("higher order patches");
	return D3DERR_INVALIDCALL;
}

//-----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device::DeletePatch(UINT Handle)
{
	return D3D_OK;
}

} // namespace VitaD3D8
