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

// VitaAudioManager.cpp /////////////////////////////////////////////////////////
// See VitaAudioManager.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaAudioManager.h"

#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/GameCommon.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "GameClient/VideoPlayer.h"

#include "VitaDiagnostics.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/// What the manager allocates for every sound. The class it derives from knows the block
/// by the ma_sound at its beginning, which is all it uses.
struct VitaSound
{
	ma_sound sound;
	ma_audio_buffer_ref samples;	///< effect: where it is in the decoded samples
	VitaAudioEffect *effect;		///< effect: the samples it plays, or waits for
	VitaAudioStream *stream;		///< music or speech: the request to open it, while that is waited for
	ma_sound_group *group;
	ma_uint32 flags;				///< as the engine asked for the sound
	uint64_t asked;					///< when
	Bool standIn;					///< plays nothing: what it stands for is not in memory yet
	Bool streamed;					///< music or speech
	Bool paused;					///< held by pauseAudio()
};

namespace
{

// The main output port of the console only takes 48 kHz.
const int OUTPUT_SAMPLE_RATE = 48000;
const int OUTPUT_CHANNELS = 2;

// Frames handed to the output per call: 21 ms. The call returns when the console has
// taken them, which is what paces the output thread.
const int OUTPUT_FRAMES = 1024;

// Above the main thread (160), so that a long frame cannot starve the output.
const int OUTPUT_THREAD_PRIORITY = 96;
const SceSize OUTPUT_THREAD_STACK = 256 * 1024;

// The engine mixes a block in pieces of this many frames. A sound that is given a start
// time starts with the first piece that begins at or after it, so this is how exactly one
// sound can be made to follow another (see prepareFollower()).
const ma_uint32 MIX_CHUNK_FRAMES = 256;
static_assert(OUTPUT_FRAMES % MIX_CHUNK_FRAMES == 0, "a block handed to the output has to be a whole number of pieces");

// How long before its end a sound effect gets what follows it made ready. The game looks
// at its sounds once per frame, which takes 60 to 100 ms and more in a battle; the
// shortest piece of a loop in the game's data lasts 370 ms.
const ma_uint64 FOLLOWER_LEAD_FRAMES = OUTPUT_SAMPLE_RATE * 3 / 10;

// miniaudio's thread that decodes music and speech while they play; the MP3 decoder keeps
// its working buffers on the stack. One and no more: with two, miniaudio 0.11.23 has been
// seen to write to memory it had freed (see Platform/Vita/README.md).
const ma_uint32 JOB_THREADS = 1;
const size_t JOB_THREAD_STACK = 512 * 1024;

// Blocks between two lines about the cost of mixing in the log of a frame profile, and
// the time between two about sounds that had to wait for their file.
const UnsignedInt MIXING_REPORT_BLOCKS = 480;

// With "+VITA_OUTPUT_SOUND=1" in vita_args.txt the first 30 seconds of what goes to the speakers are
// written to a WAV file, to be compared with what the same sounds give on a PC.
const char *const OUTPUT_SOUND_PATH = "ux0:data/Generals/UserData/output_sound.wav";
const UnsignedInt OUTPUT_SOUND_SECONDS = 30;

void writeOutputSound(const std::vector<ma_int16> &recording)
{
	FILE *file = fopen(OUTPUT_SOUND_PATH, "wb");
	if (file == nullptr)
		return;

	const uint32_t dataBytes = static_cast<uint32_t>(recording.size() * sizeof(ma_int16));
	const uint32_t riffBytes = 36 + dataBytes;
	const uint32_t formatBytes = 16;
	const uint16_t pcm = 1;
	const uint16_t channels = OUTPUT_CHANNELS;
	const uint32_t rate = OUTPUT_SAMPLE_RATE;
	const uint32_t byteRate = rate * channels * 2;
	const uint16_t blockAlign = channels * 2;
	const uint16_t bits = 16;
	fwrite("RIFF", 1, 4, file);
	fwrite(&riffBytes, 4, 1, file);
	fwrite("WAVEfmt ", 1, 8, file);
	fwrite(&formatBytes, 4, 1, file);
	fwrite(&pcm, 2, 1, file);
	fwrite(&channels, 2, 1, file);
	fwrite(&rate, 4, 1, file);
	fwrite(&byteRate, 4, 1, file);
	fwrite(&blockAlign, 2, 1, file);
	fwrite(&bits, 2, 1, file);
	fwrite("data", 1, 4, file);
	fwrite(&dataBytes, 4, 1, file);
	fwrite(recording.data(), sizeof(ma_int16), recording.size(), file);
	fclose(file);
	fprintf(stderr, "[audio] the first %u seconds of the output written to %s\n", static_cast<unsigned int>(OUTPUT_SOUND_SECONDS), OUTPUT_SOUND_PATH);
	fflush(stderr);
}
const uint64_t LOADING_REPORT_US = 10 * 1000 * 1000;

// The decoded samples of the sound effects that are kept in memory. A mission of the
// game plays some 350 different ones, which take 30 MB.
const size_t DEFAULT_EFFECT_BUDGET_MB = 40;

// A sound effect whose file was not in memory when it was asked for starts when the file
// is. One that would start later than this is not played at all: a shot that is heard a
// second after it was fired is worse than one that is not heard. Loops start whenever.
const uint64_t LATE_LIMIT_US = 700 * 1000;

//-----------------------------------------------------------------------------
VitaSound *toVitaSound(ma_sound *sound)
{
	return reinterpret_cast<VitaSound *>(sound);
}

const VitaSound *toVitaSound(const ma_sound *sound)
{
	return reinterpret_cast<const VitaSound *>(sound);
}

//-----------------------------------------------------------------------------
VitaSound *allocateSound(ma_uint32 flags, ma_sound_group *group)
{
	VitaSound *made = static_cast<VitaSound *>(malloc(sizeof(VitaSound)));
	if (made != nullptr)
	{
		made->effect = nullptr;
		made->stream = nullptr;
		made->group = group;
		made->flags = flags;
		made->asked = VitaDiagnostics::now();
		made->standIn = FALSE;
		made->streamed = FALSE;
		made->paused = FALSE;
	}
	return made;
}

//-----------------------------------------------------------------------------
/// A sound that has played to its end. miniaudio marks it when the last sample is read
/// and stops it with the next piece it mixes; only then can it be started again.
Bool hasEnded(const ma_sound *sound)
{
	return !ma_sound_is_playing(sound) && ma_sound_at_end(sound);
}

//-----------------------------------------------------------------------------
/// Music or speech whose file turned out not to be playable. It never reaches its end.
Bool hasFailed(const ma_sound *sound)
{
	const VitaSound *vitaSound = toVitaSound(sound);
	if (vitaSound->standIn || !vitaSound->streamed || sound->pResourceManagerDataSource == nullptr)
		return FALSE;

	const ma_result result = ma_resource_manager_data_source_result(sound->pResourceManagerDataSource);
	return result != MA_SUCCESS && result != MA_BUSY;
}

//-----------------------------------------------------------------------------
/// The file of the part of an event that is played next.
AsciiString getPortionFile(AudioEventRTS *event)
{
	switch (event->getNextPlayPortion())
	{
		case PP_Attack:
			return event->getAttackFilename();
		case PP_Sound:
			return event->getFilename();
		case PP_Decay:
			return event->getDecayFilename();
		default:
			return AsciiString::TheEmptyString;
	}
}

} // namespace

