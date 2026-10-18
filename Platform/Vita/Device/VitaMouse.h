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

// VitaMouse.h ////////////////////////////////////////////////////////////////
// Mouse device for the PS Vita. The pointer is driven by the touch screen and
// the left stick (see VitaInput). The console has no system cursor, so the
// pointer is drawn by the engine through W3DMouse's polygon cursor mode.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "W3DDevice/GameClient/W3DMouse.h"

// VitaMouse ------------------------------------------------------------------
class VitaMouse : public W3DMouse
{
public:
	enum Button
	{
		BUTTON_LEFT,
		BUTTON_RIGHT,
		BUTTON_MIDDLE
	};

	VitaMouse();
	virtual ~VitaMouse() override;

	virtual void init() override;		///< init mouse, extend this functionality, do not replace
	virtual void reset() override;		///< reset the system
	virtual void initCursorResources() override;	///< nothing to load ahead of the display on this platform
	virtual void draw() override;		///< draw the pointer

	// Events are positioned in game UI coordinates and timed in milliseconds.
	void addMoveEvent(Int x, Int y, UnsignedInt timeMs);
	void addButtonEvent(Button button, Bool down, Int x, Int y, UnsignedInt timeMs);
	void addWheelEvent(Int wheelDelta, Int x, Int y, UnsignedInt timeMs);

protected:
	virtual void capture() override;		///< the pointer cannot leave the screen
	virtual void releaseCapture() override;

	/// get the next event available in the buffer
	virtual UnsignedByte getMouseEvent(MouseIO *result, Bool flush) override;

private:
	MouseIO *nextFreeEvent(Int x, Int y, UnsignedInt timeMs);
	void drawFallbackPointer();

	MouseIO m_eventBuffer[Mouse::NUM_MOUSE_EVENTS];
	UnsignedInt m_nextFreeIndex;	///< insert new events at this index
	UnsignedInt m_nextGetIndex;		///< events are retrieved from this index
	UnsignedInt m_eventCount;		///< events waiting in the buffer

	// Double click detection per button
	UnsignedInt m_lastDownTimeMs[3];
	ICoord2D m_lastDownPos[3];
	ICoord2D m_lastPos;				///< position of the previous event, for the movement delta
};

extern VitaMouse *TheVitaMouse;	///< set while the mouse device exists, for VitaInput
