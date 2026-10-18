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

// VitaSilentAudioManager.h ///////////////////////////////////////////////////
// Audio manager without an output device. The game logic keeps requesting and
// tracking audio events through the AudioManager base class; nothing is played.
// Used when the build has no audio backend and for headless runs.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/GameAudio.h"

// VitaSilentAudioManager -----------------------------------------------------
class VitaSilentAudioManager : public AudioManager
{
public:
#if defined(RTS_DEBUG)
	virtual void audioDebugDisplay(DebugDisplayInterface *dd, void *userData, FILE *fp = nullptr) override {}
#endif
	virtual void stopAudio(AudioAffect which) override {}
	virtual void pauseAudio(AudioAffect which) override {}
	virtual void resumeAudio(AudioAffect which) override {}
	virtual void pauseAmbient(Bool shouldPause) override {}
	virtual void killAudioEventImmediately(AudioHandle audioEvent) override {}

	virtual AsciiString nextMusicTrack() override { return AsciiString::TheEmptyString; }
	virtual AsciiString prevMusicTrack() override { return AsciiString::TheEmptyString; }
	virtual Bool isMusicPlaying() const override { return FALSE; }
	// Scripts wait for tracks to finish; a track that never plays has finished.
	virtual Bool hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const override { return TRUE; }

	virtual void openDevice() override {}
	virtual void closeDevice() override {}
	virtual void *getDevice() override { return nullptr; }

	virtual void notifyOfAudioCompletion(UnsignedInt audioCompleted, UnsignedInt flags) override {}

	virtual UnsignedInt getProviderCount() const override { return 0; }
	virtual AsciiString getProviderName(UnsignedInt providerNum) const override { return AsciiString::TheEmptyString; }
	virtual UnsignedInt getProviderIndex(AsciiString providerName) const override { return 0; }
	virtual void selectProvider(UnsignedInt providerNdx) override {}
	virtual void unselectProvider() override {}
	virtual UnsignedInt getSelectedProvider() const override { return 0; }
	virtual void setSpeakerType(UnsignedInt speakerType) override {}
	virtual UnsignedInt getSpeakerType() override { return 0; }

	virtual UnsignedInt getNum2DSamples() const override { return 0; }
	virtual UnsignedInt getNum3DSamples() const override { return 0; }
	virtual UnsignedInt getNumStreams() const override { return 0; }
	virtual UnsignedInt getNumAvailable2DSamples() const override { return 0; }
	virtual UnsignedInt getNumAvailable3DSamples() const override { return 0; }

	virtual Bool doesViolateLimit(AudioEventRTS *event) const override { return FALSE; }
	virtual Bool isPlayingLowerPriority(AudioEventRTS *event) const override { return FALSE; }
	virtual Bool isPlayingAlready(AudioEventRTS *event) const override { return FALSE; }
	virtual Bool isObjectPlayingVoice(UnsignedInt objID) const override { return FALSE; }

	virtual void adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume) override {}
	virtual void removePlayingAudio(AsciiString eventName) override {}
	virtual void removeAllDisabledAudio() override {}

	virtual void *getHandleForBink() override { return nullptr; }
	virtual void releaseHandleForBink() override {}

	virtual void friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay) override {}

	virtual void setPreferredProvider(AsciiString providerNdx) override {}
	virtual void setPreferredSpeaker(AsciiString speakerType) override {}

	virtual Real getFileLengthMS(AsciiString strToLoad) const override { return 0.0f; }
	virtual void closeAnySamplesUsingFile(const void *fileToClose) override {}

	virtual void setDeviceListenerPosition() override {}
};
