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

// VitaAudioManager.h ///////////////////////////////////////////////////////////
// Sound on the PS Vita: the engine's miniaudio manager with what the console needs
// done differently.
//
//  - There is no audio device miniaudio knows. The engine mixes without one, and a
//    thread hands what it mixed to the console's audio output.
//  - There is no FFmpeg. Files are decoded by miniaudio (WAV and MP3 is all the game
//    ships).
//  - Nothing that reads from the memory card or waits for a decoder happens on the
//    game's thread. VitaAudioLoader does it on a thread of its own; a sound whose
//    file is not in memory yet is a stand-in that plays nothing until it is.
//  - Sound effects stay decoded in memory. Music and speech are decoded as they play.
//  - What a sound is followed by is decided here, as the original audio code did it:
//    the rounds of a loop, the attack before and the decay after it, the music starting
//    over. The class this one derives from lets go of every sound at its end.
//  - Pausing holds the sounds that are playing, as the original did. The class this one
//    derives from silences everything, also what starts during the pause.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "MiniAudioDevice/MiniAudioManager.h"
#include "VitaAudioLoader.h"

#include <psp2/types.h>

#include <map>

struct VitaSound;

class VitaAudioManager : public MiniAudioManager
{
public:
	VitaAudioManager();
	virtual ~VitaAudioManager() override;

	virtual void openDevice() override;
	virtual void closeDevice() override;

	virtual Real getFileLengthMS(AsciiString strToLoad) const override;

	virtual Bool isMusicPlaying() const override;
	virtual Bool hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const override;

	virtual void stopAudio(AudioAffect which) override;
	virtual void pauseAudio(AudioAffect which) override;
	virtual void resumeAudio(AudioAffect which) override;

	virtual void processPlayingList() override;

protected:
	virtual void setDeviceListenerPosition() override;
	virtual ma_sound *createSound(const AsciiString &fileToPlay, ma_uint32 flags, ma_sound_group *groupToUse, PlayingAudio *audio) override;
	virtual void releaseMiniAudioHandles(PlayingAudio *release) override;

private:
	Bool updatePlaying(PlayingAudio *playing);
	Bool updateStream(PlayingAudio *playing);
	Bool isTooQuiet(AudioEventRTS *event, const Coord3D &position) const;
	Bool isAffected(const PlayingAudio *playing, AudioAffect which) const;

	// What follows a sound effect that has played to its end.
	AsciiString decideWhatFollows(PlayingAudio *playing, Bool &requestAgain);
	void prepareFollower(PlayingAudio *playing);
	Bool startFollower(PlayingAudio *playing);
	ma_sound *loadFollower(PlayingAudio *playing, const AsciiString &file);
	Bool getEndTime(ma_sound *sound, ma_uint64 &endTime);

	// Making sounds and letting go of them.
	ma_sound *loadSound(const AsciiString &file, ma_uint32 flags, ma_sound_group *group);
	VitaSound *makeStandIn(ma_uint32 flags, ma_sound_group *group);
	VitaSound *makeEffectSound(VitaAudioEffect *effect, ma_uint32 flags, ma_sound_group *group);
	Bool resolveStandIn(ma_sound *&sound);
	void discardSound(ma_sound *sound);
	void trimEffects();
	void reportLoading();

	Bool locateFile(const AsciiString &path, AsciiString &location) const;

	static int outputThread(SceSize argumentSize, void *argument);

	mutable VitaAudioLoader m_loader;

	Bool m_open;				///< openDevice() succeeded; everything below is valid
	SceUID m_outputThread;
	volatile Bool m_stopOutput;	///< tells the output thread to end

	// Counted up by the output thread before and after it mixes a block: odd while it does.
	UnsignedInt m_mixSequence;

	size_t m_effectBudget;		///< how much memory the decoded effects may take
	Bool m_effectsGrew;			///< effects have been added since the budget was last looked at

	// The music track that was started last and how often it has run through; scripts wait for that.
	AsciiString m_musicTrack;
	Int m_musicRounds;

	// For the log of a frame profile: sounds that had to wait for their file.
	UnsignedInt m_lateSounds;
	UnsignedInt m_droppedSounds;
	uint64_t m_lateTime;
	uint64_t m_latest;
	uint64_t m_lastReport;

	// Where the files that were asked for are, and how long they play. Both are asked
	// for again and again and cost a look into the archives, or a read from the card.
	mutable std::map<AsciiString, AsciiString> m_locations;
	mutable std::map<AsciiString, Real> m_lengths;
};
