/*
**	Command & Conquer Generals Zero Hour(tm)
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

// FILE: W3DTreeBatch.h ///////////////////////////////////////////////////////////////////////////
// GeneralsX @performance Claude 19/11/2026 What W3DModelDraw needs of a renderer that draws the trees
// and shrubs that stand still together, in a few draw calls.
//
// The trees of the base game are objects like any other, and each was drawn like any other: put
// into the scene, lit, handed to the mesh renderer and drawn with a draw call of its own. A map has
// more than a thousand of them, and on the PS Vita that was a quarter of the time of a frame. An
// intact tree that stands still looks the same on every frame, so its model can be drawn from a
// buffer that holds many such trees at once. The tree stays an object: it can burn, spread the fire,
// be crushed and fall; W3DModelDraw takes it out of the batch as soon as it does anything but stand.
//
// The renderer belongs to the game that has one (Generals: W3DTreeBatchRenderer); without one,
// TheW3DTreeBatch is null and every model is drawn on its own, as before.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

class RenderObjClass;
class W3DModelDraw;

class W3DTreeBatchInterface
{
public:
	virtual ~W3DTreeBatchInterface() {}

	/// Draws the model from now on, where it is placed now, until removeTree(). False if it
	/// cannot (a model that is not one simple mesh); the model is then drawn on its own. A tree
	/// that is not shown is held without being drawn, until showTree().
	virtual bool addTree(const W3DModelDraw *owner, RenderObjClass *model, bool shown) = 0;

	/// Stops drawing the model of this owner.
	virtual void removeTree(const W3DModelDraw *owner) = 0;

	/// Whether the model of this owner is drawn while it is in the batch: not while the fog of war
	/// hides it (W3DModelDraw::setFullyObscuredByShroud()), as the scene would not draw it either.
	virtual void showTree(const W3DModelDraw *owner, bool shown) = 0;

	/// Why the last addTree() said no, for the log.
	virtual const char *lastRefusal() const = 0;
};

extern W3DTreeBatchInterface *TheW3DTreeBatch;
