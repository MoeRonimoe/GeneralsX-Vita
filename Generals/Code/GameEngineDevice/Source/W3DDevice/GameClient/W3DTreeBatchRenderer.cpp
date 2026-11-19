/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// FILE: W3DTreeBatchRenderer.cpp /////////////////////////////////////////////////////////////////
// GeneralsX @performance Claude 19/11/2026 See W3DTreeBatchRenderer.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "W3DDevice/GameClient/W3DTreeBatchRenderer.h"

#include "Common/Debug.h"

#include "WW3D2/camera.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/matpass.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/texture.h"
#include "WW3D2/vertmaterial.h"
#include "WWMath/aabox.h"
#include "WWMath/colmath.h"
#include "WWMath/frustum.h"
#include "WWMath/wwmath.h"

#include <algorithm>
#include <cstring>

#if defined(__vita__) && defined(VITA_FRAME_PROFILE)
#include <VitaDiagnostics.h>
#include <cstdio>
#endif

namespace
{
	/// Width of the square of the map whose trees of one kind are drawn together, in world units.
	/// A chunk that is partly in the view is drawn whole, which costs the graphics chip little; a
	/// draw call costs the game's thread and the render thread, so chunks are large.
	const Real CELL_SIZE = 1000.0f;

	/// The size buffers are made for: what is needed, and a quarter more for trees that come later.
	Int capacityFor(Int count)
	{
		return count + count / 4 + 64;
	}

	/// The meshes a model draws: the model itself if it is a mesh, or the meshes of a level of detail
	/// model with one level, leaving out the box that only gives its bounds (as HLodClass::Render()
	/// does) and meshes that are hidden (as MeshClass::Render() does). The trees of the base game are
	/// of the second kind, palms with a mesh for the trunk and one for the leaves. Takes a reference
	/// to each mesh, like Get_Sub_Object(). Returns -1 for a model with anything else in it.
	Int collectMeshes(RenderObjClass *model, MeshClass **meshes, Int maxMeshes)
	{
		if (model->Class_ID() == RenderObjClass::CLASSID_MESH)
		{
			model->Add_Ref();
			meshes[0] = static_cast<MeshClass *>(model);
			return 1;
		}

		if (model->Class_ID() != RenderObjClass::CLASSID_HLOD || model->Get_LOD_Count() != 1)
			return -1;

		Int count = 0;
		const Int subCount = model->Get_Num_Sub_Objects();
		for (Int i = 0; i < subCount; ++i)
		{
			RenderObjClass *sub = model->Get_Sub_Object(i);
			if (sub == nullptr)
				continue;
			if (sub->Class_ID() == RenderObjClass::CLASSID_OBBOX || (sub->Class_ID() == RenderObjClass::CLASSID_MESH && !sub->Is_Not_Hidden_At_All()))
			{
				sub->Release_Ref();
				continue;
			}
			if (sub->Class_ID() != RenderObjClass::CLASSID_MESH || count == maxMeshes)
			{
				sub->Release_Ref();
				for (Int j = 0; j < count; ++j)
					meshes[j]->Release_Ref();
				return -1;
			}
			meshes[count++] = static_cast<MeshClass *>(sub);
		}
		return count;
	}

	/// Whether the batch draws the mesh the same way as the mesh renderer would: one pass, one
	/// texture, material and shader for the whole mesh, no skin, nothing overridden for this instance
	/// (DX8TextureCategoryClass::Render()).
	bool meshFits(MeshClass *mesh, Int maxVertices, Int maxIndices)
	{
		MeshModelClass *meshModel = mesh->Peek_Model();
		if (meshModel == nullptr || mesh->Get_Alpha_Override() != 1.0f)
			return false;
		const void *userData = mesh->Get_User_Data();
		if (userData != nullptr && *static_cast<const int *>(userData) == RenderObjClass::USER_DATA_MATERIAL_OVERRIDE)
			return false;
		return !meshModel->Get_Flag(MeshGeometryClass::SKIN) && meshModel->Get_Pass_Count() == 1 &&
			!meshModel->Has_Texture_Array(0, 0) && !meshModel->Has_Material_Array(0) && !meshModel->Has_Shader_Array(0) &&
			meshModel->Peek_Single_Texture(0, 0) != nullptr && meshModel->Peek_Single_Material(0) != nullptr &&
			meshModel->Get_UV_Array(0, 0) != nullptr &&
			meshModel->Get_Vertex_Count() > 0 && meshModel->Get_Vertex_Count() <= maxVertices &&
			meshModel->Get_Polygon_Count() > 0 && meshModel->Get_Polygon_Count() * 3 <= maxIndices;
	}
}

