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

// VitaMouse.cpp //////////////////////////////////////////////////////////////
// Mouse device for the PS Vita, see VitaMouse.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaMouse.h"

#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/Image.h"
#include "GameLogic/GameLogic.h"

#include <cstdlib>
#include <cstring>

VitaMouse *TheVitaMouse = nullptr;

namespace
{

// Two presses of the same button count as a double click when they are this close.
const UnsignedInt DOUBLE_CLICK_MS = 400;
const Int DOUBLE_CLICK_DISTANCE = 12;

} // namespace

//-----------------------------------------------------------------------------
VitaMouse::VitaMouse() :
	m_nextFreeIndex(0),
	m_nextGetIndex(0),
	m_eventCount(0)
{
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	memset(m_lastDownTimeMs, 0, sizeof(m_lastDownTimeMs));
	memset(m_lastDownPos, 0, sizeof(m_lastDownPos));
	m_lastPos.x = 0;
	m_lastPos.y = 0;

	TheVitaMouse = this;
}

//-----------------------------------------------------------------------------
VitaMouse::~VitaMouse()
{
	if (TheVitaMouse == this)
		TheVitaMouse = nullptr;
}

//-----------------------------------------------------------------------------
void VitaMouse::init()
{
	// W3DMouse only extends its Win32 base on Windows; do that part here.
	Mouse::init();

	// Touch reports where the finger is, and the stick pointer is tracked by VitaInput.
	m_inputMovesAbsolute = TRUE;

	W3DMouse::init();

	// There is no system cursor to fall back on.
	setRedrawMode(RM_POLYGON);
}

//-----------------------------------------------------------------------------
void VitaMouse::reset()
{
	Mouse::reset();
	W3DMouse::reset();
}

//-----------------------------------------------------------------------------
void VitaMouse::initCursorResources()
{
}

//-----------------------------------------------------------------------------
void VitaMouse::capture()
{
	onCursorCaptured(TRUE);
}

//-----------------------------------------------------------------------------
void VitaMouse::releaseCapture()
{
	onCursorCaptured(FALSE);
}

//-----------------------------------------------------------------------------
void VitaMouse::draw()
{
	W3DMouse::draw();

	// Mods and stripped data sets do not always map an image to every cursor. A pointer the
	// player cannot see makes the game unusable on a device without a system cursor.
	if (m_visible && m_currentCursor != NONE && TheMappedImageCollection != nullptr)
	{
		const AsciiString &imageName = m_cursorInfo[m_currentCursor].imageName;
		if (imageName.isEmpty() || TheMappedImageCollection->findImageByName(imageName) == nullptr)
			drawFallbackPointer();
	}
}

//-----------------------------------------------------------------------------
void VitaMouse::drawFallbackPointer()
{
	if (TheDisplay == nullptr)
		return;

	const Int x = m_currMouse.pos.x;
	const Int y = m_currMouse.pos.y;
	const Color outline = GameMakeColor(0, 0, 0, 255);
	const Color fill = GameMakeColor(255, 255, 255, 255);

	// A plain arrow: two edges from the tip and the closing edge.
	TheDisplay->drawLine(x, y, x, y + 16, 3.0f, outline);
	TheDisplay->drawLine(x, y, x + 12, y + 12, 3.0f, outline);
	TheDisplay->drawLine(x, y + 16, x + 12, y + 12, 3.0f, outline);

	TheDisplay->drawLine(x, y, x, y + 16, 1.0f, fill);
	TheDisplay->drawLine(x, y, x + 12, y + 12, 1.0f, fill);
	TheDisplay->drawLine(x, y + 16, x + 12, y + 12, 1.0f, fill);
}

//-----------------------------------------------------------------------------
MouseIO *VitaMouse::nextFreeEvent(Int x, Int y, UnsignedInt timeMs)
{
	// When the game stops reading (loading screens) drop the oldest event, not the newest:
	// a lost button release would leave the button stuck down.
	if (m_eventCount == Mouse::NUM_MOUSE_EVENTS)
	{
		m_nextGetIndex = (m_nextGetIndex + 1) % Mouse::NUM_MOUSE_EVENTS;
		--m_eventCount;
	}

	MouseIO *event = &m_eventBuffer[m_nextFreeIndex];
	m_nextFreeIndex = (m_nextFreeIndex + 1) % Mouse::NUM_MOUSE_EVENTS;
	++m_eventCount;

	memset(event, 0, sizeof(*event));
	event->pos.x = x;
	event->pos.y = y;
	event->deltaPos.x = x - m_lastPos.x;
	event->deltaPos.y = y - m_lastPos.y;
	event->time = timeMs;
	event->leftState = MBS_None;
	event->rightState = MBS_None;
	event->middleState = MBS_None;

	m_lastPos.x = x;
	m_lastPos.y = y;
	return event;
}

//-----------------------------------------------------------------------------
void VitaMouse::addMoveEvent(Int x, Int y, UnsignedInt timeMs)
{
	nextFreeEvent(x, y, timeMs);
}

//-----------------------------------------------------------------------------
void VitaMouse::addButtonEvent(Button button, Bool down, Int x, Int y, UnsignedInt timeMs)
{
	MouseIO *event = nextFreeEvent(x, y, timeMs);

	MouseButtonState state = down ? MBS_Down : MBS_Up;
	if (down)
	{
		const Bool quick = timeMs - m_lastDownTimeMs[button] <= DOUBLE_CLICK_MS;
		const Bool close = abs(x - m_lastDownPos[button].x) <= DOUBLE_CLICK_DISTANCE
			&& abs(y - m_lastDownPos[button].y) <= DOUBLE_CLICK_DISTANCE;
		if (m_lastDownTimeMs[button] != 0 && quick && close)
		{
			state = MBS_DoubleClick;
			// A third press starts over instead of chaining double clicks.
			m_lastDownTimeMs[button] = 0;
		}
		else
		{
			m_lastDownTimeMs[button] = timeMs;
			m_lastDownPos[button].x = x;
			m_lastDownPos[button].y = y;
		}
	}

	const UnsignedInt frame = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 1;
	switch (button)
	{
		case BUTTON_LEFT:
			event->leftState = state;
			event->leftFrame = frame;
			break;

		case BUTTON_RIGHT:
			event->rightState = state;
			event->rightFrame = frame;
			break;

		case BUTTON_MIDDLE:
			event->middleState = state;
			event->middleFrame = frame;
			break;
	}
}

//-----------------------------------------------------------------------------
void VitaMouse::addWheelEvent(Int wheelDelta, Int x, Int y, UnsignedInt timeMs)
{
	MouseIO *event = nextFreeEvent(x, y, timeMs);
	event->wheelPos = wheelDelta;
}

//-----------------------------------------------------------------------------
UnsignedByte VitaMouse::getMouseEvent(MouseIO *result, Bool flush)
{
	if (m_eventCount == 0)
		return MOUSE_NONE;

	*result = m_eventBuffer[m_nextGetIndex];
	m_nextGetIndex = (m_nextGetIndex + 1) % Mouse::NUM_MOUSE_EVENTS;
	--m_eventCount;

	return MOUSE_OK;
}
