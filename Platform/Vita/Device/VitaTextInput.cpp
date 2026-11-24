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

// VitaTextInput.cpp //////////////////////////////////////////////////////////
// See VitaTextInput.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaTextInput.h"

#include "VitaInput.h"

#include "GameClient/Gadget.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>

#include <VitaSystemDialog.h>

#include <cstdio>
#include <cstring>

namespace
{

// What a text field takes as the Enter key among typed characters (VK_RETURN).
const WideChar ENTER_CHARACTER = 0x0D;

// The system reads the first and writes the second for as long as its dialog is open.
SceWChar16 s_initialText[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
SceWChar16 s_typedText[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
const SceWChar16 s_noTitle[1] = { 0 };

bool s_systemPrepared = false;
bool s_open = false;
bool s_resultTaken = false;
bool s_confirmed = false;
GameWindow *s_field = nullptr;	///< the text field the keyboard is open for

//-----------------------------------------------------------------------------
void logFailure(const char *what, int code)
{
	fprintf(stderr, "[text] %s failed (0x%08X): no on-screen keyboard\n", what, static_cast<unsigned int>(code));
	fflush(stderr);
}

//-----------------------------------------------------------------------------
// The keyboard is a module of the system that has to be loaded, and the system's dialogs
// speak the language and use the confirm button the console is set to.
bool prepareSystem()
{
	if (s_systemPrepared)
		return true;

	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) < 0)
	{
		const int loaded = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
		if (loaded < 0)
		{
			logFailure("loading the keyboard module", loaded);
			return false;
		}
	}

	SceCommonDialogConfigParam config;
	sceCommonDialogConfigParamInit(&config);

	SceAppUtilInitParam init;
	SceAppUtilBootParam boot;
	memset(&init, 0, sizeof(init));
	memset(&boot, 0, sizeof(boot));
	if (sceAppUtilInit(&init, &boot) >= 0)
	{
		int language = 0;
		int enterButton = 0;
		if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language) >= 0)
			config.language = static_cast<SceSystemParamLang>(language);
		if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enterButton) >= 0)
			config.enterButtonAssign = static_cast<SceSystemParamEnterButtonAssign>(enterButton);
	}

	const int configured = sceCommonDialogSetConfigParam(&config);
	if (configured < 0)
	{
		logFailure("setting up the system's dialogs", configured);
		return false;
	}

	s_systemPrepared = true;
	return true;
}

//-----------------------------------------------------------------------------
bool isTextField(GameWindow *window)
{
	return window != nullptr && BitIsSet(window->winGetStyle(), GWS_ENTRY_FIELD) && window->winGetUserData() != nullptr
		&& window->winGetEnabled() && !window->winIsHidden();
}

//-----------------------------------------------------------------------------
bool contains(GameWindow *window, Int x, Int y)
{
	Int left = 0;
	Int top = 0;
	Int width = 0;
	Int height = 0;
	window->winGetScreenPosition(&left, &top);
	window->winGetSize(&width, &height);
	return x >= left && x < left + width && y >= top && y < top + height;
}

//-----------------------------------------------------------------------------
bool open(GameWindow *field)
{
	if (!prepareSystem())
		return false;

	const EntryData *entry = static_cast<const EntryData *>(field->winGetUserData());

	// The field keeps one place of its length for the end of its text.
	Int maxLength = entry->maxTextLen - 1;
	if (maxLength < 1)
		maxLength = 1;
	if (maxLength > SCE_IME_DIALOG_MAX_TEXT_LENGTH)
		maxLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;

	// The fields hold characters of the basic plane only, which are one UTF-16 unit each.
	const UnicodeString text = GadgetTextEntryGetText(field);
	Int length = 0;
	for (Int i = 0; i < text.getLength() && length < maxLength; ++i)
	{
		const UnsignedInt character = static_cast<UnsignedInt>(text.getCharAt(i));
		if (character == 0 || character > 0xFFFF || (character >= 0xD800 && character <= 0xDFFF))
			continue;
		s_initialText[length++] = static_cast<SceWChar16>(character);
	}
	s_initialText[length] = 0;
	memset(s_typedText, 0, sizeof(s_typedText));

	SceImeDialogParam param;
	sceImeDialogParamInit(&param);
	param.supportedLanguages = 0x0001FFFF;
	param.languagesForced = SCE_FALSE;
	if (entry->numericalOnly)
		param.type = SCE_IME_TYPE_NUMBER;
	else if (entry->aSCIIOnly || entry->alphaNumericalOnly)
		param.type = SCE_IME_TYPE_BASIC_LATIN;
	else
		param.type = SCE_IME_TYPE_DEFAULT;
	param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	param.textBoxMode = entry->secretText ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	param.title = s_noTitle;
	param.maxTextLength = static_cast<SceUInt32>(maxLength);
	param.initialText = s_initialText;
	param.inputTextBuffer = s_typedText;

	const int opened = sceImeDialogInit(&param);
	if (opened < 0)
	{
		logFailure("opening the keyboard", opened);
		return false;
	}

	s_open = true;
	s_resultTaken = false;
	s_confirmed = false;
	s_field = field;
	VitaSystemDialog::setShown(true);
	VitaInput::setBlocked(TRUE);
	return true;
}

