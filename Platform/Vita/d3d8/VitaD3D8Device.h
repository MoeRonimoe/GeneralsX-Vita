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

// VitaD3D8Device.h ///////////////////////////////////////////////////////////
// IDirect3DDevice8 on vitaGL.
//
// The device records the Direct3D state the engine sets and translates it into
// fixed function OpenGL state right before a draw call, one group at a time and
// only for the groups that changed (VitaD3D8State.cpp).
//
// It is two halves. The methods of IDirect3DDevice8 run on the game's thread: they
// keep the state as the engine has set it and write every change, and every draw
// call, into the stream of VitaD3D8RenderThread.h. The other half runs where the
// stream is read, on the render thread: it keeps the state as it was when the
// call it is making was written, translates it and calls vitaGL. Each half has
// its own members; what both read is only written while the other one rests.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "VitaD3D8.h"
#include "VitaD3D8Format.h"
#include "VitaD3D8Resources.h"

namespace VitaD3D8
{

/// Capabilities reported to the engine, shared by IDirect3D8 and the device.
void FillDeviceCaps(D3DCAPS8 &caps);

/// Forget the device; called from its destructor.
void ClearDevice(Device *device);

/// Row vector product a x b, the order in which Direct3D composes transforms.
/// out may be one of the operands.
void MultiplyMatrix(D3DMATRIX &out, const D3DMATRIX &a, const D3DMATRIX &b);
const D3DMATRIX &GetIdentityMatrix();

class Device : public Unknown<IDirect3DDevice8>
{
public:
	Device(IDirect3D8 *parent, const D3DDEVICE_CREATION_PARAMETERS &creation, const D3DPRESENT_PARAMETERS &presentation);

	/// The presentation parameters in effect, with the defaults the caller left open filled in.
	void GetPresentationParameters(D3DPRESENT_PARAMETERS &presentation) const { presentation = m_presentation; }

	/// A texture upload has bound its texture to the GL texture unit that was active, in place
	/// of the texture of that stage. Called where the upload is made, by the thread that makes
	/// the calls to vitaGL.
	void NoteTextureUpload() { m_dirty |= m_activeUnit >= 0 ? DIRTY_TEXTURE_0 << m_activeUnit : DIRTY_ALL_TEXTURES; }

	/// Gives a GL texture new pixels for its top level, and has vitaGL make the lower levels
	/// from them if the texture has any. Run where the stream of the render thread is read
	/// (VitaD3D8State.cpp); Texture::ReplacePixelsLater() writes it there.
	void ReplaceTexturePixels(GLuint glTexture, GLint internalFormat, GLsizei width, GLsizei height,
		GLenum format, GLenum type, const void *pixels, bool mipMapped);

