// Runs the loading side of the Vita's sound (Device/VitaAudioLoader) and miniaudio on a PC
// the way VitaAudioManager uses them, against the game's archives, and looks for what the
// console has no way of showing: writes to freed memory, sounds that never arrive, a
// thread that hangs.
//
// Every block miniaudio frees is filled with a pattern and kept for a while. A pattern
// that has changed when the block is finally let go of means somebody wrote to the block
// after the free.
//
//   python make_list.py <game directory> files.txt
//   g++ -O2 -std=c++17 -I <directory of miniaudio.h> -I ../../Device -o stresstest stresstest.cpp -lpthread
//   stresstest files.txt <rounds> [job threads of miniaudio, default 1]
//
// Built with MinGW-w64 on Windows; elsewhere the functions under "What the console has"
// need their POSIX names back. It should be run again before the pinned miniaudio release
// or the way sounds are created and let go of changes.
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_NULL
#define MA_NO_RUNTIME_LINKING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_FLAC
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <fcntl.h>
#include <io.h>
#include <pthread.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// What the console has and Windows has not.
static pthread_mutex_t s_preadMutex = PTHREAD_MUTEX_INITIALIZER;
static long long s_reads = 0, s_bytesRead = 0;

static ssize_t host_pread(int descriptor, void *buffer, size_t size, long long offset)
{
	pthread_mutex_lock(&s_preadMutex);
	ssize_t got = -1;
	if (_lseeki64(descriptor, offset, SEEK_SET) == offset)
		got = _read(descriptor, buffer, static_cast<unsigned int>(size));
	++s_reads;
	if (got > 0)
		s_bytesRead += got;
	pthread_mutex_unlock(&s_preadMutex);
	return got;
}

static int host_open(const char *path, int flags)
{
	return _open(path, flags | _O_BINARY);
}

#define pread(d, b, s, o) host_pread(d, b, s, o)
#define open(p, f) host_open(p, f)
#define lseek(d, o, w) _lseeki64(d, o, w)

#include "VitaAudioLoader.cpp"

#undef open
#undef pread
#undef lseek

// THE ALLOCATOR ----------------------------------------------------------------
struct Header
{
	size_t size;
	size_t magic;
};

static const size_t LIVE = 0x4C495645u;
static const size_t DEAD = 0x44454144u;
static const unsigned char DEAD_FILL = 0xDD;
static const size_t QUARANTINE_BYTES = 256u * 1024u * 1024u;

static pthread_mutex_t s_heapMutex = PTHREAD_MUTEX_INITIALIZER;
static std::deque<Header *> s_quarantine;
static size_t s_quarantined = 0;
static long s_writesAfterFree = 0, s_badFrees = 0, s_frees = 0;

static void checkAndRelease(Header *header)
{
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(header + 1);
	size_t changed = 0, first = 0;
	for (size_t i = 0; i < header->size; ++i)
	{
		if (bytes[i] != DEAD_FILL)
		{
			if (changed == 0)
				first = i;
			++changed;
		}
	}

	if (changed != 0 || header->magic != DEAD)
	{
		++s_writesAfterFree;
		if (s_writesAfterFree <= 12)
		{
			fprintf(stderr, "WRITE AFTER FREE: block of %u bytes, %u bytes changed, first at offset %u\n",
				static_cast<unsigned int>(header->size), static_cast<unsigned int>(changed), static_cast<unsigned int>(first));
		}
	}
	free(header);
}

static void *testMalloc(size_t size, void *)
{
	Header *header = static_cast<Header *>(malloc(sizeof(Header) + size));
	if (header == nullptr)
		return nullptr;
	header->size = size;
	header->magic = LIVE;
	return header + 1;
}

static void testFree(void *pointer, void *)
{
	if (pointer == nullptr)
		return;

	Header *header = static_cast<Header *>(pointer) - 1;
	pthread_mutex_lock(&s_heapMutex);
	++s_frees;
	if (header->magic != LIVE)
	{
		++s_badFrees;
		pthread_mutex_unlock(&s_heapMutex);
		return;
	}

	header->magic = DEAD;
	memset(header + 1, DEAD_FILL, header->size);
	s_quarantine.push_back(header);
	s_quarantined += header->size;
	while (s_quarantined > QUARANTINE_BYTES && s_quarantine.size() > 1)
	{
		Header *oldest = s_quarantine.front();
		s_quarantine.pop_front();
		s_quarantined -= oldest->size;
		checkAndRelease(oldest);
	}
	pthread_mutex_unlock(&s_heapMutex);
}

static void *testRealloc(void *pointer, size_t size, void *user)
{
	if (pointer == nullptr)
		return testMalloc(size, user);

	Header *header = static_cast<Header *>(pointer) - 1;
	void *replacement = testMalloc(size, user);
	if (replacement != nullptr)
	{
		memcpy(replacement, pointer, header->size < size ? header->size : size);
		testFree(pointer, user);
	}
	return replacement;
}

