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

// VitaD3D8State.cpp //////////////////////////////////////////////////////////
// Translation of the recorded Direct3D 8 state into fixed function OpenGL.
//
// Conventions that differ between the two APIs and are bridged here:
//
//   Matrices      Direct3D multiplies row vectors with row major matrices, OpenGL
//                 column vectors with column major ones. The two layouts are the
//                 same bytes, so matrices are passed through unchanged.
//   Clip space    Direct3D keeps z in 0..w, OpenGL in -w..w. The projection is
//                 adjusted so the depth buffer holds the same values as on Windows.
//   Pixel centers Direct3D 8 puts them on integer coordinates, OpenGL on half
//                 integers. The projection carries the half pixel offset, which
//                 keeps the engine's 2D drawing aligned to the pixel grid.
//   Winding       Direct3D's front faces are clockwise.
//   Colors        Vertex colors are reordered when the buffers are filled.
///////////////////////////////////////////////////////////////////////////////

#include "VitaD3D8Device.h"

#include <VitaDiagnostics.h>

#include <cfloat>

// VitaD3D8GL.h is left out on purpose. What is in this file runs where the stream of the
// render thread is read, and its calls to vitaGL are made where they are written. Nothing
// in here may write to the stream or wait for it.

namespace VitaD3D8
{

namespace
{

float BitsToFloat(DWORD bits)
{
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

/// D3DCOLOR (ARGB in a word) as red, green, blue, alpha floats.
void ColorToFloats(D3DCOLOR color, GLfloat out[4])
{
	out[0] = ((color >> 16) & 0xFF) / 255.0f;
	out[1] = ((color >> 8) & 0xFF) / 255.0f;
	out[2] = (color & 0xFF) / 255.0f;
	out[3] = ((color >> 24) & 0xFF) / 255.0f;
}

GLenum ToGLCompare(DWORD compare)
{
	switch (compare)
	{
		case D3DCMP_NEVER:			return GL_NEVER;
		case D3DCMP_LESS:			return GL_LESS;
		case D3DCMP_EQUAL:			return GL_EQUAL;
		case D3DCMP_LESSEQUAL:		return GL_LEQUAL;
		case D3DCMP_GREATER:		return GL_GREATER;
		case D3DCMP_NOTEQUAL:		return GL_NOTEQUAL;
		case D3DCMP_GREATEREQUAL:	return GL_GEQUAL;
		default:					return GL_ALWAYS;
	}
}

GLenum ToGLBlend(DWORD blend)
{
	switch (blend)
	{
		case D3DBLEND_ZERO:			return GL_ZERO;
		case D3DBLEND_ONE:			return GL_ONE;
		case D3DBLEND_SRCCOLOR:		return GL_SRC_COLOR;
		case D3DBLEND_INVSRCCOLOR:	return GL_ONE_MINUS_SRC_COLOR;
		case D3DBLEND_SRCALPHA:		return GL_SRC_ALPHA;
		case D3DBLEND_INVSRCALPHA:	return GL_ONE_MINUS_SRC_ALPHA;
		case D3DBLEND_DESTALPHA:	return GL_DST_ALPHA;
		case D3DBLEND_INVDESTALPHA:	return GL_ONE_MINUS_DST_ALPHA;
		case D3DBLEND_DESTCOLOR:	return GL_DST_COLOR;
		case D3DBLEND_INVDESTCOLOR:	return GL_ONE_MINUS_DST_COLOR;
		case D3DBLEND_SRCALPHASAT:	return GL_SRC_ALPHA_SATURATE;
		default:					return GL_ONE;
	}
}

GLenum ToGLStencilOp(DWORD operation)
{
	switch (operation)
	{
		case D3DSTENCILOP_ZERO:		return GL_ZERO;
		case D3DSTENCILOP_REPLACE:	return GL_REPLACE;
		case D3DSTENCILOP_INCRSAT:	return GL_INCR;
		case D3DSTENCILOP_DECRSAT:	return GL_DECR;
		case D3DSTENCILOP_INVERT:	return GL_INVERT;
		case D3DSTENCILOP_INCR:		return GL_INCR_WRAP;
		case D3DSTENCILOP_DECR:		return GL_DECR_WRAP;
		default:					return GL_KEEP;
	}
}

GLint ToGLWrap(DWORD address)
{
	switch (address)
	{
		case D3DTADDRESS_MIRROR:	return GL_MIRRORED_REPEAT;
		case D3DTADDRESS_CLAMP:
		case D3DTADDRESS_BORDER:	return GL_CLAMP_TO_EDGE;
		default:					return GL_REPEAT;
	}
}

/// Slot of a texture environment parameter in the per unit cache, or -1.
int GetEnvSlot(GLenum name)
{
	switch (name)
	{
		case GL_TEXTURE_ENV_MODE:	return 0;
		case GL_COMBINE_RGB:		return 1;
		case GL_COMBINE_ALPHA:		return 2;
		case GL_SRC0_RGB:			return 3;
		case GL_SRC1_RGB:			return 4;
		case GL_SRC2_RGB:			return 5;
		case GL_SRC0_ALPHA:			return 6;
		case GL_SRC1_ALPHA:			return 7;
		case GL_SRC2_ALPHA:			return 8;
		case GL_OPERAND0_RGB:		return 9;
		case GL_OPERAND1_RGB:		return 10;
		case GL_OPERAND2_RGB:		return 11;
		case GL_OPERAND0_ALPHA:		return 12;
		case GL_OPERAND1_ALPHA:		return 13;
		case GL_OPERAND2_ALPHA:		return 14;
		case GL_RGB_SCALE:			return 15;
		case GL_ALPHA_SCALE:		return 16;
		default:					return -1;
	}
}

/// One input of a texture combiner.
struct CombineSource
{
	DWORD argument;		///< D3DTA_* with modifiers
	bool alphaOnly;		///< the blend factor of the interpolating operations reads alpha
};

/// A Direct3D texture operation expressed as an OpenGL combiner function.
struct CombineFunction
{
	GLint mode;
	GLint scale;
	int sourceCount;
	CombineSource sources[3];
};

void GetCombineFunction(DWORD operation, DWORD argument1, DWORD argument2, DWORD argument0, CombineFunction &function)
{
	function.mode = GL_MODULATE;
	function.scale = 1;
	function.sourceCount = 2;
	function.sources[0].argument = argument1;
	function.sources[0].alphaOnly = false;
	function.sources[1].argument = argument2;
	function.sources[1].alphaOnly = false;
	function.sources[2].argument = D3DTA_CURRENT;
	function.sources[2].alphaOnly = true;

	switch (operation)
	{
		case D3DTOP_DISABLE:
			// Only reached for the alpha operation of a stage whose color operation is
			// active: the alpha passes through unchanged.
			function.mode = GL_REPLACE;
			function.sourceCount = 1;
			function.sources[0].argument = D3DTA_CURRENT;
			break;

		case D3DTOP_SELECTARG1:
			function.mode = GL_REPLACE;
			function.sourceCount = 1;
			break;

		case D3DTOP_SELECTARG2:
			function.mode = GL_REPLACE;
			function.sourceCount = 1;
			function.sources[0].argument = argument2;
			break;

		case D3DTOP_MODULATE:
			break;

		case D3DTOP_MODULATE2X:
			function.scale = 2;
			break;

		case D3DTOP_MODULATE4X:
			function.scale = 4;
			break;

		case D3DTOP_ADD:
			function.mode = GL_ADD;
			break;

		case D3DTOP_ADDSMOOTH:
			// arg1 + arg2 - arg1 * arg2. The combiner has no such function without spending
			// its constant color; a saturating add is close for the highlights it is used on.
			function.mode = GL_ADD;
			break;

		case D3DTOP_ADDSIGNED:
			function.mode = GL_ADD_SIGNED;
			break;

		case D3DTOP_ADDSIGNED2X:
			function.mode = GL_ADD_SIGNED;
			function.scale = 2;
			break;

		case D3DTOP_SUBTRACT:
			function.mode = GL_SUBTRACT;
			break;

		case D3DTOP_BLENDDIFFUSEALPHA:
			function.mode = GL_INTERPOLATE;
			function.sourceCount = 3;
			function.sources[2].argument = D3DTA_DIFFUSE;
			break;

		case D3DTOP_BLENDTEXTUREALPHA:
		case D3DTOP_BLENDTEXTUREALPHAPM:
			function.mode = GL_INTERPOLATE;
			function.sourceCount = 3;
			function.sources[2].argument = D3DTA_TEXTURE;
			break;

		case D3DTOP_BLENDFACTORALPHA:
			function.mode = GL_INTERPOLATE;
			function.sourceCount = 3;
			function.sources[2].argument = D3DTA_TFACTOR;
			break;

		case D3DTOP_BLENDCURRENTALPHA:
			function.mode = GL_INTERPOLATE;
			function.sourceCount = 3;
			function.sources[2].argument = D3DTA_CURRENT;
			break;

		case D3DTOP_LERP:
			function.mode = GL_INTERPOLATE;
			function.sourceCount = 3;
			function.sources[2].argument = argument0;
			function.sources[2].alphaOnly = false;
			break;

		case D3DTOP_DOTPRODUCT3:
			function.mode = GL_DOT3_RGB;
			break;

		default:
			// Bump mapping and the premodulate family are not advertised in the caps.
			LogUnsupportedOnce("a texture stage operation outside the advertised set; using modulate");
			break;
	}
}

#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
/// vitaGL's generation of texture coordinates for the upper word of D3DTSS_TEXCOORDINDEX.
GLenum ToVglTexCoordGen(DWORD generation)
{
	switch (generation)
	{
		case D3DTSS_TCI_CAMERASPACEPOSITION:			return VGL_TEXCOORD_GEN_EYE_POSITION;
		case D3DTSS_TCI_CAMERASPACENORMAL:				return VGL_TEXCOORD_GEN_EYE_NORMAL;
		case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:	return VGL_TEXCOORD_GEN_EYE_REFLECTION;
		default:										return GL_NONE;
	}
}
#endif

} // namespace

//-----------------------------------------------------------------------------
void MultiplyMatrix(D3DMATRIX &out, const D3DMATRIX &a, const D3DMATRIX &b)
{
	D3DMATRIX result;
	for (int row = 0; row < 4; ++row)
	{
		for (int column = 0; column < 4; ++column)
		{
			result.m[row][column] = a.m[row][0] * b.m[0][column] + a.m[row][1] * b.m[1][column]
				+ a.m[row][2] * b.m[2][column] + a.m[row][3] * b.m[3][column];
		}
	}
	out = result;
}

//-----------------------------------------------------------------------------
const D3DMATRIX &GetIdentityMatrix()
{
	static D3DMATRIX identity;
	static bool initialized = false;
	if (!initialized)
	{
		memset(&identity, 0, sizeof(identity));
		identity.m[0][0] = 1.0f;
		identity.m[1][1] = 1.0f;
		identity.m[2][2] = 1.0f;
		identity.m[3][3] = 1.0f;
		initialized = true;
	}
	return identity;
}

//-----------------------------------------------------------------------------
/// Adds the time of a part of a draw call to the frame statistics.
class PhaseTimer
{
public:
	explicit PhaseTimer(std::atomic<uint32_t> &sum) : m_sum(sum), m_start(VitaDiagnostics::profileStart()) {}
	~PhaseTimer()
	{
		if (m_start != 0)
			m_sum.fetch_add(static_cast<uint32_t>(VitaDiagnostics::now() - m_start), std::memory_order_relaxed);
	}

private:
	std::atomic<uint32_t> &m_sum;
	uint64_t m_start;
};

//-----------------------------------------------------------------------------
void Device::SetCapability(GLenum capability, bool enabled, int &cache)
{
	const int wanted = enabled ? 1 : 0;
	if (cache == wanted)
		return;

	if (enabled)
		glEnable(capability);
	else
		glDisable(capability);
	cache = wanted;
}

//-----------------------------------------------------------------------------
void Device::SetActiveTextureUnit(UINT unit)
{
	if (m_activeUnit == static_cast<int>(unit))
		return;

	glActiveTexture(GL_TEXTURE0 + unit);
	m_activeUnit = static_cast<int>(unit);
}

//-----------------------------------------------------------------------------
void Device::SetTexEnv(UINT unit, GLenum name, GLint value)
{
	TextureUnitCache &cache = m_unitCache[unit];
	const int slot = GetEnvSlot(name);
	if (slot >= 0)
	{
		if (cache.envValues[slot] == value)
			return;
		cache.envValues[slot] = value;
	}

	SetActiveTextureUnit(unit);
	glTexEnvi(GL_TEXTURE_ENV, name, value);
}

//-----------------------------------------------------------------------------
void Device::SetTexEnvColor(UINT unit, D3DCOLOR color)
{
	TextureUnitCache &cache = m_unitCache[unit];
	if (cache.envColorKnown && cache.envColor == color)
		return;

	GLfloat values[4];
	ColorToFloats(color, values);

	SetActiveTextureUnit(unit);
	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, values);
	cache.envColor = color;
	cache.envColorKnown = true;
}

//-----------------------------------------------------------------------------
GLuint Device::GetWhiteTexture()
{
	if (m_whiteTexture == 0)
	{
		// Stands in for a stage that combines colors without a texture bound to it.
		// OpenGL only runs a unit's combiner while a texture is enabled on it.
		enum { SIZE = 8 };
		uint8_t pixels[SIZE * SIZE * 4];
		memset(pixels, 0xFF, sizeof(pixels));

		glGenTextures(1, &m_whiteTexture);
		glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
	}

	return m_whiteTexture;
}

//-----------------------------------------------------------------------------
D3DMATRIX *Device::RecordedState::GetMatrix(D3DTRANSFORMSTATETYPE state)
{
	if (state == D3DTS_WORLD)
		return &world;
	if (state == D3DTS_VIEW)
		return &view;
	if (state == D3DTS_PROJECTION)
		return &projection;
	if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7)
		return &textureMatrix[static_cast<UINT>(state) - D3DTS_TEXTURE0];