//-------------------------------------------------------------------------------------------------
W3DTreeBatchRenderer::W3DTreeBatchRenderer() :
	m_treesAdded(0),
	m_treesRemoved(0),
	m_lastRefusal("")
{
}

//-------------------------------------------------------------------------------------------------
W3DTreeBatchRenderer::~W3DTreeBatchRenderer()
{
	for (size_t i = 0; i < m_chunks.size(); ++i)
		freeChunk(m_chunks[i]);
	m_chunks.clear();

	for (size_t i = 0; i < m_types.size(); ++i)
	{
		REF_PTR_RELEASE(m_types[i].texture);
		REF_PTR_RELEASE(m_types[i].material);
	}
	m_types.clear();
}

//-------------------------------------------------------------------------------------------------
bool W3DTreeBatchRenderer::addTree(const W3DModelDraw *owner, RenderObjClass *model, bool shown)
{
	if (owner == nullptr || model == nullptr)
		return false;
	removeTree(owner);

	MeshClass *meshes[MAX_TREE_MESHES];
	const Int meshCount = collectMeshes(model, meshes, MAX_TREE_MESHES);
	if (meshCount <= 0)
	{
		m_lastRefusal = "model of other parts than meshes";
		return false;
	}

	bool fits = true;
	for (Int i = 0; i < meshCount && fits; ++i)
		fits = meshFits(meshes[i], MAX_TREE_VERTICES, MAX_CHUNK_INDICES);

	if (fits)
	{
		std::vector<Place> &places = m_places[owner];
		places.clear();
		for (Int i = 0; i < meshCount; ++i)
			places.push_back(addMesh(owner, meshes[i], shown));
		++m_treesAdded;
	}
	else
	{
		m_lastRefusal = "mesh the batch cannot draw";
	}

	for (Int i = 0; i < meshCount; ++i)
		meshes[i]->Release_Ref();
	return fits;
}

