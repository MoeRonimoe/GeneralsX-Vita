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

// VitaDiagnostics.h //////////////////////////////////////////////////////////
// What the log needs to say for a run to be understood without a debugger or a
// profiler: where a C++ exception was thrown, how much memory was left at that
// moment, and which part of the engine the time of a frame went to.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstddef>
#include <cstdint>

namespace VitaDiagnostics
{

struct MemoryState
{
	size_t heapUsed;		///< bytes allocated from the C heap
	size_t heapGrown;		///< bytes the C heap has claimed from its reservation so far
	size_t heapLimit;		///< size of the reservation (_newlib_heap_size_user)
	size_t freeMain;		///< application memory nobody has claimed yet
	size_t freeCdram;		///< unclaimed GPU mapped memory
	size_t freePhycont;		///< unclaimed physically contiguous memory
};

MemoryState getMemoryState();

/// Write the state of the C heap and of the application's memory budget to the log.
void logMemory(const char *label);

/// Report std::terminate() in the log. Throws are reported without any setup.
void install();

/// Work whose share of a frame the log reports. Whoever does the work adds its time to
/// the slot; the graphics layer, which knows where a frame ends, takes the sums.
enum TimeSlot
{
	TIME_GAME_LOGIC,
	TIME_SLOT_COUNT
};

/// Microseconds since the process started.
uint64_t now();

/// True while the time of a frame is being measured. Reading the clock a few thousand
/// times per frame costs a few percent of it, so the timers below do nothing unless this
/// is set; the environment variable VITA_PROFILE turns it on (see VitaMain.cpp).
extern bool g_profiling;

void setProfiling(bool enabled);

/// The start time for a measurement that only runs while a frame profile is taken:
/// now() then, and 0 otherwise, which the timers take as "do not measure".
inline uint64_t profileStart() { return g_profiling ? now() : 0; }

/// Draw calls since the process started, counted by the graphics layer. The named
/// timers take the difference, which tells which part of the engine issues how many.
extern uint32_t g_drawCalls;

void addTime(TimeSlot slot, uint64_t microseconds);

/// The time added to a slot since the last call, which starts a new sum.
uint64_t takeTime(TimeSlot slot);

/// Adds the time between its construction and destruction to a slot.
class ScopedTime
{
public:
	explicit ScopedTime(TimeSlot slot) : m_slot(slot), m_start(profileStart()) {}
	~ScopedTime()
	{
		if (m_start != 0)
			addTime(m_slot, now() - m_start);
	}

private:
	TimeSlot m_slot;
	uint64_t m_start;
};

/// One of the timers the engine has all over its code (USE_PERF_TIMER in
/// Common/PerfTimer.h). On the Vita they feed the log instead of the engine's own
/// profiling display. The time is inclusive: a timer running inside another counts in both.
class NamedTimer
{
public:
	/// name has to stay valid; the timers are static objects named by string literals.
	explicit NamedTimer(const char *name);

	const char *name;
	uint64_t time;			///< sum over the calls that were timed
	uint32_t draws;			///< draw calls issued during the calls that were timed
	uint32_t calls;
	uint32_t timedCalls;
	uint32_t sampleMask;	///< a call is timed when its number has none of these bits set; grows for timers that run very often
	NamedTimer *next;
};

/// Runs a named timer between its construction and destruction.
class NamedTimerScope
{
public:
	explicit NamedTimerScope(NamedTimer &timer) :
		m_timer(timer),
		m_start(g_profiling && (timer.calls++ & timer.sampleMask) == 0 ? now() : 0),
		m_draws(g_drawCalls)
	{
	}

	~NamedTimerScope()
	{
		if (m_start != 0)
		{
			m_timer.time += now() - m_start;
			m_timer.draws += g_drawCalls - m_draws;
			++m_timer.timedCalls;
		}
	}

private:
	NamedTimer &m_timer;
	uint64_t m_start;
	uint32_t m_draws;
};

/// Write the named timers that took the most time since the last call to the log, as
/// milliseconds per frame, and start them over.
void logNamedTimers(uint32_t frames);

/// Starts taking samples of where the calling thread is, if "+VITA_SAMPLE=<per second>" is
/// in vita_args.txt, and writes them to a file when the game ends (VitaSampler.cpp;
/// tools/samples.py reads the file).
void startSampling();
void finishSampling();

/// Goes into every sample. The game sets it to the step of the logic of the mission that is
/// played, and to 0 where there is none.
extern uint32_t g_sampleTag;

/// Measures how fast the console moves memory and whether two threads get a core each, and
/// writes that to the log (VitaMachineTest.cpp). Takes a second and 32 MiB while it runs.
void runMachineTest();

/// While a frame profile is taken: one request of the engine to its file system. The log
/// names the files that took the most time since its last line, with the timers.
void noteFileOperation(const char *operation, const char *name, uint64_t microseconds);

} // namespace VitaDiagnostics