//-----------------------------------------------------------------------------
VitaAudioManager::VitaAudioManager() :
	m_open(FALSE),
	m_outputThread(-1),
	m_stopOutput(FALSE),
	m_mixSequence(0),
	m_effectBudget(DEFAULT_EFFECT_BUDGET_MB * 1024 * 1024),
	m_effectsGrew(FALSE),
	m_musicRounds(0),
	m_lateSounds(0),
	m_droppedSounds(0),
	m_lateTime(0),
	m_latest(0),
	m_lastReport(0)
{
	// "+VITA_AUDIO_CACHE_MB=n" in vita_args.txt.
	const char *budget = getenv("VITA_AUDIO_CACHE_MB");
	if (budget != nullptr && atoi(budget) > 0)
		m_effectBudget = static_cast<size_t>(atoi(budget)) * 1024 * 1024;
}

//-----------------------------------------------------------------------------
VitaAudioManager::~VitaAudioManager()
{
	// The base class closes what it opened itself, which is nothing here.
	closeDevice();
}

//-----------------------------------------------------------------------------
void VitaAudioManager::openDevice()
{
	if (m_open || !TheGlobalData->m_audioOn)
		return;

	// Only music and speech go through the resource manager.
	ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
	resourceConfig.pVFS = m_loader.getFileSystem();
	resourceConfig.decodedFormat = ma_format_s16;
	resourceConfig.decodedChannels = 0;
	resourceConfig.decodedSampleRate = 0;
	resourceConfig.jobThreadCount = JOB_THREADS;
	resourceConfig.jobThreadStackSize = JOB_THREAD_STACK;

	ma_result result = ma_resource_manager_init(&resourceConfig, &m_resourceManager);
	if (result != MA_SUCCESS)
	{
		fprintf(stderr, "[audio] no resource manager: %s\n", ma_result_description(result));
		setOn(false, AudioAffect_All);
		return;
	}

	ma_engine_config engineConfig = ma_engine_config_init();
	engineConfig.noDevice = MA_TRUE;
	engineConfig.channels = OUTPUT_CHANNELS;
	engineConfig.sampleRate = OUTPUT_SAMPLE_RATE;
	engineConfig.periodSizeInFrames = MIX_CHUNK_FRAMES;
	engineConfig.pResourceManager = &m_resourceManager;

	result = ma_engine_init(&engineConfig, &m_engine);
	if (result != MA_SUCCESS)
	{
		fprintf(stderr, "[audio] no engine: %s\n", ma_result_description(result));
		ma_resource_manager_uninit(&m_resourceManager);
		setOn(false, AudioAffect_All);
		return;
	}

	ma_sound_group_init(&m_engine, 0, nullptr, &m_musicGroup);
	ma_sound_group_init(&m_engine, 0, nullptr, &m_soundGroup);
	ma_sound_group_init(&m_engine, 0, nullptr, &m_sound3DGroup);
	ma_sound_group_init(&m_engine, 0, nullptr, &m_speechGroup);

	// In the game's world z points up. miniaudio takes y for it, and the listener, who
	// looks along y unless the camera is turned, could not tell left from right.
	ma_engine_listener_set_world_up(&m_engine, 0, 0.0f, 0.0f, 1.0f);

	m_open = TRUE;
	m_stopOutput = FALSE;

	if (!m_loader.start(&m_engine))
		fprintf(stderr, "[audio] no loader thread: nothing will be heard\n");

	VitaAudioManager *self = this;
	m_outputThread = sceKernelCreateThread("GeneralsAudio", outputThread, OUTPUT_THREAD_PRIORITY, OUTPUT_THREAD_STACK, 0, 0, nullptr);
	if (m_outputThread < 0 || sceKernelStartThread(m_outputThread, sizeof(self), &self) < 0)
	{
		// The game then runs as it would with sound, only nobody hears it.
		fprintf(stderr, "[audio] no output thread: 0x%08X\n", static_cast<unsigned int>(m_outputThread));
		if (m_outputThread >= 0)
			sceKernelDeleteThread(m_outputThread);
		m_outputThread = -1;
	}

	fprintf(stderr, "[audio] miniaudio %s, %d Hz, %d frames per buffer, %u MB for sound effects\n", ma_version_string(),
		OUTPUT_SAMPLE_RATE, OUTPUT_FRAMES, static_cast<unsigned int>(m_effectBudget / (1024 * 1024)));
	fflush(stderr);

	TheAudio->refreshCachedVariables();
}

//-----------------------------------------------------------------------------
void VitaAudioManager::closeDevice()
{
	if (!m_open)
		return;

	// Nobody may be mixing while the sounds and the engine go away.
	if (m_outputThread >= 0)
	{
		m_stopOutput = TRUE;
		sceKernelWaitThreadEnd(m_outputThread, nullptr, nullptr);
		sceKernelDeleteThread(m_outputThread);
		m_outputThread = -1;
	}

	// The loader lets go of the music and speech it is handed here before it ends.
	stopAllAudioImmediately();
	m_loader.stop();
	m_loader.freeEffects();

	ma_sound_group_uninit(&m_speechGroup);
	ma_sound_group_uninit(&m_sound3DGroup);
	ma_sound_group_uninit(&m_soundGroup);
	ma_sound_group_uninit(&m_musicGroup);
	ma_engine_uninit(&m_engine);
	ma_resource_manager_uninit(&m_resourceManager);

	VitaAudioLoader::closeArchives();
	m_open = FALSE;
}

//-----------------------------------------------------------------------------
// The game can run with its sound switched off ("-noaudio"), and opening the device can
// fail. There is no mixer to talk to then.
void VitaAudioManager::setDeviceListenerPosition()
{
	if (m_open)
		MiniAudioManager::setDeviceListenerPosition();
}