static void emptyQuarantine()
{
	pthread_mutex_lock(&s_heapMutex);
	while (!s_quarantine.empty())
	{
		Header *oldest = s_quarantine.front();
		s_quarantine.pop_front();
		checkAndRelease(oldest);
	}
	s_quarantined = 0;
	pthread_mutex_unlock(&s_heapMutex);
}

// THE TEST ------------------------------------------------------------------------
static ma_resource_manager s_resources;
static ma_engine s_engine;
static VitaAudioLoader s_loader;
static volatile bool s_stopMixing = false;
static double s_loudest = 0.0;

static ma_thread_result MA_THREADCALL mixerThread(void *)
{
	static float mixed[1024 * 2];
	while (!s_stopMixing)
	{
		ma_uint64 frames = 0;
		ma_engine_read_pcm_frames(&s_engine, mixed, 1024, &frames);
		for (ma_uint64 i = 0; i < frames * 2; ++i)
		{
			if (fabs(mixed[i]) > s_loudest)
				s_loudest = fabs(mixed[i]);
		}
		ma_sleep(2);
	}
	return (ma_thread_result)0;
}

static std::vector<std::string> readList(const char *path)
{
	std::vector<std::string> lines;
	FILE *f = fopen(path, "rb");
	if (f == nullptr)
		return lines;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (n > 0)
			lines.push_back(line);
	}
	fclose(f);
	return lines;
}

/// What the manager allocates for a sound: the sound first, what it plays from after it.
struct TestSound
{
	ma_sound sound;
	ma_audio_buffer_ref samples;
	VitaAudioEffect *effect;
	VitaAudioStream *stream;
	bool streamed;
};

static const ma_uint32 PLAIN = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
static const ma_uint32 STREAM = PLAIN | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_UNKNOWN_LENGTH;

static long s_effectSounds = 0, s_effectsNotReady = 0, s_effectsFailed = 0;
static long s_streamsOpened = 0, s_streamsPlayed = 0, s_streamsAbandoned = 0, s_streamsFailed = 0;

/// A sound of an effect that is in memory, as the manager makes it on the game's thread.
static TestSound *playEffect(const std::string &location)
{
	VitaAudioEffect *effect = s_loader.getEffect(location);
	const int state = VitaAudioLoader::getState(effect);
	if (state != VitaAudioEffect::READY)
	{
		++(state == VitaAudioEffect::FAILED ? s_effectsFailed : s_effectsNotReady);
		return nullptr;
	}

	TestSound *made = static_cast<TestSound *>(malloc(sizeof(TestSound)));
	made->effect = effect;
	made->stream = nullptr;
	made->streamed = false;
	ma_audio_buffer_ref_init(ma_format_s16, effect->channels, effect->samples, effect->frames, &made->samples);
	made->samples.sampleRate = effect->sampleRate;
	if (ma_sound_init_from_data_source(&s_engine, &made->samples, PLAIN, nullptr, &made->sound) != MA_SUCCESS)
	{
		free(made);
		return nullptr;
	}

	ma_sound_start(&made->sound);
	++s_effectSounds;
	return made;
}

static TestSound *openStream(const std::string &location)
{
	TestSound *made = static_cast<TestSound *>(malloc(sizeof(TestSound)));
	made->effect = nullptr;
	made->streamed = true;
	made->stream = s_loader.openStream(location, STREAM, nullptr, &made->sound);
	++s_streamsOpened;
	return made;
}

