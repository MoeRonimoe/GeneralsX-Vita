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

// VitaInput.cpp //////////////////////////////////////////////////////////////
// See VitaInput.h for the control scheme.
///////////////////////////////////////////////////////////////////////////////

#include "VitaInput.h"

#include "VitaKeyboard.h"
#include "VitaMouse.h"

#include "Common/GameUtility.h"
#include "GameClient/Display.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/SelectionInfo.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include <VitaPacing.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{

// The front panel reports touches at twice the display resolution.
const Real TOUCH_TO_SCREEN = 0.5f;

// Analog sticks report 0..255 with the centre at 128.
const Real STICK_CENTER = 128.0f;
const Real STICK_RANGE = 127.0f;
const Real STICK_DEAD_ZONE = 0.16f;

// Pointer speed of the left stick at full deflection, in panel pixels per second.
const Real POINTER_SPEED = 900.0f;

// Deflection of the right stick (after the response curve) that counts as a held scroll key.
const Real SCROLL_THRESHOLD = 0.2f;

// Interval between wheel notches while a zoom direction is held.
const UnsignedInt ZOOM_REPEAT_MS = 60;

struct ButtonKey
{
	unsigned int button;
	KeyDefType key;
};

// Buttons that act as plain keys.
const ButtonKey s_buttonKeys[] =
{
	{ SCE_CTRL_LTRIGGER, KEY_LCTRL },
	{ SCE_CTRL_RTRIGGER, KEY_LSHIFT },
	{ SCE_CTRL_TRIANGLE, KEY_Q },
	{ SCE_CTRL_SQUARE, KEY_A },
	{ SCE_CTRL_START, KEY_ESC },
	{ SCE_CTRL_SELECT, KEY_SPACE },
	{ SCE_CTRL_LEFT, KEY_KP4 },
	{ SCE_CTRL_RIGHT, KEY_KP6 },
};

Bool s_initialized = FALSE;

// Samples of the pad and of the touch panel looked at per poll: the last half second.
enum { MAX_SAMPLES = 32 };

unsigned int s_previousButtons = 0;
Bool s_touchActive = FALSE;

// Time stamps of the newest samples that have been turned into events.
Bool s_historyStarted = FALSE;
SceUInt64 s_lastPadStamp = 0;
SceUInt64 s_lastTouchStamp = 0;

// Pointer position in panel pixels. Kept as float so slow stick movement accumulates.
Real s_pointerX = VitaInput::SCREEN_WIDTH * 0.5f;
Real s_pointerY = VitaInput::SCREEN_HEIGHT * 0.5f;

// Scroll keys currently held by the right stick: up, down, left, right.
Bool s_scrollHeld[4] = { FALSE, FALSE, FALSE, FALSE };
const KeyDefType s_scrollKeys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };

UnsignedInt s_lastPollMs = 0;
UnsignedInt s_lastZoomMs = 0;

// A fingertip covers a soldier several times over, and the engine picks what is under one
// exact pixel. A tap that hits nothing is therefore moved onto the nearest unit within this
// many panel pixels (about three millimetres). The environment variable
// VITA_TOUCH_SNAP_RADIUS overrides it; 0 switches the help off.
const Int DEFAULT_SNAP_RADIUS = 24;
Int s_snapRadius = DEFAULT_SNAP_RADIUS;

// A touch that moves further than this from where it began is a drag (a selection box),
// and follows the finger exactly again.
const Real SNAP_RELEASE_DISTANCE = 10.0f;

// Points between the feet and the head of a unit that are tried as the place to tap it.
const Int SNAP_HEIGHT_STEPS = 4;

// What the current touch is shifted by, and where it began.
Bool s_snapActive = FALSE;
Real s_snapOffsetX = 0.0f;
Real s_snapOffsetY = 0.0f;
Real s_snapOriginX = 0.0f;
Real s_snapOriginY = 0.0f;

UnsignedInt nowMs()
{
	return static_cast<UnsignedInt>(sceKernelGetProcessTimeWide() / 1000ULL);
}