//-----------------------------------------------------------------------------
// The field may have gone while the keyboard was open, when its menu closed. A window that
// is destroyed loses the focus, and nothing the player does can move it meanwhile.
GameWindow *fieldStillThere()
{
	if (TheWindowManager == nullptr)
		return nullptr;
	GameWindow *field = TheWindowManager->winGetFocus();
	return field == s_field && isTextField(field) ? field : nullptr;
}

//-----------------------------------------------------------------------------
void handOver(GameWindow *field)
{
	GadgetTextEntrySetText(field, UnicodeString::TheEmptyString);
	for (const SceWChar16 *typed = s_typedText; *typed != 0; ++typed)
	{
		// Like the typed text of the other platforms: no characters beyond the basic plane,
		// whose halves the fields would take for two characters.
		if (*typed >= 0xD800 && *typed <= 0xDFFF)
			continue;
		if (*typed == L'\n' || *typed == L'\r')
			continue;
		TheWindowManager->winSendInputMsg(field, GWM_IME_CHAR, static_cast<WindowMsgData>(*typed), 0);
	}

	// Emptied fields send nothing above; their menus have to hear of the change too.
	TheWindowManager->winSendSystemMsg(field->winGetOwner(), GEM_UPDATE_TEXT, (WindowMsgData)field, 0);
	TheWindowManager->winSendInputMsg(field, GWM_IME_CHAR, static_cast<WindowMsgData>(ENTER_CHARACTER), 0);
}

//-----------------------------------------------------------------------------
void pollOpenDialog()
{
	const SceCommonDialogStatus status = sceImeDialogGetStatus();
	if (status == SCE_COMMON_DIALOG_STATUS_RUNNING)
		return;

	if (status == SCE_COMMON_DIALOG_STATUS_FINISHED && !s_resultTaken)
	{
		SceImeDialogResult result;
		memset(&result, 0, sizeof(result));
		s_confirmed = sceImeDialogGetResult(&result) >= 0 && result.result == SCE_COMMON_DIALOG_RESULT_OK
			&& result.button == SCE_IME_DIALOG_BUTTON_ENTER;
		s_resultTaken = true;
	}

	// The system writes into the buffers until its dialog has ended; if it cannot end yet,
	// it is asked again next frame.
	if (status == SCE_COMMON_DIALOG_STATUS_FINISHED && sceImeDialogTerm() < 0)
		return;

	s_open = false;
	VitaSystemDialog::setShown(false);
	VitaInput::setBlocked(FALSE);

	GameWindow *field = s_confirmed ? fieldStillThere() : nullptr;
	s_field = nullptr;
	if (field != nullptr)
		handOver(field);
}

} // namespace

//-----------------------------------------------------------------------------
void VitaTextInput::update()
{
	if (s_open)
	{
		pollOpenDialog();
		return;
	}

	// The click that gives a text field the focus has been handled by the time the release
	// is looked at here, one frame later.
	int x = 0;
	int y = 0;
	if (!VitaInput::takeLeftRelease(x, y) || TheWindowManager == nullptr)
		return;

	GameWindow *focus = TheWindowManager->winGetFocus();
	if (isTextField(focus) && contains(focus, x, y))
		open(focus);
}