	// IDirect3DDevice8 methods
	HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
	UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;
	HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD Bytes) override;
	HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8 **ppD3D8) override;
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8 *pCaps) override;
	HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) override;
	HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
	HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) override;
	void STDMETHODCALLTYPE SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags) override;
	WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL bShow) override;
	HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain8 **pSwapChain) override;
	HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
	HRESULT STDMETHODCALLTYPE Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) override;
	HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer) override;
	HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override;
	void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3DGAMMARAMP *ramp) override;
	void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP *pRamp) override;
	HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture) override;
	HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture) override;
	HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture) override;
	HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer) override;
	HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer) override;
	HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, WINBOOL Lockable, IDirect3DSurface8 **ppSurface) override;
	HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface) override;
	HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface) override;
	HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8 *src_surface, const RECT *src_rects, UINT rect_count, IDirect3DSurface8 *dst_surface, const POINT *dst_points) override;
	HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture) override;
	HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8 *pDestSurface) override;
	HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil) override;
	HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) override;
	HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) override;
	HRESULT STDMETHODCALLTYPE BeginScene() override;
	HRESULT STDMETHODCALLTYPE EndScene() override;
	HRESULT STDMETHODCALLTYPE Clear(DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
	HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override;
	HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override;
	HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override;
	HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8 *viewport) override;
	HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8 *pViewport) override;
	HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8 *material) override;
	HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8 *pMaterial) override;
	HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT8 *light) override;
	HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT8 *pLight) override;
	HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, WINBOOL Enable) override;
	HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, WINBOOL *pEnable) override;
	HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override;
	HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override;
	HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
	HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override;
	HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
	HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD *pToken) override;
	HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD Token) override;
	HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD Token) override;
	HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD Token) override;
	HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD *pToken) override;
	HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8 *clip_status) override;
	HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8 *pClipStatus) override;
	HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture) override;
	HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture) override;
	HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
	HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
	HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override;
	HRESULT STDMETHODCALLTYPE GetInfo(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize) override;
	HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override;
	HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override;
	HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override;
	HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override;
	HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices, UINT startIndex, UINT primCount) override;
	HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) override;
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, D3DFORMAT index_format, const void *data, UINT stride) override;
	HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) override;
	HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *declaration, const DWORD *byte_code, DWORD *shader, DWORD usage) override;
	HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD Handle) override;
	HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD *pHandle) override;
	HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD Handle) override;
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg_idx, const void *data, DWORD count) override;
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) override;
	HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
	HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
	HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) override;
	HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) override;
	HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) override;
	HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) override;
	HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, DWORD *shader) override;
	HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD Handle) override;
	HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD *pHandle) override;
	HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD Handle) override;
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg_idx, const void *data, DWORD count) override;
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) override;
	HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
	HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segment_count, const D3DRECTPATCH_INFO *patch_info) override;
	HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segment_count, const D3DTRIPATCH_INFO *patch_info) override;
	HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override;

protected:
	virtual ~Device() override;