/// Fill order with the indices of the samples from the oldest to the newest. The order in
/// which the system hands out its buffered samples is not documented.
template <class Sample>
void sortSamples(const Sample *samples, int count, int *order)
{
	for (int i = 0; i < count; ++i)
	{
		int position = i;
		while (position > 0 && samples[order[position - 1]].timeStamp > samples[i].timeStamp)
		{
			order[position] = order[position - 1];
			--position;
		}
		order[position] = i;
	}
}

/// Time of a sample on the clock the mouse events use. The samples carry the system's
/// time; the newest one is taken to be from now.
UnsignedInt sampleTimeMs(SceUInt64 stamp, SceUInt64 newestStamp, UnsignedInt now)
{
	const SceUInt64 age = (newestStamp - stamp) / 1000ULL;
	return age < now ? now - static_cast<UnsignedInt>(age) : 0;
}

/// Stick axis as -1..1 with the dead zone removed and a quadratic response curve.
Real readAxis(unsigned char raw)
{
	Real value = (static_cast<Real>(raw) - STICK_CENTER) / STICK_RANGE;
	if (value > 1.0f)
		value = 1.0f;
	if (value < -1.0f)
		value = -1.0f;

	const Real magnitude = fabsf(value);
	if (magnitude < STICK_DEAD_ZONE)
		return 0.0f;

	const Real scaled = (magnitude - STICK_DEAD_ZONE) / (1.0f - STICK_DEAD_ZONE);
	return (value < 0.0f ? -1.0f : 1.0f) * scaled * scaled;
}

void clampPointer()
{
	if (s_pointerX < 0.0f)
		s_pointerX = 0.0f;
	if (s_pointerY < 0.0f)
		s_pointerY = 0.0f;
	if (s_pointerX > VitaInput::SCREEN_WIDTH - 1)
		s_pointerX = VitaInput::SCREEN_WIDTH - 1;
	if (s_pointerY > VitaInput::SCREEN_HEIGHT - 1)
		s_pointerY = VitaInput::SCREEN_HEIGHT - 1;
}

/// Pointer position scaled from the panel to the resolution the game renders its UI at.
void gamePointer(Int &x, Int &y)
{
	Real scaleX = 1.0f;
	Real scaleY = 1.0f;
	if (TheDisplay != nullptr && TheDisplay->getWidth() > 0 && TheDisplay->getHeight() > 0)
	{
		scaleX = static_cast<Real>(TheDisplay->getWidth()) / VitaInput::SCREEN_WIDTH;
		scaleY = static_cast<Real>(TheDisplay->getHeight()) / VitaInput::SCREEN_HEIGHT;
	}

	x = static_cast<Int>(s_pointerX * scaleX);
	y = static_cast<Int>(s_pointerY * scaleY);
}

