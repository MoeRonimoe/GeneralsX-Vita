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

// VitaPacing.h ///////////////////////////////////////////////////////////////
// Keeping the game at its speed on a console that cannot draw it at its speed.
//
// The game's logic takes 30 steps for a second of the game, and the engine takes
// at most one step per frame it draws. The console draws 20 frames per second
// in an ordinary scene and 13 in a crowded one, so a second of the game takes one
// and a half to two and a half seconds: everything moves in slow motion, and the
// voices of a mission, which run by the clock, are done before what they are
// about.
//
// With catching up on, the logic is owed all the time that passes, and when it
// is half a step or more behind, the next picture is not drawn. A frame without
// a picture takes less than half the time, so the logic gets its 30 steps where
// that is possible at all, at fewer pictures per second. Never two pictures in a
// row are left out, and none where that would put the pictures more than a tenth
// of a second apart on average: a crowded scene gets two or three pictures
// between those left out, or all of them. What a mission shows by itself, with
// the camera moved by its script or the player's input switched off, is drawn
// in full; there the pictures matter more than the pace.
//
// It is on unless "+VITA_CATCHUP=0" is in vita_args.txt; both shoulder buttons
// held and Select switch it while playing.
///////////////////////////////////////////////////////////////////////////////

#pragma once

namespace VitaPacing
{

void setCatchUp(bool enabled);
bool isCatchUp();

/// Told by the engine after it decided whether the logic takes a step in this frame: the
/// time the logic is still owed, and the time of one step, in seconds.
void setLogicDebt(float owedSeconds, float stepSeconds);

/// Asked by the display once for every picture it is about to draw. True if this one is to
/// be left out. gameIsRunning: a mission is being played, its time is not standing still,
/// and it is the player's to play, not the mission's to show.
bool leaveOutPicture(bool gameIsRunning);

/// Told at the end of every frame how long its work took, in seconds, and whether a mission
/// was running in it. From this it is known how far apart the pictures would be.
void noteFrame(float seconds, bool gameIsRunning);

/// Pictures drawn and pictures left out since this was last asked, for the log.
void takeCounts(unsigned int &drawn, unsigned int &leftOut);

} // namespace VitaPacing
