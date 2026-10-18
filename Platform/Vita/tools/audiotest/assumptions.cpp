// Checks on a PC what VitaAudioManager assumes about miniaudio and about VitaAudioLoader:
// an effect in memory plays at its own speed and tells how far it is, a stand-in takes
// what a sound is told and hands it on, a stopped sound goes on where it was.
//
//   g++ -O2 -std=c++17 -I <directory of miniaudio.h> -I ../../Device -o assumptions assumptions.cpp -lpthread
//   assumptions "<one line of the list make_list.py writes, a sound effect>"
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
#include <string>

static pthread_mutex_t s_preadMutex = PTHREAD_MUTEX_INITIALIZER;
static ssize_t host_pread(int descriptor, void *buffer, size_t size, long long offset)
{
	pthread_mutex_lock(&s_preadMutex);
	ssize_t got = -1;
	if (_lseeki64(descriptor, offset, SEEK_SET) == offset)
		got = _read(descriptor, buffer, static_cast<unsigned int>(size));
	pthread_mutex_unlock(&s_preadMutex);
	return got;
}
static int host_open(const char *path, int flags) { return _open(path, flags | _O_BINARY); }
#define pread(d, b, s, o) host_pread(d, b, s, o)
#define open(p, f) host_open(p, f)
#define lseek(d, o, w) _lseeki64(d, o, w)
#include "VitaAudioLoader.cpp"
#undef open
#undef pread
#undef lseek

static ma_resource_manager resources;
static ma_engine engine;
static VitaAudioLoader loader;
static const ma_uint32 PLAIN = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;

struct TestSound
{
	ma_sound sound;
	ma_audio_buffer_ref samples;
};

/// Mixes one block. Returns its loudness and where in it the first sample was heard.
static double pump(long long *firstLoud = nullptr)
{
	static float mixed[1024 * 2];
	const long long begin = static_cast<long long>(ma_engine_get_time_in_pcm_frames(&engine));
	ma_uint64 frames = 0;
	ma_engine_read_pcm_frames(&engine, mixed, 1024, &frames);
	double sum = 0.0;
	for (ma_uint64 i = 0; i < frames; ++i)
	{
		const double level = fabs(mixed[i * 2]) + fabs(mixed[i * 2 + 1]);
		if (level > 0.0002 && firstLoud != nullptr && *firstLoud < 0)
			*firstLoud = begin + static_cast<long long>(i);
		sum += level;
	}
	return sum / 2048.0;
}

