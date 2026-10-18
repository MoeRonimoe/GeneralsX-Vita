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

// VitaStubs.cpp //////////////////////////////////////////////////////////////
// Desktop services the engine calls that have no counterpart on the PS Vita.
///////////////////////////////////////////////////////////////////////////////

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/OSDisplay.h"

#include <cstdio>

//-----------------------------------------------------------------------------
/// The busy state keeps a desktop from starting its screen saver. The Vita's idle timers
/// are reset by the controls, which the game reads every frame.
void OSDisplaySetBusyState(Bool busyDisplay, Bool busySystem)
{
}

//-----------------------------------------------------------------------------
/// There are no system message boxes. The warning goes to the log so it is not lost.
OSDisplayButtonType OSDisplayWarningBox(AsciiString title, AsciiString message, UnsignedInt buttonFlags, UnsignedInt otherFlags)
{
	fprintf(stderr, "WARNING: %s: %s\n", title.str(), message.str());
	return OSDBT_OK;
}
