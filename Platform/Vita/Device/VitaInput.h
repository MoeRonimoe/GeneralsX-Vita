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

// VitaInput.h ////////////////////////////////////////////////////////////////
// Translates the PS Vita controls into the mouse and keyboard events the game
// expects.
//
//   Front touch        move the pointer and hold the left mouse button
//   Left stick         move the pointer
//   Cross / Circle     left / right mouse button
//   Right stick        scroll the map (arrow keys)
//   D-pad up / down    zoom (mouse wheel)
//   D-pad left / right rotate the camera (numpad 4 / 6)
//   L / R              Ctrl / Shift
//   Triangle           select all units (Q)
//   Square             attack move (A)
//   Start / Select     Escape / jump to the last radar event (Space)
///////////////////////////////////////////////////////////////////////////////

#pragma once

namespace VitaInput
{
	/// Physical size of the panel; touch and pointer coordinates are scaled from it.
	enum
	{
		SCREEN_WIDTH = 960,
		SCREEN_HEIGHT = 544
	};

	/// Open the controller and touch ports. Safe to call more than once.
	void init();

	/// Read the controls and queue events on TheMouse and TheKeyboard. Call once per frame.
	void poll();
}