//-----------------------------------------------------------------------------
/// The mixer: takes what the engine has mixed and hands it to the console, which blocks
/// until it has room for more.
int VitaAudioManager::outputThread(SceSize argumentSize, void *argument)
{
	VitaAudioManager *manager = *static_cast<VitaAudioManager **>(argument);

	const int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUTPUT_FRAMES, OUTPUT_SAMPLE_RATE, SCE_AUDIO_OUT_MODE_STEREO);
	if (port < 0)
	{
		fprintf(stderr, "[audio] no output port: 0x%08X\n", static_cast<unsigned int>(port));
		fflush(stderr);
		return 0;
	}

	static float mixed[OUTPUT_FRAMES * OUTPUT_CHANNELS];

	// GeneralsX @bugfix Claude 15/10/2026 sceAudioOutOutput() returns when the console starts to
	// play the block it was given, and it plays it from there. With one block, the next one was
	// mixed into it while it played: of every 21 ms, all after the first few was the following
	// block's, a buzz at 47 Hz that was heard as a rattle, "too much bass" and feedback, worst where
	// mixing is quick, in movies and briefings. Two blocks take turns; 64 bytes is the alignment
	// the console asks for.
	alignas(64) static ma_int16 blocks[2][OUTPUT_FRAMES * OUTPUT_CHANNELS];
	UnsignedInt nextBlock = 0;

	// The sum of everything that sounds can be larger than what the output takes, which is
	// 1. Cut off at 1 it rasps: a dozen explosions at once did. So a block that is too loud
	// is turned down as a whole to where its loudest sample fits, and the blocks after it
	// are let up again a little at a time; nothing is changed while the sum fits.
	const float LOUDEST = 0.98f;
	const float LET_UP_PER_BLOCK = 0.04f;
	const UnsignedInt RAMP_FRAMES = 128;
	float gain = 1.0f;

	const uint64_t blockTime = static_cast<uint64_t>(OUTPUT_FRAMES) * 1000000 / OUTPUT_SAMPLE_RATE;
	uint64_t mixingTime = 0;
	uint64_t longestMix = 0;
	UnsignedInt mixedBlocks = 0;
	UnsignedInt loudBlocks = 0;
	UnsignedInt lateBlocks = 0;
	float loudest = 0.0f;

	// sceAudioOutOutput() returns when the console has room for the next block. What passes
	// until the next call, mixing and whatever else runs on the core, has to be less than a
	// block, or the console runs out and the sound has a gap.
	uint64_t outputReturned = 0;
	uint64_t longestAway = 0;
	UnsignedInt gaps = 0;

	std::vector<ma_int16> recording;
	if (getenv("VITA_OUTPUT_SOUND") != nullptr)
		recording.reserve(OUTPUT_SOUND_SECONDS * OUTPUT_SAMPLE_RATE * OUTPUT_CHANNELS);

	while (!manager->m_stopOutput)
	{
		const uint64_t started = VitaDiagnostics::now();

		// Odd while a block is being mixed, see getEndTime().
		__atomic_add_fetch(&manager->m_mixSequence, 1, __ATOMIC_SEQ_CST);
		ma_uint64 frames = 0;
		if (ma_engine_read_pcm_frames(&manager->m_engine, mixed, OUTPUT_FRAMES, &frames) != MA_SUCCESS)
			frames = 0;
		__atomic_add_fetch(&manager->m_mixSequence, 1, __ATOMIC_SEQ_CST);

		const UnsignedInt count = static_cast<UnsignedInt>(frames) * OUTPUT_CHANNELS;
		float peak = 0.0f;
		for (UnsignedInt i = 0; i < count; ++i)
		{
			const float size = fabsf(mixed[i]);
			if (size > peak)
				peak = size;
		}

		const float before = gain;
		const float fitting = peak > LOUDEST ? LOUDEST / peak : 1.0f;
		gain = fitting < gain ? fitting : (gain + LET_UP_PER_BLOCK < fitting ? gain + LET_UP_PER_BLOCK : fitting);
		if (before != 1.0f || gain != 1.0f)
		{
			// From the gain of the block before to this one's over the first few milliseconds,
			// so that the change is not a click.
			const UnsignedInt ramp = RAMP_FRAMES * OUTPUT_CHANNELS < count ? RAMP_FRAMES * OUTPUT_CHANNELS : count;
			for (UnsignedInt i = 0; i < ramp; ++i)
				mixed[i] *= before + (gain - before) * (static_cast<float>(i / OUTPUT_CHANNELS) / RAMP_FRAMES);
			for (UnsignedInt i = ramp; i < count; ++i)
				mixed[i] *= gain;
		}

		ma_int16 *samples = blocks[nextBlock];
		nextBlock ^= 1;
		ma_pcm_f32_to_s16(samples, mixed, frames * OUTPUT_CHANNELS, ma_dither_mode_none);
		if (frames < OUTPUT_FRAMES)
			memset(samples + frames * OUTPUT_CHANNELS, 0, (OUTPUT_FRAMES - frames) * OUTPUT_CHANNELS * sizeof(ma_int16));

		// For the log, about every ten seconds: what would have rasped, and whether mixing a
		// block comes close to the time the block lasts, which would be heard as gaps.
		const uint64_t took = VitaDiagnostics::now() - started;
		mixingTime += took;
		if (took > longestMix)
			longestMix = took;
		if (took > blockTime * 3 / 4)
			++lateBlocks;
		if (peak > 1.0f)
			++loudBlocks;
		if (peak > loudest)
			loudest = peak;

		if (outputReturned != 0)
		{
			const uint64_t away = VitaDiagnostics::now() - outputReturned;
			if (away > longestAway)
				longestAway = away;
			if (away > blockTime)
				++gaps;
		}

		if (++mixedBlocks == MIXING_REPORT_BLOCKS)
		{
			if (loudBlocks != 0 || lateBlocks != 0 || gaps != 0 || VitaDiagnostics::g_profiling)
			{
				fprintf(stderr, "[audio] mixing takes %.2f ms of the %.1f ms a block lasts, %.1f ms at most, %u of %u blocks more than three quarters of it; "
					"%u blocks were louder than the output takes, up to %.2f times, and were turned down; "
					"%u blocks were handed over more than a block after the one before, the latest after %.1f ms\n",
					mixingTime / (1000.0 * mixedBlocks), blockTime / 1000.0, longestMix / 1000.0, static_cast<unsigned int>(lateBlocks),
					static_cast<unsigned int>(mixedBlocks), static_cast<unsigned int>(loudBlocks), loudest,
					static_cast<unsigned int>(gaps), longestAway / 1000.0);
				fflush(stderr);
			}

			longestAway = 0;
			gaps = 0;

			mixingTime = 0;
			longestMix = 0;
			mixedBlocks = 0;
			loudBlocks = 0;
			lateBlocks = 0;
			loudest = 0.0f;
		}

		// The recording begins with the first block that is heard, not while the game loads.
		if (recording.capacity() != 0 && (!recording.empty() || peak > 0.001f))
		{
			recording.insert(recording.end(), samples, samples + OUTPUT_FRAMES * OUTPUT_CHANNELS);
			if (recording.size() >= OUTPUT_SOUND_SECONDS * OUTPUT_SAMPLE_RATE * OUTPUT_CHANNELS)
			{
				writeOutputSound(recording);
				std::vector<ma_int16>().swap(recording);
			}
		}

		sceAudioOutOutput(port, samples);
		outputReturned = VitaDiagnostics::now();
	}

	sceAudioOutReleasePort(port);
	return 0;
}