	return nullptr;
}

//-----------------------------------------------------------------------------
bool Device::RecordedState::IsStageActive(UINT stage) const
{
	// A disabled color operation ends the cascade for all later stages.
	for (UINT i = 0; i <= stage; ++i)
	{
		if (textureStageStates[i][D3DTSS_COLOROP] == D3DTOP_DISABLE)
			return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
bool Device::RecordedState::GeneratesFromNormals() const
{
	for (UINT stage = 0; stage < MAX_TEXTURE_STAGES && IsStageActive(stage); ++stage)
	{
		const DWORD generation = textureStageStates[stage][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000u;
		if (generation == D3DTSS_TCI_CAMERASPACENORMAL || generation == D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
void Device::InvalidateStateCache()
{
	for (TextureUnitCache &unit : m_unitCache)
	{
		for (GLint &value : unit.envValues)
			value = -1;
		unit.envColorKnown = false;
		unit.envColor = 0;
		unit.enabled = -1;
		unit.coordArrayEnabled = -1;
	}

	m_activeUnit = -1;
	m_capLighting = -1;
	m_capFog = -1;
	m_capBlend = -1;
	m_capAlphaTest = -1;
	m_capDepthTest = -1;
	m_capCullFace = -1;
	m_capStencilTest = -1;
	m_capPolygonOffset = -1;
	m_capNormalize = -1;
	m_capColorMaterial = -1;
	// Not known after a reset: whatever was drawn before may have left a buffer behind.
	m_colorPointerBuffer = ~0u;
	m_normalMatrixPending = true;
	m_lastLayoutTransformed = false;
	m_lastLayoutHadNormal = false;
	m_lastLayoutHadDiffuse = false;

	m_dirty = DIRTY_EVERYTHING;
}

//-----------------------------------------------------------------------------
void Device::ReleaseGLObjects()
{
	if (m_whiteTexture != 0)
	{
		glDeleteTextures(1, &m_whiteTexture);
		m_whiteTexture = 0;
	}
}

//-----------------------------------------------------------------------------
void Device::ReplaceTexturePixels(GLuint glTexture, GLint internalFormat, GLsizei width, GLsizei height,
	GLenum format, GLenum type, const void *pixels, bool mipMapped)
{
	glBindTexture(GL_TEXTURE_2D, glTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, pixels);

	// vitaGL builds the lower levels of uncompressed textures itself.
	if (mipMapped)
		glGenerateMipmap(GL_TEXTURE_2D);

	NoteTextureUpload();
}

//-----------------------------------------------------------------------------
/// Clears the whole target, or the part of it that box names.
void Device::ClearTarget(GLbitfield mask, D3DCOLOR color, float z, DWORD stencil, const ClearBox *box)
{
	// Direct3D clears regardless of the write masks; OpenGL honors them.
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilMask(0xFFFFFFFF);
	m_dirty |= DIRTY_DEPTH | DIRTY_RASTER | DIRTY_STENCIL;

	glClearColor(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, ((color >> 24) & 0xFF) / 255.0f);
	glClearDepthf(z);
	glClearStencil(static_cast<GLint>(stencil));

	if (box == nullptr)
	{
		glDisable(GL_SCISSOR_TEST);
		glClear(mask);
		return;
	}

	glEnable(GL_SCISSOR_TEST);
	glScissor(box->x, box->y, box->width, box->height);
	glClear(mask);
	glDisable(GL_SCISSOR_TEST);
}

//-----------------------------------------------------------------------------
/// swapColors: the vertices are a copy made for this call (Device::KeepVertices) and have
/// their colors as Direct3D orders them, B, G, R, A. vitaGL reads R, G, B, A.
void Device::DrawArrays(const VertexLayout &layout, const uint8_t *vertices, GLuint glBuffer, GLenum mode, GLsizei count,
	bool swapColors)
{
	if (swapColors)
		SwapVertexColors(layout, const_cast<uint8_t *>(vertices), static_cast<UINT>(count));

	ApplyState(layout);
	SetVertexArrays(layout, vertices, glBuffer);
	glDrawArrays(mode, 0, count);
}

//-----------------------------------------------------------------------------
/// indices is the address of the indices in system memory, or the offset into
/// glIndexBuffer if that is not 0.
void Device::DrawElements(const VertexLayout &layout, const uint8_t *vertices, GLuint glBuffer, GLuint glIndexBuffer,
	GLenum mode, GLuint first, GLuint last, GLsizei count, GLenum type, const void *indices, bool swapColors)
{
	if (swapColors)
		SwapVertexColors(layout, const_cast<uint8_t *>(vertices) + first * layout.stride, last - first + 1);

	ApplyState(layout);
	SetVertexArrays(layout, vertices, glBuffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glIndexBuffer);
	glDrawRangeElements(mode, first, last, count, type, indices);
}

//-----------------------------------------------------------------------------
void Device::ApplyState(const VertexLayout &layout)
{
	const PhaseTimer timer(g_stateTime);

	// Some groups depend on what the vertices carry.
	if (layout.isTransformed != m_lastLayoutTransformed)
	{
		m_lastLayoutTransformed = layout.isTransformed;
		m_dirty |= DIRTY_TRANSFORM | DIRTY_VIEWPORT | DIRTY_LIGHTING | DIRTY_FOG;
	}

	const bool hasNormal = layout.normalOffset >= 0;
	const bool hasDiffuse = layout.diffuseOffset >= 0;
	if (hasNormal != m_lastLayoutHadNormal || hasDiffuse != m_lastLayoutHadDiffuse)
	{
		m_lastLayoutHadNormal = hasNormal;
		m_lastLayoutHadDiffuse = hasDiffuse;
		m_dirty |= DIRTY_LIGHTING;
	}

	if (m_dirty == 0)
		return;

#ifdef VITA_FRAME_PROFILE
	if (VitaDiagnostics::g_profiling)
		LogStateCombination(layout);
#endif

	if (m_dirty & DIRTY_VIEWPORT)
		ApplyViewport();
	if (m_dirty & DIRTY_DEPTH)
		ApplyDepth();
	if (m_dirty & DIRTY_BLEND)
		ApplyBlend();
	if (m_dirty & DIRTY_ALPHA_TEST)
		ApplyAlphaTest();
	if (m_dirty & DIRTY_RASTER)
		ApplyRaster();
	if (m_dirty & DIRTY_STENCIL)
		ApplyStencil();
	if (m_dirty & DIRTY_FOG)
		ApplyFog();

	if (m_dirty & DIRTY_LIGHTING)
		ApplyLighting(layout);

	// Light sources and clip planes are handed to OpenGL with the view matrix loaded as
	// model view matrix; the transform is applied afterwards and restores it.
	if (m_dirty & DIRTY_LIGHT_SOURCES)
	{
		ApplyLightSources();
		m_dirty |= DIRTY_TRANSFORM;
	}
	if (m_dirty & DIRTY_CLIP_PLANES)
	{
		ApplyClipPlanes();
		m_dirty |= DIRTY_TRANSFORM;
	}

	for (UINT stage = 0; stage < MAX_TEXTURE_STAGES; ++stage)
	{
		if (m_dirty & (DIRTY_TEXTURE_0 << stage))
			ApplyTextureStage(stage, layout);
	}

	const bool transformLoaded = (m_dirty & DIRTY_TRANSFORM) != 0;
	if (transformLoaded)
	{
		ApplyTransform(layout);
		m_normalMatrixPending = true;
	}

	m_dirty = 0;

	// vitaGL works the normal matrix out only when the first draw call after a change of
	// the model view matrix is a lit one, and passes it to the shader only together with
	// all other uniforms: after a change of shader or at the start of a frame, not when
	// the matrix changes. Left alone, every mesh of a run with the same kind of state was
	// lit with the orientation of the first one, and the lighting of whole groups of
	// objects (all trees, say) jumped whenever effects changed the order of the draw
	// calls. Selecting "no program" is vitaGL's public way of marking all uniforms as
	// changed; the model view matrix is loaded again if an unlit draw has used it first.
	// Nothing is dirty for a draw call that differs from the previous one in neither
	// lighting nor matrices, so returning early above is safe.
	//
	// Texture coordinates generated from normals take the normal matrix as well, lit or
	// not (see ApplyTextureMatrix); a change of the generation marks the stage dirty, so
	// returning early stays safe.
#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
	const bool normalMatrixUsed = m_capLighting == 1 || m_back.GeneratesFromNormals();
#else
	const bool normalMatrixUsed = m_capLighting == 1;
#endif
	if (normalMatrixUsed && m_normalMatrixPending)
	{
		if (!transformLoaded)
			ApplyTransform(layout);

		glUseProgram(0);
		m_normalMatrixPending = false;
	}
}

//-----------------------------------------------------------------------------
void Device::LogStateCombination(const VertexLayout &layout)
{
	// What decides how the fixed function pipeline has to be set up. Colors and matrices
	// are left out: they vary per object without changing the kind of setup.
	enum { MAX_COMBINATIONS = 256, LOGGED_STAGES = 3 };
	static uint32_t seen[MAX_COMBINATIONS];
	static UINT seenCount = 0;

	static const D3DRENDERSTATETYPE renderStates[] =
	{
		D3DRS_LIGHTING, D3DRS_COLORVERTEX, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
		D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_SPECULARENABLE,
		D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC,
		D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_FOGENABLE
	};
	static const D3DTEXTURESTAGESTATETYPE stageStates[] =
	{
		D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
		D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS
	};

	UINT lights = 0;
	for (UINT i = 0; i < MAX_LIGHTS; ++i)
	{
		if (m_back.lightEnabled[i])
			++lights;
	}

	// Only the stages that are in use and that the console has a texture unit for have a
	// texture here.
	DWORD formats[LOGGED_STAGES];
	for (UINT stage = 0; stage < LOGGED_STAGES; ++stage)
	{
		const bool known = stage < MAX_TEXTURE_STAGES && m_back.IsStageActive(stage) && m_boundTextures[stage].glTexture != 0;
		formats[stage] = known ? m_boundTextures[stage].format : 0;
	}

	// FNV-1a over the values.
	uint32_t hash = 2166136261u;
	const auto mix = [&hash](DWORD value)
	{
		hash = (hash ^ value) * 16777619u;
	};

	mix(layout.fvf);
	mix(lights);
	for (const D3DRENDERSTATETYPE state : renderStates)
		mix(m_back.renderStates[state]);
	for (UINT stage = 0; stage < LOGGED_STAGES; ++stage)
	{
		mix(formats[stage]);
		for (const D3DTEXTURESTAGESTATETYPE state : stageStates)
			mix(m_back.textureStageStates[stage][state]);
	}

	for (UINT i = 0; i < seenCount; ++i)
	{
		if (seen[i] == hash)
			return;
	}
	if (seenCount >= MAX_COMBINATIONS)
		return;
	seen[seenCount++] = hash;

	const DWORD *rs = m_back.renderStates;
	Log("state %u: fvf 0x%x%s, lighting %u with %u lights, colorvertex %u, sources d%u a%u e%u s%u, specular %u; "
		"blend %u (%u, %u), alpha test %u (func %u), z %u write %u func %u, cull %u, fog %u; "
		"material d(%.2f %.2f %.2f %.2f) a(%.2f %.2f %.2f) e(%.2f %.2f %.2f), ambient 0x%08x, texture factor 0x%08x",
		static_cast<unsigned>(seenCount), static_cast<unsigned>(layout.fvf), layout.isTransformed ? " (transformed)" : "",
		static_cast<unsigned>(rs[D3DRS_LIGHTING]), static_cast<unsigned>(lights), static_cast<unsigned>(rs[D3DRS_COLORVERTEX]),
		static_cast<unsigned>(rs[D3DRS_DIFFUSEMATERIALSOURCE]), static_cast<unsigned>(rs[D3DRS_AMBIENTMATERIALSOURCE]),
		static_cast<unsigned>(rs[D3DRS_EMISSIVEMATERIALSOURCE]), static_cast<unsigned>(rs[D3DRS_SPECULARMATERIALSOURCE]),
		static_cast<unsigned>(rs[D3DRS_SPECULARENABLE]),
		static_cast<unsigned>(rs[D3DRS_ALPHABLENDENABLE]), static_cast<unsigned>(rs[D3DRS_SRCBLEND]), static_cast<unsigned>(rs[D3DRS_DESTBLEND]),
		static_cast<unsigned>(rs[D3DRS_ALPHATESTENABLE]), static_cast<unsigned>(rs[D3DRS_ALPHAFUNC]),
		static_cast<unsigned>(rs[D3DRS_ZENABLE]), static_cast<unsigned>(rs[D3DRS_ZWRITEENABLE]), static_cast<unsigned>(rs[D3DRS_ZFUNC]),
		static_cast<unsigned>(rs[D3DRS_CULLMODE]), static_cast<unsigned>(rs[D3DRS_FOGENABLE]),
		m_back.material.Diffuse.r, m_back.material.Diffuse.g, m_back.material.Diffuse.b, m_back.material.Diffuse.a,
		m_back.material.Ambient.r, m_back.material.Ambient.g, m_back.material.Ambient.b,
		m_back.material.Emissive.r, m_back.material.Emissive.g, m_back.material.Emissive.b,
		static_cast<unsigned>(rs[D3DRS_AMBIENT]), static_cast<unsigned>(rs[D3DRS_TEXTUREFACTOR]));

	for (UINT stage = 0; stage < LOGGED_STAGES; ++stage)
	{
		const DWORD *ts = m_back.textureStageStates[stage];
		Log("state %u, stage %u: texture format %u, color op %u (0x%x, 0x%x), alpha op %u (0x%x, 0x%x), coordinates 0x%x, transform %u",
			static_cast<unsigned>(seenCount), static_cast<unsigned>(stage), static_cast<unsigned>(formats[stage]),
			static_cast<unsigned>(ts[D3DTSS_COLOROP]), static_cast<unsigned>(ts[D3DTSS_COLORARG1]), static_cast<unsigned>(ts[D3DTSS_COLORARG2]),
			static_cast<unsigned>(ts[D3DTSS_ALPHAOP]), static_cast<unsigned>(ts[D3DTSS_ALPHAARG1]), static_cast<unsigned>(ts[D3DTSS_ALPHAARG2]),
			static_cast<unsigned>(ts[D3DTSS_TEXCOORDINDEX]), static_cast<unsigned>(ts[D3DTSS_TEXTURETRANSFORMFLAGS]));

		// Stages behind a disabled one are not used.
		if (ts[D3DTSS_COLOROP] == D3DTOP_DISABLE)
			break;
	}
}

//-----------------------------------------------------------------------------
void Device::ApplyTransform(const VertexLayout &layout)
{
	D3DMATRIX projection;

	if (layout.isTransformed)
	{
		// Screen space vertices: map back buffer pixels straight to clip space, with the
		// vertical axis flipped and the half pixel offset of Direct3D's pixel centers.
		const float width = static_cast<float>(m_presentation.BackBufferWidth);
		const float height = static_cast<float>(m_presentation.BackBufferHeight);

		projection = GetIdentityMatrix();
		projection.m[0][0] = 2.0f / width;
		projection.m[1][1] = -2.0f / height;
		projection.m[2][2] = 2.0f;
		projection.m[3][0] = -1.0f + 1.0f / width;
		projection.m[3][1] = 1.0f - 1.0f / height;
		projection.m[3][2] = -1.0f;

		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(&projection.m[0][0]);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		return;
	}

	projection = m_back.projection;

	const float offsetX = m_back.viewport.Width != 0 ? 1.0f / m_back.viewport.Width : 0.0f;
	const float offsetY = m_back.viewport.Height != 0 ? -1.0f / m_back.viewport.Height : 0.0f;
	for (int row = 0; row < 4; ++row)
	{
		const float w = projection.m[row][3];
		projection.m[row][0] += w * offsetX;
		projection.m[row][1] += w * offsetY;
		projection.m[row][2] = 2.0f * projection.m[row][2] - w;
	}

	D3DMATRIX modelView;
	MultiplyMatrix(modelView, m_back.world, m_back.view);

	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(&projection.m[0][0]);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(&modelView.m[0][0]);
}

//-----------------------------------------------------------------------------
void Device::ApplyViewport()
{
	if (m_lastLayoutTransformed)
	{
		// Screen space vertices address the whole target and carry their final depth.
		glViewport(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
		glDepthRangef(0.0f, 1.0f);
		return;
	}

	// Direct3D counts rows from the top of the target, OpenGL from the bottom.
	const float bottom = static_cast<float>(m_presentation.BackBufferHeight) - (m_back.viewport.Y + m_back.viewport.Height);
	glViewport(static_cast<GLint>(m_back.viewport.X * m_displayScaleX + 0.5f),
		static_cast<GLint>(bottom * m_displayScaleY + 0.5f),
		static_cast<GLsizei>(m_back.viewport.Width * m_displayScaleX + 0.5f),
		static_cast<GLsizei>(m_back.viewport.Height * m_displayScaleY + 0.5f));
	glDepthRangef(m_back.viewport.MinZ, m_back.viewport.MaxZ);
}

//-----------------------------------------------------------------------------
void Device::ApplyDepth()
{
	SetCapability(GL_DEPTH_TEST, m_back.renderStates[D3DRS_ZENABLE] != D3DZB_FALSE, m_capDepthTest);
	glDepthFunc(ToGLCompare(m_back.renderStates[D3DRS_ZFUNC]));
	glDepthMask(m_back.renderStates[D3DRS_ZWRITEENABLE] != 0 ? GL_TRUE : GL_FALSE);

	// A higher bias draws nearer. Only code that sets the render state directly gets
	// here; WW3D shifts its projection instead because the caps do not offer a depth bias.
	const DWORD bias = m_back.renderStates[D3DRS_ZBIAS];
	SetCapability(GL_POLYGON_OFFSET_FILL, bias != 0, m_capPolygonOffset);
	if (bias != 0)
		glPolygonOffset(-1.0f * bias, -2.0f * bias);
}

//-----------------------------------------------------------------------------
void Device::ApplyBlend()
{
	SetCapability(GL_BLEND, m_back.renderStates[D3DRS_ALPHABLENDENABLE] != 0, m_capBlend);

	DWORD source = m_back.renderStates[D3DRS_SRCBLEND];
	DWORD destination = m_back.renderStates[D3DRS_DESTBLEND];
	if (source == D3DBLEND_BOTHSRCALPHA)
	{
		source = D3DBLEND_SRCALPHA;
		destination = D3DBLEND_INVSRCALPHA;
	}
	else if (source == D3DBLEND_BOTHINVSRCALPHA)
	{
		source = D3DBLEND_INVSRCALPHA;
		destination = D3DBLEND_SRCALPHA;
	}
	glBlendFunc(ToGLBlend(source), ToGLBlend(destination));

	GLenum equation = GL_FUNC_ADD;
	switch (m_back.renderStates[D3DRS_BLENDOP])
	{
		case D3DBLENDOP_SUBTRACT:		equation = GL_FUNC_SUBTRACT; break;
		case D3DBLENDOP_REVSUBTRACT:	equation = GL_FUNC_REVERSE_SUBTRACT; break;
		case D3DBLENDOP_MIN:			equation = GL_MIN; break;
		case D3DBLENDOP_MAX:			equation = GL_MAX; break;
		default:						break;
	}
	glBlendEquation(equation);
}

//-----------------------------------------------------------------------------
void Device::ApplyAlphaTest()
{
	SetCapability(GL_ALPHA_TEST, m_back.renderStates[D3DRS_ALPHATESTENABLE] != 0, m_capAlphaTest);
	glAlphaFunc(ToGLCompare(m_back.renderStates[D3DRS_ALPHAFUNC]), (m_back.renderStates[D3DRS_ALPHAREF] & 0xFF) / 255.0f);
}

//-----------------------------------------------------------------------------
void Device::ApplyRaster()
{
	// Direct3D's front faces wind clockwise on screen.
	glFrontFace(GL_CW);

	const DWORD cull = m_back.renderStates[D3DRS_CULLMODE];
	SetCapability(GL_CULL_FACE, cull == D3DCULL_CW || cull == D3DCULL_CCW, m_capCullFace);
	if (cull == D3DCULL_CCW)
		glCullFace(GL_BACK);
	else if (cull == D3DCULL_CW)
		glCullFace(GL_FRONT);

	switch (m_back.renderStates[D3DRS_FILLMODE])
	{
		case D3DFILL_POINT:		glPolygonMode(GL_FRONT_AND_BACK, GL_POINT); break;
		case D3DFILL_WIREFRAME:	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); break;
		default:				glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); break;
	}

	glShadeModel(m_back.renderStates[D3DRS_SHADEMODE] == D3DSHADE_FLAT ? GL_FLAT : GL_SMOOTH);

	const DWORD mask = m_back.renderStates[D3DRS_COLORWRITEENABLE];
	glColorMask((mask & D3DCOLORWRITEENABLE_RED) ? GL_TRUE : GL_FALSE,
		(mask & D3DCOLORWRITEENABLE_GREEN) ? GL_TRUE : GL_FALSE,
		(mask & D3DCOLORWRITEENABLE_BLUE) ? GL_TRUE : GL_FALSE,
		(mask & D3DCOLORWRITEENABLE_ALPHA) ? GL_TRUE : GL_FALSE);
}

//-----------------------------------------------------------------------------
void Device::ApplyStencil()
{
	SetCapability(GL_STENCIL_TEST, m_back.renderStates[D3DRS_STENCILENABLE] != 0, m_capStencilTest);

	glStencilFunc(ToGLCompare(m_back.renderStates[D3DRS_STENCILFUNC]),
		static_cast<GLint>(m_back.renderStates[D3DRS_STENCILREF]), m_back.renderStates[D3DRS_STENCILMASK]);
	glStencilOp(ToGLStencilOp(m_back.renderStates[D3DRS_STENCILFAIL]),
		ToGLStencilOp(m_back.renderStates[D3DRS_STENCILZFAIL]),
		ToGLStencilOp(m_back.renderStates[D3DRS_STENCILPASS]));
	glStencilMask(m_back.renderStates[D3DRS_STENCILWRITEMASK]);
}

//-----------------------------------------------------------------------------
void Device::ApplyFog()
{
	// Table fog takes precedence over vertex fog; both are computed per vertex here.
	DWORD mode = m_back.renderStates[D3DRS_FOGTABLEMODE];
	if (mode == D3DFOG_NONE)
		mode = m_back.renderStates[D3DRS_FOGVERTEXMODE];

	// Screen space vertices bring their fog factor in the specular alpha, which the fixed
	// function pipeline of vitaGL has no input for.
	const bool enabled = m_back.renderStates[D3DRS_FOGENABLE] != 0 && mode != D3DFOG_NONE && !m_lastLayoutTransformed;
	SetCapability(GL_FOG, enabled, m_capFog);
	if (!enabled)
		return;

	switch (mode)
	{
		case D3DFOG_EXP:	glFogi(GL_FOG_MODE, GL_EXP); break;
		case D3DFOG_EXP2:	glFogi(GL_FOG_MODE, GL_EXP2); break;
		default:			glFogi(GL_FOG_MODE, GL_LINEAR); break;
	}

	GLfloat color[4];
	ColorToFloats(m_back.renderStates[D3DRS_FOGCOLOR], color);
	glFogfv(GL_FOG_COLOR, color);
	glFogf(GL_FOG_START, BitsToFloat(m_back.renderStates[D3DRS_FOGSTART]));
	glFogf(GL_FOG_END, BitsToFloat(m_back.renderStates[D3DRS_FOGEND]));
	glFogf(GL_FOG_DENSITY, BitsToFloat(m_back.renderStates[D3DRS_FOGDENSITY]));
}

//-----------------------------------------------------------------------------
void Device::ApplyLighting(const VertexLayout &layout)
{
	// Screen space vertices are already lit.
	const bool lighting = m_back.renderStates[D3DRS_LIGHTING] != 0 && !layout.isTransformed;
	SetCapability(GL_LIGHTING, lighting, m_capLighting);
	if (!lighting)
	{
		SetCapability(GL_COLOR_MATERIAL, false, m_capColorMaterial);
		return;
	}

	// Direct3D picks each material color from the material or the vertex. OpenGL can let
	// the vertex color replace one material color, or ambient and diffuse together.
	GLenum colorMaterial = 0;
	if (m_back.renderStates[D3DRS_COLORVERTEX] != 0 && layout.diffuseOffset >= 0)
	{
		const bool diffuse = m_back.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] == D3DMCS_COLOR1;
		const bool ambientFromVertex = m_back.renderStates[D3DRS_AMBIENTMATERIALSOURCE] == D3DMCS_COLOR1;
		const bool emissive = m_back.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] == D3DMCS_COLOR1;

		if (diffuse && ambientFromVertex)
			colorMaterial = GL_AMBIENT_AND_DIFFUSE;
		else if (diffuse)
			colorMaterial = GL_DIFFUSE;
		else if (ambientFromVertex)
			colorMaterial = GL_AMBIENT;
		else if (emissive)
			colorMaterial = GL_EMISSION;
	}

	// This has to come before the material: vitaGL drops a material color that is set
	// while that color follows the vertex, and when the following ends it leaves the last
	// vertex color in its place. Set afterwards, the material of a mesh drawn right after
	// one with vertex colors was lost, and the mesh changed its brightness with the order
	// the objects happened to be drawn in.
	if (colorMaterial != 0)
		glColorMaterial(GL_FRONT_AND_BACK, colorMaterial);
	SetCapability(GL_COLOR_MATERIAL, colorMaterial != 0, m_capColorMaterial);

	GLfloat ambient[4];
	ColorToFloats(m_back.renderStates[D3DRS_AMBIENT], ambient);
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);

	// D3DCOLORVALUE is four floats in red, green, blue, alpha order.
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, &m_back.material.Ambient.r);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, &m_back.material.Diffuse.r);
	glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, &m_back.material.Emissive.r);

	if (m_back.renderStates[D3DRS_SPECULARENABLE] != 0)
	{
		float power = m_back.material.Power;
		if (power < 0.0f)
			power = 0.0f;
		if (power > 128.0f)
			power = 128.0f;
		glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, &m_back.material.Specular.r);
		glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, power);
	}
	else
	{
		const GLfloat none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, none);
	}

	SetCapability(GL_NORMALIZE, m_back.renderStates[D3DRS_NORMALIZENORMALS] != 0, m_capNormalize);
}

