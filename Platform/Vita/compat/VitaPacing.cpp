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

// VitaPacing.cpp /////////////////////////////////////////////////////////////
// See VitaPacing.h. Everything here is the game's thread's.
///////////////////////////////////////////////////////////////////////////////

#include "VitaPacing.h"

namespace VitaPacing
{

namespace
{

// Pictures are to follow each other this closely at least, on average. A scene that takes
// so long to draw that leaving out every other picture would spread them further gets
// two or three pictures between those left out, or all of them.
const float LONGEST_BETWEEN_PICTURES = 0.100f;

// No frame is shorter than this: the engine's limit of 60 frames per second.
const float SHORTEST_FRAME = 1.0f / 60.0f;

// A frame that took longer than this read a file or built a window, and says nothing
// about how long frames take.
const float LONGEST_FRAME_COUNTED = 0.250f;

bool s_catchUp = false;
float s_owedSeconds = 0.0f;
float s_stepSeconds = 1.0f / 30.0f;

bool s_asked = false;			///< the display asked about a picture in the frame that is running
bool s_leftOut = false;			///< and was told to leave it out
unsigned int s_drawnInARow = 0;	///< pictures drawn since one was left out

// How long a frame with a picture and one without take, of late. 0: not known yet.
float s_withPicture = 0.0f;
float s_withoutPicture = 0.0f;

unsigned int s_drawnCount = 0;
unsigned int s_leftOutCount = 0;

//-----------------------------------------------------------------------------
void Average(float &average, float seconds)
{
	average = average == 0.0f ? seconds : average + (seconds - average) * 0.1f;
}

//-----------------------------------------------------------------------------
/// How many pictures are to be drawn between two that are left out; 0 if none may be.
unsigned int PicturesBetween()
{
	if (s_withPicture == 0.0f)
		return 1;

	// Until one was left out: what the frames without a picture took where it was measured.
	float without = s_withoutPicture != 0.0f ? s_withoutPicture : 0.45f * s_withPicture;
	if (without < SHORTEST_FRAME)
		without = SHORTEST_FRAME;

	for (unsigned int between = 1; between <= 3; ++between)
	{
		if (s_withPicture + without / between <= LONGEST_BETWEEN_PICTURES)
			return between;
	}

	return 0;
}

} // namespace

//-----------------------------------------------------------------------------
void setCatchUp(bool enabled)
{
	s_catchUp = enabled;
	s_drawnInARow = 0;
}

//-----------------------------------------------------------------------------
bool isCatchUp()
{
	return s_catchUp;
}

//-----------------------------------------------------------------------------
void setLogicDebt(float owedSeconds, float stepSeconds)
{
	s_owedSeconds = owedSeconds;
	s_stepSeconds = stepSeconds;
}

//-----------------------------------------------------------------------------
bool leaveOutPicture(bool gameIsRunning)
{
	// Half a step behind: the frame without a picture that follows lasts the other half at
	// least, so the logic takes a step in it. A frame with neither a step nor a picture
	// would be the worst of both.
	const unsigned int between = PicturesBetween();
	const bool leaveOut = s_catchUp && gameIsRunning && between != 0 && s_drawnInARow >= between
		&& s_owedSeconds >= 0.5f * s_stepSeconds;

	s_asked = true;
	s_leftOut = leaveOut;
	if (leaveOut)
	{
		s_drawnInARow = 0;
		++s_leftOutCount;
	}
	else
	{
		++s_drawnInARow;
		++s_drawnCount;
	}

	return leaveOut;
}

//-----------------------------------------------------------------------------
void noteFrame(float seconds, bool gameIsRunning)
{
	// Only the frames of a running mission say how long its frames take.
	if (s_asked && gameIsRunning && seconds <= LONGEST_FRAME_COUNTED)
		Average(s_leftOut ? s_withoutPicture : s_withPicture, seconds);

	s_asked = false;
}

//-----------------------------------------------------------------------------
void takeCounts(unsigned int &drawn, unsigned int &leftOut)
{
	drawn = s_drawnCount;
	leftOut = s_leftOutCount;
	s_drawnCount = 0;
	s_leftOutCount = 0;
}

} // namespace VitaPacing
