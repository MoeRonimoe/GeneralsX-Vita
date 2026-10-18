// Tests the Direct3D 8 layer (../../d3d8) on a PC.
//
// The program makes the calls the game makes: it creates textures and buffers, fills and
// changes them, sets render states, texture stages, matrices, materials and lights, draws
// from static and dynamic buffers and from memory, clears, shows frames, lets go of
// resources that were just drawn with, and resets the device. What it does is random and
// the same in every run. mockgl.cpp takes the place of vitaGL and writes down what the
// calls add up to at each draw call.
//
// Two things are compared. The trace with the render thread has to be the trace without
// it (VITA_RENDER_THREAD=0): nothing gets lost, out of order or overwritten on the way to
// the other thread. And the trace of a changed layer has to be the trace of one that is
// known to draw right on the console.
//
// run.sh builds and runs it; it needs a compiler that has the Direct3D 8 headers, which
// MinGW-w64 does.
//
//   d3d8test <trace file> [frames, default 60] [seed, default 1]
#include <d3d8.h>

#include <VitaD3D8Tags.h>
#include <VitaDiagnostics.h>

#include "VitaD3D8RenderThread.h"
#include "mockgl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

enum
{
	TEXTURE_COUNT = 16,
	MESH_BUFFER_COUNT = 6,
	MESHES_PER_BUFFER = 3,
	MESH_VERTICES = 200,
	MESH_INDICES = 600,
	DYNAMIC_VERTICES = 2048,
	DYNAMIC_INDICES = 4096
};

IDirect3DDevice8 *s_device = nullptr;
unsigned int s_mismatches = 0;
uint32_t s_seed = 1;

//-----------------------------------------------------------------------------
uint32_t Next()
{
	s_seed ^= s_seed << 13;
	s_seed ^= s_seed >> 17;
	s_seed ^= s_seed << 5;
	return s_seed;
}

uint32_t Pick(uint32_t count)
{
	return Next() % count;
}

float Fraction()
{
	return (Next() & 0xFFFF) / 65535.0f;
}

void FillRandom(void *memory, size_t size)
{
	uint8_t *bytes = static_cast<uint8_t *>(memory);
	for (size_t i = 0; i < size; ++i)
		bytes[i] = static_cast<uint8_t>(Next() >> 8);
}

