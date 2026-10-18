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

// VitaSampler.cpp ////////////////////////////////////////////////////////////
// Where the game's thread spends its time, found by looking.
//
// The timers of VitaDiagnostics.h only tell about the places somebody thought
// of putting one. This asks the other way round: a few hundred times per second
// a thread of its own takes down which function the game's thread is in and
// what its stack holds. At the end the samples are written to a file, and
// tools/samples.py turns them into a list of functions by time.
//
// The system does not let a program like this one stop another of its threads
// and read its registers (sceKernelSuspendThreadForVM answers 0x80020008). So
// the game's thread says where it is itself: in a build with
// -DVITA_CALL_PROFILE=ON the compiler puts a call at the start of every
// function (-pg), and the few instructions that call runs (__gnu_mcount_nc, at
// the end of this file) leave the function's address and the stack pointer
// where the sampler finds them, if the thread is the game's; the other threads
// run the same functions and leave nothing. That costs a sixth of the frame
// rate and makes code that calls a lot look slower than it is, so it is a
// build of its own, for measuring. Time spent in what is not compiled that way
// (the C library, vitaGL, the system) counts for the function that called it.
//
// "+VITA_SAMPLE=250" in vita_args.txt turns the sampler on, with the number of
// samples per second.
///////////////////////////////////////////////////////////////////////////////

#include "VitaDiagnostics.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
{
	// Bounds of the code segment, provided by the linker script.
	extern char __executable_start[];
	extern char __etext[];

	/// What the code at the start of every function writes, for the thread that is watched.
	struct VitaCallMark
	{
		volatile uint32_t function;	///< an address in the function entered last
		volatile uint32_t stack;	///< where that function's return address is on the stack
		volatile uint32_t thread;	///< what the thread register of the watched thread holds
	};
	VitaCallMark g_vitaCallMark = { 0, 0, 0xFFFFFFFFu };
}

namespace VitaDiagnostics
{

uint32_t g_sampleTag = 0;

namespace
{

const char *const SAMPLE_FILE_PATH = "ux0:data/Generals/UserData/vita_samples.bin";

enum
{
	// A sample: the function the thread was in, the tag of the moment, and the words of
	// its stack that point into the code. tools/samples.py tells the return addresses
	// among those from what happens to look like one.
	SAMPLE_WORDS = 24,
	SAMPLE_FUNCTION = 0,
	SAMPLE_TAG = 2,
	SAMPLE_FIRST_CALLER = 3,

	MAX_SAMPLES = 90000,		///< six minutes at 250 per second, 8.2 MB
	STACK_WORDS_SEARCHED = 512,
	DEFAULT_RATE = 250,
	MAX_RATE = 1000,

	SAMPLER_PRIORITY = 96,		///< ahead of the game's threads, which have 160
	SAMPLER_STACK_BYTES = 64 * 1024,
	WRITE_CHUNK_BYTES = 256 * 1024
};

uintptr_t s_stackStart = 0;
uintptr_t s_stackEnd = 0;
uint32_t s_periodUs = 0;

uint32_t *s_samples = nullptr;
std::atomic<uint32_t> s_sampleCount(0);
std::atomic<bool> s_running(false);
std::atomic<bool> s_stopped(true);
uint32_t s_unmarked = 0;

//-----------------------------------------------------------------------------
bool isCode(uint32_t value)
{
	return value >= reinterpret_cast<uintptr_t>(__executable_start) && value < reinterpret_cast<uintptr_t>(__etext);
}

//-----------------------------------------------------------------------------
void takeSample()
{
	// The game's thread goes on while this looks, so the function and the stack may be
	// those of two moments. Among thousands of samples that does not show.
	const uint32_t function = g_vitaCallMark.function;
	const uintptr_t stack = g_vitaCallMark.stack;
	if (function == 0)
	{
		++s_unmarked;
		return;
	}

	uint32_t words[STACK_WORDS_SEARCHED];
	uint32_t wordCount = 0;
	if ((stack & 3) == 0 && stack >= s_stackStart && stack < s_stackEnd)
	{
		wordCount = static_cast<uint32_t>((s_stackEnd - stack) / 4);
		if (wordCount > STACK_WORDS_SEARCHED)
			wordCount = STACK_WORDS_SEARCHED;
		memcpy(words, reinterpret_cast<const void *>(stack), wordCount * 4);
	}

	const uint32_t index = s_sampleCount.load(std::memory_order_relaxed);
	if (index >= MAX_SAMPLES)
		return;

	uint32_t *sample = s_samples + static_cast<size_t>(index) * SAMPLE_WORDS;
	memset(sample, 0, SAMPLE_WORDS * sizeof(uint32_t));
	sample[SAMPLE_FUNCTION] = function;
	sample[SAMPLE_TAG] = g_sampleTag;

	uint32_t callers = SAMPLE_FIRST_CALLER;
	for (uint32_t word = 0; word < wordCount && callers < SAMPLE_WORDS; ++word)
	{
		if (isCode(words[word]))
			sample[callers++] = words[word];
	}

	s_sampleCount.store(index + 1, std::memory_order_release);
}

//-----------------------------------------------------------------------------
int samplerThread(SceSize, void *)
{
	while (s_running.load())
	{
		sceKernelDelayThread(s_periodUs);
		takeSample();
	}

	s_stopped.store(true);
	return 0;
}

} // namespace

//-----------------------------------------------------------------------------
void startSampling()
{
	const char *wanted = getenv("VITA_SAMPLE");
	if (wanted == nullptr)
		return;

#ifndef VITA_CALL_PROFILE
	fprintf(stderr, "[sample] this build does not say where its threads are; samples need one made with -DVITA_CALL_PROFILE=ON\n");
	fflush(stderr);
#else
	int rate = atoi(wanted);
	if (rate < 10)
		rate = DEFAULT_RATE;
	if (rate > MAX_RATE)
		rate = MAX_RATE;
	s_periodUs = 1000000u / static_cast<uint32_t>(rate);

	// The caller is the thread that is watched.
	SceKernelThreadInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
	{
		fprintf(stderr, "[sample] nothing known about the game's thread; no samples will be taken\n");
		fflush(stderr);
		return;
	}
	s_stackStart = reinterpret_cast<uintptr_t>(info.stack);
	s_stackEnd = s_stackStart + static_cast<uintptr_t>(info.stackSize);

	s_samples = static_cast<uint32_t *>(malloc(static_cast<size_t>(MAX_SAMPLES) * SAMPLE_WORDS * sizeof(uint32_t)));
	if (s_samples == nullptr)
	{
		fprintf(stderr, "[sample] no memory for the samples\n");
		fflush(stderr);
		return;
	}

	const SceUID thread = sceKernelCreateThread("sampler", samplerThread, SAMPLER_PRIORITY, SAMPLER_STACK_BYTES, 0, 0, nullptr);
	if (thread < 0)
	{
		fprintf(stderr, "[sample] no thread for the sampler: 0x%08x\n", static_cast<unsigned int>(thread));
		fflush(stderr);
		free(s_samples);
		s_samples = nullptr;
		return;
	}

	// From here on the functions of this thread leave their marks. The thread register
	// holds the address of the thread's own storage, which no two threads share.
	uint32_t threadRegister = 0;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(threadRegister));
	g_vitaCallMark.thread = threadRegister;

