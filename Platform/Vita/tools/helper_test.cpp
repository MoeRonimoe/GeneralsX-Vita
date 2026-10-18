// helper_test.cpp ////////////////////////////////////////////////////////////
// Runs Platform/Vita/compat/VitaHelper.cpp on the build machine: every pass of
// every loop must be made exactly once, whoever makes it, however the two
// threads fall over each other, after however long a pause, and with the helper
// switched off and on in between.
//
// From the repository root, in one line:
//   g++ -O2 -std=c++17 -pthread -I Platform/Vita/compat/include
//       Platform/Vita/tools/helper_test.cpp Platform/Vita/compat/VitaHelper.cpp
//       -o build/hosttest/helper_test.exe
///////////////////////////////////////////////////////////////////////////////

#include "VitaHelper.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace
{

struct Loop
{
	std::vector<std::atomic<unsigned int> > made;
	std::thread::id caller;
	std::atomic<unsigned int> byOthers;
	unsigned int work;
	uint32_t generation;

	explicit Loop(unsigned int count) : made(count), byOthers(0), work(0), generation(0) {}
};

std::atomic<uint32_t> s_openGeneration(0);
std::atomic<unsigned int> s_passesOfClosedLoops(0);
volatile float s_sink = 0.0f;

//-----------------------------------------------------------------------------
void Pass(void *context, unsigned int index)
{
	Loop *loop = static_cast<Loop *>(context);

	// A pass of a loop that has returned would be the worst thing the helper could do.
	if (loop->generation != s_openGeneration.load())
		s_passesOfClosedLoops.fetch_add(1);

	float sum = 0.0f;
	for (unsigned int i = 0; i < loop->work; ++i)
		sum += static_cast<float>(i ^ index) * 0.5f;
	s_sink = sum;

	loop->made[index].fetch_add(1);
	if (std::this_thread::get_id() != loop->caller)
		loop->byOthers.fetch_add(1);
}

//-----------------------------------------------------------------------------
uint32_t Random(uint32_t &state)
{
	state = state * 1664525u + 1013904223u;
	return state >> 8;
}

//-----------------------------------------------------------------------------
uint64_t Microseconds()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

//-----------------------------------------------------------------------------
int main(int argc, char **argv)
{
	const unsigned int loops = argc > 1 ? static_cast<unsigned int>(atoi(argv[1])) : 300000;
	uint32_t seed = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 1;

	unsigned int failures = 0;
	uint64_t passes = 0;
	uint64_t byHelper = 0;
	unsigned int loopsWithHelper = 0;
	unsigned int passesWhileOff = 0;
	bool enabled = true;

	for (unsigned int number = 0; number < loops; ++number)
	{
		if (Random(seed) % 50 == 0)
		{
			enabled = !enabled;
			VitaHelper::setEnabled(enabled);
		}

		// Mostly the few short passes the game has, sometimes none or one, sometimes many,
		// and now and then long ones or a pause that lets the helper fall asleep.
		const uint32_t kind = Random(seed) % 100;
		unsigned int count = kind < 5 ? kind % 2 : kind < 90 ? 2 + Random(seed) % 60 : 100 + Random(seed) % 3000;
		Loop loop(count);
		loop.caller = std::this_thread::get_id();
		loop.work = Random(seed) % 100 < 80 ? Random(seed) % 64 : Random(seed) % (count > 100 ? 300 : 4000);
		loop.generation = number + 1;

		// A pause that is longer than the helper looks for work, so that it has to be woken,
		// and one that ends about when it stops looking.
		const uint32_t pause = Random(seed) % 200;
		if (pause < 2)
		{
			const uint64_t until = Microseconds() + (pause == 0 ? 1500 + Random(seed) % 2000 : 900 + Random(seed) % 200);
			while (Microseconds() < until)
			{
			}
		}
		else if (Random(seed) % 5000 == 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1 + Random(seed) % 12));
		}

		s_openGeneration.store(loop.generation);
		VitaHelper::forEach(count, Pass, &loop);
		s_openGeneration.store(0);

		for (unsigned int index = 0; index < count; ++index)
		{
			if (loop.made[index].load() != 1)
			{
				if (failures < 10)
					printf("loop %u: pass %u of %u was made %u times\n", number, index, count, loop.made[index].load());
				++failures;
			}
		}

		passes += count;
		byHelper += loop.byOthers.load();
		if (loop.byOthers.load() != 0)
			++loopsWithHelper;
		if (!enabled)
			passesWhileOff += loop.byOthers.load();
	}

	VitaHelper::setEnabled(true);

	if (passesWhileOff != 0)
	{
		printf("the helper made %u passes while it was switched off\n", passesWhileOff);
		++failures;
	}

	if (s_passesOfClosedLoops.load() != 0)
	{
		printf("%u passes were made of loops that had returned\n", s_passesOfClosedLoops.load());
		++failures;
	}

	unsigned int countedAll = 0;
	unsigned int countedByHelper = 0;
	VitaHelper::takePassCounts(countedAll, countedByHelper);
	if (countedAll != static_cast<unsigned int>(passes) || countedByHelper != static_cast<unsigned int>(byHelper))
	{
		printf("the helper counted %u passes and %u of its own, the test %llu and %llu\n", countedAll, countedByHelper,
			static_cast<unsigned long long>(passes), static_cast<unsigned long long>(byHelper));
		++failures;
	}

	printf("%u loops, %llu passes, %llu by the helper, which took part in %u loops\n", loops,
		static_cast<unsigned long long>(passes), static_cast<unsigned long long>(byHelper), loopsWithHelper);

	// What it is for: the same work in less time.
	{
		Loop loop(400);
		loop.caller = std::this_thread::get_id();
		loop.work = 20000;
		loop.generation = loops + 1;
		s_openGeneration.store(loop.generation);

		uint64_t alone = ~0ull;
		uint64_t together = ~0ull;
		for (int round = 0; round < 20; ++round)
		{
			uint64_t began = Microseconds();
			for (unsigned int index = 0; index < 400; ++index)
				Pass(&loop, index);
			uint64_t took = Microseconds() - began;
			if (took < alone)
				alone = took;

			began = Microseconds();
			VitaHelper::forEach(400, Pass, &loop);
			took = Microseconds() - began;
			if (took < together)
				together = took;
		}

		printf("400 long passes: %llu us alone, %llu us with the helper\n", static_cast<unsigned long long>(alone),
			static_cast<unsigned long long>(together));
	}

	if (failures != 0)
	{
		printf("FAILED\n");
		return 1;
	}

	printf("ok\n");
	return 0;
}