//-------------------------------------------------------------------------------------------------
W3DTreeBatchRenderer::Place W3DTreeBatchRenderer::addMesh(const W3DModelDraw *owner, MeshClass *mesh, bool shown)
{
	MeshModelClass *meshModel = mesh->Peek_Model();
	const Int vertexCount = meshModel->Get_Vertex_Count();
	const Int polygonCount = meshModel->Get_Polygon_Count();
	const Vector3 *positions = meshModel->Get_Vertex_Array();
	const Vector3 *normals = meshModel->Get_Vertex_Normal_Array();
	const Vector2 *uvs = meshModel->Get_UV_Array(0, 0);
	const TriIndex *polygons = meshModel->Get_Polygon_Array();

	// Where the mesh is: the model was just placed, and the mesh sits on a bone of it.
	const Matrix3D world = mesh->Get_Transform();
	Vector3 origin;
	world.Get_Translation(&origin);

	const Int typeIndex = findOrAddType(meshModel->Peek_Single_Texture(0, 0), meshModel->Peek_Single_Material(0), meshModel->Get_Single_Shader(0));
	const Int cellX = static_cast<Int>(WWMath::Floor(origin.X / CELL_SIZE));
	const Int cellY = static_cast<Int>(WWMath::Floor(origin.Y / CELL_SIZE));
	Chunk *chunk = findOrAddChunk(typeIndex, cellX, cellY, vertexCount, polygonCount * 3);

	const Int firstVertex = static_cast<Int>(chunk->vertices.size());
	for (Int i = 0; i < vertexCount; ++i)
	{
		Vector3 position;
		Matrix3D::Transform_Vector(world, positions[i], &position);
		Vector3 normal = normals != nullptr ? normals[i] : Vector3(0.0f, 0.0f, 1.0f);
		Matrix3D::Rotate_Vector(world, normal, &normal);
		normal.Normalize();

		VertexFormatXYZNUV1 vertex;
		vertex.x = position.X;
		vertex.y = position.Y;
		vertex.z = position.Z;
		vertex.nx = normal.X;
		vertex.ny = normal.Y;
		vertex.nz = normal.Z;
		vertex.u1 = uvs[i].X;
		vertex.v1 = uvs[i].Y;
		chunk->vertices.push_back(vertex);

		if (chunk->vertices.size() == 1 && chunk->members.empty())
		{
			chunk->boundsMin = position;
			chunk->boundsMax = position;
		}
		else
		{
			chunk->boundsMin.Update_Min(position);
			chunk->boundsMax.Update_Max(position);
		}
	}

	Member member;
	member.owner = owner;
	member.firstIndex = static_cast<Int>(chunk->memberIndices.size());
	member.indexCount = polygonCount * 3;
	member.present = TRUE;
	member.shown = shown;
	for (Int i = 0; i < polygonCount; ++i)
	{
		chunk->memberIndices.push_back(static_cast<UnsignedShort>(firstVertex + polygons[i].I));
		chunk->memberIndices.push_back(static_cast<UnsignedShort>(firstVertex + polygons[i].J));
		chunk->memberIndices.push_back(static_cast<UnsignedShort>(firstVertex + polygons[i].K));
	}
	chunk->members.push_back(member);
	++chunk->presentCount;
	chunk->verticesChanged = TRUE;
	chunk->indicesChanged = TRUE;

	Place place;
	place.chunk = chunk;
	place.member = static_cast<Int>(chunk->members.size()) - 1;
	return place;
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::removeTree(const W3DModelDraw *owner)
{
	PlaceMap::iterator it = m_places.find(owner);
	if (it == m_places.end())
		return;

	for (size_t i = 0; i < it->second.size(); ++i)
	{
		Chunk *chunk = it->second[i].chunk;
		Member &member = chunk->members[it->second[i].member];
		if (member.present)
		{
			member.present = FALSE;
			--chunk->presentCount;
			chunk->indicesChanged = TRUE;
		}
	}
	m_places.erase(it);
	++m_treesRemoved;
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::showTree(const W3DModelDraw *owner, bool shown)
{
	PlaceMap::iterator it = m_places.find(owner);
	if (it == m_places.end())
		return;

	// Only the index buffer of the chunk is written again, which does not wait for the render thread.
	for (size_t i = 0; i < it->second.size(); ++i)
	{
		Chunk *chunk = it->second[i].chunk;
		Member &member = chunk->members[it->second[i].member];
		if (member.shown != static_cast<Bool>(shown))
		{
			member.shown = shown;
			chunk->indicesChanged = TRUE;
		}
	}
}

//-------------------------------------------------------------------------------------------------
Int W3DTreeBatchRenderer::findOrAddType(TextureClass *texture, VertexMaterialClass *material, const ShaderClass &shader)
{
	for (size_t i = 0; i < m_types.size(); ++i)
	{
		if (m_types[i].texture == texture && m_types[i].material == material && m_types[i].shader.Get_Bits() == shader.Get_Bits())
			return static_cast<Int>(i);
	}

	TreeType type;
	type.texture = texture;
	type.material = material;
	type.shader = shader;
	texture->Add_Ref();
	material->Add_Ref();
	m_types.push_back(type);
	return static_cast<Int>(m_types.size()) - 1;
}

//-------------------------------------------------------------------------------------------------
W3DTreeBatchRenderer::Chunk *W3DTreeBatchRenderer::findOrAddChunk(Int typeIndex, Int cellX, Int cellY, Int vertexCount, Int indexCount)
{
	size_t insertAt = m_chunks.size();
	for (size_t i = 0; i < m_chunks.size(); ++i)
	{
		Chunk *chunk = m_chunks[i];
		if (chunk->typeIndex == typeIndex)
		{
			insertAt = i + 1;
			if (chunk->cellX == cellX && chunk->cellY == cellY &&
					static_cast<Int>(chunk->vertices.size()) + vertexCount <= MAX_CHUNK_VERTICES &&
					static_cast<Int>(chunk->memberIndices.size()) + indexCount <= MAX_CHUNK_INDICES)
				return chunk;
		}
	}

	Chunk *chunk = new Chunk;
	chunk->typeIndex = typeIndex;
	chunk->cellX = cellX;
	chunk->cellY = cellY;
	chunk->presentCount = 0;
	chunk->boundsMin.Set(0.0f, 0.0f, 0.0f);
	chunk->boundsMax.Set(0.0f, 0.0f, 0.0f);
	chunk->vertexBuffer = nullptr;
	chunk->indexBuffer = nullptr;
	chunk->bufferVertexCount = 0;
	chunk->bufferIndexCount = 0;
	chunk->vertexCapacity = 0;
	chunk->indexCapacity = 0;
	chunk->verticesChanged = FALSE;
	chunk->indicesChanged = FALSE;
	m_chunks.insert(m_chunks.begin() + insertAt, chunk);
	return chunk;
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::freeChunk(Chunk *chunk)
{
	REF_PTR_RELEASE(chunk->vertexBuffer);
	REF_PTR_RELEASE(chunk->indexBuffer);
	delete chunk;
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::updateBuffers(Chunk *chunk)
{
	if (chunk->verticesChanged)
	{
		const Int count = static_cast<Int>(chunk->vertices.size());
		if (chunk->vertexBuffer == nullptr || count > chunk->vertexCapacity)
		{
			REF_PTR_RELEASE(chunk->vertexBuffer);
			chunk->vertexCapacity = std::min(capacityFor(count), static_cast<Int>(MAX_CHUNK_VERTICES));
			chunk->vertexBuffer = NEW_REF(DX8VertexBufferClass, (DX8_FVF_XYZNUV1, static_cast<unsigned short>(chunk->vertexCapacity), DX8VertexBufferClass::USAGE_DEFAULT));
		}
		{
			VertexBufferClass::WriteLockClass lock(chunk->vertexBuffer);
			memcpy(lock.Get_Vertex_Array(), &chunk->vertices[0], count * sizeof(VertexFormatXYZNUV1));
		}
		chunk->bufferVertexCount = count;
		chunk->verticesChanged = FALSE;
	}

	if (chunk->indicesChanged)
	{
		m_indexScratch.clear();
		for (size_t i = 0; i < chunk->members.size(); ++i)
		{
			const Member &member = chunk->members[i];
			if (member.present && member.shown)
			{
				m_indexScratch.insert(m_indexScratch.end(), chunk->memberIndices.begin() + member.firstIndex,
					chunk->memberIndices.begin() + member.firstIndex + member.indexCount);
			}
		}

		chunk->bufferIndexCount = static_cast<Int>(m_indexScratch.size());
		if (chunk->bufferIndexCount > 0)
		{
			if (chunk->indexBuffer == nullptr || chunk->bufferIndexCount > chunk->indexCapacity)
			{
				REF_PTR_RELEASE(chunk->indexBuffer);
				chunk->indexCapacity = std::min(capacityFor(chunk->bufferIndexCount), static_cast<Int>(MAX_CHUNK_INDICES));
				chunk->indexBuffer = NEW_REF(DX8IndexBufferClass, (static_cast<unsigned short>(chunk->indexCapacity)));
			}
			IndexBufferClass::WriteLockClass lock(chunk->indexBuffer);
			memcpy(lock.Get_Index_Array(), &m_indexScratch[0], chunk->bufferIndexCount * sizeof(UnsignedShort));
		}
		chunk->indicesChanged = FALSE;
	}
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::drawChunk(const Chunk *chunk) const
{
	DX8Wrapper::Set_Vertex_Buffer(chunk->vertexBuffer);
	DX8Wrapper::Set_Index_Buffer(chunk->indexBuffer, 0);
	DX8Wrapper::Draw_Triangles(0, static_cast<unsigned short>(chunk->bufferIndexCount / 3), 0,
		static_cast<unsigned short>(chunk->bufferVertexCount));
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::render(RenderInfoClass &rinfo, LightEnvironmentClass *lightEnv, MaterialPassClass *shroudPass)
{
	// Chunks whose trees have all left go; the others get the buffers of what they hold now.
	for (size_t i = 0; i < m_chunks.size(); )
	{
		Chunk *chunk = m_chunks[i];
		if (chunk->presentCount == 0)
		{
			for (size_t m = 0; m < chunk->members.size(); ++m)
			{
				DEBUG_ASSERTCRASH(!chunk->members[m].present, ("tree batch chunk without trees has a tree"));
			}
			freeChunk(chunk);
			m_chunks.erase(m_chunks.begin() + i);
			continue;
		}
		updateBuffers(chunk);
		++i;
	}

	m_visible.clear();
	const FrustumClass &frustum = rinfo.Camera.Get_Frustum();
	for (size_t i = 0; i < m_chunks.size(); ++i)
	{
		const Chunk *chunk = m_chunks[i];
		if (chunk->bufferIndexCount == 0 || chunk->indexBuffer == nullptr || chunk->vertexBuffer == nullptr)
			continue;
		AABoxClass box;
		box.Init_Min_Max(chunk->boundsMin, chunk->boundsMax);
		if (CollisionMath::Overlap_Test(frustum, box) == CollisionMath::OUTSIDE)
			continue;
		m_visible.push_back(chunk);
	}

	logStatistics(static_cast<UnsignedInt>(m_visible.size()));
	if (m_visible.empty())
		return;

	DX8Wrapper::Set_World_Identity();
	DX8Wrapper::Set_Light_Environment(lightEnv);
	DX8Wrapper::Set_Texture(1, nullptr);

	Int typeIndex = -1;
	for (size_t i = 0; i < m_visible.size(); ++i)
	{
		const Chunk *chunk = m_visible[i];
		if (chunk->typeIndex != typeIndex)
		{
			typeIndex = chunk->typeIndex;
			const TreeType &type = m_types[typeIndex];
			DX8Wrapper::Set_Texture(0, type.texture);
			DX8Wrapper::Set_Material(type.material);
			DX8Wrapper::Set_Shader(type.shader);
		}
		drawChunk(chunk);
	}

	// The fog of war over them: drawn again, darkened where the shroud texture says so, on the
	// pixels the first pass drew (the pass compares depth for equality).
	if (shroudPass != nullptr)
	{
		shroudPass->Install_Materials();
		for (size_t i = 0; i < m_visible.size(); ++i)
			drawChunk(m_visible[i]);
		shroudPass->UnInstall_Materials();
	}
}

//-------------------------------------------------------------------------------------------------
void W3DTreeBatchRenderer::logStatistics(UnsignedInt chunksDrawn)
{
#if defined(__vita__) && defined(VITA_FRAME_PROFILE)
	// With the PS Vita's frame profile the log says every ten seconds how many trees the batch draws.
	static uint64_t s_periodStart = 0;
	static UnsignedInt s_frames = 0;
	static UnsignedInt s_chunksDrawn = 0;
	if (!VitaDiagnostics::g_profiling)
		return;

	const uint64_t now = VitaDiagnostics::now();
	++s_frames;
	s_chunksDrawn += chunksDrawn;
	if (s_periodStart == 0)
	{
		s_periodStart = now;
		s_frames = 0;
		s_chunksDrawn = 0;
		return;
	}
	if (now - s_periodStart < 10 * 1000 * 1000 || s_frames == 0)
		return;

	unsigned int hidden = 0;
	for (PlaceMap::const_iterator it = m_places.begin(); it != m_places.end(); ++it)
	{
		if (!it->second.empty() && !it->second[0].chunk->members[it->second[0].member].shown)
			++hidden;
	}
	fprintf(stderr, "[trees] %u trees in the batch (%u of them hidden by the fog of war), in %u chunks of %u kinds; %.1f chunks drawn per frame; %u trees came in and %u left since the last line\n",
		static_cast<unsigned int>(m_places.size()), hidden, static_cast<unsigned int>(m_chunks.size()), static_cast<unsigned int>(m_types.size()),
		static_cast<double>(s_chunksDrawn) / s_frames, m_treesAdded, m_treesRemoved);
	fflush(stderr);
	s_periodStart = now;
	s_frames = 0;
	s_chunksDrawn = 0;
	m_treesAdded = 0;
	m_treesRemoved = 0;
#else
	(void)chunksDrawn;
#endif
}
