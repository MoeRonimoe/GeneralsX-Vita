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

// VitaHelper.h ///////////////////////////////////////////////////////////////
// A second pair of hands for the game's thread.
//
// The console has three cores for the game. One runs the game, one makes the
// calls to vitaGL, the third did nothing. This gives the game's thread a way
// to share out a loop whose passes do not depend on each other: both threads
// take passes until none are left.
///////////////////////////////////////////////////////////////////////////////

#pragma once

namespace VitaHelper
{

/// Calls task(context, index) once for every index below count, some from the calling
/// thread and some from the helper thread, and returns when all have returned.
///
/// The passes run at the same time, so they must not write to anything they share, and
/// must not do anything that only the game's thread may do: no engine allocators, no
/// strings, nothing that draws. One caller at a time: this is for the game's thread.
///
/// The helper is never waited for to begin. If it sleeps, the caller signals it and begins
/// with the passes itself.
void forEach(unsigned int count, void (*task)(void *context, unsigned int index), void *context);

/// Without the helper the calling thread makes all the passes, as if there were no helper
/// thread. For measuring what the helper is worth. For the game's thread.
void setEnabled(bool enabled);

/// How many passes were made since this was last asked, and how many of them by the helper
/// thread, which says whether it is of use: none means that it was asleep whenever a loop
/// was open. For the game's thread.
void takePassCounts(unsigned int &all, unsigned int &byHelper);

/// What forEach() has taken since this was last asked, for the loops of two passes or more.
struct LoopTimes
{
	unsigned long long sharedMicroseconds;	///< loops the helper could take part in
	unsigned int sharedPasses;
	unsigned long long aloneMicroseconds;	///< loops made while it was switched off
	unsigned int alonePasses;
};

/// For the game's thread.
LoopTimes takeLoopTimes();

} // namespace VitaHelper