private:
	/// Groups of Direct3D state that are translated to GL together.
	enum DirtyFlags : uint32_t
	{
		DIRTY_TRANSFORM		= 1u << 0,	///< world, view or projection
		DIRTY_VIEWPORT		= 1u << 1,
		DIRTY_DEPTH			= 1u << 2,	///< depth test, depth write, depth bias
		DIRTY_BLEND			= 1u << 3,
		DIRTY_ALPHA_TEST	= 1u << 4,
		DIRTY_RASTER		= 1u << 5,	///< culling, fill mode, shading, color write mask
		DIRTY_STENCIL		= 1u << 6,
		DIRTY_FOG			= 1u << 7,
		DIRTY_LIGHTING		= 1u << 8,	///< lighting switches and material
		DIRTY_CLIP_PLANES	= 1u << 9,
		DIRTY_LIGHT_SOURCES	= 1u << 10,	///< the lights themselves; they also follow the view
		DIRTY_TEXTURE_0		= 1u << 16,	///< one bit per texture stage follows
		DIRTY_ALL_TEXTURES	= 0xFFu << 16,
		DIRTY_EVERYTHING	= 0xFFFFFFFFu
	};

	enum
	{
		RENDER_STATE_COUNT = 256,
		TEXTURE_STAGE_STATE_COUNT = 33,
		TRANSFORM_TEXTURE_COUNT = 8
	};

	/// The Direct3D state the engine sets, apart from what it draws from and with (buffers,
	/// vertex format, textures).
	struct RecordedState
	{
		DWORD renderStates[RENDER_STATE_COUNT];
		DWORD textureStageStates[MAX_D3D_TEXTURE_STAGES][TEXTURE_STAGE_STATE_COUNT];
		D3DMATRIX world;
		D3DMATRIX view;
		D3DMATRIX projection;
		D3DMATRIX textureMatrix[TRANSFORM_TEXTURE_COUNT];
		D3DVIEWPORT8 viewport;
		D3DMATERIAL8 material;
		D3DLIGHT8 lights[MAX_LIGHTS];
		bool lightEnabled[MAX_LIGHTS];
		float clipPlanes[MAX_CLIP_PLANES][4];

		/// The matrix of a transform state, or nullptr for one this layer has no use for.
		D3DMATRIX *GetMatrix(D3DTRANSFORMSTATETYPE state);

		/// False if the stage or one before it has its color operation disabled, which
		/// ends the cascade.
		bool IsStageActive(UINT stage) const;

		/// True if an active stage generates its texture coordinates from the camera space
		/// normal or reflection vector.
		bool GeneratesFromNormals() const;
	};

	/// What the render thread needs of the texture of a stage. It does not get the texture
	/// itself, which the engine may have let go of by the time the draw call is made.
	struct BoundTexture
	{
		GLuint glTexture;					///< 0 if the stage has none
		Texture::SamplerState *sampler;		///< see Texture::GetAppliedSamplerState()
		bool hasMipMaps;
		DWORD format;						///< the D3DFORMAT, for the log
	};

	/// A part of the target in OpenGL's coordinates.
	struct ClearBox
	{
		GLint x;
		GLint y;
		GLsizei width;
		GLsizei height;
	};

	// THE GAME'S THREAD ---------------------------------------------------------
	// The methods of IDirect3DDevice8 and what they call. They change the state the engine
	// sees and write what follows from it into the stream of the render thread.
	static void InitGraphics();
	void ApplyPresentationParameters(const D3DPRESENT_PARAMETERS &presentation);
	void SetDefaultState();
	uint32_t GetRenderStateDirtyFlags(D3DRENDERSTATETYPE state) const;

	/// Before a draw call: uploads the textures of the stages in use that were written to,
	/// and tells the render thread which textures the stages have if that changed.
	void SendTextures();
	bool PrepareStreamDraw(const VertexLayout *&layout, const uint8_t *&vertices, GLuint &glBuffer, UINT firstVertex);
	bool PrepareUserVertices(const VertexLayout *&layout, const void *data, UINT stride, UINT vertexCount);
	const uint8_t *KeepVertices(const uint8_t *source, UINT size);
	const void *RebaseIndices(const uint8_t *source, UINT indexCount, UINT indexSize, UINT &lowest, UINT &highest);
	static bool GetPrimitiveMode(D3DPRIMITIVETYPE type, UINT primitiveCount, GLenum &mode, UINT &vertexCount);

	// THE RENDER THREAD (VitaD3D8State.cpp) -------------------------------------
	// Everything that calls vitaGL to draw. These run where the stream is read: later than
	// the engine's call, and on the game's thread only if there is no render thread.
	void InvalidateStateCache();
	void ReleaseGLObjects();
	void ClearTarget(GLbitfield mask, D3DCOLOR color, float z, DWORD stencil, const ClearBox *box);
	void DrawArrays(const VertexLayout &layout, const uint8_t *vertices, GLuint glBuffer, GLenum mode, GLsizei count,
		bool swapColors);
	void DrawElements(const VertexLayout &layout, const uint8_t *vertices, GLuint glBuffer, GLuint glIndexBuffer,
		GLenum mode, GLuint first, GLuint last, GLsizei count, GLenum type, const void *indices, bool swapColors);

	/// Translate what changed of the recorded state to OpenGL.
	void ApplyState(const VertexLayout &layout);

	/// Write the combination of fixed function settings a draw is about to use to the log,
	/// the first time it is seen. Called in builds with VITA_FRAME_PROFILE.
	void LogStateCombination(const VertexLayout &layout);
	void ApplyTransform(const VertexLayout &layout);
	void ApplyViewport();
	void ApplyDepth();
	void ApplyBlend();
	void ApplyAlphaTest();
	void ApplyRaster();
	void ApplyStencil();
	void ApplyFog();
	void ApplyLighting(const VertexLayout &layout);
	void ApplyLightSources();
	void ApplyClipPlanes();
	void ApplyTextureStage(UINT stage, const VertexLayout &layout);
	void ApplyTextureMatrix(UINT stage, DWORD texCoordIndex);
	void SetTexEnv(UINT unit, GLenum name, GLint value);
	void SetTexEnvColor(UINT unit, D3DCOLOR color);
	void SetCapability(GLenum capability, bool enabled, int &cache);
	void SetActiveTextureUnit(UINT unit);
	GLuint GetWhiteTexture();

	/// Point the GL vertex arrays at count vertices of the given layout. vertices is the
	/// address of the first vertex in system memory, or the offset into glBuffer.
	void SetVertexArrays(const VertexLayout &layout, const uint8_t *vertices, GLuint glBuffer);

	// BOTH THREADS --------------------------------------------------------------
	// Set when the device is created or reset, while the render thread has nothing to do.
	IDirect3D8 *m_parent;
	D3DDEVICE_CREATION_PARAMETERS m_creation;
	D3DPRESENT_PARAMETERS m_presentation;

	Surface *m_backBuffer;
	Surface *m_depthStencil;

	// The back buffer the engine asked for is stretched over the panel when its size differs.
	float m_displayScaleX;
	float m_displayScaleY;

	// THE GAME'S THREAD ---------------------------------------------------------
	/// What the engine has set. It answers the Get calls and tells whether a Set call
	/// changes anything; every change is written into the stream.
	RecordedState m_front;
	Texture *m_textures[MAX_D3D_TEXTURE_STAGES];
	uint32_t m_texturesToSend;					///< bit per stage whose texture was set since the render thread was told
	uint32_t m_textureWritesSeen;				///< g_textureWrites when the stages were last looked at
	bool m_colorOpChanged;						///< a color operation changed since then: other stages may count now
	GLuint m_sentTextures[MAX_TEXTURE_STAGES];	///< the GL texture the render thread was told for each stage
	VertexBuffer *m_streamBuffer;
	UINT m_streamStride;
	IndexBuffer *m_indexBuffer;
	UINT m_baseVertexIndex;
	DWORD m_vertexShader;		///< always a flexible vertex format code; programmable shaders are not offered
	DWORD m_pixelShader;
	uint32_t m_texGenStages;	///< bit per stage whose coordinates are generated from the position or normal

	// Without a render thread: the copy of the vertices of a draw call from system memory,
	// whose colors are reordered in it (KeepVertices).
	uint8_t *m_userVertices;
	UINT m_userVerticesSize;
	uint8_t *m_indexScratch;		///< indices of the draw call in progress, moved down to start at 0
	UINT m_indexScratchSize;

	bool m_cursorVisible;

	// THE RENDER THREAD ---------------------------------------------------------
	/// What the engine had set when it wrote the call that is being made.
	RecordedState m_back;
	BoundTexture m_boundTextures[MAX_TEXTURE_STAGES];
	uint32_t m_dirty;

	// What the GL state currently reflects, to avoid redundant calls. vitaGL rebuilds its
	// fixed function shader key on every glTexEnv, even one that changes nothing.
	struct TextureUnitCache
	{
		enum { ENV_VALUE_COUNT = 24 };
		GLint envValues[ENV_VALUE_COUNT];
		D3DCOLOR envColor;
		bool envColorKnown;
		int enabled;			///< -1 unknown, 0 disabled, 1 enabled
		int coordArrayEnabled;
	};
	TextureUnitCache m_unitCache[MAX_TEXTURE_STAGES];
	int m_activeUnit;
	int m_capLighting;
	int m_capFog;
	int m_capBlend;
	int m_capAlphaTest;
	int m_capDepthTest;
	int m_capCullFace;
	int m_capStencilTest;
	int m_capPolygonOffset;
	int m_capNormalize;
	int m_capColorMaterial;
	GLuint m_colorPointerBuffer;	///< buffer object the last color pointer referred to, 0 for none
	bool m_normalMatrixPending;		///< no lit draw call has used the model view matrix that is loaded yet
	bool m_lastLayoutTransformed;	///< projection currently holds the screen space mapping
	bool m_lastLayoutHadNormal;
	bool m_lastLayoutHadDiffuse;

	GLuint m_whiteTexture;
};

} // namespace VitaD3D8