DWORD FloatBits(float value)
{
	DWORD bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

void Mismatch(const char *what)
{
	if (s_mismatches++ < 20)
		fprintf(stderr, "d3d8test: %s\n", what);
}

///////////////////////////////////////////////////////////////////////////////
// Textures
///////////////////////////////////////////////////////////////////////////////

struct TestTexture
{
	IDirect3DTexture8 *texture;
	bool compressed;
};

TestTexture s_textures[TEXTURE_COUNT];

//-----------------------------------------------------------------------------
void FillTexture(IDirect3DTexture8 *texture, DWORD levels)
{
	for (DWORD level = 0; level < levels; ++level)
	{
		D3DSURFACE_DESC description;
		D3DLOCKED_RECT locked;
		if (FAILED(texture->GetLevelDesc(level, &description)) || FAILED(texture->LockRect(level, &locked, nullptr, 0)))
		{
			Mismatch("a texture level could not be locked");
			continue;
		}

		FillRandom(locked.pBits, description.Size);
		texture->UnlockRect(level);
	}
}

//-----------------------------------------------------------------------------
void MakeTexture(TestTexture &slot)
{
	static const D3DFORMAT formats[] =
	{
		D3DFMT_DXT1, D3DFMT_A8R8G8B8, D3DFMT_DXT5, D3DFMT_R5G6B5, D3DFMT_DXT3, D3DFMT_A4R4G4B4,
		D3DFMT_X8R8G8B8, D3DFMT_A1R5G5B5, D3DFMT_R8G8B8, D3DFMT_L8, D3DFMT_A8L8, D3DFMT_A8, D3DFMT_DXT1, D3DFMT_DXT5
	};

	D3DFORMAT format = formats[Pick(sizeof(formats) / sizeof(formats[0]))];
	const UINT width = 4u << Pick(6);
	const UINT height = 4u << Pick(6);
	const UINT levels = Pick(3) == 0 ? 1 : 0;

	slot.texture = nullptr;
	if (FAILED(s_device->CreateTexture(width, height, levels, 0, format, D3DPOOL_MANAGED, &slot.texture)) || slot.texture == nullptr)
	{
		format = D3DFMT_A8R8G8B8;
		if (FAILED(s_device->CreateTexture(width, height, levels, 0, format, D3DPOOL_MANAGED, &slot.texture)) || slot.texture == nullptr)
		{
			Mismatch("no texture could be created");
			return;
		}
	}

	slot.compressed = format == D3DFMT_DXT1 || format == D3DFMT_DXT3 || format == D3DFMT_DXT5;
	FillTexture(slot.texture, slot.texture->GetLevelCount());
}

//-----------------------------------------------------------------------------
/// Lets go of a texture that may be set on a stage, and may just have been drawn with,
/// and puts a new one in its place.
void ReplaceTexture(TestTexture &slot)
{
	if (slot.texture != nullptr)
		slot.texture->Release();
	MakeTexture(slot);
}

//-----------------------------------------------------------------------------
/// Writes the top level of a texture again, as the game does with the textures it draws
/// text and the radar into.
void RewriteTexture(TestTexture &slot)
{
	// The layer gives up its copy of a compressed texture once vitaGL has it.
	if (slot.texture == nullptr || slot.compressed)
		return;

	FillTexture(slot.texture, 1);
}

///////////////////////////////////////////////////////////////////////////////
// Buffers
///////////////////////////////////////////////////////////////////////////////

const DWORD MESH_FORMATS[] =
{
	D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1,
	D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2,
	D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
	D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2,
	D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1,
	D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2
};

UINT VertexSize(DWORD format)
{
	UINT size = (format & D3DFVF_XYZRHW) == D3DFVF_XYZRHW ? 16 : 12;
	if (format & D3DFVF_NORMAL)
		size += 12;
	if (format & D3DFVF_DIFFUSE)
		size += 4;
	if (format & D3DFVF_SPECULAR)
		size += 4;
	size += 8 * ((format & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
	return size;
}

/// A vertex and an index buffer that several meshes share, the way WW3D packs them.
struct MeshBuffer
{
	IDirect3DVertexBuffer8 *vertices;
	IDirect3DIndexBuffer8 *indices;
	DWORD format;
	UINT stride;
	bool absoluteIndices;	///< the indices count from the start of the buffer, not from the mesh
};

MeshBuffer s_meshBuffers[MESH_BUFFER_COUNT];

//-----------------------------------------------------------------------------
void FillMeshIndices(MeshBuffer &buffer, UINT mesh)
{
	BYTE *memory = nullptr;
	if (FAILED(buffer.indices->Lock(mesh * MESH_INDICES * 2, MESH_INDICES * 2, &memory, 0)))
	{
		Mismatch("an index buffer could not be locked");
		return;
	}

	uint16_t *indices = reinterpret_cast<uint16_t *>(memory);
	const UINT first = buffer.absoluteIndices ? mesh * MESH_VERTICES : 0;
	for (UINT i = 0; i < MESH_INDICES; ++i)
		indices[i] = static_cast<uint16_t>(first + Pick(MESH_VERTICES));
	buffer.indices->Unlock();
}

//-----------------------------------------------------------------------------
void FillMeshVertices(MeshBuffer &buffer, UINT firstVertex, UINT count)
{
	BYTE *memory = nullptr;
	if (FAILED(buffer.vertices->Lock(firstVertex * buffer.stride, count * buffer.stride, &memory, 0)))
	{
		Mismatch("a vertex buffer could not be locked");
		return;
	}

	FillRandom(memory, count * buffer.stride);
	buffer.vertices->Unlock();
}

//-----------------------------------------------------------------------------
void MakeMeshBuffer(MeshBuffer &buffer, UINT number)
{
	buffer.format = MESH_FORMATS[number % (sizeof(MESH_FORMATS) / sizeof(MESH_FORMATS[0]))];
	buffer.stride = VertexSize(buffer.format);
	buffer.absoluteIndices = (number & 2) != 0;
	buffer.vertices = nullptr;
	buffer.indices = nullptr;

	// Every other buffer is created without saying what its vertices are; the layer then
	// learns it from the first draw call.
	const UINT vertexCount = MESHES_PER_BUFFER * MESH_VERTICES;
	if (FAILED(s_device->CreateVertexBuffer(vertexCount * buffer.stride, D3DUSAGE_WRITEONLY, (number & 1) ? buffer.format : 0,
			D3DPOOL_MANAGED, &buffer.vertices))
		|| FAILED(s_device->CreateIndexBuffer(MESHES_PER_BUFFER * MESH_INDICES * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
			D3DPOOL_MANAGED, &buffer.indices)))
	{
		Mismatch("no buffer could be created");
		return;
	}

	FillMeshVertices(buffer, 0, vertexCount);
	for (UINT mesh = 0; mesh < MESHES_PER_BUFFER; ++mesh)
		FillMeshIndices(buffer, mesh);
}

//-----------------------------------------------------------------------------
void ReleaseMeshBuffer(MeshBuffer &buffer)
{
	if (buffer.vertices != nullptr)
		buffer.vertices->Release();
	if (buffer.indices != nullptr)
		buffer.indices->Release();
	buffer.vertices = nullptr;
	buffer.indices = nullptr;
}

// Buffers the vertices of a frame are written into piece by piece and drawn from right away.
const DWORD DYNAMIC_FORMAT = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
const DWORD SCREEN_FORMAT = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

IDirect3DVertexBuffer8 *s_dynamicVertices = nullptr;
IDirect3DVertexBuffer8 *s_screenVertices = nullptr;
IDirect3DIndexBuffer8 *s_dynamicIndices = nullptr;
UINT s_dynamicIndexOffset = 0;

/// Where a dynamic vertex buffer is written next, and what was written since it was last
/// discarded: all of that may be drawn again.
struct DynamicWrites
{
	enum { MAX_RANGES = 24 };

	UINT offset;
	UINT first[MAX_RANGES];
	UINT count[MAX_RANGES];
	UINT ranges;
};
DynamicWrites s_dynamicWrites = {};
DynamicWrites s_screenWrites = {};

//-----------------------------------------------------------------------------
void MakeDynamicBuffers()
{
	const DWORD usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
	if (FAILED(s_device->CreateVertexBuffer(DYNAMIC_VERTICES * VertexSize(DYNAMIC_FORMAT), usage, DYNAMIC_FORMAT, D3DPOOL_DEFAULT, &s_dynamicVertices))
		|| FAILED(s_device->CreateVertexBuffer(DYNAMIC_VERTICES * VertexSize(SCREEN_FORMAT), usage, SCREEN_FORMAT, D3DPOOL_DEFAULT, &s_screenVertices))
		|| FAILED(s_device->CreateIndexBuffer(DYNAMIC_INDICES * 2, usage, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &s_dynamicIndices)))
	{
		Mismatch("no dynamic buffer could be created");
		return;
	}

	// One of the two is written the way the engine writes its own dynamic buffer, and says so.
	// A layer that does not know the tag refuses it, which is as good.
	const DWORD keepsRules = 1;
	s_dynamicVertices->SetPrivateData(VITAD3D8_KEEPS_DYNAMIC_BUFFER_RULES, &keepsRules, sizeof(keepsRules), 0);
}

///////////////////////////////////////////////////////////////////////////////
// State
///////////////////////////////////////////////////////////////////////////////

void SetState(D3DRENDERSTATETYPE state, DWORD value)
{
	s_device->SetRenderState(state, value);

	DWORD read = ~value;
	if (FAILED(s_device->GetRenderState(state, &read)) || read != value)
		Mismatch("a render state does not read back as it was set");
}

//-----------------------------------------------------------------------------
void SetStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
{
	s_device->SetTextureStageState(stage, state, value);

	DWORD read = ~value;
	if (FAILED(s_device->GetTextureStageState(stage, state, &read)) || read != value)
		Mismatch("a texture stage state does not read back as it was set");
}

//-----------------------------------------------------------------------------
struct StateChoice
{
	D3DRENDERSTATETYPE state;
	UINT count;
	DWORD values[6];
};

const StateChoice STATE_CHOICES[] =
{
	{ D3DRS_ZENABLE, 3, { D3DZB_TRUE, D3DZB_TRUE, D3DZB_FALSE } },
	{ D3DRS_ZWRITEENABLE, 2, { TRUE, FALSE } },
	{ D3DRS_ZFUNC, 4, { D3DCMP_LESSEQUAL, D3DCMP_LESS, D3DCMP_ALWAYS, D3DCMP_EQUAL } },
	{ D3DRS_ZBIAS, 5, { 0, 0, 0, 1, 4 } },
	{ D3DRS_ALPHABLENDENABLE, 2, { FALSE, TRUE } },
	{ D3DRS_SRCBLEND, 5, { D3DBLEND_ONE, D3DBLEND_SRCALPHA, D3DBLEND_ZERO, D3DBLEND_DESTCOLOR, D3DBLEND_BOTHSRCALPHA } },
	{ D3DRS_DESTBLEND, 4, { D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DBLEND_SRCCOLOR } },
	{ D3DRS_BLENDOP, 4, { D3DBLENDOP_ADD, D3DBLENDOP_ADD, D3DBLENDOP_ADD, D3DBLENDOP_REVSUBTRACT } },
	{ D3DRS_ALPHATESTENABLE, 2, { FALSE, TRUE } },
	{ D3DRS_ALPHAREF, 3, { 0x60, 0x80, 0x01 } },
	{ D3DRS_ALPHAFUNC, 2, { D3DCMP_GREATEREQUAL, D3DCMP_GREATER } },
	{ D3DRS_CULLMODE, 3, { D3DCULL_CCW, D3DCULL_CW, D3DCULL_NONE } },
	{ D3DRS_FILLMODE, 4, { D3DFILL_SOLID, D3DFILL_SOLID, D3DFILL_SOLID, D3DFILL_WIREFRAME } },
	{ D3DRS_SHADEMODE, 3, { D3DSHADE_GOURAUD, D3DSHADE_GOURAUD, D3DSHADE_FLAT } },
	{ D3DRS_COLORWRITEENABLE, 3, { 0xF, 0xF, 0x0 } },
	{ D3DRS_STENCILENABLE, 3, { FALSE, FALSE, TRUE } },
	{ D3DRS_STENCILFUNC, 3, { D3DCMP_ALWAYS, D3DCMP_EQUAL, D3DCMP_NOTEQUAL } },
	{ D3DRS_STENCILREF, 3, { 0, 1, 0x80 } },
	{ D3DRS_STENCILMASK, 2, { 0xFFFFFFFF, 0x80 } },
	{ D3DRS_STENCILWRITEMASK, 2, { 0xFFFFFFFF, 0x7F } },
	{ D3DRS_STENCILFAIL, 2, { D3DSTENCILOP_KEEP, D3DSTENCILOP_ZERO } },
	{ D3DRS_STENCILZFAIL, 2, { D3DSTENCILOP_KEEP, D3DSTENCILOP_INCR } },
	{ D3DRS_STENCILPASS, 3, { D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_INCRSAT } },
	{ D3DRS_FOGENABLE, 2, { FALSE, TRUE } },
	{ D3DRS_FOGCOLOR, 3, { 0x00808080, 0x00C0A080, 0x00000000 } },
	{ D3DRS_FOGVERTEXMODE, 2, { D3DFOG_NONE, D3DFOG_LINEAR } },
	{ D3DRS_FOGTABLEMODE, 3, { D3DFOG_NONE, D3DFOG_NONE, D3DFOG_EXP } },
	{ D3DRS_LIGHTING, 3, { TRUE, TRUE, FALSE } },
	{ D3DRS_AMBIENT, 3, { 0x00404040, 0x00FFFFFF, 0x00102030 } },
	{ D3DRS_COLORVERTEX, 2, { TRUE, FALSE } },
	{ D3DRS_SPECULARENABLE, 3, { FALSE, FALSE, TRUE } },
	{ D3DRS_NORMALIZENORMALS, 2, { FALSE, TRUE } },
	{ D3DRS_DIFFUSEMATERIALSOURCE, 2, { D3DMCS_MATERIAL, D3DMCS_COLOR1 } },
	{ D3DRS_AMBIENTMATERIALSOURCE, 2, { D3DMCS_MATERIAL, D3DMCS_COLOR1 } },
	{ D3DRS_EMISSIVEMATERIALSOURCE, 3, { D3DMCS_MATERIAL, D3DMCS_MATERIAL, D3DMCS_COLOR1 } },
	{ D3DRS_SPECULARMATERIALSOURCE, 2, { D3DMCS_MATERIAL, D3DMCS_COLOR2 } },
	{ D3DRS_CLIPPLANEENABLE, 5, { 0, 0, 0, 1, 3 } },
	{ D3DRS_TEXTUREFACTOR, 3, { 0xFFFFFFFF, 0x80FF8040, 0x60606060 } }
};

void ChangeRenderStates(UINT count)
{
	for (UINT i = 0; i < count; ++i)
	{
		const StateChoice &choice = STATE_CHOICES[Pick(sizeof(STATE_CHOICES) / sizeof(STATE_CHOICES[0]))];
		SetState(choice.state, choice.values[Pick(choice.count)]);
	}

	if (Pick(16) == 0)
	{
		SetState(D3DRS_FOGSTART, FloatBits(10.0f + 100.0f * Fraction()));
		SetState(D3DRS_FOGEND, FloatBits(500.0f + 500.0f * Fraction()));
		SetState(D3DRS_FOGDENSITY, FloatBits(Fraction()));
	}
}

//-----------------------------------------------------------------------------
void ChangeStage(DWORD stage)
{
	static const DWORD colorOperations[] =
	{
		D3DTOP_MODULATE, D3DTOP_MODULATE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_ADD, D3DTOP_MODULATE2X,
		D3DTOP_BLENDTEXTUREALPHA, D3DTOP_ADDSMOOTH, D3DTOP_BLENDCURRENTALPHA, D3DTOP_DISABLE, D3DTOP_DISABLE,
		D3DTOP_BLENDFACTORALPHA, D3DTOP_SUBTRACT, D3DTOP_DOTPRODUCT3, D3DTOP_LERP, D3DTOP_ADDSIGNED
	};
	static const DWORD alphaOperations[] = { D3DTOP_SELECTARG1, D3DTOP_MODULATE, D3DTOP_DISABLE, D3DTOP_SELECTARG2, D3DTOP_ADD };
	static const DWORD arguments[] =
	{
		D3DTA_TEXTURE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TFACTOR,
		D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE, D3DTA_TEXTURE | D3DTA_COMPLEMENT, D3DTA_SPECULAR
	};
	static const DWORD coordinates[] =
	{
		0, 0, 1, 1, D3DTSS_TCI_CAMERASPACEPOSITION, D3DTSS_TCI_CAMERASPACEPOSITION | 1,
		D3DTSS_TCI_CAMERASPACENORMAL, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR | 1
	};
	static const DWORD transforms[] = { D3DTTFF_DISABLE, D3DTTFF_DISABLE, D3DTTFF_COUNT2, D3DTTFF_COUNT3 };
	static const DWORD filters[] = { D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
	static const DWORD mipFilters[] = { D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR };
	static const DWORD addresses[] = { D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_CLAMP, D3DTADDRESS_MIRROR };

#define PICK_OF(table) table[Pick(sizeof(table) / sizeof(table[0]))]
	switch (Pick(8))
	{
		case 0:
		case 1:
			SetStageState(stage, D3DTSS_COLOROP, stage == 0 && Pick(8) != 0 ? D3DTOP_MODULATE : PICK_OF(colorOperations));
			SetStageState(stage, D3DTSS_COLORARG1, PICK_OF(arguments));
			SetStageState(stage, D3DTSS_COLORARG2, PICK_OF(arguments));
			break;

		case 2:
			SetStageState(stage, D3DTSS_ALPHAOP, PICK_OF(alphaOperations));
			SetStageState(stage, D3DTSS_ALPHAARG1, PICK_OF(arguments));
			SetStageState(stage, D3DTSS_ALPHAARG2, PICK_OF(arguments));
			break;

		case 3:
			SetStageState(stage, D3DTSS_TEXCOORDINDEX, PICK_OF(coordinates));
			SetStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, PICK_OF(transforms));
			break;

		case 4:
			SetStageState(stage, D3DTSS_MINFILTER, PICK_OF(filters));
			SetStageState(stage, D3DTSS_MAGFILTER, PICK_OF(filters));
			SetStageState(stage, D3DTSS_MIPFILTER, PICK_OF(mipFilters));
			break;

		case 5:
			SetStageState(stage, D3DTSS_ADDRESSU, PICK_OF(addresses));
			SetStageState(stage, D3DTSS_ADDRESSV, PICK_OF(addresses));
			break;

		case 6:
			SetStageState(stage, D3DTSS_COLOROP, PICK_OF(colorOperations));
			SetStageState(stage, D3DTSS_COLORARG0, PICK_OF(arguments));
			break;

		default:
			// The usual: the texture times what came before.
			SetStageState(stage, D3DTSS_COLOROP, D3DTOP_MODULATE);
			SetStageState(stage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			SetStageState(stage, D3DTSS_COLORARG2, stage == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
			SetStageState(stage, D3DTSS_TEXCOORDINDEX, stage);
			break;
	}
#undef PICK_OF
}

//-----------------------------------------------------------------------------
void ChangeTexture(DWORD stage)
{
	IDirect3DTexture8 *texture = Pick(6) == 0 ? nullptr : s_textures[Pick(TEXTURE_COUNT)].texture;

	// Not a texture that another stage has. Direct3D keeps the filter and the addressing of a
	// texture per stage and OpenGL per texture, so one texture on two stages has those of one
	// of them, and which one is not something two versions of the layer have to agree on.
	for (DWORD other = 0; other < 8 && texture != nullptr; ++other)
	{
		IDirect3DBaseTexture8 *set = nullptr;
		if (other != stage && SUCCEEDED(s_device->GetTexture(other, &set)) && set == texture)
			texture = nullptr;
		if (set != nullptr)
			set->Release();
	}

	s_device->SetTexture(stage, texture);

	IDirect3DBaseTexture8 *read = nullptr;
	if (FAILED(s_device->GetTexture(stage, &read)) || read != texture)
		Mismatch("a texture does not read back as it was set");
	if (read != nullptr)
		read->Release();
}

//-----------------------------------------------------------------------------
D3DMATRIX MakeTransform(float spread)
{
	const float angle = 6.2831853f * Fraction();
	const float tilt = 3.1415927f * (Fraction() - 0.5f);
	const float scale = 0.5f + Fraction();

	D3DMATRIX matrix;
	memset(&matrix, 0, sizeof(matrix));
	matrix.m[0][0] = scale * cosf(angle);
	matrix.m[0][1] = scale * sinf(angle);
	matrix.m[1][0] = -scale * sinf(angle) * cosf(tilt);
	matrix.m[1][1] = scale * cosf(angle) * cosf(tilt);
	matrix.m[1][2] = scale * sinf(tilt);
	matrix.m[2][1] = -scale * sinf(tilt);
	matrix.m[2][2] = scale * cosf(tilt);
	matrix.m[3][0] = spread * (Fraction() - 0.5f);
	matrix.m[3][1] = spread * (Fraction() - 0.5f);
	matrix.m[3][2] = spread * Fraction();
	matrix.m[3][3] = 1.0f;
	return matrix;
}

//-----------------------------------------------------------------------------
void SetMatrix(D3DTRANSFORMSTATETYPE state, const D3DMATRIX &matrix)
{
	s_device->SetTransform(state, &matrix);

	D3DMATRIX read;
	memset(&read, 0xFF, sizeof(read));
	if (FAILED(s_device->GetTransform(state, &read)) || memcmp(&read, &matrix, sizeof(matrix)) != 0)
		Mismatch("a matrix does not read back as it was set");
}

//-----------------------------------------------------------------------------
void SetProjection()
{
	const float nearPlane = 1.0f + 9.0f * Fraction();
	const float farPlane = 1000.0f + 4000.0f * Fraction();
	const float scaleY = 1.0f / tanf(0.3f + 0.4f * Fraction());

	D3DMATRIX matrix;
	memset(&matrix, 0, sizeof(matrix));
	matrix.m[0][0] = scaleY * 0.75f;
	matrix.m[1][1] = scaleY;
	matrix.m[2][2] = farPlane / (farPlane - nearPlane);
	matrix.m[2][3] = 1.0f;
	matrix.m[3][2] = -nearPlane * farPlane / (farPlane - nearPlane);
	SetMatrix(D3DTS_PROJECTION, matrix);
}

//-----------------------------------------------------------------------------
D3DCOLORVALUE MakeColor()
{
	D3DCOLORVALUE color;
	color.r = Fraction();
	color.g = Fraction();
	color.b = Fraction();
	color.a = Pick(2) ? 1.0f : Fraction();
	return color;
}

//-----------------------------------------------------------------------------
void ChangeMaterial()
{
	// The game has a handful of materials and sets the one of each mesh, so most of the
	// calls bring one the device has seen before.
	static D3DMATERIAL8 materials[6];
	static bool made = false;
	if (!made)
	{
		for (D3DMATERIAL8 &material : materials)
		{
			material.Diffuse = MakeColor();
			material.Ambient = MakeColor();
			material.Specular = MakeColor();
			material.Emissive = MakeColor();
			material.Power = 200.0f * Fraction() - 20.0f;
		}
		made = true;
	}

	const D3DMATERIAL8 &material = materials[Pick(6)];
	s_device->SetMaterial(&material);

	D3DMATERIAL8 read;
	memset(&read, 0xFF, sizeof(read));
	if (FAILED(s_device->GetMaterial(&read)) || memcmp(&read, &material, sizeof(material)) != 0)
		Mismatch("the material does not read back as it was set");
}

//-----------------------------------------------------------------------------
void ChangeLights()
{
	static D3DLIGHT8 lights[5];
	static bool made = false;
	if (!made)
	{
		memset(lights, 0, sizeof(lights));
		for (UINT i = 0; i < 5; ++i)
		{
			D3DLIGHT8 &light = lights[i];
			light.Type = i < 3 ? D3DLIGHT_DIRECTIONAL : (i == 3 ? D3DLIGHT_POINT : D3DLIGHT_SPOT);
			light.Diffuse = MakeColor();
			light.Specular = MakeColor();
			light.Ambient = MakeColor();
			light.Position.x = 200.0f * Fraction();
			light.Position.y = 200.0f * Fraction();
			light.Position.z = 50.0f * Fraction();
			light.Direction.x = Fraction() - 0.5f;
			light.Direction.y = Fraction() - 0.5f;
			light.Direction.z = -Fraction();
			light.Range = 300.0f;
			light.Attenuation0 = i == 4 ? 0.0f : 1.0f;
			light.Attenuation1 = i == 4 ? 0.0f : 0.01f * Fraction();
		}
		made = true;
	}

	for (DWORD index = 0; index < 4; ++index)
	{
		if (Pick(2))
		{
			const D3DLIGHT8 &light = lights[Pick(5)];
			s_device->SetLight(index, &light);

			D3DLIGHT8 read;
			memset(&read, 0xFF, sizeof(read));
			if (FAILED(s_device->GetLight(index, &read)) || memcmp(&read, &light, sizeof(light)) != 0)
				Mismatch("a light does not read back as it was set");
		}

		if (Pick(2))
		{
			const WINBOOL enable = Pick(3) != 0;
			s_device->LightEnable(index, enable);

			WINBOOL read = !enable;
			if (FAILED(s_device->GetLightEnable(index, &read)) || read != enable)
				Mismatch("a light's switch does not read back as it was set");
		}
	}
}

//-----------------------------------------------------------------------------
void ChangeClipPlane()
{
	const DWORD index = Pick(2);
	float plane[4] = { Fraction() - 0.5f, Fraction() - 0.5f, 1.0f, -10.0f * Fraction() };
	s_device->SetClipPlane(index, plane);

	float read[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (FAILED(s_device->GetClipPlane(index, read)) || memcmp(read, plane, sizeof(plane)) != 0)
		Mismatch("a clip plane does not read back as it was set");
}

//-----------------------------------------------------------------------------
void SetViewport(DWORD x, DWORD y, DWORD width, DWORD height)
{
	D3DVIEWPORT8 viewport;
	viewport.X = x;
	viewport.Y = y;
	viewport.Width = width;
	viewport.Height = height;
	viewport.MinZ = 0.0f;
	viewport.MaxZ = 1.0f;
	s_device->SetViewport(&viewport);

	D3DVIEWPORT8 read;
	memset(&read, 0xFF, sizeof(read));
	if (FAILED(s_device->GetViewport(&read)) || memcmp(&read, &viewport, sizeof(viewport)) != 0)
		Mismatch("the viewport does not read back as it was set");
}

//-----------------------------------------------------------------------------
/// What the game changes from one mesh to the next.
void ChangeForNextDraw()
{
	if (Pick(2))
		ChangeRenderStates(1 + Pick(5));
	if (Pick(3) == 0)
		ChangeMaterial();
	if (Pick(4) == 0)
		ChangeLights();

	for (DWORD stage = 0; stage < 2; ++stage)
	{
		if (Pick(2))
			ChangeStage(stage);
		if (Pick(2))
			ChangeTexture(stage);
	}

	// Stages the console has no texture unit for.
	if (Pick(24) == 0)
	{
		const DWORD stage = 2 + Pick(3);
		ChangeStage(stage);
		ChangeTexture(stage);
	}

	SetMatrix(D3DTS_WORLD, MakeTransform(500.0f));
	if (Pick(6) == 0)
		SetMatrix(Pick(2) ? D3DTS_TEXTURE0 : D3DTS_TEXTURE1, MakeTransform(2.0f));
	if (Pick(20) == 0)
		ChangeClipPlane();
}

///////////////////////////////////////////////////////////////////////////////
// Drawing
///////////////////////////////////////////////////////////////////////////////

void DrawMesh()
{
	MeshBuffer &buffer = s_meshBuffers[Pick(MESH_BUFFER_COUNT)];
	if (buffer.vertices == nullptr || buffer.indices == nullptr)
		return;

	const UINT mesh = Pick(MESHES_PER_BUFFER);
	s_device->SetVertexShader(buffer.format);
	s_device->SetStreamSource(0, buffer.vertices, buffer.stride);

	if (Pick(8) == 0)
	{
		// Without indices.
		const UINT count = 3 * (1 + Pick(MESH_VERTICES / 3));
		s_device->DrawPrimitive(D3DPT_TRIANGLELIST, mesh * MESH_VERTICES, count / 3);
		return;
	}

	// A part of the mesh's triangles, as with the game's meshes that have several materials.
	const UINT firstTriangle = Pick(MESH_INDICES / 3 / 2);
	const UINT triangles = 1 + Pick(MESH_INDICES / 3 - firstTriangle);

	if (buffer.absoluteIndices)
	{
		s_device->SetIndices(buffer.indices, 0);
		s_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, mesh * MESH_VERTICES, MESH_VERTICES,
			mesh * MESH_INDICES + firstTriangle * 3, triangles);
	}
	else
	{
		s_device->SetIndices(buffer.indices, mesh * MESH_VERTICES);
		s_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, MESH_VERTICES, mesh * MESH_INDICES + firstTriangle * 3, triangles);
	}

	IDirect3DVertexBuffer8 *readVertices = nullptr;
	UINT readStride = 0;
	if (FAILED(s_device->GetStreamSource(0, &readVertices, &readStride)) || readVertices != buffer.vertices || readStride != buffer.stride)
		Mismatch("the stream source does not read back as it was set");
	if (readVertices != nullptr)
		readVertices->Release();
}

//-----------------------------------------------------------------------------
/// Vertices that are written for this one draw call, as for particles, lines and the
/// game's two dimensional drawing.
void DrawDynamic(bool screenSpace)
{
	IDirect3DVertexBuffer8 *vertexBuffer = screenSpace ? s_screenVertices : s_dynamicVertices;
	DynamicWrites &writes = screenSpace ? s_screenWrites : s_dynamicWrites;
	UINT &vertexOffset = writes.offset;
	const DWORD format = screenSpace ? SCREEN_FORMAT : DYNAMIC_FORMAT;
	const UINT stride = VertexSize(format);
	if (vertexBuffer == nullptr || s_dynamicIndices == nullptr)
		return;

	const UINT vertexCount = 4 + Pick(60);
	const UINT indexCount = 3 * (2 + Pick(30));

	// The buffer starts over when it is full, and now and then before, as at the start of a
	// frame: several times within what the render thread has not drawn yet.
	if (vertexOffset + vertexCount > DYNAMIC_VERTICES || Pick(25) == 0)
		vertexOffset = 0;
	if (vertexOffset == 0)
		writes.ranges = 0;
	if (s_dynamicIndexOffset + indexCount > DYNAMIC_INDICES)
		s_dynamicIndexOffset = 0;

	BYTE *memory = nullptr;
	if (FAILED(vertexBuffer->Lock(vertexOffset * stride, vertexCount * stride, &memory, vertexOffset == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE)))
	{
		Mismatch("a dynamic vertex buffer could not be locked");
		return;
	}
	FillRandom(memory, vertexCount * stride);
	vertexBuffer->Unlock();

	s_device->SetVertexShader(format);
	s_device->SetStreamSource(0, vertexBuffer, stride);

	if (Pick(4) == 0)
	{
		s_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, vertexOffset, vertexCount - 2);
	}
	else
	{
		if (FAILED(s_dynamicIndices->Lock(s_dynamicIndexOffset * 2, indexCount * 2, &memory, s_dynamicIndexOffset == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE)))
		{
			Mismatch("the dynamic index buffer could not be locked");
			return;
		}
		uint16_t *indices = reinterpret_cast<uint16_t *>(memory);
		for (UINT i = 0; i < indexCount; ++i)
			indices[i] = static_cast<uint16_t>(Pick(vertexCount));
		s_dynamicIndices->Unlock();

		s_device->SetIndices(s_dynamicIndices, vertexOffset);
		s_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, vertexCount, s_dynamicIndexOffset, indexCount / 3);
		s_dynamicIndexOffset += indexCount;
	}

	const UINT slot = writes.ranges < DynamicWrites::MAX_RANGES ? writes.ranges++ : Pick(DynamicWrites::MAX_RANGES);
	writes.first[slot] = vertexOffset;
	writes.count[slot] = vertexCount;
	vertexOffset += vertexCount;

	// The engine draws what it wrote in several passes.
	if (Pick(3) == 0)
		s_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, writes.first[slot], writes.count[slot] - 2);

	// And something it wrote before, which the buffer still has.
	if (Pick(5) == 0)
	{
		const UINT earlier = Pick(writes.ranges);
		s_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, writes.first[earlier], writes.count[earlier] - 2);
	}

	// The engine writes the skins that did not fit over the ones that did, after it has drawn
	// those: a lock that promises not to do that.
	if (Pick(30) == 0)
	{
		const UINT again = Pick(writes.ranges);
		if (SUCCEEDED(vertexBuffer->Lock(writes.first[again] * stride, writes.count[again] * stride, &memory, D3DLOCK_NOOVERWRITE)))
		{
			FillRandom(memory, writes.count[again] * stride);
			vertexBuffer->Unlock();
			s_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, writes.first[again], writes.count[again] - 2);
		}
	}

	// A lock that promises nothing, for some of the vertices. Everything else in the buffer
	// stays, and is drawn after it.
	if (Pick(60) == 0)
	{
		const UINT changed = Pick(writes.ranges);
		if (SUCCEEDED(vertexBuffer->Lock(writes.first[changed] * stride, writes.count[changed] * stride, &memory, 0)))
		{
			FillRandom(memory, writes.count[changed] * stride);
			vertexBuffer->Unlock();
			s_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, writes.first[changed], writes.count[changed] - 2);

			const UINT other = Pick(writes.ranges);
			s_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, writes.first[other], writes.count[other] - 2);
		}
	}
}