//-----------------------------------------------------------------------------
/// Finds out where the bytes of a file are, in the form VitaAudioLoader takes. Runs on
/// the game's thread; the archive directory is the engine's.
Bool VitaAudioManager::locateFile(const AsciiString &path, AsciiString &location) const
{
	const std::map<AsciiString, AsciiString>::const_iterator known = m_locations.find(path);
	if (known != m_locations.end())
	{
		location = known->second;
		return !location.isEmpty();
	}

	location.clear();

	// The archives first, unlike the engine, which lets a loose file replace an archived
	// one: looking for a loose file is a question to the memory card, and the game's
	// audio is all archived.
	if (TheArchiveFileSystem != nullptr)
	{
		ArchivedDirectoryInfo *directory = TheArchiveFileSystem->friend_getArchivedDirectoryInfo(path.str());
		if (directory != nullptr)
		{
			AsciiString lowered = path;
			lowered.toLower();
			const char *name = lowered.str();
			for (const char *c = lowered.str(); *c != '\0'; ++c)
			{
				if (*c == '\\' || *c == '/')
					name = c + 1;
			}

			const ArchivedFileLocationMap::const_iterator found = directory->m_files.find(AsciiString(name));
			const ArchivedFileInfo *info = found != directory->m_files.end() ? found->second->getArchivedFileInfo(path) : nullptr;
			if (info != nullptr)
				location.format(">%s|%u|%u|%s", info->m_archiveFilename.str(), info->m_offset, info->m_size, path.str());
		}
	}

	if (location.isEmpty() && TheLocalFileSystem != nullptr && TheLocalFileSystem->doesFileExist(path.str()))
		location = path;

	m_locations[path] = location;
	return !location.isEmpty();
}

// MAKING SOUNDS AND LETTING GO OF THEM -------------------------------------------

//-----------------------------------------------------------------------------
/// What the class this derives from calls to get the sound of an event that is to be played.
ma_sound *VitaAudioManager::createSound(const AsciiString &fileToPlay, ma_uint32 flags, ma_sound_group *groupToUse, PlayingAudio *audio)
{
	AsciiString file = fileToPlay;
	AudioEventRTS *event = audio != nullptr ? audio->m_audioEventRTS.Peek() : nullptr;

	if (event != nullptr && groupToUse == &m_musicGroup)
	{
		m_musicTrack = event->getEventName();
		m_musicRounds = 0;
	}
	else if (event != nullptr && (groupToUse == &m_soundGroup || groupToUse == &m_sound3DGroup))
	{
		// The caller asks for the event's main sound. An event may begin with an attack.
		const AsciiString portion = getPortionFile(event);
		if (!portion.isEmpty())
			file = portion;
	}

	return loadSound(file, flags, groupToUse);
}

//-----------------------------------------------------------------------------
/// Makes a sound of a file without waiting for the file: if it is not in memory, the
/// sound is a stand-in that resolveStandIn() replaces when it is. Does not start it.
ma_sound *VitaAudioManager::loadSound(const AsciiString &file, ma_uint32 flags, ma_sound_group *group)
{
	static VitaDiagnostics::NamedTimer timer("Audio_loadSound");
	const VitaDiagnostics::NamedTimerScope scope(timer);

	// An event may have no sound at all.
	if (!m_open || file.isEmpty())
		return nullptr;

	AsciiString location;
	if (!locateFile(file, location))
	{
		static Int reported = 0;
		if (reported < 20)
		{
			++reported;
			fprintf(stderr, "[audio] not found: %s\n", file.str());
		}
		return nullptr;
	}

	// Music and speech run for seconds to minutes and are decoded as they play. The
	// loader opens them: that waits for miniaudio's decoding thread. A stream is not asked
	// for its length, which for an MP3 means reading the whole file.
	if (group == &m_musicGroup || group == &m_speechGroup)
	{
		VitaSound *opened = allocateSound(flags, group);
		VitaSound *standIn = opened != nullptr ? makeStandIn(flags, group) : nullptr;
		if (standIn == nullptr)
		{
			free(opened);
			return nullptr;
		}

		opened->streamed = TRUE;
		standIn->streamed = TRUE;
		standIn->stream = m_loader.openStream(location.str(),
			flags | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_UNKNOWN_LENGTH, group, &opened->sound);
		return &standIn->sound;
	}

	// Effects are short, played over and over, and decoded once.
	VitaAudioEffect *effect = m_loader.getEffect(location.str());
	VitaSound *made = nullptr;
	switch (VitaAudioLoader::getState(effect))
	{
		case VitaAudioEffect::READY:
			made = makeEffectSound(effect, flags, group);
			break;

		case VitaAudioEffect::LOADING:
			made = makeStandIn(flags, group);
			if (made != nullptr)
				made->effect = effect;
			m_effectsGrew = TRUE;
			break;

		default:
			break;
	}

	return made != nullptr ? &made->sound : nullptr;
}

//-----------------------------------------------------------------------------
/// Makes a sound that plays nothing and takes what a sound is told: its volume, where it
/// is, when to start. To miniaudio it is a group that nothing plays in.
VitaSound *VitaAudioManager::makeStandIn(ma_uint32 flags, ma_sound_group *group)
{
	VitaSound *made = allocateSound(flags, group);
	if (made == nullptr)
		return nullptr;

	ma_sound_config config = ma_sound_config_init_2(&m_engine);
	config.flags = flags;
	config.pInitialAttachment = group;
	if (ma_sound_init_ex(&m_engine, &config, &made->sound) != MA_SUCCESS)
	{
		free(made);
		return nullptr;
	}

	made->standIn = TRUE;
	return made;
}

