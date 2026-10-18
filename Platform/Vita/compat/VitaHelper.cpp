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

// VitaHelper.cpp /////////////////////////////////////////////////////////////
// See VitaHelper.h.
//
// One word says which loop is open and which pass is to be made next: the
// number of the loop in its upper bits, the pass in its lower ones. Both
// threads take a pass by replacing the word with the next one, and that only
// works while the loop is the one they think it is. So the helper can come
// late, or come back from being put aside by the system, and finds either a
// pass of the loop it knows or nothing to do; it can never make a pass of a
// loop that has been closed. The numbers of the loops do not come round again:
// there are 2^44 of them.
//
// After its last loop the helper looks for the next one for a moment, since
// loops come a few in a row, and then waits for a semaphore. The game's thread
// signals it when it opens a loop and the helper has said that it sleeps: a
// call into the system a few times per frame. The helper does not wake itself
// to look; a thread that naps a quarter millisecond at a time is four thousand
// timers per second that the system has to serve on some core.
///////////////////////////////////////////////////////////////////////////////

#include "VitaHelper.h"

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <semaphore.h>

#include <chrono>
#endif

namespace VitaHelper
{

namespace
{

// A lock inside the word would let the game's thread wait for a helper that the system
// has put aside.
static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "the state of the helper needs a 64 bit word that is exchanged without a lock");

typedef void (*Task)(void *context, unsigned int index);

enum : uint64_t
{
	PASS_BITS = 20,
	PASS_MASK = (1ull << PASS_BITS) - 1
};

enum : uint32_t
{
	// How long the helper looks for the next loop before it sleeps. The loops it is given
	// come a few in a row, with the draw calls of the one before in between.
	LOOK_BEFORE_SLEEP_US = 1000,
	LOOKS_PER_CLOCK = 64,

	THREAD_STACK_BYTES = 256 * 1024
};

// The open loop. Written by the game's thread while no loop is open, and used by the
// helper only for a pass it has taken from the loop it read them for.
std::atomic<Task> s_task(nullptr);
std::atomic<void *> s_context(nullptr);
std::atomic<unsigned int> s_count(0);

// (loop number << PASS_BITS) | next pass. An odd loop number is an open loop.
std::atomic<uint64_t> s_state(0);
std::atomic<unsigned int> s_finished(0);

// Set by the helper before it waits for the semaphore, taken back by whoever signals it.
std::atomic<bool> s_asleep(false);

#ifdef __vita__
SceUID s_wake = -1;
#else
sem_t s_wake;
#endif

// What was done and how much of it by the helper, for whoever wants to know whether it is
// of use.
unsigned int s_allPasses = 0;
std::atomic<unsigned int> s_helperPasses(0);
LoopTimes s_loopTimes = { 0, 0, 0, 0 };

bool s_started = false;
bool s_running = false;
bool s_enabled = true;

//-----------------------------------------------------------------------------
uint64_t Now()
{
#ifdef __vita__
	return sceKernelGetProcessTimeWide();
#else
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

//-----------------------------------------------------------------------------
bool CreateWake()
{
#ifdef __vita__
	s_wake = sceKernelCreateSema("GeneralsHelper", 0, 0, 1, nullptr);
	return s_wake >= 0;
#else
	return sem_init(&s_wake, 0, 0) == 0;
#endif
}

//-----------------------------------------------------------------------------
void WaitForWake()
{
#ifdef __vita__
	sceKernelWaitSema(s_wake, 1, nullptr);
#else
	sem_wait(&s_wake);
#endif
}

//-----------------------------------------------------------------------------
void Wake()
{
#ifdef __vita__
	// The semaphore holds one signal at most. One that is refused was not needed.
	sceKernelSignalSema(s_wake, 1);
#else
	sem_post(&s_wake);
#endif
}

//-----------------------------------------------------------------------------
/// Takes the next pass of the loop with the given number. False if that loop has no pass
/// left or is not the open one any more.
bool TakePass(uint64_t loopNumber, unsigned int count, unsigned int &index)
{
	uint64_t state = s_state.load();
	for (;;)
	{
		if ((state >> PASS_BITS) != loopNumber || (state & PASS_MASK) >= count)
			return false;

		if (s_state.compare_exchange_weak(state, state + 1))
		{
			index = static_cast<unsigned int>(state & PASS_MASK);
			return true;
		}
	}
}

//-----------------------------------------------------------------------------
/// True if a loop is open that the helper has not been to.
bool IsLoopWaiting(uint64_t lastLoop, uint64_t &loopNumber)
{
	loopNumber = s_state.load() >> PASS_BITS;
	return (loopNumber & 1) != 0 && loopNumber != lastLoop;
}

//-----------------------------------------------------------------------------
void *ThreadMain(void *)
{
	uint64_t lastLoop = 0;
	uint64_t lastWork = Now();

	for (;;)
	{
		uint64_t loopNumber = 0;
		bool waiting = false;
		for (uint32_t looks = 0; looks < LOOKS_PER_CLOCK && !waiting; ++looks)
			waiting = IsLoopWaiting(lastLoop, loopNumber);

		if (waiting)
		{
			// The loop was open when it was looked at, so this is its description, or, if it
			// has been closed since, something no pass will be taken with.
			const Task task = s_task.load();
			void *const context = s_context.load();
			const unsigned int count = s_count.load();

			unsigned int index;
			unsigned int passes = 0;
			while (TakePass(loopNumber, count, index))
			{
				task(context, index);
				s_finished.fetch_add(1);
				++passes;
			}

			s_helperPasses.fetch_add(passes, std::memory_order_relaxed);
			lastLoop = loopNumber;
			lastWork = Now();
			continue;
		}

		if (Now() - lastWork <= LOOK_BEFORE_SLEEP_US)
			continue;

		// Say that a signal is wanted, then look once more: whoever opened a loop before
		// reading that has not signalled. If the loop was opened after, the signal that
		// comes with it is left over and ends the next wait early, which does no harm.
		s_asleep.store(true);
		if (IsLoopWaiting(lastLoop, loopNumber))
		{
			s_asleep.store(false);
			continue;
		}

		WaitForWake();
		s_asleep.store(false);
		lastWork = Now();
	}

	return nullptr;
}

//-----------------------------------------------------------------------------
void Start()
{
	s_started = true;

	if (CreateWake())
	{
		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, THREAD_STACK_BYTES);
		pthread_t thread;
		s_running = pthread_create(&thread, &attributes, ThreadMain, nullptr) == 0;
		pthread_attr_destroy(&attributes);
	}

	if (!s_running)
	{
		fprintf(stderr, "[helper] no helper thread; the game's thread works alone\n");
		fflush(stderr);
	}
}

} // namespace