//-----------------------------------------------------------------------------
/// Vertices handed over with the draw call.
void DrawFromMemory()
{
	struct ScreenVertex
	{
		float x, y, z, rhw;
		DWORD color;
		float u, v;
	};
	struct PlainVertex
	{
		float x, y, z;
		float u, v;
	};

	static const uint16_t quad[6] = { 0, 1, 2, 2, 1, 3 };

	if (Pick(3) == 0)
	{
		// Without colors the layer has nothing to convert.
		PlainVertex vertices[4];
		FillRandom(vertices, sizeof(vertices));
		s_device->SetVertexShader(D3DFVF_XYZ | D3DFVF_TEX1);
		if (Pick(2))
			s_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(PlainVertex));
		else
			s_device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, quad, D3DFMT_INDEX16, vertices, sizeof(PlainVertex));
		return;
	}

	ScreenVertex vertices[4];
	FillRandom(vertices, sizeof(vertices));
	s_device->SetVertexShader(SCREEN_FORMAT);
	if (Pick(2))
		s_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ScreenVertex));
	else
		s_device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, quad, D3DFMT_INDEX16, vertices, sizeof(ScreenVertex));
}

//-----------------------------------------------------------------------------
void Clear(UINT width, UINT height)
{
	const DWORD flags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | (Pick(3) == 0 ? D3DCLEAR_STENCIL : 0);
	const D3DCOLOR color = Next();

	if (Pick(5) == 0)
	{
		D3DRECT rects[2];
		for (D3DRECT &rect : rects)
		{
			rect.x1 = static_cast<LONG>(Pick(width / 2));
			rect.y1 = static_cast<LONG>(Pick(height / 2));
			rect.x2 = rect.x1 + 1 + static_cast<LONG>(Pick(width / 2));
			rect.y2 = rect.y1 + 1 + static_cast<LONG>(Pick(height / 2));
		}
		s_device->Clear(1 + Pick(2), rects, flags, color, 1.0f, 0);
	}
	else
	{
		s_device->Clear(0, nullptr, flags, color, 1.0f, Pick(2) ? 0 : 0x80);
	}
}

