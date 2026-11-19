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

// FILE: W3DTreeBatchRenderer.h ///////////////////////////////////////////////////////////////////
// GeneralsX @performance Claude 19/11/2026 Draws the trees and shrubs that stand still, many in one draw
// call (W3DTreeBatch.h says why and what W3DModelDraw does with it).
//
// A tree that comes in is copied into the batch: the vertices of its mesh in world space, with
// their normals and texture coordinates, and its triangles. Trees whose meshes have the same
// texture, material and shader are drawn together, in chunks of the map CELL_SIZE wide, so that a
// chunk outside the view is passed over. The trees are lit by the hardware with the light
// environment the scene gives every object that only the sun and the moon reach, which is what
// each tree was lit with on its own, and the fog of war is laid over them in a second pass, as
// RTS3DScene does for every object (W3DShroudMaterialPassClass). A tree the fog of war hides
// (black where it was never seen, or a fogged one the ghost object draws as the player last saw it)
// stays in its chunk without its triangles being drawn, as the scene skips such objects.
//
// A tree that leaves the batch drops its triangles from its chunk's index buffer; its vertices stay
// until the chunk is empty. The buffers of a chunk are written again on the frame after the chunk
// changed, and only made anew when they are too small: making a buffer waits for the render
// thread. The scene puts all trees of a mission in when it begins (RTS3DScene::primeTreeBatch()),
// so that the buffers are made then.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "W3DDevice/GameClient/W3DTreeBatch.h"

#include "Lib/BaseType.h"
#include "WW3D2/dx8fvf.h"
#include "WW3D2/shader.h"
#include "WWMath/vector3.h"

#include <unordered_map>
#include <vector>

class DX8IndexBufferClass;
class DX8VertexBufferClass;
class LightEnvironmentClass;
class MeshClass;
class MaterialPassClass;
class RenderInfoClass;
class TextureClass;
class VertexMaterialClass;

class W3DTreeBatchRenderer : public W3DTreeBatchInterface
{
public:
	W3DTreeBatchRenderer();
	virtual ~W3DTreeBatchRenderer() override;

	virtual bool addTree(const W3DModelDraw *owner, RenderObjClass *model, bool shown) override;
	virtual void removeTree(const W3DModelDraw *owner) override;
	virtual void showTree(const W3DModelDraw *owner, bool shown) override;
	virtual const char *lastRefusal() const override { return m_lastRefusal; }

	/// Draws the trees in the view of the camera. shroudPass, when there is a shroud, darkens them
	/// where it is fogged.
	void render(RenderInfoClass &rinfo, LightEnvironmentClass *lightEnv, MaterialPassClass *shroudPass);

private:
	enum
	{
		MAX_CHUNK_VERTICES = 16384,
		MAX_CHUNK_INDICES = 49152,
		MAX_TREE_VERTICES = 4096,
		MAX_TREE_MESHES = 4
	};

	/// What the trees of a chunk are drawn with.
	struct TreeType
	{
		TextureClass *texture;
		VertexMaterialClass *material;
		ShaderClass shader;
	};

	struct Member
	{
		const W3DModelDraw *owner;
		Int firstIndex;		///< in Chunk::memberIndices
		Int indexCount;
		Bool present;		///< false once the tree left; its triangles stay in memberIndices
		Bool shown;			///< false while the fog of war hides the tree
	};

	struct Chunk
	{
		Int typeIndex;
		Int cellX;
		Int cellY;
		std::vector<VertexFormatXYZNUV1> vertices;
		std::vector<UnsignedShort> memberIndices;		///< the triangles of every member that came in, in order
		std::vector<Member> members;
		Int presentCount;
		Vector3 boundsMin;
		Vector3 boundsMax;
		DX8VertexBufferClass *vertexBuffer;
		DX8IndexBufferClass *indexBuffer;
		Int bufferVertexCount;		///< vertices in the buffer
		Int bufferIndexCount;		///< indices in the buffer that are drawn
		Int vertexCapacity;			///< what the buffers were made for
		Int indexCapacity;
		Bool verticesChanged;
		Bool indicesChanged;
	};

	struct Place
	{
		Chunk *chunk;
		Int member;
	};

	typedef std::unordered_map<const W3DModelDraw *, std::vector<Place> > PlaceMap;

	Place addMesh(const W3DModelDraw *owner, MeshClass *mesh, bool shown);
	Int findOrAddType(TextureClass *texture, VertexMaterialClass *material, const ShaderClass &shader);
	Chunk *findOrAddChunk(Int typeIndex, Int cellX, Int cellY, Int vertexCount, Int indexCount);
	void updateBuffers(Chunk *chunk);
	void drawChunk(const Chunk *chunk) const;
	void freeChunk(Chunk *chunk);
	void logStatistics(UnsignedInt chunksDrawn);

	std::vector<TreeType> m_types;
	std::vector<Chunk *> m_chunks;							///< sorted by type, so that a type's state is set once
	PlaceMap m_places;															///< where the meshes of each tree are
	std::vector<const Chunk *> m_visible;
	std::vector<UnsignedShort> m_indexScratch;
	UnsignedInt m_treesAdded;
	UnsignedInt m_treesRemoved;
	const char *m_lastRefusal;
};
