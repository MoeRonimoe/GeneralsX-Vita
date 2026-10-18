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

// VitaAudioLoader.cpp //////////////////////////////////////////////////////////
// See VitaAudioLoader.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaAudioLoader.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace
{

// First character of the name of an archived file: >archive|offset|size|name. The name
// comes last because miniaudio picks its decoder by the extension.
const char LOCATION_MARKER = '>';
const char LOCATION_SEPARATOR = '|';

enum { MAX_PATH_LENGTH = 512 };

// What the callbacks read from the card at a time when they are asked for less. A WAV
// header is a dozen reads of a few bytes, an MP3 is read in pieces of a few kilobytes.
const size_t FILE_BUFFER_BYTES = 64 * 1024;

// The decoders keep their working buffers on the stack.
const size_t LOADER_THREAD_STACK = 512 * 1024;

// ARCHIVES -------------------------------------------------------------------
// The archives the audio lives in, opened once and read by position, which needs no
// file pointer and so no lock around a read. A process may only have 64 files open; the
// game's sounds, speech and music are in five archives.
enum { MAX_OPEN_ARCHIVES = 12 };

struct OpenArchive
{
	char path[MAX_PATH_LENGTH];
	int descriptor;
};

OpenArchive s_archives[MAX_OPEN_ARCHIVES];
int s_archiveCount = 0;
pthread_mutex_t s_archiveMutex = PTHREAD_MUTEX_INITIALIZER;

//-----------------------------------------------------------------------------
/// The engine writes its paths with backslashes.
void copyPath(char *destination, const char *source, size_t length)
{
	if (length >= MAX_PATH_LENGTH)
		length = MAX_PATH_LENGTH - 1;

	for (size_t i = 0; i < length; ++i)
		destination[i] = source[i] == '\\' ? '/' : source[i];
	destination[length] = '\0';
}

//-----------------------------------------------------------------------------
int getArchiveDescriptor(const char *path)
{
	int descriptor = -1;

	pthread_mutex_lock(&s_archiveMutex);

	for (int i = 0; i < s_archiveCount && descriptor < 0; ++i)
	{
		if (strcmp(s_archives[i].path, path) == 0)
			descriptor = s_archives[i].descriptor;
	}

	if (descriptor < 0 && s_archiveCount < MAX_OPEN_ARCHIVES)
	{
		descriptor = open(path, O_RDONLY);
		if (descriptor >= 0)
		{
			strcpy(s_archives[s_archiveCount].path, path);
			s_archives[s_archiveCount].descriptor = descriptor;
			++s_archiveCount;
		}
		else
		{
			fprintf(stderr, "[audio] cannot open %s\n", path);
		}
	}

	pthread_mutex_unlock(&s_archiveMutex);
	return descriptor;
}

//-----------------------------------------------------------------------------
void closeOpenArchives()
{
	pthread_mutex_lock(&s_archiveMutex);

	for (int i = 0; i < s_archiveCount; ++i)
		close(s_archives[i].descriptor);
	s_archiveCount = 0;

	pthread_mutex_unlock(&s_archiveMutex);
}

// FILES ----------------------------------------------------------------------
/// Where the bytes of a file are.
struct FilePlace
{
	int descriptor;
	bool ownsDescriptor;	///< a file of its own, not a part of an archive
	ma_int64 start;			///< where the file begins in what the descriptor refers to
	ma_int64 size;
};

//-----------------------------------------------------------------------------
ma_result findFile(const char *location, FilePlace &place)
{
	char path[MAX_PATH_LENGTH];

	if (location[0] == LOCATION_MARKER)
	{
		const char *archiveEnd = strchr(location, LOCATION_SEPARATOR);
		const char *offsetEnd = archiveEnd != nullptr ? strchr(archiveEnd + 1, LOCATION_SEPARATOR) : nullptr;
		const char *sizeEnd = offsetEnd != nullptr ? strchr(offsetEnd + 1, LOCATION_SEPARATOR) : nullptr;
		if (sizeEnd == nullptr)
			return MA_INVALID_ARGS;

		copyPath(path, location + 1, archiveEnd - (location + 1));
		place.descriptor = getArchiveDescriptor(path);
		place.ownsDescriptor = false;
		place.start = strtoul(archiveEnd + 1, nullptr, 10);
		place.size = strtoul(offsetEnd + 1, nullptr, 10);
		return place.descriptor >= 0 ? MA_SUCCESS : MA_DOES_NOT_EXIST;
	}

	copyPath(path, location, strlen(location));
	place.descriptor = open(path, O_RDONLY);
	if (place.descriptor < 0)
		return MA_DOES_NOT_EXIST;

	place.ownsDescriptor = true;
	place.start = 0;
	place.size = lseek(place.descriptor, 0, SEEK_END);
	if (place.size < 0)
	{
		close(place.descriptor);
		return MA_IO_ERROR;
	}

	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
void leaveFile(FilePlace &place)
{
	if (place.ownsDescriptor)
		close(place.descriptor);
}

//-----------------------------------------------------------------------------
/// Reads from a position in a file. Returns what it got, which is less than asked for
/// only at the end of the file or after an error.
size_t readAt(const FilePlace &place, ma_int64 position, void *destination, size_t size)
{
	if (position >= place.size)
		return 0;
	if (static_cast<ma_int64>(size) > place.size - position)
		size = static_cast<size_t>(place.size - position);

	size_t total = 0;
	while (total < size)
	{
		const ssize_t got = pread(place.descriptor, static_cast<char *>(destination) + total, size - total,
			static_cast<off_t>(place.start + position + static_cast<ma_int64>(total)));
		if (got <= 0)
			break;

		total += static_cast<size_t>(got);
	}

	return total;
}

// FILES FOR MINIAUDIO ------------------------------------------------------------
struct OpenFile
{
	FilePlace place;
	ma_int64 cursor;

	// The part of the file that was read ahead.
	ma_int64 bufferStart;
	size_t bufferFill;
	unsigned char buffer[FILE_BUFFER_BYTES];
};

//-----------------------------------------------------------------------------
ma_result fileOpen(ma_vfs *fileSystem, const char *location, ma_uint32 openMode, ma_vfs_file *file)
{
	if (location == nullptr || file == nullptr || (openMode & MA_OPEN_MODE_WRITE) != 0)
		return MA_INVALID_ARGS;

	FilePlace place;
	const ma_result found = findFile(location, place);
	if (found != MA_SUCCESS)
		return found;

	OpenFile *opened = static_cast<OpenFile *>(malloc(sizeof(OpenFile)));
	if (opened == nullptr)
	{
		leaveFile(place);
		return MA_OUT_OF_MEMORY;
	}

	opened->place = place;
	opened->cursor = 0;
	opened->bufferStart = 0;
	opened->bufferFill = 0;
	*file = opened;
	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
ma_result fileClose(ma_vfs *fileSystem, ma_vfs_file file)
{
	OpenFile *opened = static_cast<OpenFile *>(file);
	if (opened == nullptr)
		return MA_INVALID_ARGS;

	leaveFile(opened->place);
	free(opened);
	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
ma_result fileRead(ma_vfs *fileSystem, ma_vfs_file file, void *destination, size_t sizeInBytes, size_t *bytesRead)
{
	OpenFile *opened = static_cast<OpenFile *>(file);
	if (opened == nullptr || destination == nullptr)
		return MA_INVALID_ARGS;

	unsigned char *out = static_cast<unsigned char *>(destination);
	size_t total = 0;

	while (total < sizeInBytes && opened->cursor < opened->place.size)
	{
		size_t wanted = sizeInBytes - total;
		if (static_cast<ma_int64>(wanted) > opened->place.size - opened->cursor)
			wanted = static_cast<size_t>(opened->place.size - opened->cursor);

		// What was read ahead.
		if (opened->cursor >= opened->bufferStart && opened->cursor < opened->bufferStart + static_cast<ma_int64>(opened->bufferFill))
		{
			const size_t offset = static_cast<size_t>(opened->cursor - opened->bufferStart);
			const size_t count = std::min(wanted, opened->bufferFill - offset);
			memcpy(out + total, opened->buffer + offset, count);
			total += count;
			opened->cursor += count;
			continue;
		}

		// A large read goes straight to where it is wanted.
		if (wanted >= FILE_BUFFER_BYTES)
		{
			const size_t count = readAt(opened->place, opened->cursor, out + total, wanted);
			if (count == 0)
				break;

			total += count;
			opened->cursor += count;
			continue;
		}

		opened->bufferStart = opened->cursor;
		opened->bufferFill = readAt(opened->place, opened->cursor, opened->buffer, FILE_BUFFER_BYTES);
		if (opened->bufferFill == 0)
			break;
	}

	if (bytesRead != nullptr)
		*bytesRead = total;

	if (total == 0 && sizeInBytes > 0)
		return opened->cursor >= opened->place.size ? MA_AT_END : MA_IO_ERROR;
	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
ma_result fileSeek(ma_vfs *fileSystem, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
{
	OpenFile *opened = static_cast<OpenFile *>(file);
	if (opened == nullptr)
		return MA_INVALID_ARGS;

	ma_int64 position = offset;
	if (origin == ma_seek_origin_current)
		position += opened->cursor;
	else if (origin == ma_seek_origin_end)
		position += opened->place.size;

	if (position < 0 || position > opened->place.size)
		return MA_BAD_SEEK;

	opened->cursor = position;
	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
ma_result fileTell(ma_vfs *fileSystem, ma_vfs_file file, ma_int64 *cursor)
{
	const OpenFile *opened = static_cast<const OpenFile *>(file);
	if (opened == nullptr || cursor == nullptr)
		return MA_INVALID_ARGS;

	*cursor = opened->cursor;
	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
ma_result fileInfo(ma_vfs *fileSystem, ma_vfs_file file, ma_file_info *info)
{
	const OpenFile *opened = static_cast<const OpenFile *>(file);
	if (opened == nullptr || info == nullptr)
		return MA_INVALID_ARGS;

	info->sizeInBytes = static_cast<ma_uint64>(opened->place.size);
	return MA_SUCCESS;
}

//-----------------------------------------------------------------------------
/// The name of a file without where it is, for the log.
const char *getName(const std::string &location)
{
	const size_t separator = location.rfind(LOCATION_SEPARATOR);
	return location.c_str() + (separator == std::string::npos ? 0 : separator + 1);
}

} // namespace

//-----------------------------------------------------------------------------
VitaAudioLoader::VitaAudioLoader() :
	m_engine(nullptr),
	m_running(false),
	m_stopping(false),
	m_useCount(0)
{
	memset(&m_fileSystem, 0, sizeof(m_fileSystem));
	m_fileSystem.onOpen = fileOpen;
	m_fileSystem.onClose = fileClose;
	m_fileSystem.onRead = fileRead;
	m_fileSystem.onSeek = fileSeek;
	m_fileSystem.onTell = fileTell;
	m_fileSystem.onInfo = fileInfo;

	pthread_mutex_init(&m_lock, nullptr);
	pthread_cond_init(&m_wake, nullptr);
}

//-----------------------------------------------------------------------------
VitaAudioLoader::~VitaAudioLoader()
{
	stop();
	freeEffects();

	pthread_cond_destroy(&m_wake);
	pthread_mutex_destroy(&m_lock);
}

//-----------------------------------------------------------------------------
bool VitaAudioLoader::start(ma_engine *engine)
{
	if (m_running)
		return true;

	m_engine = engine;
	m_stopping = false;

	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, LOADER_THREAD_STACK);
	m_running = pthread_create(&m_thread, &attributes, threadEntry, this) == 0;
	pthread_attr_destroy(&attributes);
	return m_running;
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::stop()
{
	if (!m_running)
		return;

	pthread_mutex_lock(&m_lock);
	m_stopping = true;
	pthread_cond_signal(&m_wake);
	pthread_mutex_unlock(&m_lock);

	pthread_join(m_thread, nullptr);
	m_running = false;
}

//-----------------------------------------------------------------------------
void *VitaAudioLoader::threadEntry(void *argument)
{
	static_cast<VitaAudioLoader *>(argument)->run();
	return nullptr;
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::run()
{
	pthread_mutex_lock(&m_lock);

	for (;;)
	{
		if (!m_jobs.empty())
		{
			const Job job = m_jobs.front();
			m_jobs.pop_front();
			const bool load = !m_stopping;
			pthread_mutex_unlock(&m_lock);

			if (job.effect != nullptr)
			{
				if (load)
					decodeEffect(job.effect);
				else
					__atomic_store_n(&job.effect->state, static_cast<int>(VitaAudioEffect::FAILED), __ATOMIC_SEQ_CST);
			}
			else
			{
				openStreamNow(job.stream, load);
			}

			pthread_mutex_lock(&m_lock);
		}
		else if (!m_discards.empty())
		{
			ma_sound *sound = m_discards.front();
			m_discards.pop_front();
			pthread_mutex_unlock(&m_lock);

			ma_sound_uninit(sound);
			free(sound);

			pthread_mutex_lock(&m_lock);
		}
		else if (m_stopping)
		{
			break;
		}
		else
		{
			pthread_cond_wait(&m_wake, &m_lock);
		}
	}

	pthread_mutex_unlock(&m_lock);
}

//-----------------------------------------------------------------------------
/// Reads a sound effect from the card in one piece and decodes all of it.
void VitaAudioLoader::decodeEffect(VitaAudioEffect *effect)
{
	int state = VitaAudioEffect::FAILED;

	FilePlace place;
	if (findFile(effect->location.c_str(), place) == MA_SUCCESS)
	{
		const size_t fileSize = static_cast<size_t>(place.size);
		void *file = malloc(fileSize > 0 ? fileSize : 1);
		if (file != nullptr && readAt(place, 0, file, fileSize) == fileSize)
		{
			// Sixteen bits at the rate and with the channels of the file: half the memory of
			// what the mixer works with, and the conversion while mixing is cheap.
			ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
			ma_decoder decoder;
			if (ma_decoder_init_memory(file, fileSize, &config, &decoder) == MA_SUCCESS)
			{
				const ma_uint32 channels = decoder.outputChannels;
				ma_uint64 length = 0;
				if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS)
					length = 0;

				// A decoder that cannot tell the length gets room that grows.
				ma_uint64 capacity = length > 0 ? length : 32768;
				ma_int16 *samples = static_cast<ma_int16 *>(malloc(static_cast<size_t>(capacity) * channels * sizeof(ma_int16)));
				ma_uint64 frames = 0;

				while (samples != nullptr)
				{
					if (frames == capacity)
					{
						if (length > 0)
							break;

						capacity *= 2;
						ma_int16 *larger = static_cast<ma_int16 *>(realloc(samples, static_cast<size_t>(capacity) * channels * sizeof(ma_int16)));
						if (larger == nullptr)
						{
							free(samples);
							samples = nullptr;
							break;
						}
						samples = larger;
					}

					ma_uint64 got = 0;
					const ma_result result = ma_decoder_read_pcm_frames(&decoder, samples + frames * channels, capacity - frames, &got);
					frames += got;
					if (result != MA_SUCCESS || got == 0)
						break;
				}

				if (samples != nullptr && frames > 0 && channels > 0)
				{
					effect->samples = samples;
					effect->frames = frames;
					effect->channels = channels;
					effect->sampleRate = decoder.outputSampleRate;
					effect->bytes = static_cast<size_t>(frames) * channels * sizeof(ma_int16);
					state = VitaAudioEffect::READY;
				}
				else
				{
					free(samples);
				}

				ma_decoder_uninit(&decoder);
			}
		}

		free(file);
		leaveFile(place);
	}

	if (state == VitaAudioEffect::FAILED)
		fprintf(stderr, "[audio] cannot decode %s\n", getName(effect->location));

	// Last: the game's thread uses the effect as soon as it sees this.
	__atomic_store_n(&effect->state, state, __ATOMIC_SEQ_CST);
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::openStreamNow(VitaAudioStream *stream, bool load)
{
	ma_result result = MA_ERROR;
	if (load)
	{
		result = ma_sound_init_from_file(m_engine, stream->location.c_str(), stream->flags, stream->group, nullptr, stream->sound);
		if (result != MA_SUCCESS)
			fprintf(stderr, "[audio] cannot play %s: %s\n", getName(stream->location), ma_result_description(result));
	}

	pthread_mutex_lock(&m_lock);
	const bool abandoned = stream->abandoned;
	if (!abandoned)
	{
		__atomic_store_n(&stream->state, static_cast<int>(result == MA_SUCCESS ? VitaAudioStream::READY : VitaAudioStream::FAILED),
			__ATOMIC_SEQ_CST);
	}
	pthread_mutex_unlock(&m_lock);

	// Whoever asked has gone: the request is this thread's to end.
	if (abandoned)
	{
		if (result == MA_SUCCESS)
			ma_sound_uninit(stream->sound);
		free(stream->sound);
		delete stream;
	}
}

//-----------------------------------------------------------------------------
VitaAudioEffect *VitaAudioLoader::getEffect(const std::string &location)
{
	const std::map<std::string, VitaAudioEffect *>::iterator known = m_effects.find(location);
	if (known != m_effects.end())
	{
		known->second->lastUsed = ++m_useCount;
		return known->second;
	}

	VitaAudioEffect *effect = new VitaAudioEffect;
	effect->location = location;
	effect->state = m_running ? VitaAudioEffect::LOADING : VitaAudioEffect::FAILED;
	effect->samples = nullptr;
	effect->frames = 0;
	effect->channels = 0;
	effect->sampleRate = 0;
	effect->bytes = 0;
	effect->lastUsed = ++m_useCount;
	m_effects[location] = effect;

	if (m_running)
	{
		Job job = { effect, nullptr };
		pthread_mutex_lock(&m_lock);
		m_jobs.push_back(job);
		pthread_cond_signal(&m_wake);
		pthread_mutex_unlock(&m_lock);
	}

	return effect;
}

//-----------------------------------------------------------------------------
int VitaAudioLoader::getState(const VitaAudioEffect *effect)
{
	return __atomic_load_n(&effect->state, __ATOMIC_SEQ_CST);
}

//-----------------------------------------------------------------------------
size_t VitaAudioLoader::getEffectBytes() const
{
	size_t total = 0;
	for (std::map<std::string, VitaAudioEffect *>::const_iterator it = m_effects.begin(); it != m_effects.end(); ++it)
	{
		if (getState(it->second) == VitaAudioEffect::READY)
			total += it->second->bytes;
	}
	return total;
}

//-----------------------------------------------------------------------------
namespace
{

bool usedEarlier(const VitaAudioEffect *a, const VitaAudioEffect *b)
{
	return a->lastUsed < b->lastUsed;
}

} // namespace

//-----------------------------------------------------------------------------
void VitaAudioLoader::trimEffects(size_t budgetBytes, const std::vector<const VitaAudioEffect *> &inUse)
{
	size_t total = getEffectBytes();
	if (total <= budgetBytes)
		return;

	const std::set<const VitaAudioEffect *> playing(inUse.begin(), inUse.end());
	std::vector<VitaAudioEffect *> idle;
	for (std::map<std::string, VitaAudioEffect *>::const_iterator it = m_effects.begin(); it != m_effects.end(); ++it)
	{
		if (getState(it->second) == VitaAudioEffect::READY && playing.find(it->second) == playing.end())
			idle.push_back(it->second);
	}
	std::sort(idle.begin(), idle.end(), usedEarlier);

	for (size_t i = 0; i < idle.size() && total > budgetBytes; ++i)
	{
		VitaAudioEffect *effect = idle[i];
		total -= effect->bytes;
		m_effects.erase(effect->location);
		free(effect->samples);
		delete effect;
	}
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::freeEffects()
{
	for (std::map<std::string, VitaAudioEffect *>::iterator it = m_effects.begin(); it != m_effects.end(); ++it)
	{
		free(it->second->samples);
		delete it->second;
	}
	m_effects.clear();
}

//-----------------------------------------------------------------------------
VitaAudioStream *VitaAudioLoader::openStream(const std::string &location, ma_uint32 flags, ma_sound_group *group, ma_sound *sound)
{
	VitaAudioStream *stream = new VitaAudioStream;
	stream->location = location;
	stream->flags = flags;
	stream->group = group;
	stream->sound = sound;
	stream->state = m_running ? VitaAudioStream::LOADING : VitaAudioStream::FAILED;
	stream->abandoned = false;

	if (m_running)
	{
		Job job = { nullptr, stream };
		pthread_mutex_lock(&m_lock);
		m_jobs.push_back(job);
		pthread_cond_signal(&m_wake);
		pthread_mutex_unlock(&m_lock);
	}

	return stream;
}

//-----------------------------------------------------------------------------
int VitaAudioLoader::getState(const VitaAudioStream *stream)
{
	return __atomic_load_n(&stream->state, __ATOMIC_SEQ_CST);
}

//-----------------------------------------------------------------------------
ma_sound *VitaAudioLoader::finishStream(VitaAudioStream *stream)
{
	ma_sound *sound = stream->sound;
	if (getState(stream) != VitaAudioStream::READY)
	{
		free(sound);
		sound = nullptr;
	}

	delete stream;
	return sound;
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::abandonStream(VitaAudioStream *stream)
{
	pthread_mutex_lock(&m_lock);
	const bool loading = getState(stream) == VitaAudioStream::LOADING;
	if (loading)
		stream->abandoned = true;
	pthread_mutex_unlock(&m_lock);

	// The thread ends a request that it is still working on, see openStreamNow().
	if (loading)
		return;

	ma_sound *sound = finishStream(stream);
	if (sound != nullptr)
		discard(sound);
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::discard(ma_sound *sound)
{
	if (!m_running)
	{
		ma_sound_uninit(sound);
		free(sound);
		return;
	}

	pthread_mutex_lock(&m_lock);
	m_discards.push_back(sound);
	pthread_cond_signal(&m_wake);
	pthread_mutex_unlock(&m_lock);
}

//-----------------------------------------------------------------------------
void VitaAudioLoader::closeArchives()
{
	closeOpenArchives();
}
