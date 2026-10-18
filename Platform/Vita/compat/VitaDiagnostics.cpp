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

// VitaDiagnostics.cpp ////////////////////////////////////////////////////////
// The engine reports most fatal conditions by throwing an error code, and its
// main loop turns every one of them into the same "Uncaught Exception" exit.
// A hardware fault leaves a core dump behind; a throw leaves nothing. So every
// throw is written to the log with its type, its value and the call chain that
// led to it, as offsets into the code segment that tools/resolve_log.py turns
// back into function names.
///////////////////////////////////////////////////////////////////////////////

#include "VitaDiagnostics.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <malloc.h>
#include <stdint.h>

// The SDK's compiler ships the ARM unwinder interface without the unwind.h that
// normally wraps it.
#include <unwind-arm-common.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <string>
#include <typeinfo>

extern "C"
{
	extern unsigned int _newlib_heap_size_user;

	// Bounds of the code segment, provided by the linker script.
	extern char __executable_start[];
	extern char __etext[];

	// The link option --wrap=__cxa_throw sends every throw to __wrap___cxa_throw and
	// makes the original available under this name.
	void __real___cxa_throw(void *thrown, std::type_info *type, void (*destructor)(void *)) __attribute__((noreturn));
	void __wrap___cxa_throw(void *thrown, std::type_info *type, void (*destructor)(void *)) __attribute__((noreturn));
}

namespace
{

// An exception that is caught and handled is harmless, and a loop of them must not fill
// the memory card. The ones that end the game are among the first.
const int MAX_LOGGED_THROWS = 48;

const int MAX_FRAMES = 40;

// Stack words searched for return addresses when the unwinder gives up early.
const int STACK_SCAN_WORDS = 384;

// Number of the program counter among the core registers of the unwinder.
const int UNWIND_REGISTER_PC = 15;

const double MEGABYTE = 1024.0 * 1024.0;

uint64_t s_timeSlots[VitaDiagnostics::TIME_SLOT_COUNT];

// The named timers register themselves during static initialization, in no particular
// order; a plain pointer is ready before any of them.
VitaDiagnostics::NamedTimer *s_namedTimers = nullptr;

// Timers below this share of a frame are left out of the log.
const double NAMED_TIMER_MIN_MS = 0.5;

const int MAX_LOGGED_TIMERS = 28;

// A timer that runs more often than this per frame only times a part of its calls: some
// sit in functions that are called thousands of times, where reading the clock on every
// call would cost more than what it measures. The sum is scaled up for the log.
const double MAX_TIMED_CALLS_PER_FRAME = 32.0;

// Largest mask a timer can get: one call in 4096 is timed.
const uint32_t MAX_SAMPLE_MASK = 0xFFF;

// The call counts only say something about a frame when frames were actually drawn, not
// while a loading screen stands still.
const uint32_t MIN_FRAMES_TO_JUDGE_TIMERS = 20;

struct Trace
{
	uintptr_t frames[MAX_FRAMES];
	int count;
};

//-----------------------------------------------------------------------------
bool isCodeAddress(uintptr_t address)
{
	return address >= reinterpret_cast<uintptr_t>(__executable_start) && address < reinterpret_cast<uintptr_t>(__etext);
}

//-----------------------------------------------------------------------------
/// A return address points behind the call. The call to a function that does not return
/// can be the last instruction of its function, so step back into it.
unsigned int codeOffset(uintptr_t returnAddress)
{
	return static_cast<unsigned int>((returnAddress & ~static_cast<uintptr_t>(1)) - 2 - reinterpret_cast<uintptr_t>(__executable_start));
}

//-----------------------------------------------------------------------------
_Unwind_Reason_Code collectFrame(struct _Unwind_Context *context, void *argument)
{
	Trace *trace = static_cast<Trace *>(argument);
	if (trace->count >= MAX_FRAMES)
		return _URC_END_OF_STACK;

	const uintptr_t address = _Unwind_GetGR(context, UNWIND_REGISTER_PC);
	if (!isCodeAddress(address))
		return _URC_END_OF_STACK;

	trace->frames[trace->count++] = address;
	return _URC_NO_REASON;
}

//-----------------------------------------------------------------------------
void logFrames(const char *label, const Trace &trace)
{
	fprintf(stderr, "[throw]   %s:", label);
	for (int i = 0; i < trace.count; ++i)
		fprintf(stderr, " +0x%x", codeOffset(trace.frames[i]));
	fprintf(stderr, "\n");
}

//-----------------------------------------------------------------------------
/// Every stack word that looks like a return address into Thumb code. Unlike the
/// unwinder's result this contains leftovers of calls that have already returned.
void scanStack(const void *stackPointer, Trace &trace)
{
	const uintptr_t *words = static_cast<const uintptr_t *>(stackPointer);
	trace.count = 0;
	for (int i = 0; i < STACK_SCAN_WORDS && trace.count < MAX_FRAMES; ++i)
	{
		if ((words[i] & 1) != 0 && isCodeAddress(words[i]))
			trace.frames[trace.count++] = words[i];
	}
}

//-----------------------------------------------------------------------------
void logThrow(const void *thrown, const std::type_info *type, const void *returnAddress, const void *stackPointer)
{
	static int count = 0;
	if (count >= MAX_LOGGED_THROWS)
		return;
	++count;

	// Error codes and other scalars are the first word of the thrown object; for a class
	// this is its first member.
	uint32_t firstWord;
	__builtin_memcpy(&firstWord, thrown, sizeof(firstWord));

	const VitaDiagnostics::MemoryState memory = VitaDiagnostics::getMemoryState();
	fprintf(stderr, "[throw] #%d type %s, first word 0x%08x, thrown at +0x%x; heap %.1f of %.1f MB in use\n",
		count, type != nullptr ? type->name() : "?", static_cast<unsigned int>(firstWord),
		codeOffset(reinterpret_cast<uintptr_t>(returnAddress)), memory.heapUsed / MEGABYTE, memory.heapLimit / MEGABYTE);

	Trace trace;
	trace.count = 0;
	_Unwind_Backtrace(collectFrame, &trace);
	logFrames("call chain", trace);

	if (trace.count < 4)
	{
		scanStack(stackPointer, trace);
		logFrames("stack scan", trace);
	}

	if (count == MAX_LOGGED_THROWS)
		fprintf(stderr, "[throw] further exceptions are not logged\n");

	fflush(stderr);
}

//-----------------------------------------------------------------------------
void onTerminate()
{
	fprintf(stderr, "FATAL: std::terminate() called\n");
	VitaDiagnostics::logMemory("terminate");
	fflush(stderr);
	abort();
}

} // namespace