//-----------------------------------------------------------------------------
/// Makes a sound of an effect that is in memory, which costs no more than the sound.
VitaSound *VitaAudioManager::makeEffectSound(VitaAudioEffect *effect, ma_uint32 flags, ma_sound_group *group)
{
	VitaSound *made = allocateSound(flags, group);
	if (made == nullptr)
		return nullptr;

	made->effect = effect;
	ma_audio_buffer_ref_init(ma_format_s16, effect->channels, effect->samples, effect->frames, &made->samples);
	made->samples.sampleRate = effect->sampleRate;
	if (ma_sound_init_from_data_source(&m_engine, &made->samples, flags, group, &made->sound) != MA_SUCCESS)
	{
		free(made);
		return nullptr;
	}

	return made;
}

//-----------------------------------------------------------------------------
/// Puts the sound a stand-in stands for in its place once its file is in memory, set up
/// as the stand-in was. Returns FALSE if the file turned out not to be playable.
Bool VitaAudioManager::resolveStandIn(ma_sound *&sound)
{
	VitaSound *standIn = toVitaSound(sound);
	if (!standIn->standIn)
		return TRUE;

	VitaSound *made = nullptr;
	if (standIn->streamed)
	{
		if (standIn->stream == nullptr)
			return FALSE;
		if (VitaAudioLoader::getState(standIn->stream) == VitaAudioStream::LOADING)
			return TRUE;

		ma_sound *opened = m_loader.finishStream(standIn->stream);
		standIn->stream = nullptr;
		if (opened == nullptr)
			return FALSE;
		made = toVitaSound(opened);
	}
	else
	{
		const int state = VitaAudioLoader::getState(standIn->effect);
		if (state == VitaAudioEffect::LOADING)
			return TRUE;

		m_effectsGrew = TRUE;
		if (state == VitaAudioEffect::READY)
			made = makeEffectSound(standIn->effect, standIn->flags, standIn->group);
		if (made == nullptr)
			return FALSE;
	}

	ma_sound *from = &standIn->sound;
	ma_sound *to = &made->sound;

	ma_sound_set_volume(to, ma_sound_get_volume(from));
	if ((standIn->flags & MA_SOUND_FLAG_NO_SPATIALIZATION) == 0)
	{
		const ma_vec3f position = ma_sound_get_position(from);
		ma_sound_set_min_distance(to, ma_sound_get_min_distance(from));
		ma_sound_set_max_distance(to, ma_sound_get_max_distance(from));
		ma_sound_set_position(to, position.x, position.y, position.z);
	}

	// Started, and with the time to start at, if the stand-in was. One that pauseAudio()
	// has stopped stays stopped.
	made->paused = standIn->paused;
	if (ma_node_get_state(from) == ma_node_state_started)
	{
		ma_sound_set_start_time_in_pcm_frames(to, ma_node_get_state_time(from, ma_node_state_started));
		ma_sound_start(to);
	}

	const uint64_t waited = VitaDiagnostics::now() - standIn->asked;
	++m_lateSounds;
	m_lateTime += waited;
	if (waited > m_latest)
		m_latest = waited;

	sound = to;
	ma_sound_uninit(from);
	free(standIn);
	return TRUE;
}

//-----------------------------------------------------------------------------
void VitaAudioManager::discardSound(ma_sound *sound)
{
	VitaSound *vitaSound = toVitaSound(sound);

	if (vitaSound->standIn)
	{
		// The loader ends what it was opening for the stand-in.
		if (vitaSound->stream != nullptr)
			m_loader.abandonStream(vitaSound->stream);
		ma_sound_uninit(sound);
		free(vitaSound);
	}
	else if (vitaSound->streamed)
	{
		// Letting go of music or speech waits for miniaudio's decoding thread. It is
		// silent from here on, and the rest is the loader's.
		ma_sound_stop(sound);
		m_loader.discard(sound);
	}
	else
	{
		ma_sound_uninit(sound);
		ma_audio_buffer_ref_uninit(&vitaSound->samples);
		free(vitaSound);
	}
}

//-----------------------------------------------------------------------------
/// Every way the class this derives from lets go of a sound ends here.
void VitaAudioManager::releaseMiniAudioHandles(PlayingAudio *release)
{
	if (release->m_sound != nullptr)
	{
		discardSound(release->m_sound);
		release->m_sound = nullptr;
	}

	if (release->m_nextSound != nullptr)
	{
		discardSound(release->m_nextSound);
		release->m_nextSound = nullptr;
	}

	MiniAudioManager::releaseMiniAudioHandles(release);
}

//-----------------------------------------------------------------------------
/// Lets go of the decoded effects that were used longest ago when there are too many.
void VitaAudioManager::trimEffects()
{
	m_effectsGrew = FALSE;
	if (m_loader.getEffectBytes() <= m_effectBudget)
		return;

	std::vector<const VitaAudioEffect *> inUse;
	std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_fadingAudio, &m_stoppedAudio };
	for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i)
	{
		for (std::list<PlayingAudio *>::iterator it = lists[i]->begin(); it != lists[i]->end(); ++it)
		{
			const PlayingAudio *playing = *it;
			if (playing == nullptr)
				continue;
			if (playing->m_sound != nullptr && toVitaSound(playing->m_sound)->effect != nullptr)
				inUse.push_back(toVitaSound(playing->m_sound)->effect);
			if (playing->m_nextSound != nullptr && toVitaSound(playing->m_nextSound)->effect != nullptr)
				inUse.push_back(toVitaSound(playing->m_nextSound)->effect);
		}
	}

	m_loader.trimEffects(m_effectBudget, inUse);
}

//-----------------------------------------------------------------------------
/// While a frame profile is taken: how long sounds had to wait for their files.
void VitaAudioManager::reportLoading()
{
	const uint64_t now = VitaDiagnostics::now();
	if (now - m_lastReport < LOADING_REPORT_US)
		return;

	m_lastReport = now;
	if (m_lateSounds != 0 || m_droppedSounds != 0)
	{
		fprintf(stderr, "[audio] %u sounds waited for their file, %.0f ms on average, %.0f ms at most; %u came too late to be played; "
			"%u effects in memory, %.1f MB\n", m_lateSounds, m_lateSounds != 0 ? m_lateTime / (1000.0 * m_lateSounds) : 0.0,
			m_latest / 1000.0, m_droppedSounds, static_cast<unsigned int>(m_loader.getEffectCount()),
			m_loader.getEffectBytes() / (1024.0 * 1024.0));
		fflush(stderr);
	}

	m_lateSounds = 0;
	m_droppedSounds = 0;
	m_lateTime = 0;
	m_latest = 0;
}

// THE SOUNDS THAT ARE PLAYING ------------------------------------------------

