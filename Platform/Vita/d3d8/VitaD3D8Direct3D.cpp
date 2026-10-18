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

// VitaD3D8Direct3D.cpp ///////////////////////////////////////////////////////
// IDirect3D8: the one adapter of the console, its display modes and the
// capabilities the engine sizes its rendering paths by. Also the entry point
// Direct3DCreate8() and the logging helpers.
///////////////////////////////////////////////////////////////////////////////

#include "VitaD3D8Device.h"

#include <pthread.h>

namespace VitaD3D8
{

namespace
{

Device *s_device = nullptr;

// The panel has one resolution. It is offered in the two color depths the engine asks for.
const D3DDISPLAYMODE s_displayModes[] =
{
	{ DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_REFRESH_RATE, D3DFMT_X8R8G8B8 },
	{ DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_REFRESH_RATE, D3DFMT_R5G6B5 },
};

bool IsDisplayFormat(D3DFORMAT format)
{
	return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 || format == D3DFMT_R5G6B5;
}

} // namespace

//-----------------------------------------------------------------------------
void Log(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fputs("[d3d8] ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

//-----------------------------------------------------------------------------
void LogUnsupportedOnce(const char *what)
{
	// Callers pass string literals, so the address identifies the message.
	enum { MAX_MESSAGES = 96 };
	static const char *reported[MAX_MESSAGES];
	static int count = 0;

	// The game's thread and the render thread both come here.
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&lock);

	bool known = false;
	for (int i = 0; i < count && !known; ++i)
		known = reported[i] == what;
	if (!known && count < MAX_MESSAGES)
		reported[count++] = what;

	pthread_mutex_unlock(&lock);

	if (!known)
		Log("not supported: %s", what);
}

//-----------------------------------------------------------------------------
Device *GetDevice()
{
	return s_device;
}

//-----------------------------------------------------------------------------
HRESULT GetDeviceInterface(IDirect3DDevice8 **ppDevice)
{
	if (ppDevice == nullptr)
		return D3DERR_INVALIDCALL;

	*ppDevice = s_device;
	if (s_device == nullptr)
		return D3DERR_INVALIDCALL;

	s_device->AddRef();
	return D3D_OK;
}

//-----------------------------------------------------------------------------
/// Called by the device when it is destroyed.
void ClearDevice(Device *device)
{
	if (s_device == device)
		s_device = nullptr;
}

//-----------------------------------------------------------------------------
void FillDeviceCaps(D3DCAPS8 &caps)
{
	memset(&caps, 0, sizeof(caps));

	caps.DeviceType = D3DDEVTYPE_HAL;
	caps.AdapterOrdinal = 0;
	caps.PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;

	caps.DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION
		| D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX
		| D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY
		| D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_EXECUTESYSTEMMEMORY;

	caps.PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW
		| D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP;

	// D3DPRASTERCAPS_ZBIAS is deliberately absent: without it WW3D biases coplanar
	// geometry by shifting the projection, which does not depend on how a GPU scales
	// polygon offsets.
	caps.RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX
		| D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_WFOG;

	const DWORD compareCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL
		| D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL
		| D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
	caps.ZCmpCaps = compareCaps;
	caps.AlphaCmpCaps = compareCaps;

	const DWORD blendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR
		| D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA
		| D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR
		| D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT;
	caps.SrcBlendCaps = blendCaps;
	caps.DestBlendCaps = blendCaps;

	caps.ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB
		| D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;

	// Power of two sizes only: vitaGL builds mip chains for power of two dimensions.
	caps.TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA
		| D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_POW2;

	caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR
		| D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR
		| D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;

	caps.TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP;

	caps.LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND
		| D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;

	caps.MaxTextureWidth = ADVERTISED_TEXTURE_SIZE;
	caps.MaxTextureHeight = ADVERTISED_TEXTURE_SIZE;
	caps.MaxTextureRepeat = 8192;
	caps.MaxTextureAspectRatio = ADVERTISED_TEXTURE_SIZE;
	caps.MaxAnisotropy = 1;
	caps.MaxVertexW = 1.0e10f;

	caps.StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE
		| D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT
		| D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;

	// The low word is the number of texture coordinate sets a vertex may carry.
	caps.FVFCaps = D3DFVFCAPS_DONOTSTRIPELEMENTS | 8;

	caps.TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2
		| D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X
		| D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X
		| D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH
		| D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA
		| D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDCURRENTALPHA;

	caps.MaxTextureBlendStages = MAX_TEXTURE_STAGES;
	caps.MaxSimultaneousTextures = MAX_TEXTURE_STAGES;

	caps.VertexProcessingCaps = D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS
		| D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_LOCALVIEWER;
	caps.MaxActiveLights = MAX_LIGHTS;
	caps.MaxUserClipPlanes = MAX_CLIP_PLANES;

	// A point size of one tells the engine there are no point sprites.
	caps.MaxPointSize = 1.0f;

	caps.MaxPrimitiveCount = 0xFFFF;
	caps.MaxVertexIndex = 0xFFFF;
	caps.MaxStreams = MAX_STREAMS;
	caps.MaxStreamStride = 255;

	// No programmable shaders: the engine takes its fixed function paths.
	caps.VertexShaderVersion = D3DVS_VERSION(0, 0);
	caps.PixelShaderVersion = D3DPS_VERSION(0, 0);
}

//-----------------------------------------------------------------------------
class Direct3D : public Unknown<IDirect3D8>
{
public:
	HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) override
	{
		return D3DERR_INVALIDCALL;
	}

	UINT STDMETHODCALLTYPE GetAdapterCount() override
	{
		return 1;
	}

	HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *pIdentifier) override
	{
		if (Adapter != 0 || pIdentifier == nullptr)
			return D3DERR_INVALIDCALL;

		memset(pIdentifier, 0, sizeof(*pIdentifier));
		strcpy(pIdentifier->Driver, "vitaGL");
		strcpy(pIdentifier->Description, "PlayStation Vita (PowerVR SGX543MP4+)");
		pIdentifier->VendorId = 0x1010;	// Imagination Technologies
		pIdentifier->DeviceId = 0x0543;
		pIdentifier->DriverVersion.QuadPart = 1;
		return D3D_OK;
	}

	UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter) override
	{
		return Adapter == 0 ? sizeof(s_displayModes) / sizeof(s_displayModes[0]) : 0;
	}

	HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE *pMode) override
	{
		if (Adapter != 0 || pMode == nullptr || Mode >= sizeof(s_displayModes) / sizeof(s_displayModes[0]))
			return D3DERR_INVALIDCALL;

		*pMode = s_displayModes[Mode];
		return D3D_OK;
	}

	HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override
	{
		if (Adapter != 0 || pMode == nullptr)
			return D3DERR_INVALIDCALL;

		*pMode = s_displayModes[0];
		return D3D_OK;
	}

	HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, WINBOOL Windowed) override
	{
		if (Adapter != 0 || CheckType != D3DDEVTYPE_HAL)
			return D3DERR_NOTAVAILABLE;

		return IsDisplayFormat(DisplayFormat) && IsDisplayFormat(BackBufferFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
	}

	HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override
	{
		if (Adapter != 0)
			return D3DERR_INVALIDCALL;

		switch (RType)
		{
			case D3DRTYPE_TEXTURE:
				// Rendering into textures and depth textures are not implemented. Saying so
				// here makes the engine skip the effects built on them.
				if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
					return D3DERR_NOTAVAILABLE;
				return IsTextureFormatSupported(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;

			case D3DRTYPE_SURFACE:
				if (Usage & D3DUSAGE_DEPTHSTENCIL)
					return IsDepthFormatSupported(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
				if (Usage & D3DUSAGE_RENDERTARGET)
					return IsDisplayFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
				return GetFormatInfo(CheckFormat) != nullptr ? D3D_OK : D3DERR_NOTAVAILABLE;

			default:
				// Cube and volume textures.
				return D3DERR_NOTAVAILABLE;
		}
	}

	HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, WINBOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) override
	{
		return MultiSampleType == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
	}

	HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override
	{
		return IsDisplayFormat(RenderTargetFormat) && IsDepthFormatSupported(DepthStencilFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
	}

	HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 *pCaps) override
	{
		if (Adapter != 0 || pCaps == nullptr)
			return D3DERR_INVALIDCALL;

		FillDeviceCaps(*pCaps);
		return D3D_OK;
	}

	HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override
	{
		return nullptr;
	}

	HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice8 **ppReturnedDeviceInterface) override
	{
		if (ppReturnedDeviceInterface == nullptr || pPresentationParameters == nullptr)
			return D3DERR_INVALIDCALL;

		*ppReturnedDeviceInterface = nullptr;
		if (Adapter != 0)
			return D3DERR_INVALIDCALL;

		// vitaGL sets up the GPU once per process and has no shutdown. A second device
		// would have nothing to initialize, and the engine never asks for one.
		if (s_device != nullptr)
		{
			LogUnsupportedOnce("a second Direct3D device");
			return D3DERR_INVALIDCALL;
		}

		D3DDEVICE_CREATION_PARAMETERS creation;
		creation.AdapterOrdinal = Adapter;
		creation.DeviceType = DeviceType;
		creation.hFocusWindow = hFocusWindow;
		creation.BehaviorFlags = BehaviorFlags;

		s_device = new Device(this, creation, *pPresentationParameters);
		*ppReturnedDeviceInterface = s_device;

		// The device may have filled in what the caller left open (back buffer size).
		s_device->GetPresentationParameters(*pPresentationParameters);
		return D3D_OK;
	}
};

} // namespace VitaD3D8

//-----------------------------------------------------------------------------
IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion)
{
	VitaD3D8::Log("Direct3DCreate8: Direct3D 8 on vitaGL");
	return new VitaD3D8::Direct3D;
}
