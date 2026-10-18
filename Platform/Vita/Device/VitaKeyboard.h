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

// VitaKeyboard.h /////////////////////////////////////////////////////////////
// Keyboard device for the PS Vita. The console has no keyboard; VitaInput maps
// buttons and the right stick onto the keys the game binds its commands to.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "GameClient/Keyboard.h"
#include "GameClient/KeyDefs.h"

// VitaKeyboard ---------------------------------------------------------------
class VitaKeyboard : public Keyboard
{
public:
	VitaKeyboard();
	virtual ~VitaKeyboard() override;

	virtual void init() override;		///< initialize the keyboard, extending init functionality
	virtual void reset() override;		///< reset keyboard system
	virtual Bool getCapsState() override;	///< there is no caps lock

	/// Queue a key transition for the next keyboard update.
	void addKeyEvent(KeyDefType key, Bool down);

protected:
	/// get the next key event, KEY_NONE when the queue is empty
	virtual void getKey(KeyboardIO *key) override;

private:
	struct KeyEvent
	{
		KeyDefType key;
		Bool down;
	};

	enum { MAX_KEY_EVENTS = 64 };

	KeyEvent m_eventBuffer[MAX_KEY_EVENTS];
	UnsignedInt m_nextFreeIndex;	///< insert new events at this index
	UnsignedInt m_nextGetIndex;		///< events are retrieved from this index
	UnsignedInt m_eventCount;		///< events waiting in the buffer
};

extern VitaKeyboard *TheVitaKeyboard;	///< set while the keyboard device exists, for VitaInput