//-----------------------------------------------------------------------------
void VitaAudioManager::processPlayingList()
{
	static VitaDiagnostics::NamedTimer timer("Audio_playingList");
	const VitaDiagnostics::NamedTimerScope scope(timer);

	for (std::list<PlayingAudio *>::iterator it = m_playingSounds.begin(); it != m_playingSounds.end(); )
	{
		PlayingAudio *playing = *it;
		if (playing != nullptr && updatePlaying(playing))
		{
			++it;
			continue;
		}

		it = m_playingSounds.erase(it);
		releasePlayingAudio(playing);
	}

	if (m_volumeHasChanged)
	{
		m_volumeHasChanged = false;

		// The sound of a movie does not go through the mixer.
		if (TheVideoPlayer != nullptr)
			TheVideoPlayer->setVolume(getVolume(AudioAffect_Speech));
	}

	if (m_effectsGrew)
		trimEffects();
	if (VitaDiagnostics::g_profiling)
		reportLoading();
}

//-----------------------------------------------------------------------------
/// Looks after one sound that is playing. Returns FALSE when the game is done with it.
Bool VitaAudioManager::updatePlaying(PlayingAudio *playing)
{
	AudioEventRTS *event = playing->m_audioEventRTS.Peek();
	if (playing->m_sound == nullptr || event == nullptr || event->getAudioEventInfo() == nullptr)
		return FALSE;

	// What was not in memory when the sound was asked for may be by now.
	if (!resolveStandIn(playing->m_sound))
		return FALSE;
	if (playing->m_nextSound != nullptr && !resolveStandIn(playing->m_nextSound))
	{
		discardSound(playing->m_nextSound);
		playing->m_nextSound = nullptr;
	}

	if (playing->m_type == PAT_Stream)
		return updateStream(playing);

	if (toVitaSound(playing->m_sound)->paused)
		return TRUE;

	Bool waiting = toVitaSound(playing->m_sound)->standIn;
	if (waiting)
	{
		// Nothing has been heard of it yet, so there is nothing to end properly.
		if (playing->m_requestStop)
			return FALSE;

		const Bool looping = (event->getAudioEventInfo()->m_control & AC_LOOP) != 0;
		if (!looping && VitaDiagnostics::now() - toVitaSound(playing->m_sound)->asked > LATE_LIMIT_US)
		{
			++m_droppedSounds;
			return FALSE;
		}
	}
	else
	{
		// A sound that has played to its end is followed by what its event says. What
		// follows was started at a time set in advance and may be over itself after a
		// long frame.
		while (hasEnded(playing->m_sound))
		{
			if (!startFollower(playing))
				return FALSE;
		}

		waiting = toVitaSound(playing->m_sound)->standIn;
	}

	ma_sound *sound = playing->m_sound;

	// Nothing here stops a sound before its end. One that is silent for another reason
	// than waiting for its time to start would stay in the list for good.
	const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&m_engine);
	if (!waiting && !ma_sound_is_playing(sound) && !ma_sound_at_end(sound) && ma_node_get_state_time(sound, ma_node_state_started) <= now)
		return FALSE;

	if (m_volumeHasChanged)
	{
		adjustPlayingVolume(playing);
		if (playing->m_nextSound != nullptr)
			ma_sound_set_volume(playing->m_nextSound, ma_sound_get_volume(sound));
	}

	if (playing->m_type == PAT_3DSample)
	{
		const Coord3D *position = getCurrentPositionFromEvent(event);
		if (position == nullptr)
			return FALSE;

		if (event->isDead())
		{
			// What made the sound is gone. The sound is treated as if the game had stopped it.
			playing->m_requestStop = true;
		}
		else
		{
			if (isTooQuiet(event, *position))
				return FALSE;

			ma_sound_set_position(sound, position->x, position->y, position->z);
			if (playing->m_nextSound != nullptr)
				ma_sound_set_position(playing->m_nextSound, position->x, position->y, position->z);
		}
	}

	// A sound effect that is told to stop is not cut off: it plays to its end, a loop does
	// not start another round, and the decay follows if the event has one.
	if (!waiting)
		prepareFollower(playing);
	return TRUE;
}

//-----------------------------------------------------------------------------
/// Looks after the music or a piece of speech. Returns FALSE when it is over.
Bool VitaAudioManager::updateStream(PlayingAudio *playing)
{
	AudioEventRTS *event = playing->m_audioEventRTS.Peek();
	const Bool music = event->getAudioEventInfo()->m_soundType == AT_Music;
	const VitaSound *current = toVitaSound(playing->m_sound);

	// Speech that is told to stop stops at once, and so does music.
	Bool keep = !playing->m_requestStop;
	if (keep && !current->standIn && !current->paused)
	{
		keep = !hasFailed(playing->m_sound);
		if (keep && hasEnded(playing->m_sound))
		{
			// Music plays until other music takes its place: starting a sound that is at
			// its end plays it from the beginning. Speech is over.
			keep = music && ma_sound_start(playing->m_sound) == MA_SUCCESS;
			if (keep && event->getEventName() == m_musicTrack)
				++m_musicRounds;
		}
	}

	if (!keep)
	{
		// Speech that must not be interrupted keeps all other speech out until it is over.
		if (!music && getDisallowSpeech())
			setDisallowSpeech(FALSE);
		return FALSE;
	}

	if (m_volumeHasChanged)
		adjustPlayingVolume(playing);
	return TRUE;
}

//-----------------------------------------------------------------------------
/// Whether a positional sound has become too quiet to be worth mixing. As the original
/// judged it: the volume falls with the distance beyond the event's minimum distance and
/// is nothing at its maximum. Whoever plays the sound starts it again when it is in range.
Bool VitaAudioManager::isTooQuiet(AudioEventRTS *event, const Coord3D &position) const
{
	const AudioEventInfo *info = event->getAudioEventInfo();
	if (BitIsSet(info->m_type, ST_GLOBAL) || info->m_priority == AP_CRITICAL)
		return FALSE;

	Coord3D toListener = m_listenerPosition;
	toListener.sub(position);
	const Real distance = toListener.length();
	if (distance >= info->m_maxDistance)
		return TRUE;

	Real volume = event->getVolume() * event->getVolumeShift();
	if (distance > info->m_minDistance)
		volume *= info->m_minDistance / distance;
	return volume < getAudioSettings()->m_minVolume;
}

// WHAT FOLLOWS A SOUND EFFECT ------------------------------------------------