/// Looks for a unit close to a tap that hit none. Returns the point to tap instead, in
/// panel pixels; that point is one the engine's own picking resolves to the unit.
Bool findUnitNearTap(Real tapX, Real tapY, Real &unitX, Real &unitY)
{
	if (s_snapRadius <= 0 || TheTacticalView == nullptr || TheGameClient == nullptr || TheGameLogic == nullptr
		|| TheInGameUI == nullptr || TheDisplay == nullptr || !TheGameLogic->isInGame())
		return FALSE;

	const Real scaleX = static_cast<Real>(TheDisplay->getWidth()) / VitaInput::SCREEN_WIDTH;
	const Real scaleY = static_cast<Real>(TheDisplay->getHeight()) / VitaInput::SCREEN_HEIGHT;
	if (scaleX <= 0.0f || scaleY <= 0.0f)
		return FALSE;

	ICoord2D tap;
	tap.x = static_cast<Int>(tapX * scaleX);
	tap.y = static_cast<Int>(tapY * scaleY);

	// A tap on the interface is meant for the interface.
	if (TheWindowManager != nullptr)
	{
		for (GameWindow *window = TheWindowManager->getWindowUnderCursor(tap.x, tap.y); window != nullptr; window = window->winGetParent())
		{
			if (!BitIsSet(window->winGetStatus(), WIN_STATUS_SEE_THRU))
				return FALSE;
		}
	}

	const PickType pickType = static_cast<PickType>(getPickTypesForContext(TheInGameUI->isInForceAttackMode()));
	if (TheTacticalView->pickDrawable(&tap, FALSE, pickType) != nullptr)
		return FALSE;

	const Int playerIndex = rts::getObservedOrLocalPlayerIndex_Safe();
	const Int radius = static_cast<Int>(s_snapRadius * scaleX);
	Int bestDistance = radius * radius + 1;
	Bool found = FALSE;
	ICoord2D best = tap;

	for (Drawable *draw = TheGameClient->firstDrawable(); draw != nullptr; draw = draw->getNextDrawable())
	{
		const Object *object = draw->getObject();
		if (object == nullptr || object->isEffectivelyDead() || draw->isDrawableEffectivelyHidden())
			continue;

		// Buildings are large enough to be hit, and would catch every tap next to them.
		if (!object->isKindOf(KINDOF_INFANTRY) && !object->isKindOf(KINDOF_VEHICLE))
			continue;

		const ObjectShroudStatus shroud = object->getShroudedStatus(playerIndex);
		if (shroud != OBJECTSHROUD_CLEAR && shroud != OBJECTSHROUD_PARTIAL_CLEAR)
			continue;

		Coord3D point = *draw->getPosition();
		const Real feet = point.z;
		const Real height = object->getGeometryInfo().getMaxHeightAbovePosition();

		for (Int step = 0; step < SNAP_HEIGHT_STEPS; ++step)
		{
			point.z = feet + height * (step + 0.5f) / SNAP_HEIGHT_STEPS;

			ICoord2D screen;
			if (!TheTacticalView->worldToScreen(&point, &screen))
				continue;

			const Int dx = screen.x - tap.x;
			const Int dy = screen.y - tap.y;
			const Int distance = dx * dx + dy * dy;
			if (distance >= bestDistance)
				continue;

			// Only a point that picking really assigns to this unit is of any use: another
			// object may stand in front of it.
			if (TheTacticalView->pickDrawable(&screen, FALSE, pickType) != draw)
				continue;

			bestDistance = distance;
			best = screen;
			found = TRUE;
		}
	}

	if (!found)
		return FALSE;

	unitX = best.x / scaleX;
	unitY = best.y / scaleY;
	return TRUE;
}

} // namespace

//-----------------------------------------------------------------------------
void VitaInput::init()
{
	if (s_initialized)
		return;

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

	const char *snapRadius = getenv("VITA_TOUCH_SNAP_RADIUS");
	if (snapRadius != nullptr)
		s_snapRadius = atoi(snapRadius);

	s_lastPollMs = nowMs();
	s_lastZoomMs = s_lastPollMs;
	s_initialized = TRUE;
}

