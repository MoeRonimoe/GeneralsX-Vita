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

// VitaTextInput.h ////////////////////////////////////////////////////////////
// Typing into the game's text fields with the console's on-screen keyboard.
//
// The console has no keyboard, so a text field (the name of a saved game, a
// chat line, a player's name) could only keep the text it was given. A tap on
// a text field, or Cross with the pointer on it, opens the system's keyboard
// with the field's text in it. What is confirmed there replaces the text, one
// character at a time through the field's own input (GWM_IME_CHAR, as typed
// text arrives on the other platforms), so the field's rules on length and
// characters apply; then comes the Enter a keyboard would give, which sends a
// chat line or saves a replay, as on a computer. Cancelling leaves the field as
// it was. While the keyboard is open the game gets no input.
///////////////////////////////////////////////////////////////////////////////

#pragma once

namespace VitaTextInput
{
	/// Opens the keyboard for a text field that was just clicked, and hands its text to
	/// the field when it closes. Call once per frame, before VitaInput::poll().
	void update();
}