//-----------------------------------------------------------------------------
/// Says which file follows the one a sound effect is playing, and moves the event on to
/// it: the next round of a loop, the sound after the attack, the decay after the sound.
/// Nothing follows if the name is empty; requestAgain then says whether the event is to
/// ask for itself again, which a loop with a pause between its rounds does.
AsciiString VitaAudioManager::decideWhatFollows(PlayingAudio *playing, Bool &requestAgain)
{
	AudioEventRTS *event = playing->m_audioEventRTS.Peek();
	requestAgain = FALSE;

	if ((event->getAudioEventInfo()->m_control & AC_LOOP) != 0)
	{
		if (event->getNextPlayPortion() == PP_Attack)
			event->setNextPlayPortion(PP_Sound);

		if (event->getNextPlayPortion() == PP_Sound)
		{
			event->decreaseLoopCount();
			if (!playing->m_requestStop && event->hasMoreLoops())
			{
				// Every round may be another one of the event's sounds, after another pause.
				event->generateFilename();
				requestAgain = event->getDelay() > MSEC_PER_LOGICFRAME_REAL;
				return requestAgain ? AsciiString::TheEmptyString : event->getFilename();
			}
		}
	}

	event->advanceNextPlayPortion();
	return getPortionFile(event);
}

//-----------------------------------------------------------------------------
/// Makes what follows a sound effect ready shortly before the effect ends and sets it to
/// start at that time. The game only looks at its sounds once per frame; started then,
/// every round of a loop would begin up to a frame late.
void VitaAudioManager::prepareFollower(PlayingAudio *playing)
{
	AudioEventRTS *event = playing->m_audioEventRTS.Peek();
	const AudioEventInfo *info = event->getAudioEventInfo();
	const Bool looping = (info->m_control & AC_LOOP) != 0;
	const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&m_engine);

	// The loop was told to stop after its next round had been made ready. The round is not
	// played unless it has begun.
	if (playing->m_nextSound != nullptr && playing->m_requestStop && looping && event->getNextPlayPortion() == PP_Sound
		&& ma_node_get_state_time(playing->m_nextSound, ma_node_state_started) > now)
	{
		discardSound(playing->m_nextSound);
		playing->m_nextSound = nullptr;
	}

	if (playing->m_nextSound != nullptr)
		return;

	// Most effects are followed by nothing, and nothing follows a decay.
	const PortionToPlay portion = event->getNextPlayPortion();
	if (portion == PP_Decay || portion == PP_Done)
		return;
	if (!looping && portion == PP_Sound && event->getDecayFilename().isEmpty())
		return;

	// A loop with a pause between its rounds asks for itself again when a round ends.
	if (looping && info->m_delayMax > MSEC_PER_LOGICFRAME_REAL)
		return;

	ma_uint64 endTime = 0;
	if (!getEndTime(playing->m_sound, endTime) || endTime > now + FOLLOWER_LEAD_FRAMES)
		return;

	Bool requestAgain = FALSE;
	const AsciiString file = decideWhatFollows(playing, requestAgain);
	if (file.isEmpty())
		return;

	ma_sound *follower = loadFollower(playing, file);
	if (follower == nullptr)
		return;

	// A sound starts with the first piece of a block that begins at or after its start
	// time. The nearer boundary keeps what is missing or doubled below 3 ms.
	const ma_uint64 startTime = (endTime + MIX_CHUNK_FRAMES / 2) / MIX_CHUNK_FRAMES * MIX_CHUNK_FRAMES;
	ma_sound_set_start_time_in_pcm_frames(follower, startTime);
	if (ma_sound_start(follower) != MA_SUCCESS)
	{
		discardSound(follower);
		return;
	}

	playing->m_nextSound = follower;
}

//-----------------------------------------------------------------------------
/// Puts what follows a sound effect that has played to its end in its place. Returns
/// FALSE if nothing follows.
Bool VitaAudioManager::startFollower(PlayingAudio *playing)
{
	if (playing->m_nextSound == nullptr)
	{
		// Nothing was made ready ahead of time: the effect was too short for it, or the
		// frame too long.
		Bool requestAgain = FALSE;
		const AsciiString file = decideWhatFollows(playing, requestAgain);
		if (requestAgain)
		{
			// The event waits in the list of requests until its pause is over.
			AudioRequest *request = allocateAudioRequest();
			request->m_pendingEvent = playing->m_audioEventRTS;
			request->m_requiresCheckForSample = true;
			appendAudioRequest(request);
			return FALSE;
		}

		if (file.isEmpty())
			return FALSE;

		// If this fails, the sound goes with the entry it now belongs to.
		playing->m_nextSound = loadFollower(playing, file);
		if (playing->m_nextSound == nullptr || ma_sound_start(playing->m_nextSound) != MA_SUCCESS)
			return FALSE;
	}

	discardSound(playing->m_sound);
	playing->m_sound = playing->m_nextSound;
	playing->m_nextSound = nullptr;
	return TRUE;
}

//-----------------------------------------------------------------------------
/// Makes the sound that follows the one an entry is playing, set up like that one. Does
/// not start it.
ma_sound *VitaAudioManager::loadFollower(PlayingAudio *playing, const AsciiString &file)
{
	const Bool positional = playing->m_type == PAT_3DSample;
	ma_sound *sound = loadSound(file, positional ? 0 : (MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH),
		positional ? &m_sound3DGroup : &m_soundGroup);
	if (sound == nullptr)
		return nullptr;

	if (positional)
	{
		const ma_vec3f position = ma_sound_get_position(playing->m_sound);
		ma_sound_set_min_distance(sound, ma_sound_get_min_distance(playing->m_sound));
		ma_sound_set_max_distance(sound, ma_sound_get_max_distance(playing->m_sound));
		ma_sound_set_position(sound, position.x, position.y, position.z);
	}

	ma_sound_set_volume(sound, ma_sound_get_volume(playing->m_sound));
	return sound;
}

//-----------------------------------------------------------------------------
/// Says when a sound effect will have played its last sample, in the engine's time.
Bool VitaAudioManager::getEndTime(ma_sound *sound, ma_uint64 &endTime)
{
	const VitaSound *vitaSound = toVitaSound(sound);
	if (vitaSound->standIn || vitaSound->streamed || vitaSound->effect == nullptr || vitaSound->effect->sampleRate == 0)
		return FALSE;

	const ma_uint64 rate = vitaSound->effect->sampleRate;
	const ma_uint64 length = vitaSound->effect->frames;

	// How far the sound has been read and what time it is belong together for certain only
	// between two blocks; the output thread counts up before and after it mixes one. While
	// a block is mixed the sound can be a piece ahead of the clock, which makes the answer
	// 5 ms too early. That is taken if the block is not over after a few looks.
	for (Int attempt = 0; ; ++attempt)
	{
		const UnsignedInt sequence = __atomic_load_n(&m_mixSequence, __ATOMIC_SEQ_CST);
		const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&m_engine);
		ma_uint64 cursor = 0;
		if (ma_sound_get_cursor_in_pcm_frames(sound, &cursor) != MA_SUCCESS)
			return FALSE;

		const ma_uint64 left = length > cursor ? length - cursor : 0;
		endTime = now + (left * OUTPUT_SAMPLE_RATE + rate - 1) / rate;

		const Bool betweenBlocks = (sequence & 1) == 0 && __atomic_load_n(&m_mixSequence, __ATOMIC_SEQ_CST) == sequence;
		if (betweenBlocks || attempt == 2)
			return TRUE;
	}
}

