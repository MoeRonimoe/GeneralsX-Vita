// Runs the stream between the game's thread and the render thread (d3d8/VitaD3D8RenderThread)
// on a PC, with functions that check what they are given in place of vitaGL: every call
// arrives once and in order, data kept in the stream is still what it was when the call that
// uses it is made, a call that is waited for sees everything written before it, and the
// writer is never more than a frame ahead.
//
//   g++ -O2 -std=c++17 -I ../d3d8 -o renderthread_test renderthread_test.cpp -lpthread
//   renderthread_test [frames, default 300]
#include "VitaD3D8RenderThread.cpp"

#include <thread>
#include <vector>

using namespace VitaD3D8;

static std::thread::id s_gameThread;
static unsigned int s_expectedSequence = 0;
static unsigned int s_orderErrors = 0, s_dataErrors = 0, s_threadErrors = 0;
static unsigned int s_draws = 0, s_states = 0, s_frames = 0;
static std::atomic<unsigned int> s_framesShownByRenderer(0);
static float s_lastMatrix = 0.0f;

static void CheckThread()
{
	if (std::this_thread::get_id() == s_gameThread)
		++s_threadErrors;
}

static void FakeState(unsigned int sequence, int value, float other)
{
	CheckThread();
	if (sequence != s_expectedSequence++)
		++s_orderErrors;
	++s_states;
	(void)value;
	(void)other;
}

static unsigned int Checksum(const unsigned char *data, unsigned int size)
{
	unsigned int sum = 2166136261u;
	for (unsigned int i = 0; i < size; ++i)
		sum = (sum ^ data[i]) * 16777619u;
	return sum;
}

static void FakeDraw(unsigned int sequence, const void *vertices, unsigned int size, unsigned int checksum)
{
	CheckThread();
	if (sequence != s_expectedSequence++)
		++s_orderErrors;
	if (Checksum(static_cast<const unsigned char *>(vertices), size) != checksum)
		++s_dataErrors;
	++s_draws;
}

static void FakeSwap(unsigned int sequence)
{
	CheckThread();
	if (sequence != s_expectedSequence++)
		++s_orderErrors;
	++s_frames;
	s_framesShownByRenderer.fetch_add(1);

	// A frame takes the renderer a while now and then.
	if ((s_frames % 7) == 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
}

int main(int argc, char **argv)
{
	const unsigned int frames = argc > 1 ? static_cast<unsigned int>(atoi(argv[1])) : 300;
	s_gameThread = std::this_thread::get_id();

#ifdef _WIN32
	_putenv_s("VITA_RENDER_THREAD", "1");
#else
	setenv("VITA_RENDER_THREAD", "1", 1);
#endif
	if (!RenderThread::Start() || !RenderThread::g_queueing)
	{
		printf("the render thread did not start\n");
		return 2;
	}

	std::vector<unsigned char> vertices(600 * 1024);
	unsigned int sequence = 0, seed = 99, syncErrors = 0, aheadErrors = 0, mostAhead = 0, written = 0, synced = 0;
	const uint64_t began = RenderThread::Now();

	for (unsigned int frame = 0; frame < frames; ++frame)
	{
		for (unsigned int draw = 0; draw < 400; ++draw)
		{
			for (int state = 0; state < 6; ++state)
				RenderThread::Call(FakeState, sequence++, state, 0.5f);

			// Vertices from memory that is written again right away. Mostly a few
			// kilobytes, now and then more than a block of the stream holds.
			seed = seed * 1664525u + 1013904223u;
			unsigned int size = 64 + (seed >> 8) % 6000;
			if ((seed & 0x3FF) == 0)
				size = 300 * 1024 + (seed >> 12) % (250 * 1024);
			for (unsigned int i = 0; i < size; i += 61)
				vertices[i] = static_cast<unsigned char>(seed >> (i & 15));
			const unsigned int checksum = Checksum(vertices.data(), size);

			const void *kept = RenderThread::Keep(vertices.data(), size);
			RenderThread::Call(FakeDraw, sequence++, kept, size, checksum);
			RenderThread::Publish();
			++written;

			// What a copied value looks like at the other end.
			if ((draw % 97) == 0)
			{
				const float value = static_cast<float>(sequence);
				RenderThread::Defer([value] { s_lastMatrix = value; });
			}

			// A call that is waited for: it must see everything written before it, and
			// what it calls must be made at once.
			if ((seed & 0x7F) == 0)
			{
				unsigned int seen = 0, nested = 0;
				const unsigned int expected = sequence;
				RenderThread::Sync([&]
				{
					seen = s_expectedSequence;
					RenderThread::Call(FakeState, s_expectedSequence, 0, 0.0f);
					nested = s_expectedSequence;
				});
				++sequence;	// for the nested call
				if (seen != expected || nested != expected + 1 || !RenderThread::g_queueing)
					++syncErrors;
				++synced;
			}
		}

		RenderThread::Call(FakeSwap, sequence++);
		RenderThread::EndFrame();

		const unsigned int ahead = frame + 1 - s_framesShownByRenderer.load();
		if (ahead > mostAhead)
			mostAhead = ahead;
		if (ahead > 2)
			++aheadErrors;
	}

	RenderThread::Sync([] {});
	const double seconds = (RenderThread::Now() - began) / 1000000.0;

	printf("%u frames, %u draw calls written, %u waited-for calls, %.2f s (%.2f ms per frame for the writer)\n", frames, written, synced,
		seconds, seconds * 1000.0 / frames);
	printf("the renderer saw %u frames, %u draw calls, %u other calls; busy for %.2f s\n", s_frames, s_draws, s_states,
		RenderThread::TakeBusyTime() / 1000000.0);
	printf("out of order %u, wrong data %u, made on the game's thread %u, waited-for calls that saw the wrong state %u, "
		"times the writer was more than a frame ahead %u (most: %u)\n", s_orderErrors, s_dataErrors, s_threadErrors, syncErrors, aheadErrors, mostAhead);

	const bool good = s_frames == frames && s_draws == written && s_orderErrors == 0 && s_dataErrors == 0 && s_threadErrors == 0
		&& syncErrors == 0 && aheadErrors == 0 && s_expectedSequence == sequence;
	printf(good ? "all good\n" : "FAILED\n");
	return good ? 0 : 1;
}