//-----------------------------------------------------------------------------
void forEach(unsigned int count, void (*task)(void *context, unsigned int index), void *context)
{
	if (!s_started)
		Start();

	s_allPasses += count;

	if (!s_running || !s_enabled || count < 2 || count > PASS_MASK)
	{
		const uint64_t began = Now();
		for (unsigned int index = 0; index < count; ++index)
			task(context, index);

		if (count >= 2)
		{
			s_loopTimes.aloneMicroseconds += Now() - began;
			s_loopTimes.alonePasses += count;
		}
		return;
	}

	const uint64_t began = Now();

	// Open the loop. Nobody uses the description before the state says so.
	s_task.store(task);
	s_context.store(context);
	s_count.store(count);
	s_finished.store(0);

	const uint64_t loopNumber = (s_state.load() >> PASS_BITS) + 1;
	s_state.store(loopNumber << PASS_BITS);

	if (s_asleep.load() && s_asleep.exchange(false))
		Wake();

	unsigned int index;
	while (TakePass(loopNumber, count, index))
	{
		task(context, index);
		s_finished.fetch_add(1);
	}

	// The helper may be in the middle of its last pass, which is short.
	while (s_finished.load() != count)
	{
	}

	// Close it: an even number, which no pass is taken from.
	s_state.store((loopNumber + 1) << PASS_BITS);

	s_loopTimes.sharedMicroseconds += Now() - began;
	s_loopTimes.sharedPasses += count;
}

//-----------------------------------------------------------------------------
void setEnabled(bool enabled)
{
	s_enabled = enabled;
}

//-----------------------------------------------------------------------------
LoopTimes takeLoopTimes()
{
	const LoopTimes times = s_loopTimes;
	s_loopTimes = LoopTimes();
	return times;
}

//-----------------------------------------------------------------------------
void takePassCounts(unsigned int &all, unsigned int &byHelper)
{
	all = s_allPasses;
	s_allPasses = 0;
	byHelper = s_helperPasses.exchange(0, std::memory_order_relaxed);
}

} // namespace VitaHelper