// WHAT THE GAME ASKS OF ALL SOUNDS ---------------------------------------------

//-----------------------------------------------------------------------------
Bool VitaAudioManager::isAffected(const PlayingAudio *playing, AudioAffect which) const
{
	switch (playing->m_type)
	{
		case PAT_Sample:
			return BitIsSet(which, AudioAffect_Sound);

		case PAT_3DSample:
			return BitIsSet(which, AudioAffect_Sound3D);

		case PAT_Stream:
		{
			const AudioEventInfo *info = playing->m_audioEventRTS ? playing->m_audioEventRTS->getAudioEventInfo() : nullptr;
			const Bool music = info != nullptr && info->m_soundType == AT_Music;
			return BitIsSet(which, music ? AudioAffect_Music : AudioAffect_Speech);
		}

		default:
			return FALSE;
	}
}

//-----------------------------------------------------------------------------
/// Holds the sounds of the given kinds that are playing. What is started afterwards
/// plays: the game pauses its sound for a message that has a sound and a voice of its own.
void VitaAudioManager::pauseAudio(AudioAffect which)
{
	if (!m_open)
		return;

	const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&m_engine);
	for (std::list<PlayingAudio *>::iterator it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it)
	{
		PlayingAudio *playing = *it;
		if (playing == nullptr || playing->m_sound == nullptr || !isAffected(playing, which))
			continue;

		// What was to follow is decided again when the sound goes on, unless it has begun.
		if (playing->m_nextSound != nullptr)
		{
			if (ma_node_get_state_time(playing->m_nextSound, ma_node_state_started) > now)
			{
				discardSound(playing->m_nextSound);
				playing->m_nextSound = nullptr;
			}
			else
			{
				ma_sound_stop(playing->m_nextSound);
				toVitaSound(playing->m_nextSound)->paused = TRUE;
			}
		}

		ma_sound_stop(playing->m_sound);
		toVitaSound(playing->m_sound)->paused = TRUE;
	}

	// As the original did: what was asked for but has not begun is not played.
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin(); it != m_audioRequests.end(); )
	{
		AudioRequest *request = *it;
		if (request != nullptr && request->m_request == AR_Play)
		{
			deleteInstance(request);
			it = m_audioRequests.erase(it);
		}
		else
		{
			++it;
		}
	}
}

//-----------------------------------------------------------------------------
void VitaAudioManager::resumeAudio(AudioAffect which)
{
	if (!m_open)
		return;

	for (std::list<PlayingAudio *>::iterator it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it)
	{
		PlayingAudio *playing = *it;
		if (playing == nullptr || !isAffected(playing, which))
			continue;

		ma_sound *sounds[] = { playing->m_sound, playing->m_nextSound };
		for (size_t i = 0; i < sizeof(sounds) / sizeof(sounds[0]); ++i)
		{
			if (sounds[i] == nullptr || !toVitaSound(sounds[i])->paused)
				continue;

			// Starting a sound that is at its end would play it again.
			toVitaSound(sounds[i])->paused = FALSE;
			if (!ma_sound_at_end(sounds[i]))
				ma_sound_start(sounds[i]);
		}
	}
}

//-----------------------------------------------------------------------------
/// Ends the sounds of the given kinds that are playing.
void VitaAudioManager::stopAudio(AudioAffect which)
{
	if (!m_open)
		return;

	for (std::list<PlayingAudio *>::iterator it = m_playingSounds.begin(); it != m_playingSounds.end(); )
	{
		PlayingAudio *playing = *it;
		if (playing != nullptr && isAffected(playing, which))
		{
			it = m_playingSounds.erase(it);
			releasePlayingAudio(playing);
		}
		else
		{
			++it;
		}
	}
}

//-----------------------------------------------------------------------------
Bool VitaAudioManager::isMusicPlaying() const
{
	for (std::list<PlayingAudio *>::const_iterator it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it)
	{
		const PlayingAudio *playing = *it;
		if (playing == nullptr || playing->m_sound == nullptr || playing->m_type != PAT_Stream || !playing->m_audioEventRTS)
			continue;

		const AudioEventInfo *info = playing->m_audioEventRTS->getAudioEventInfo();
		if (info != nullptr && info->m_soundType == AT_Music && !toVitaSound(playing->m_sound)->paused)
			return TRUE;
	}

	return FALSE;
}

//-----------------------------------------------------------------------------
/// Scripts wait for the music to have played through. The track is the one started last.
Bool VitaAudioManager::hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const
{
	return trackName == m_musicTrack && m_musicRounds >= numberOfTimes;
}

//-----------------------------------------------------------------------------
Real VitaAudioManager::getFileLengthMS(AsciiString strToLoad) const
{
	if (!m_open || strToLoad.isEmpty())
		return 0.0f;

	const std::map<AsciiString, Real>::const_iterator known = m_lengths.find(strToLoad);
	if (known != m_lengths.end())
		return known->second;

	Real length = 0.0f;

	// The length of an MP3 is only known after going through all of it, megabytes on the
	// memory card. Only the music is MP3, and nothing waits for a music track by its length.
	const char *extension = strToLoad.reverseFind('.');
	const Bool compressed = extension != nullptr && strcasecmp(extension, ".mp3") == 0;

	// This is the one place where the game's thread reads a sound file itself: whoever
	// asks wants the answer now. It is the header, once per file.
	AsciiString location;
	if (!compressed && locateFile(strToLoad, location))
	{
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init_default();
		if (ma_decoder_init_vfs(m_loader.getFileSystem(), location.str(), &config, &decoder) == MA_SUCCESS)
		{
			ma_uint64 frames = 0;
			if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && decoder.outputSampleRate != 0)
				length = static_cast<Real>(frames) * 1000.0f / static_cast<Real>(decoder.outputSampleRate);
			ma_decoder_uninit(&decoder);
		}
	}

	m_lengths[strToLoad] = length;
	return length;
}