static TestSound *makeEffectSound(VitaAudioEffect *effect)
{
	TestSound *made = static_cast<TestSound *>(malloc(sizeof(TestSound)));
	ma_audio_buffer_ref_init(ma_format_s16, effect->channels, effect->samples, effect->frames, &made->samples);
	made->samples.sampleRate = effect->sampleRate;
	if (ma_sound_init_from_data_source(&engine, &made->samples, PLAIN, nullptr, &made->sound) != MA_SUCCESS)
		return nullptr;
	return made;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 2)
		return 2;

	ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
	resourceConfig.pVFS = loader.getFileSystem();
	resourceConfig.decodedFormat = ma_format_s16;
	resourceConfig.jobThreadCount = 1;
	ma_resource_manager_init(&resourceConfig, &resources);
	ma_engine_config engineConfig = ma_engine_config_init();
	engineConfig.noDevice = MA_TRUE;
	engineConfig.channels = 2;
	engineConfig.sampleRate = 48000;
	engineConfig.periodSizeInFrames = 256;
	engineConfig.pResourceManager = &resources;
	if (ma_engine_init(&engineConfig, &engine) != MA_SUCCESS || !loader.start(&engine))
		return 3;

	VitaAudioEffect *effect = loader.getEffect(argv[1]);
	for (int i = 0; i < 500 && VitaAudioLoader::getState(effect) == VitaAudioEffect::LOADING; ++i)
		ma_sleep(10);
	if (VitaAudioLoader::getState(effect) != VitaAudioEffect::READY)
	{
		printf("the effect did not load\n");
		return 4;
	}
	const double seconds = static_cast<double>(effect->frames) / effect->sampleRate;
	printf("effect: %u frames, %u channel(s), %u Hz, %.3f s, %u bytes decoded\n", static_cast<unsigned int>(effect->frames),
		effect->channels, effect->sampleRate, seconds, static_cast<unsigned int>(effect->bytes));
	bool good = true;

	// 1. At its own speed, and it tells how far it is.
	{
		TestSound *sound = makeEffectSound(effect);
		ma_sound_start(&sound->sound);
		const long long begin = static_cast<long long>(ma_engine_get_time_in_pcm_frames(&engine));
		double loudest = 0.0;
		ma_uint64 cursorMid = 0;
		int blocks = 0;
		while (!ma_sound_at_end(&sound->sound) && blocks < 2000)
		{
			const double level = pump();
			if (level > loudest)
				loudest = level;
			if (++blocks == 10)
				ma_sound_get_cursor_in_pcm_frames(&sound->sound, &cursorMid);
		}
		const double played = (static_cast<long long>(ma_engine_get_time_in_pcm_frames(&engine)) - begin) / 48000.0;
		const double expectedCursor = 10 * 1024.0 * effect->sampleRate / 48000.0;
		printf("1. played for %.3f s (file %.3f s), loudest block %.3f, cursor after 10 blocks %u (expected about %.0f)\n",
			played, seconds, loudest, static_cast<unsigned int>(cursorMid), expectedCursor);
		good = good && fabs(played - seconds) < 0.03 && loudest > 0.001 && fabs(cursorMid - expectedCursor) < 16;
		ma_sound_uninit(&sound->sound);
		free(sound);
	}

	// 2. A stand-in hands on what it was told: the volume and the time to start at.
	{
		ma_sound *standIn = static_cast<ma_sound *>(malloc(sizeof(TestSound)));
		ma_sound_config config = ma_sound_config_init_2(&engine);
		config.flags = PLAIN;
		const ma_result made = ma_sound_init_ex(&engine, &config, standIn);
		const ma_uint64 startTime = (ma_engine_get_time_in_pcm_frames(&engine) + 5000) / 256 * 256;
		ma_sound_set_volume(standIn, 0.25f);
		ma_sound_set_start_time_in_pcm_frames(standIn, startTime);
		ma_sound_start(standIn);
		const double quiet = pump() + pump();
		printf("2. stand-in: made %s, at end %d, state %s, loudness %.4f\n", ma_result_description(made),
			static_cast<int>(ma_sound_at_end(standIn)),
			ma_node_get_state(standIn) == ma_node_state_started ? "started" : "stopped", quiet);
		good = good && made == MA_SUCCESS && quiet == 0.0 && ma_node_get_state(standIn) == ma_node_state_started;

		TestSound *sound = makeEffectSound(effect);
		ma_sound_set_volume(&sound->sound, ma_sound_get_volume(standIn));
		if (ma_node_get_state(standIn) == ma_node_state_started)
		{
			ma_sound_set_start_time_in_pcm_frames(&sound->sound, ma_node_get_state_time(standIn, ma_node_state_started));
			ma_sound_start(&sound->sound);
		}
		ma_sound_uninit(standIn);
		free(standIn);

		long long firstLoud = -1;
		double loudest = 0.0;
		for (int i = 0; i < 40; ++i)
		{
			const double level = pump(&firstLoud);
			if (level > loudest)
				loudest = level;
		}
		printf("   the sound in its place: first heard at %lld, set to start at %u, volume %.2f, loudest block %.3f\n", firstLoud,
			static_cast<unsigned int>(startTime), ma_sound_get_volume(&sound->sound), loudest);
		good = good && firstLoud >= static_cast<long long>(startTime) && firstLoud < static_cast<long long>(startTime) + 2400;
		ma_sound_uninit(&sound->sound);
		free(sound);
	}

	// 3. Stopped and started again, as pauseAudio() and resumeAudio() do it.
	{
		TestSound *sound = makeEffectSound(effect);
		ma_sound_start(&sound->sound);
		for (int i = 0; i < 10; ++i)
			pump();
		ma_sound_stop(&sound->sound);
		ma_uint64 before = 0, after = 0;
		pump();
		ma_sound_get_cursor_in_pcm_frames(&sound->sound, &before);
		double paused = 0.0;
		for (int i = 0; i < 20; ++i)
			paused += pump();
		ma_sound_get_cursor_in_pcm_frames(&sound->sound, &after);
		const bool heldAtEnd = ma_sound_at_end(&sound->sound) != MA_FALSE;
		ma_sound_start(&sound->sound);
		int blocks = 0;
		while (!ma_sound_at_end(&sound->sound) && blocks < 2000)
		{
			pump();
			++blocks;
		}
		const double played = (10 + blocks) * 1024 / 48000.0;
		printf("3. paused: loudness %.4f, cursor %u before and %u after, at end %d; played %.3f s in all (file %.3f s)\n", paused,
			static_cast<unsigned int>(before), static_cast<unsigned int>(after), static_cast<int>(heldAtEnd), played, seconds);
		good = good && paused == 0.0 && before == after && !heldAtEnd && fabs(played - seconds) < 0.06;
		ma_sound_uninit(&sound->sound);
		free(sound);
	}

	loader.stop();
	loader.freeEffects();
	ma_engine_uninit(&engine);
	ma_resource_manager_uninit(&resources);
	VitaAudioLoader::closeArchives();
	printf(good ? "all as the manager assumes\n" : "NOT AS THE MANAGER ASSUMES\n");
	return good ? 0 : 1;
}