//-----------------------------------------------------------------------------
void Frame(UINT width, UINT height)
{
	// Mostly the whole target; a part of it as for the game's second views.
	if (Pick(6) == 0)
		SetViewport(Pick(width / 4), Pick(height / 4), width / 2, height / 2);
	else
		SetViewport(0, 0, width, height);

	Clear(width, height);
	if (Pick(4) == 0)
		s_device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

	s_device->BeginScene();
	SetMatrix(D3DTS_VIEW, MakeTransform(1000.0f));
	SetProjection();

	const UINT draws = 30 + Pick(40);
	for (UINT i = 0; i < draws; ++i)
	{
		ChangeForNextDraw();

		const UINT kind = Pick(10);
		if (kind < 6)
			DrawMesh();
		else if (kind < 8)
			DrawDynamic(false);
		else if (kind < 9)
			DrawDynamic(true);
		else
			DrawFromMemory();

		// What the game does between draw calls now and then.
		switch (Pick(60))
		{
			case 0:
			case 1:
			case 2:
			case 3:
				ReplaceTexture(s_textures[Pick(TEXTURE_COUNT)]);
				break;

			case 4:
			case 5:
			case 6:
				RewriteTexture(s_textures[Pick(TEXTURE_COUNT)]);
				break;

			case 7:
			{
				MeshBuffer &buffer = s_meshBuffers[Pick(MESH_BUFFER_COUNT)];
				const UINT number = static_cast<UINT>(&buffer - s_meshBuffers) + Pick(4) * MESH_BUFFER_COUNT;
				ReleaseMeshBuffer(buffer);
				MakeMeshBuffer(buffer, number);
				break;
			}

			case 8:
			case 9:
			{
				// A few vertices of a mesh change, as the terrain's do when the view moves.
				MeshBuffer &buffer = s_meshBuffers[Pick(MESH_BUFFER_COUNT)];
				if (buffer.vertices != nullptr)
					FillMeshVertices(buffer, Pick(MESHES_PER_BUFFER * MESH_VERTICES - 40), 1 + Pick(40));
				break;
			}

			case 10:
			{
				MeshBuffer &buffer = s_meshBuffers[Pick(MESH_BUFFER_COUNT)];
				if (buffer.indices != nullptr)
					FillMeshIndices(buffer, Pick(MESHES_PER_BUFFER));
				break;
			}

			case 11:
			case 12:
			{
				// A texture is written to while its stage is switched off, and drawn with once
				// the stage is switched on again. Nobody sets a texture in between, and the
				// stage still has to show the new pixels.
				const UINT number = Pick(TEXTURE_COUNT);
				TestTexture &slot = s_textures[number];
				if (slot.texture == nullptr || slot.compressed)
					break;

				for (DWORD stage = 0; stage < 2; ++stage)
				{
					SetStageState(stage, D3DTSS_COLOROP, D3DTOP_MODULATE);
					SetStageState(stage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
					SetStageState(stage, D3DTSS_COLORARG2, stage == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
				}

				// Another texture on the first stage, see ChangeTexture().
				s_device->SetTexture(0, s_textures[(number + 1) % TEXTURE_COUNT].texture);
				s_device->SetTexture(1, slot.texture);
				DrawMesh();

				SetStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
				RewriteTexture(slot);
				DrawMesh();

				SetStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
				DrawMesh();
				break;
			}

			default:
				break;
		}
	}

	s_device->EndScene();
	s_device->Present(nullptr, nullptr, nullptr, nullptr);
}

} // namespace

//-----------------------------------------------------------------------------
int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "d3d8test <trace file> [frames] [seed]\n");
		return 2;
	}

	const UINT frames = argc > 2 ? static_cast<UINT>(atoi(argv[2])) : 60;
	if (argc > 3 && atoi(argv[3]) != 0)
		s_seed = static_cast<uint32_t>(atoi(argv[3]));

	const char *threadSwitch = getenv("VITA_RENDER_THREAD");
	const bool threaded = threadSwitch == nullptr || atoi(threadSwitch) != 0;

	// With a frame profile the layer also writes each new kind of state to the log.
	VitaDiagnostics::g_profiling = getenv("D3D8TEST_PROFILE") != nullptr;

	if (!MockGL_Open(argv[1]))
	{
		fprintf(stderr, "d3d8test: cannot write %s\n", argv[1]);
		return 2;
	}

	IDirect3D8 *direct3D = Direct3DCreate8(D3D_SDK_VERSION);
	if (direct3D == nullptr)
	{
		fprintf(stderr, "d3d8test: no Direct3D\n");
		return 2;
	}

	D3DPRESENT_PARAMETERS presentation;
	memset(&presentation, 0, sizeof(presentation));
	presentation.BackBufferWidth = 800;
	presentation.BackBufferHeight = 600;
	presentation.BackBufferFormat = D3DFMT_X8R8G8B8;
	presentation.BackBufferCount = 1;
	presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
	presentation.Windowed = TRUE;
	presentation.EnableAutoDepthStencil = TRUE;
	presentation.AutoDepthStencilFormat = D3DFMT_D24S8;

	if (FAILED(direct3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, nullptr, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &presentation, &s_device))
		|| s_device == nullptr)
	{
		fprintf(stderr, "d3d8test: no device\n");
		return 2;
	}

	for (TestTexture &slot : s_textures)
		MakeTexture(slot);
	for (UINT i = 0; i < MESH_BUFFER_COUNT; ++i)
		MakeMeshBuffer(s_meshBuffers[i], i);
	MakeDynamicBuffers();

	UINT width = presentation.BackBufferWidth;
	UINT height = presentation.BackBufferHeight;

	for (UINT frame = 0; frame < frames; ++frame)
	{
		if (frame == frames / 2 && frames > 1)
		{
			// The game resets the device when the resolution is changed in its options.
			presentation.BackBufferWidth = 960;
			presentation.BackBufferHeight = 544;
			if (FAILED(s_device->Reset(&presentation)))
				Mismatch("the device could not be reset");
			width = presentation.BackBufferWidth;
			height = presentation.BackBufferHeight;
		}

		Frame(width, height);
	}

	for (DWORD stage = 0; stage < 8; ++stage)
		s_device->SetTexture(stage, nullptr);
	s_device->SetStreamSource(0, nullptr, 0);
	s_device->SetIndices(nullptr, 0);

	for (TestTexture &slot : s_textures)
	{
		if (slot.texture != nullptr)
			slot.texture->Release();
	}
	for (MeshBuffer &buffer : s_meshBuffers)
		ReleaseMeshBuffer(buffer);
	if (s_dynamicVertices != nullptr)
		s_dynamicVertices->Release();
	if (s_screenVertices != nullptr)
		s_screenVertices->Release();
	if (s_dynamicIndices != nullptr)
		s_dynamicIndices->Release();

	s_device->Release();
	direct3D->Release();

	// Everything that was written has been done once this returns.
	VitaD3D8::RenderThread::Sync([] {});

	const unsigned int wrong = MockGL_Close(threaded) + s_mismatches;
	fprintf(stderr, "d3d8test: %u frames %s; %u things wrong\n", static_cast<unsigned int>(frames),
		threaded ? "with a render thread" : "without a render thread", wrong);
	return wrong == 0 ? 0 : 1;
}