static void drop(TestSound *sound)
{
	if (sound == nullptr)
		return;

	if (!sound->streamed)
	{
		ma_sound_uninit(&sound->sound);
		free(sound);
	}
	else if (sound->stream != nullptr)
	{
		++s_streamsAbandoned;
		s_loader.abandonStream(sound->stream);
	}
	else
	{
		ma_sound_stop(&sound->sound);
		s_loader.discard(&sound->sound);
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 3)
		return 2;

	const std::vector<std::string> list = readList(argv[1]);
	const int rounds = atoi(argv[2]);
	const int jobThreads = argc > 3 ? atoi(argv[3]) : 1;

	std::vector<std::string> effects, streams;
	for (size_t i = 0; i < list.size(); ++i)
		(list[i].find(".mp3") != std::string::npos || list[i].find("Speech") != std::string::npos ? streams : effects).push_back(list[i]);
	if (effects.empty() || streams.empty())
	{
		printf("the list needs sound effects and music or speech\n");
		return 2;
	}

	ma_allocation_callbacks allocation;
	allocation.pUserData = nullptr;
	allocation.onMalloc = testMalloc;
	allocation.onRealloc = testRealloc;
	allocation.onFree = testFree;

	// As VitaAudioManager::openDevice() sets them up.
	ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
	resourceConfig.pVFS = s_loader.getFileSystem();
	resourceConfig.decodedFormat = ma_format_s16;
	resourceConfig.decodedChannels = 0;
	resourceConfig.decodedSampleRate = 0;
	resourceConfig.jobThreadCount = jobThreads;
	resourceConfig.jobThreadStackSize = 512 * 1024;
	resourceConfig.allocationCallbacks = allocation;
	if (ma_resource_manager_init(&resourceConfig, &s_resources) != MA_SUCCESS)
		return 3;

	ma_engine_config engineConfig = ma_engine_config_init();
	engineConfig.noDevice = MA_TRUE;
	engineConfig.channels = 2;
	engineConfig.sampleRate = 48000;
	engineConfig.periodSizeInFrames = 256;
	engineConfig.pResourceManager = &s_resources;
	engineConfig.allocationCallbacks = allocation;
	if (ma_engine_init(&engineConfig, &s_engine) != MA_SUCCESS)
		return 4;

	if (!s_loader.start(&s_engine))
		return 5;

	ma_thread thread;
	ma_thread_create(&thread, ma_thread_priority_normal, 0, mixerThread, nullptr, nullptr);

	std::vector<TestSound *> alive;
	std::vector<TestSound *> music;
	unsigned int seed = 12345;
	const size_t budget = 6u * 1024u * 1024u;	// small, so that effects are let go of all the time
	size_t mostBytes = 0;

	for (int round = 0; round < rounds; ++round)
	{
		seed = seed * 1664525u + 1013904223u;

		// Effects: most from a few files, as in a battle, some from anywhere.
		const std::string &location = effects[(seed >> 8) % (round % 4 == 0 ? effects.size() : 60)];
		TestSound *made = playEffect(location);
		if (made != nullptr)
			alive.push_back(made);

		seed = seed * 1664525u + 1013904223u;
		while (alive.size() > (seed >> 8) % 24)
		{
			const size_t which = (seed >> 16) % alive.size();
			drop(alive[which]);
			alive.erase(alive.begin() + which);
		}

		// Music and speech come and go more slowly, and some are given up before they are open.
		if (round % 16 == 0)
			music.push_back(openStream(streams[(seed >> 12) % streams.size()]));
		for (size_t i = 0; i < music.size(); ++i)
		{
			TestSound *stream = music[i];
			if (stream->stream != nullptr && VitaAudioLoader::getState(stream->stream) != VitaAudioStream::LOADING)
			{
				const bool ready = s_loader.finishStream(stream->stream) != nullptr;
				stream->stream = nullptr;
				if (ready)
				{
					ma_sound_start(&stream->sound);
					++s_streamsPlayed;
				}
				else
				{
					++s_streamsFailed;
					music.erase(music.begin() + i);	// the memory went with the request
					--i;
				}
			}
		}
		while (music.size() > 1 + (seed >> 20) % 3)
		{
			drop(music.front());
			music.erase(music.begin());
		}

		// The cache of effects, as the manager trims it.
		if (round % 8 == 0)
		{
			std::vector<const VitaAudioEffect *> inUse;
			for (size_t i = 0; i < alive.size(); ++i)
				inUse.push_back(alive[i]->effect);
			if (s_loader.getEffectBytes() > mostBytes)
				mostBytes = s_loader.getEffectBytes();
			s_loader.trimEffects(budget, inUse);
		}

		if ((seed & 0x30) == 0)
			ma_sleep(1);
	}

	for (size_t i = 0; i < alive.size(); ++i)
		drop(alive[i]);
	for (size_t i = 0; i < music.size(); ++i)
		drop(music[i]);

	// The order of VitaAudioManager::closeDevice().
	s_stopMixing = true;
	ma_thread_wait(&thread);
	s_loader.stop();
	s_loader.freeEffects();
	ma_engine_uninit(&s_engine);
	ma_resource_manager_uninit(&s_resources);
	VitaAudioLoader::closeArchives();
	emptyQuarantine();

	printf("%d rounds: effects %ld played, %ld not loaded yet, %ld failed, at most %.1f MB decoded; "
		"streams %ld asked for, %ld played, %ld given up while opening, %ld failed; loudest sample %.2f; "
		"%lld reads from disk, %.0f KB each on average\n",
		rounds, s_effectSounds, s_effectsNotReady, s_effectsFailed, mostBytes / 1048576.0,
		s_streamsOpened, s_streamsPlayed, s_streamsAbandoned, s_streamsFailed, s_loudest,
		s_reads, s_reads > 0 ? s_bytesRead / 1024.0 / s_reads : 0.0);
	printf("miniaudio freed %ld blocks: WRITES AFTER FREE %ld, bad frees %ld\n", s_frees, s_writesAfterFree, s_badFrees);

	const bool sane = s_effectSounds > rounds / 4 && s_effectsFailed == 0 && s_streamsPlayed > 0 && s_streamsFailed == 0 && s_loudest > 0.01;
	if (!sane)
		printf("THE NUMBERS ABOVE ARE NOT WHAT A WORKING LOADER GIVES\n");
	return (s_writesAfterFree != 0 || s_badFrees != 0 || !sane) ? 1 : 0;
}
