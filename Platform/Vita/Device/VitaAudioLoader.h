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

// VitaAudioLoader.h ////////////////////////////////////////////////////////////
// Everything about sound on the PS Vita that has to wait for the memory card or
// for miniaudio's decoding thread. None of it may happen on the thread the game
// runs on: one read from the card takes a millisecond or more, a WAV header is a
// dozen reads, and whatever asks miniaudio's resource manager for something waits
// until that thread has finished what it is decoding. The first version of the
// sound did both on the game's thread and stopped the game for two minutes of a
// twelve minute mission.
//
// What is here:
//
//  - Sound effects, decoded whole into memory by a thread of this class, each
//    file read from the card in one piece, and kept for the next time. The game
//    asks for an effect and gets it at once if it is there; otherwise it looks
//    again on a later frame.
//  - Music and speech, which miniaudio decodes while they play. Opening one and
//    letting go of one both wait for miniaudio's decoding thread, so both are
//    done by the thread of this class.
//  - The file callbacks miniaudio reads through: archived files by position,
//    with a buffer, so that many small reads become few large ones.
//
// The class does not know the game engine, which lets tools/audiotest run it on
// a PC. Files are named the way VitaAudioManager::locateFile() names them:
// ">archive|offset|size|name" for an archived file, a path for any other.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <miniaudio.h>

#include <pthread.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

/// A sound effect decoded into memory. Made by the game's thread, filled by the loader's.
struct VitaAudioEffect
{
	enum State { LOADING, READY, FAILED };

	std::string location;
	int state;				///< read with VitaAudioLoader::getState(); the fields below hold once it is READY
	ma_int16 *samples;
	ma_uint64 frames;
	ma_uint32 channels;
	ma_uint32 sampleRate;
	size_t bytes;			///< size of the samples
	unsigned int lastUsed;	///< a count kept by the game's thread: the least recently used are let go of first
};

/// Music or speech that the loader's thread is opening.
struct VitaAudioStream
{
	enum State { LOADING, READY, FAILED };

	std::string location;
	ma_uint32 flags;
	ma_sound_group *group;
	ma_sound *sound;		///< memory from whoever asked; holds a sound once the state is READY
	int state;
	bool abandoned;			///< nobody waits for it any more (guarded by the loader's lock)
};

class VitaAudioLoader
{
public:
	VitaAudioLoader();
	~VitaAudioLoader();

	/// Starts the thread. The engine is the one sounds are made for.
	bool start(ma_engine *engine);

	/// Ends the thread after it has let go of every sound it was given to let go of.
	/// Nothing is loaded after this; what was still waiting counts as failed.
	void stop();

	/// The file callbacks, for miniaudio's resource manager and decoders.
	ma_vfs *getFileSystem() { return &m_fileSystem; }

	// SOUND EFFECTS. All of these are for the game's thread.

	/// The effect of a file, which is being loaded if it was not asked for before.
	VitaAudioEffect *getEffect(const std::string &location);
	static int getState(const VitaAudioEffect *effect);

	/// Lets go of the effects that were used longest ago until the decoded samples of all
	/// fit the budget. The ones listed are playing and stay.
	void trimEffects(size_t budgetBytes, const std::vector<const VitaAudioEffect *> &inUse);
	size_t getEffectBytes() const;
	size_t getEffectCount() const { return m_effects.size(); }

	/// Lets go of all effects. No sound may be playing one, and the thread has to be stopped.
	void freeEffects();

	// MUSIC AND SPEECH. For the game's thread as well.

	/// Has the thread make a sound that plays a file while it is decoded, in the memory
	/// given. The state says when it is there.
	VitaAudioStream *openStream(const std::string &location, ma_uint32 flags, ma_sound_group *group, ma_sound *sound);
	static int getState(const VitaAudioStream *stream);

	/// Ends a request that is no longer LOADING. Returns the sound, or null if it failed, in
	/// which case the memory given for the sound has been freed.
	ma_sound *finishStream(VitaAudioStream *stream);

	/// Ends a request whose sound is not wanted any more, in whatever state it is. The
	/// memory given for the sound is freed, when the thread is done with it.
	void abandonStream(VitaAudioStream *stream);

	/// Has the thread uninitialize and free a sound. Letting go of music or speech waits
	/// for miniaudio's decoding thread.
	void discard(ma_sound *sound);

	/// Closes the archives the callbacks have opened. Nothing may be reading any more.
	static void closeArchives();

private:
	struct Job
	{
		VitaAudioEffect *effect;
		VitaAudioStream *stream;
	};

	static void *threadEntry(void *argument);
	void run();
	void decodeEffect(VitaAudioEffect *effect);
	void openStreamNow(VitaAudioStream *stream, bool load);

	ma_vfs_callbacks m_fileSystem;	///< miniaudio takes a pointer to this for a file system
	ma_engine *m_engine;

	pthread_t m_thread;
	bool m_running;
	pthread_mutex_t m_lock;			///< guards what follows, and VitaAudioStream::abandoned
	pthread_cond_t m_wake;
	bool m_stopping;
	std::deque<Job> m_jobs;				///< what the game waits to hear
	std::deque<ma_sound *> m_discards;	///< done when there is nothing to load

	// Only the game's thread touches the map. The thread of this class works on entries
	// that are LOADING, which are never removed.
	std::map<std::string, VitaAudioEffect *> m_effects;
	unsigned int m_useCount;
};