//-----------------------------------------------------------------------------
/// Kept apart from the material and the switches: those change with nearly every mesh,
/// the light sources only with the view or when an object stands in a different light.
void Device::ApplyLightSources()
{
	// Positions and directions are in world space; OpenGL transforms them by the model
	// view matrix in effect when they are set.
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(&m_back.view.m[0][0]);

	for (UINT i = 0; i < MAX_LIGHTS; ++i)
	{
		const GLenum id = GL_LIGHT0 + i;
		if (!m_back.lightEnabled[i])
		{
			glDisable(id);
			continue;
		}

		const D3DLIGHT8 &light = m_back.lights[i];
		glEnable(id);
		glLightfv(id, GL_AMBIENT, &light.Ambient.r);
		glLightfv(id, GL_DIFFUSE, &light.Diffuse.r);
		glLightfv(id, GL_SPECULAR, &light.Specular.r);

		GLfloat position[4];
		GLfloat constant = 1.0f;
		GLfloat linear = 0.0f;
		GLfloat quadratic = 0.0f;

		if (light.Type == D3DLIGHT_DIRECTIONAL)
		{
			// OpenGL wants the direction towards the light.
			position[0] = -light.Direction.x;
			position[1] = -light.Direction.y;
			position[2] = -light.Direction.z;
			position[3] = 0.0f;
		}
		else
		{
			// Spot lights are treated as point lights; vitaGL has no cone term.
			position[0] = light.Position.x;
			position[1] = light.Position.y;
			position[2] = light.Position.z;
			position[3] = 1.0f;

			constant = light.Attenuation0;
			linear = light.Attenuation1;
			quadratic = light.Attenuation2;
			if (constant == 0.0f && linear == 0.0f && quadratic == 0.0f)
				constant = 1.0f;
		}

		glLightfv(id, GL_POSITION, position);
		glLightfv(id, GL_CONSTANT_ATTENUATION, &constant);
		glLightfv(id, GL_LINEAR_ATTENUATION, &linear);
		glLightfv(id, GL_QUADRATIC_ATTENUATION, &quadratic);

#ifdef GL_LIGHT_RANGE_VGL
		// A point light of Direct3D gives no light beyond its range, which OpenGL does not know:
		// the flashes of guns and explosions lit the far sides of buildings, where the sun does
		// not, and those flickered. The vitaGL of tools/build-vitagl.sh takes the range; there 0
		// is none, so a range of 0, where Direct3D lights nothing, becomes the smallest one.
		GLfloat range = 0.0f;
		if (light.Type != D3DLIGHT_DIRECTIONAL)
			range = light.Range > 0.0f ? light.Range : FLT_MIN;
		glLightfv(id, GL_LIGHT_RANGE_VGL, &range);
#endif
	}
}

