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

// VitaSystemDialog.h /////////////////////////////////////////////////////////
// Whether a dialog of the system is open over the game.
//
// The console's own dialogs, like the on-screen keyboard of Device/VitaTextInput,
// are drawn by the system into the game's picture. It does that when the game
// hands over a finished picture and says that a dialog is open (vitaGL's
// vglSwapBuffers(GL_TRUE)), which the Direct3D layer asks this for.
///////////////////////////////////////////////////////////////////////////////

#pragma once

namespace VitaSystemDialog
{

/// Whether the pictures handed over have to let the system draw its dialog.
bool isShown();

/// Set while a dialog of the system is open. For the game's thread.
void setShown(bool shown);

}