	s_running.store(true);
	s_stopped.store(false);
	sceKernelStartThread(thread, 0, nullptr);

	fprintf(stderr, "[sample] taking %d samples per second of the game's thread (stack 0x%08x to 0x%08x, code 0x%08x to 0x%08x)\n",
		rate, static_cast<unsigned int>(s_stackStart), static_cast<unsigned int>(s_stackEnd),
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(__executable_start)),
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(__etext)));
	fflush(stderr);
#endif
}

//-----------------------------------------------------------------------------
void finishSampling()
{
	if (s_samples == nullptr)
		return;

	s_running.store(false);
	for (int i = 0; i < 200 && !s_stopped.load(); ++i)
		sceKernelDelayThread(10 * 1000);

	const uint32_t count = s_sampleCount.load();

	// The header says what is needed to read the rest.
	uint32_t header[8];
	memset(header, 0, sizeof(header));
	memcpy(&header[0], "VSMP", 4);
	header[1] = 1;
	header[2] = SAMPLE_WORDS;
	header[3] = count;
	header[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__executable_start));
	header[5] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__etext));
	header[6] = s_periodUs;
	header[7] = s_unmarked;

	bool written = false;
	const int file = open(SAMPLE_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (file >= 0)
	{
		written = write(file, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header));

		const char *bytes = reinterpret_cast<const char *>(s_samples);
		size_t left = static_cast<size_t>(count) * SAMPLE_WORDS * sizeof(uint32_t);
		while (written && left > 0)
		{
			const size_t chunk = left < WRITE_CHUNK_BYTES ? left : static_cast<size_t>(WRITE_CHUNK_BYTES);
			written = write(file, bytes, chunk) == static_cast<ssize_t>(chunk);
			bytes += chunk;
			left -= chunk;
		}
		close(file);
	}

	fprintf(stderr, "[sample] %u samples %s %s; %u looks found no mark\n", static_cast<unsigned int>(count),
		written ? "written to" : "COULD NOT BE WRITTEN to", SAMPLE_FILE_PATH, static_cast<unsigned int>(s_unmarked));
	fflush(stderr);

	free(s_samples);
	s_samples = nullptr;
}

} // namespace VitaDiagnostics

#ifdef VITA_CALL_PROFILE
// What a function compiled with -pg calls before anything else, having pushed its return
// address: "push {lr}; bl __gnu_mcount_nc". This has to take that word off the stack again
// and may change nothing but ip and the flags. If the thread is the watched one, it leaves
// behind an address in the function (lr) and where the function's return address is: the
// word just pushed, which is also where the function itself will save it.
__asm__(
	"	.syntax unified\n"
	"	.text\n"
	"	.align 2\n"
	"	.thumb\n"
	"	.thumb_func\n"
	"	.global __gnu_mcount_nc\n"
	"	.type __gnu_mcount_nc, %function\n"
	"__gnu_mcount_nc:\n"
	"	push {r0, r1}\n"
	"	mrc p15, 0, r0, c13, c0, 3\n"
	"	ldr r1, =g_vitaCallMark\n"
	"	ldr ip, [r1, #8]\n"
	"	cmp r0, ip\n"
	"	bne 1f\n"
	"	str lr, [r1]\n"
	"	add r0, sp, #8\n"
	"	str r0, [r1, #4]\n"
	"1:	pop {r0, r1}\n"
	"	mov ip, lr\n"
	"	pop {lr}\n"
	"	bx ip\n"
	"	.pool\n"
	"	.size __gnu_mcount_nc, .-__gnu_mcount_nc\n"
);
#endif