//-----------------------------------------------------------------------------
void Device::ApplyClipPlanes()
{
	const DWORD enabled = m_back.renderStates[D3DRS_CLIPPLANEENABLE];

	// The planes are in world space; OpenGL takes them through the inverse of the model
	// view matrix in effect when they are set.
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(&m_back.view.m[0][0]);

	for (UINT i = 0; i < MAX_CLIP_PLANES; ++i)
	{
		const GLenum id = GL_CLIP_PLANE0 + i;
		if ((enabled & (1u << i)) == 0)
		{
			glDisable(id);
			continue;
		}

		const GLdouble equation[4] =
		{
			m_back.clipPlanes[i][0], m_back.clipPlanes[i][1], m_back.clipPlanes[i][2], m_back.clipPlanes[i][3]
		};
		glClipPlane(id, equation);
		glEnable(id);
	}
}

//-----------------------------------------------------------------------------
void Device::ApplyTextureStage(UINT stage, const VertexLayout &layout)
{
	TextureUnitCache &unit = m_unitCache[stage];
	const DWORD *states = m_back.textureStageStates[stage];

	SetActiveTextureUnit(stage);

	if (!m_back.IsStageActive(stage))
	{
		if (unit.enabled != 0)
		{
			glDisable(GL_TEXTURE_2D);
			unit.enabled = 0;
		}
		return;
	}

	// The game's thread uploaded the texture when it wrote the draw call, and said which
	// GL texture it is (Device::SendTextures).
	const BoundTexture &bound = m_boundTextures[stage];
	if (bound.glTexture != 0)
		glBindTexture(GL_TEXTURE_2D, bound.glTexture);
	else
		GetWhiteTexture();

	if (unit.enabled != 1)
	{
		glEnable(GL_TEXTURE_2D);
		unit.enabled = 1;
	}

	// Sampler state belongs to the stage in Direct3D and to the texture in OpenGL.
	if (bound.glTexture != 0)
	{
		const bool linearMin = states[D3DTSS_MINFILTER] >= D3DTEXF_LINEAR;
		const DWORD mipFilter = bound.hasMipMaps ? states[D3DTSS_MIPFILTER] : D3DTEXF_NONE;

		GLint minFilter;
		if (mipFilter == D3DTEXF_NONE)
			minFilter = linearMin ? GL_LINEAR : GL_NEAREST;
		else if (mipFilter == D3DTEXF_POINT)
			minFilter = linearMin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
		else
			minFilter = linearMin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;

		const GLint magFilter = states[D3DTSS_MAGFILTER] >= D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST;
		const GLint wrapU = ToGLWrap(states[D3DTSS_ADDRESSU]);
		const GLint wrapV = ToGLWrap(states[D3DTSS_ADDRESSV]);

		Texture::SamplerState &applied = *bound.sampler;
		if (applied.minFilter != minFilter)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
			applied.minFilter = minFilter;
		}
		if (applied.magFilter != magFilter)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
			applied.magFilter = magFilter;
		}
		if (applied.wrapU != wrapU)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapU);
			applied.wrapU = wrapU;
		}
		if (applied.wrapV != wrapV)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapV);
			applied.wrapV = wrapV;
		}
	}

	// Combiner: one function for color and one for alpha.
	SetTexEnv(stage, GL_TEXTURE_ENV_MODE, GL_COMBINE);

	static const GLenum colorSourceNames[3] = { GL_SRC0_RGB, GL_SRC1_RGB, GL_SRC2_RGB };
	static const GLenum colorOperandNames[3] = { GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB };
	static const GLenum alphaSourceNames[3] = { GL_SRC0_ALPHA, GL_SRC1_ALPHA, GL_SRC2_ALPHA };
	static const GLenum alphaOperandNames[3] = { GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA };

	for (int pass = 0; pass < 2; ++pass)
	{
		const bool alpha = pass == 1;

		CombineFunction function;
		if (alpha)
			GetCombineFunction(states[D3DTSS_ALPHAOP], states[D3DTSS_ALPHAARG1], states[D3DTSS_ALPHAARG2], states[D3DTSS_ALPHAARG0], function);
		else
			GetCombineFunction(states[D3DTSS_COLOROP], states[D3DTSS_COLORARG1], states[D3DTSS_COLORARG2], states[D3DTSS_COLORARG0], function);

		// There is no dot product for alpha.
		if (alpha && function.mode == GL_DOT3_RGB)
			function.mode = GL_MODULATE;

		SetTexEnv(stage, alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB, function.mode);
		SetTexEnv(stage, alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE, function.scale);

		for (int i = 0; i < function.sourceCount; ++i)
		{
			const DWORD argument = function.sources[i].argument;

			GLint source;
			switch (argument & D3DTA_SELECTMASK)
			{
				case D3DTA_TEXTURE:
					source = GL_TEXTURE;
					break;

				case D3DTA_TFACTOR:
					source = GL_CONSTANT;
					break;

				case D3DTA_CURRENT:
				case D3DTA_TEMP:
					// The first stage has no previous result; current is the diffuse color.
					source = stage == 0 ? GL_PRIMARY_COLOR : GL_PREVIOUS;
					break;

				default:
					// Diffuse, and specular, which the pipeline does not carry separately.
					source = GL_PRIMARY_COLOR;
					break;
			}

			const bool complement = (argument & D3DTA_COMPLEMENT) != 0;
			const bool replicateAlpha = alpha || function.sources[i].alphaOnly || (argument & D3DTA_ALPHAREPLICATE) != 0;

			GLint operand;
			if (replicateAlpha)
				operand = complement ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
			else
				operand = complement ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;

			SetTexEnv(stage, alpha ? alphaSourceNames[i] : colorSourceNames[i], source);
			SetTexEnv(stage, alpha ? alphaOperandNames[i] : colorOperandNames[i], operand);
		}
	}

	SetTexEnvColor(stage, m_back.renderStates[D3DRS_TEXTUREFACTOR]);

	ApplyTextureMatrix(stage, states[D3DTSS_TEXCOORDINDEX]);
}