//-----------------------------------------------------------------------------
void __wrap___cxa_throw(void *thrown, std::type_info *type, void (*destructor)(void *))
{
	logThrow(thrown, type, __builtin_return_address(0), __builtin_frame_address(0));
	__real___cxa_throw(thrown, type, destructor);
}

namespace VitaDiagnostics
{

//-----------------------------------------------------------------------------
MemoryState getMemoryState()
{
	MemoryState state;

	const struct mallinfo heap = mallinfo();
	state.heapUsed = heap.uordblks;
	state.heapGrown = heap.arena;
	state.heapLimit = _newlib_heap_size_user;

	SceKernelFreeMemorySizeInfo unclaimed;
	unclaimed.size = sizeof(unclaimed);
	unclaimed.size_user = 0;
	unclaimed.size_cdram = 0;
	unclaimed.size_phycont = 0;
	sceKernelGetFreeMemorySize(&unclaimed);
	state.freeMain = unclaimed.size_user;
	state.freeCdram = unclaimed.size_cdram;
	state.freePhycont = unclaimed.size_phycont;

	return state;
}

//-----------------------------------------------------------------------------
void logMemory(const char *label)
{
	const MemoryState state = getMemoryState();
	fprintf(stderr, "[mem] %s: heap %.1f MB in use, grown to %.1f of %.1f MB; unclaimed: main %.1f MB, cdram %.1f MB, phycont %.1f MB\n",
		label, state.heapUsed / MEGABYTE, state.heapGrown / MEGABYTE, state.heapLimit / MEGABYTE,
		state.freeMain / MEGABYTE, state.freeCdram / MEGABYTE, state.freePhycont / MEGABYTE);
	fflush(stderr);
}

//-----------------------------------------------------------------------------
void install()
{
	std::set_terminate(onTerminate);
}

//-----------------------------------------------------------------------------
uint64_t now()
{
	return sceKernelGetProcessTimeWide();
}

//-----------------------------------------------------------------------------
void addTime(TimeSlot slot, uint64_t microseconds)
{
	s_timeSlots[slot] += microseconds;
}

//-----------------------------------------------------------------------------
uint64_t takeTime(TimeSlot slot)
{
	const uint64_t sum = s_timeSlots[slot];
	s_timeSlots[slot] = 0;
	return sum;
}

//-----------------------------------------------------------------------------
NamedTimer::NamedTimer(const char *timerName) :
	name(timerName),
	time(0),
	draws(0),
	calls(0),
	timedCalls(0),
	sampleMask(0),
	next(s_namedTimers)
{
	s_namedTimers = this;
}

//-----------------------------------------------------------------------------
bool g_profiling = false;
uint32_t g_drawCalls = 0;

//-----------------------------------------------------------------------------
void setProfiling(bool enabled)
{
	g_profiling = enabled;
	fprintf(stderr, "[time] frame profile %s\n", enabled ? "on" : "off");
}

//-----------------------------------------------------------------------------
/// What a timer's calls took per frame; the calls that were not timed are assumed to have
/// taken as long as the ones that were.
static double namedTimerMs(const NamedTimer &timer, uint32_t frames)
{
	if (timer.timedCalls == 0)
		return 0.0;

	return timer.time / 1000.0 / frames * (static_cast<double>(timer.calls) / timer.timedCalls);
}

//-----------------------------------------------------------------------------
namespace
{

struct FileTally
{
	uint32_t calls;
	uint64_t time;
};

// Requests to the file system since the last line of the log, by operation and name.
// Files are asked for by more than one thread.
std::map<std::string, FileTally> s_fileTallies;
uint32_t s_fileCalls = 0;
uint64_t s_fileTime = 0;
SceKernelLwMutexWork s_fileTallyMutex;
bool s_fileTallyMutexMade = false;

enum
{
	MAX_FILE_TALLIES = 2048,	///< different names kept between two lines of the log
	MAX_LOGGED_FILES = 6,
	MAX_LOGGED_REPEATS = 4,
	MAX_LOGGED_NAME = 96
};

void logFileOperations(uint32_t frames)
{
	if (!s_fileTallyMutexMade)
		return;

	sceKernelLockLwMutex(&s_fileTallyMutex, 1, nullptr);
	if (s_fileCalls != 0)
	{
		fprintf(stderr, "[file] per frame %.1f requests, %.1f ms, %u different names; most time:", static_cast<double>(s_fileCalls) / frames,
			s_fileTime / (1000.0 * frames), static_cast<unsigned int>(s_fileTallies.size()));

		for (int i = 0; i < MAX_LOGGED_FILES; ++i)
		{
			std::map<std::string, FileTally>::iterator largest = s_fileTallies.end();
			for (std::map<std::string, FileTally>::iterator it = s_fileTallies.begin(); it != s_fileTallies.end(); ++it)
			{
				if (it->second.time != 0 && (largest == s_fileTallies.end() || it->second.time > largest->second.time))
					largest = it;
			}
			if (largest == s_fileTallies.end())
				break;

			// A name may be anything, also what was never meant to be one.
			char name[MAX_LOGGED_NAME + 1];
			size_t length = 0;
			for (; length < MAX_LOGGED_NAME && length < largest->first.size(); ++length)
			{
				const unsigned char c = static_cast<unsigned char>(largest->first[length]);
				name[length] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
			}
			name[length] = '\0';

			fprintf(stderr, " %ux %s %.1f ms,", largest->second.calls, name, largest->second.time / 1000.0);
			largest->second.time = 0;
		}

		// What is asked for over and over need not take long each time.
		fprintf(stderr, " most often:");
		for (int i = 0; i < MAX_LOGGED_REPEATS; ++i)
		{
			std::map<std::string, FileTally>::iterator most = s_fileTallies.end();
			for (std::map<std::string, FileTally>::iterator it = s_fileTallies.begin(); it != s_fileTallies.end(); ++it)
			{
				if (it->second.calls > 1 && (most == s_fileTallies.end() || it->second.calls > most->second.calls))
					most = it;
			}
			if (most == s_fileTallies.end())
				break;

			char name[MAX_LOGGED_NAME + 1];
			size_t length = 0;
			for (; length < MAX_LOGGED_NAME && length < most->first.size(); ++length)
			{
				const unsigned char c = static_cast<unsigned char>(most->first[length]);
				name[length] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
			}
			name[length] = '\0';

			fprintf(stderr, " %ux %s,", most->second.calls, name);
			most->second.calls = 0;
		}
		fprintf(stderr, "\n");
		fflush(stderr);
	}

	s_fileTallies.clear();
	s_fileCalls = 0;
	s_fileTime = 0;
	sceKernelUnlockLwMutex(&s_fileTallyMutex, 1);
}

} // namespace

void noteFileOperation(const char *operation, const char *name, uint64_t microseconds)
{
	if (!s_fileTallyMutexMade)
	{
		// The first request comes from the game's thread, long before another one exists.
		sceKernelCreateLwMutex(&s_fileTallyMutex, "FileTally", 0, 0, nullptr);
		s_fileTallyMutexMade = true;
	}

	sceKernelLockLwMutex(&s_fileTallyMutex, 1, nullptr);
	++s_fileCalls;
	s_fileTime += microseconds;
	if (s_fileTallies.size() < MAX_FILE_TALLIES)
	{
		std::string key = operation;
		key += ' ';
		key += name != nullptr ? name : "";
		FileTally &tally = s_fileTallies[key];
		++tally.calls;
		tally.time += microseconds;
	}
	sceKernelUnlockLwMutex(&s_fileTallyMutex, 1);
}

void logNamedTimers(uint32_t frames)
{
	if (frames != 0)
		logFileOperations(frames);

	if (frames == 0 || s_namedTimers == nullptr)
		return;

	// Pick the largest ones by repeated search; there are a few dozen timers. A timer
	// that has been written out is marked by clearing its sum.
	fprintf(stderr, "[time] per frame:");
	for (int i = 0; i < MAX_LOGGED_TIMERS; ++i)
	{
		NamedTimer *largest = nullptr;
		double largestMs = 0.0;
		for (NamedTimer *timer = s_namedTimers; timer != nullptr; timer = timer->next)
		{
			const double ms = namedTimerMs(*timer, frames);
			if (ms > largestMs)
			{
				largest = timer;
				largestMs = ms;
			}
		}

		if (largest == nullptr || largestMs < NAMED_TIMER_MIN_MS)
			break;

		// "~" marks a value scaled up from a sample of the calls.
		const double scale = static_cast<double>(largest->calls) / largest->timedCalls;
		const double draws = largest->draws * scale / frames;
		fprintf(stderr, " %s %s%.1f ms (%.1fx", largest->name, largest->sampleMask != 0 ? "~" : "", largestMs,
			static_cast<double>(largest->calls) / frames);
		if (draws >= 0.5)
			fprintf(stderr, ", %.0f draws", draws);
		fprintf(stderr, "),");
		largest->timedCalls = 0;
	}
	fprintf(stderr, "\n");
	fflush(stderr);

	for (NamedTimer *timer = s_namedTimers; timer != nullptr; timer = timer->next)
	{
		// Time fewer calls of a timer that runs very often, and more again when it calms
		// down. The counts only say something when frames were actually drawn.
		if (frames >= MIN_FRAMES_TO_JUDGE_TIMERS)
		{
			const double callsPerFrame = static_cast<double>(timer->calls) / frames;
			uint32_t mask = 0;
			while (callsPerFrame / (mask + 1) > MAX_TIMED_CALLS_PER_FRAME && mask < MAX_SAMPLE_MASK)
				mask = mask * 2 + 1;
			timer->sampleMask = mask;
		}

		timer->time = 0;
		timer->draws = 0;
		timer->calls = 0;
		timer->timedCalls = 0;
	}
}

} // namespace VitaDiagnostics
