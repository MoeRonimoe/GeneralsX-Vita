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

// VitaD3D8Tags.h /////////////////////////////////////////////////////////////
// What the engine can tell the PS Vita's Direct3D layer about a resource, with
// IDirect3DResource8::SetPrivateData(). Include it after <d3d8.h>.
///////////////////////////////////////////////////////////////////////////////

#pragma once

/// For a dynamic vertex buffer, with a DWORD that is not zero: whoever writes to this buffer
/// keeps the rules of dynamic buffers.
///
///   - After a lock with D3DLOCK_DISCARD nothing that was in the buffer is drawn again.
///   - A lock with D3DLOCK_NOOVERWRITE leaves alone what draw calls have been issued for.
///
/// The layer then draws from the buffer's memory instead of from a copy made for each draw
/// call: the thread that draws is behind the one that writes, and with these rules what it
/// still has to draw is not written over. A buffer is not tagged by default because code
/// that breaks the first rule exists: the game's trees lock their buffer with
/// D3DLOCK_DISCARD and write only the trees that moved.
static const GUID VITAD3D8_KEEPS_DYNAMIC_BUFFER_RULES =
	{ 0x5d6a4c1e, 0x3b7f, 0x4e52, { 0x9a, 0x41, 0x47, 0x58, 0x56, 0x49, 0x54, 0x41 } };