//-----------------------------------------------------------------------------
void Device::ApplyTextureMatrix(UINT stage, DWORD texCoordIndex)
{
	const DWORD flags = m_back.textureStageStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS];
	const DWORD count = flags & ~static_cast<DWORD>(D3DTTFF_PROJECTED);
	const DWORD generation = texCoordIndex & 0xFFFF0000u;

	if (flags & D3DTTFF_PROJECTED)
		LogUnsupportedOnce("projected texture coordinates; the divide is skipped");

	D3DMATRIX matrix;
	bool identity = false;

	if (generation == D3DTSS_TCI_PASSTHRU)
	{
		if (count == D3DTTFF_DISABLE)
		{
			identity = true;
		}
		else
		{
			// Direct3D extends two dimensional coordinates to (u, v, 1, 0), which puts the
			// translation into the third row. OpenGL extends them to (u, v, 0, 1) and
			// expects it in the fourth.
			matrix = m_back.textureMatrix[stage];
			matrix.m[3][0] = matrix.m[2][0];
			matrix.m[3][1] = matrix.m[2][1];
			matrix.m[2][0] = 0.0f;
			matrix.m[2][1] = 0.0f;
		}
	}
	else
	{
#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
		// Coordinates generated from the camera space position, normal or reflection vector.
		// The vitaGL of tools/build-vitagl.sh computes them from the object space position or
		// normal SetVertexArrays feeds as coordinates, as Direct3D does; this matrix is only
		// the stage's transform. Direct3D extends the three components to (x, y, z, 1), as
		// vitaGL does, so the translation stays in the fourth row.
		if (count == D3DTTFF_DISABLE)
			identity = true;
		else
			matrix = m_back.textureMatrix[stage];
#else
		// Coordinates generated from the camera space position or normal. SetVertexArrays
		// feeds the object space position or normal as coordinates; this matrix takes them
		// to camera space and through the stage's transform.
		//
		// vitaGL's fixed function shaders only pass the first two components, so the
		// contribution of the object space z is lost. It is exact where the engine uses
		// this most, for layers projected straight down onto the terrain (shroud, cloud
		// shadows), and approximate for environment mapping: there the normal stands in
		// for the reflection vector, and a flat face shows one point of the reflected
		// picture all over, which jumps when the camera turns.
		D3DMATRIX worldView;
		MultiplyMatrix(worldView, m_back.world, m_back.view);

		if (generation != D3DTSS_TCI_CAMERASPACEPOSITION)
		{
			// Directions are not translated.
			worldView.m[3][0] = 0.0f;
			worldView.m[3][1] = 0.0f;
			worldView.m[3][2] = 0.0f;
			LogUnsupportedOnce("texture coordinates generated from the normal or reflection vector are approximated");
		}

		if (count == D3DTTFF_DISABLE)
			matrix = worldView;
		else
			MultiplyMatrix(matrix, worldView, m_back.textureMatrix[stage]);
#endif
	}

	SetActiveTextureUnit(stage);
