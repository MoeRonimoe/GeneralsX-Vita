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

// VitaKeyboard.cpp ///////////////////////////////////////////////////////////
// Keyboard device for the PS Vita, see VitaKeyboard.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaKeyboard.h"

#include <cstring>

VitaKeyboard *TheVitaKeyboard = nullptr;

//-----------------------------------------------------------------------------
VitaKeyboard::VitaKeyboard() :
	m_nextFreeIndex(0),
	m_nextGetIndex(0),
	m_eventCount(0)
{
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	TheVitaKeyboard = this;
}

//-----------------------------------------------------------------------------
VitaKeyboard::~VitaKeyboard()
{
	if (TheVitaKeyboard == this)
		TheVitaKeyboard = nullptr;
}

//-----------------------------------------------------------------------------
void VitaKeyboard::init()
{
	Keyboard::init();

	m_nextFreeIndex = 0;
	m_nextGetIndex = 0;
	m_eventCount = 0;
}

//-----------------------------------------------------------------------------
void VitaKeyboard::reset()
{
	Keyboard::reset();
}

//-----------------------------------------------------------------------------
Bool VitaKeyboard::getCapsState()
{
	return FALSE;
}

//-----------------------------------------------------------------------------
void VitaKeyboard::addKeyEvent(KeyDefType key, Bool down)
{
	// Drop the oldest event when the game stops reading (loading screens), so the most
	// recent state of every key, in particular its release, is what survives.
	if (m_eventCount == MAX_KEY_EVENTS)
	{
		m_nextGetIndex = (m_nextGetIndex + 1) % MAX_KEY_EVENTS;
		--m_eventCount;
	}

	m_eventBuffer[m_nextFreeIndex].key = key;
	m_eventBuffer[m_nextFreeIndex].down = down;
	m_nextFreeIndex = (m_nextFreeIndex + 1) % MAX_KEY_EVENTS;
	++m_eventCount;
}

//-----------------------------------------------------------------------------
void VitaKeyboard::getKey(KeyboardIO *key)
{
	key->status = KeyboardIO::STATUS_UNUSED;

	if (m_eventCount == 0)
	{
		key->key = KEY_NONE;
		key->state = 0;
		key->keyDownTimeMsec = 0;
		return;
	}

	const KeyEvent &event = m_eventBuffer[m_nextGetIndex];
	m_nextGetIndex = (m_nextGetIndex + 1) % MAX_KEY_EVENTS;
	--m_eventCount;

	key->key = event.key;
	key->state = event.down ? KEY_STATE_DOWN : KEY_STATE_UP;
	key->keyDownTimeMsec = event.down ? timeGetTime() : 0;
}