//-----------------------------------------------------------------------------
void VitaInput::poll()
{
	if (!s_initialized)
		init();

	VitaMouse *mouse = TheVitaMouse;
	VitaKeyboard *keyboard = TheVitaKeyboard;

	const UnsignedInt now = nowMs();
	Real elapsed = (now - s_lastPollMs) / 1000.0f;
	s_lastPollMs = now;
	// A long hitch (level load) must not fling the pointer across the screen.
	if (elapsed > 0.1f)
		elapsed = 0.1f;

	// The console samples its inputs 60 times a second and keeps the last samples. The game
	// may draw far fewer frames than that, and a tap is over within two or three samples, so
	// every sample since the last poll is looked at, not just the current state.
	static SceCtrlData pads[MAX_SAMPLES];
	static SceTouchData touches[MAX_SAMPLES];
	const int padCount = sceCtrlPeekBufferPositive(0, pads, MAX_SAMPLES);
	const int touchCount = sceTouchPeek(SCE_TOUCH_PORT_FRONT, touches, MAX_SAMPLES);
	if (padCount <= 0)
		return;

	int padOrder[MAX_SAMPLES];
	int touchOrder[MAX_SAMPLES];
	sortSamples(pads, padCount, padOrder);
	sortSamples(touches, touchCount, touchOrder);

	const SceCtrlData &pad = pads[padOrder[padCount - 1]];

	if (!s_historyStarted)
	{
		// What was pressed before the game looked at its input the first time is not for it.
		s_historyStarted = TRUE;
		s_lastPadStamp = pad.timeStamp;
		s_previousButtons = pad.buttons;
		if (touchCount > 0)
		{
			const SceTouchData &touch = touches[touchOrder[touchCount - 1]];
			s_lastTouchStamp = touch.timeStamp;
			s_touchActive = touch.reportNum > 0;
		}
		return;
	}

	// --- touch -----------------------------------------------------------------
	Bool touching = s_touchActive;
	for (int step = 0; step < touchCount; ++step)
	{
		const SceTouchData &touch = touches[touchOrder[step]];
		if (touch.timeStamp <= s_lastTouchStamp)
			continue;

		const UnsignedInt time = sampleTimeMs(touch.timeStamp, touches[touchOrder[touchCount - 1]].timeStamp, now);
		s_lastTouchStamp = touch.timeStamp;

		touching = touch.reportNum > 0;
		if (touching)
		{
			Real touchX = touch.report[0].x * TOUCH_TO_SCREEN;
			Real touchY = touch.report[0].y * TOUCH_TO_SCREEN;

			if (!s_touchActive)
			{
				// A new touch: see whether it was aimed at a unit it missed narrowly.
				Real unitX = 0.0f;
				Real unitY = 0.0f;
				s_snapActive = findUnitNearTap(touchX, touchY, unitX, unitY);
				s_snapOriginX = touchX;
				s_snapOriginY = touchY;
				s_snapOffsetX = unitX - touchX;
				s_snapOffsetY = unitY - touchY;
			}
			else if (s_snapActive && (fabsf(touchX - s_snapOriginX) > SNAP_RELEASE_DISTANCE || fabsf(touchY - s_snapOriginY) > SNAP_RELEASE_DISTANCE))
			{
				s_snapActive = FALSE;
			}

			if (s_snapActive)
			{
				touchX += s_snapOffsetX;
				touchY += s_snapOffsetY;
			}

			const Bool moved = touchX != s_pointerX || touchY != s_pointerY;
			s_pointerX = touchX;
			s_pointerY = touchY;
			clampPointer();

			Int x = 0;
			Int y = 0;
			gamePointer(x, y);

			if (mouse != nullptr)
			{
				// The pointer has to arrive before the button it is pressed with.
				if (moved || !s_touchActive)
					mouse->addMoveEvent(x, y, time);
				if (!s_touchActive)
					mouse->addButtonEvent(VitaMouse::BUTTON_LEFT, TRUE, x, y, time);
			}
		}
		else if (s_touchActive && mouse != nullptr)
		{
			Int x = 0;
			Int y = 0;
			gamePointer(x, y);
			mouse->addButtonEvent(VitaMouse::BUTTON_LEFT, FALSE, x, y, time);
		}

		s_touchActive = touching;
	}

	// --- pointer from the stick -------------------------------------------------
	if (!touching)
	{
		const Real moveX = readAxis(pad.lx);
		const Real moveY = readAxis(pad.ly);
		if (moveX != 0.0f || moveY != 0.0f)
		{
			s_pointerX += moveX * POINTER_SPEED * elapsed;
			s_pointerY += moveY * POINTER_SPEED * elapsed;
			clampPointer();

			Int x = 0;
			Int y = 0;
			gamePointer(x, y);
			if (mouse != nullptr)
				mouse->addMoveEvent(x, y, now);
		}
	}

	Int pointerX = 0;
	Int pointerY = 0;
	gamePointer(pointerX, pointerY);

	// --- buttons ---------------------------------------------------------------
	for (int step = 0; step < padCount; ++step)
	{
		const SceCtrlData &sample = pads[padOrder[step]];
		if (sample.timeStamp <= s_lastPadStamp)
			continue;

		const UnsignedInt time = sampleTimeMs(sample.timeStamp, pad.timeStamp, now);
		s_lastPadStamp = sample.timeStamp;

		unsigned int pressed = sample.buttons & ~s_previousButtons;
		unsigned int released = s_previousButtons & ~sample.buttons;
		s_previousButtons = sample.buttons;
		if (pressed == 0 && released == 0)
			continue;

		// Both shoulder buttons held and Select: switch whether the game keeps its speed by
		// leaving pictures out (VitaPacing.h). That press of Select is not the game's.
		static Bool selectWasTaken = FALSE;
		const unsigned int shoulders = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
		if ((pressed & SCE_CTRL_SELECT) != 0 && (sample.buttons & shoulders) == shoulders)
		{
			VitaPacing::setCatchUp(!VitaPacing::isCatchUp());
			if (TheInGameUI != nullptr)
			{
				TheInGameUI->messageNoFormat(UnicodeString(VitaPacing::isCatchUp()
					? L"Game speed: by the clock, pictures are left out"
					: L"Game speed: one step per picture"));
			}

			pressed &= ~SCE_CTRL_SELECT;
			selectWasTaken = TRUE;
		}
		if ((released & SCE_CTRL_SELECT) != 0 && selectWasTaken)
		{
			released &= ~SCE_CTRL_SELECT;
			selectWasTaken = FALSE;
		}


		if (mouse != nullptr)
		{
			if (pressed & SCE_CTRL_CROSS)
				mouse->addButtonEvent(VitaMouse::BUTTON_LEFT, TRUE, pointerX, pointerY, time);
			if (released & SCE_CTRL_CROSS)
				mouse->addButtonEvent(VitaMouse::BUTTON_LEFT, FALSE, pointerX, pointerY, time);

			if (pressed & SCE_CTRL_CIRCLE)
				mouse->addButtonEvent(VitaMouse::BUTTON_RIGHT, TRUE, pointerX, pointerY, time);
			if (released & SCE_CTRL_CIRCLE)
				mouse->addButtonEvent(VitaMouse::BUTTON_RIGHT, FALSE, pointerX, pointerY, time);
		}

		if (keyboard != nullptr)
		{
			for (const ButtonKey &mapping : s_buttonKeys)
			{
				if (pressed & mapping.button)
					keyboard->addKeyEvent(mapping.key, TRUE);
				if (released & mapping.button)
					keyboard->addKeyEvent(mapping.key, FALSE);
			}
		}
	}

	// --- held controls: what counts is their state now ---------------------------
	if (mouse != nullptr)
	{
		// Zoom repeats while the direction is held.
		const Bool zoomIn = (pad.buttons & SCE_CTRL_UP) != 0;
		const Bool zoomOut = (pad.buttons & SCE_CTRL_DOWN) != 0;
		if ((zoomIn || zoomOut) && now - s_lastZoomMs >= ZOOM_REPEAT_MS)
		{
			mouse->addWheelEvent(zoomIn ? MOUSE_WHEEL_DELTA : -MOUSE_WHEEL_DELTA, pointerX, pointerY, now);
			s_lastZoomMs = now;
		}
	}

	if (keyboard != nullptr)
	{
		const Real scrollX = readAxis(pad.rx);
		const Real scrollY = readAxis(pad.ry);
		const Bool wanted[4] =
		{
			scrollY < -SCROLL_THRESHOLD,
			scrollY > SCROLL_THRESHOLD,
			scrollX < -SCROLL_THRESHOLD,
			scrollX > SCROLL_THRESHOLD
		};
		for (Int i = 0; i < 4; ++i)
		{
			if (wanted[i] != s_scrollHeld[i])
			{
				keyboard->addKeyEvent(s_scrollKeys[i], wanted[i]);
				s_scrollHeld[i] = wanted[i];
			}
		}
	}
}