#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
	vglTexCoordGen(ToVglTexCoordGen(generation));
#endif
	glMatrixMode(GL_TEXTURE);
	if (identity)
		glLoadIdentity();
	else
		glLoadMatrixf(&matrix.m[0][0]);
	glMatrixMode(GL_MODELVIEW);
}

//-----------------------------------------------------------------------------
void Device::SetVertexArrays(const VertexLayout &layout, const uint8_t *vertices, GLuint glBuffer)
{
	const PhaseTimer timer(g_arrayTime);

	const GLsizei stride = static_cast<GLsizei>(layout.stride);

	glBindBuffer(GL_ARRAY_BUFFER, glBuffer);

	// Screen space vertices have a fourth component, the reciprocal of w, which only
	// matters for perspective correction of what is flat on the screen anyway.
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, stride, vertices);

	if (layout.normalOffset >= 0)
	{
		glEnableClientState(GL_NORMAL_ARRAY);
		glNormalPointer(GL_FLOAT, stride, vertices + layout.normalOffset);
	}
	else
	{
		glDisableClientState(GL_NORMAL_ARRAY);
	}

	// With lighting the vertex color only matters where Direct3D takes a material color
	// from it; ApplyLighting() has turned that into GL_COLOR_MATERIAL.
	const bool colorUsed = layout.diffuseOffset >= 0 && (m_capLighting != 1 || m_capColorMaterial == 1);
	if (colorUsed)
	{
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, stride, vertices + layout.diffuseOffset);
		m_colorPointerBuffer = glBuffer;
	}
	else
	{
		// Direct3D's default diffuse color is opaque white.
		glDisableClientState(GL_COLOR_ARRAY);
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

		// vitaGL remembers the buffer object of the last color pointer, also while the
		// array is disabled, and with lighting it reads the constant ambient material from
		// that buffer, taking the bytes of its first vertex for four floats. Lit meshes
		// came out tinted by vertex data. A color pointer without a buffer object makes it
		// use the material again.
		if (m_colorPointerBuffer != 0)
		{
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
			glBindBuffer(GL_ARRAY_BUFFER, glBuffer);
			m_colorPointerBuffer = 0;
		}
	}

	for (UINT stage = 0; stage < MAX_TEXTURE_STAGES; ++stage)
	{
		TextureUnitCache &unit = m_unitCache[stage];
		glClientActiveTexture(GL_TEXTURE0 + stage);

		if (!m_back.IsStageActive(stage))
		{
			if (unit.coordArrayEnabled != 0)
			{
				glDisableClientState(GL_TEXTURE_COORD_ARRAY);
				unit.coordArrayEnabled = 0;
			}
			continue;
		}

		const DWORD texCoordIndex = m_back.textureStageStates[stage][D3DTSS_TEXCOORDINDEX];
		const DWORD generation = texCoordIndex & 0xFFFF0000u;
		const UINT set = texCoordIndex & 0xFFFFu;

		// See ApplyTextureMatrix for how generated coordinates are produced.
		const uint8_t *coordinates = vertices;
		GLint components = 2;
		if (generation == D3DTSS_TCI_PASSTHRU)
		{
			if (set < layout.texCoordCount)
				coordinates = vertices + layout.texCoordOffset[set];
		}
		else
		{
			if (generation != D3DTSS_TCI_CAMERASPACEPOSITION && layout.normalOffset >= 0)
				coordinates = vertices + layout.normalOffset;
#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
			components = 3;
#endif
		}

		glTexCoordPointer(components, GL_FLOAT, stride, coordinates);
		if (unit.coordArrayEnabled != 1)
		{
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			unit.coordArrayEnabled = 1;
		}
	}
}

} // namespace VitaD3D8
