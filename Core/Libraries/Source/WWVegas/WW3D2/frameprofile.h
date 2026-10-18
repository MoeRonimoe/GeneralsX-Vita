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

// GeneralsX @feature Claude 11/10/2026 Timers for the parts of a frame that WW3D spends time in, read by
// the PS Vita frame profile. The game engine has its own (Common/PerfTimer.h), which this
// library cannot include.

#pragma once

#if defined(__vita__) && defined(VITA_FRAME_PROFILE)

#include <VitaDiagnostics.h>

/// Counts the time from here to the end of the enclosing block for the timer of this name.
#define WW3D_PROFILE_SCOPE(id) \
	static VitaDiagnostics::NamedTimer s_ww3dTimer_##id(#id); \
	const VitaDiagnostics::NamedTimerScope ww3dTimerScope_##id(s_ww3dTimer_##id)

#else

#define WW3D_PROFILE_SCOPE(id)

#endif
