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

// VitaMachineTest.cpp ////////////////////////////////////////////////////////
// What the console is good at, measured on the console.
//
// Two changes that should have made the soldiers' skins cheaper did not: doing
// the arithmetic four numbers at a time, and sharing the skins out between two
// cores. Both make the processor compute faster. Neither helps if the time goes
// into getting the vertices into memory, and the same would then hold for every
// other loop of the game that fills a large block. So this measures, before
// the game starts:
//
//   - how fast one thread fills, copies and writes vertices into a block that
//     fits the first cache, the second cache, or neither;
//   - how fast two threads do it at the same time, each with blocks of its own;
//   - whether two threads that only compute are as fast together as one alone,
//     which says whether they have a core each.
//
// "+VITA_MEMTEST=1" in vita_args.txt; the lines begin with "[machine]".
///////////////////////////////////////////////////////////////////////////////

#include "VitaDiagnostics.h"
#include "VitaHelper.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#else
#include <functional>
#include <thread>
#endif

namespace VitaDiagnostics
{

namespace
{

enum Kind
{
	KIND_FILL,
	KIND_COPY,
	KIND_VERTICES,
	KIND_ARITHMETIC
};

enum : size_t
{
	LARGEST_BLOCK = 8 * 1024 * 1024,
	BYTES_PER_MEASUREMENT = 16 * 1024 * 1024,
	VERTEX_BYTES = 44
};

enum : uint32_t
{
	ARITHMETIC_STEPS = 1000000,
	TRIES_FOR_TWO_THREADS = 8
};

struct Work
{
	Kind kind;
	size_t blockBytes;
	uint8_t *from[2];
	uint8_t *to[2];

	// What each pass found.
	uint64_t took[2];
	int cpu[2];
	int thread[2];
	int priority[2];
	int cpuMask[2];
};

volatile float s_sink = 0.0f;

//-----------------------------------------------------------------------------
int CurrentCpu()
{
#ifdef __vita__
	return sceKernelGetCpuId();
#else
	return 0;
#endif
}

//-----------------------------------------------------------------------------
int CurrentThread()
{
#ifdef __vita__
	return sceKernelGetThreadId();
#else
	return static_cast<int>(std::hash<std::thread::id>()(std::this_thread::get_id()) & 0x7fffffff);
#endif
}

//-----------------------------------------------------------------------------
/// Vertices as the engine's skins are written: eleven numbers each, one after the other,
/// into memory nothing has been read from.
void WriteVertices(uint8_t *to, size_t bytes)
{
	float *out = reinterpret_cast<float *>(to);
	float value = 1.0f;
	for (size_t vertex = bytes / VERTEX_BYTES; vertex != 0; --vertex, out += VERTEX_BYTES / sizeof(float))
	{
		out[0] = value;
		out[1] = value + 1.0f;
		out[2] = value + 2.0f;
		out[3] = value * 0.5f;
		out[4] = value * 0.25f;
		out[5] = value * 0.125f;
		out[6] = 1.0f;
		out[7] = value;
		out[8] = value;
		out[9] = 0.0f;
		out[10] = 0.0f;
		value += 0.25f;
	}
}

//-----------------------------------------------------------------------------
float Arithmetic(uint32_t steps)
{
	float a = 1.0f;
	float b = 0.5f;
	for (uint32_t step = 0; step < steps; ++step)
	{
		a = a * 0.999f + b;
		b = b * 1.001f - a * 0.0001f;
	}

	return a + b;
}

//-----------------------------------------------------------------------------
void Pass(void *context, unsigned int index)
{
	Work &work = *static_cast<Work *>(context);

	const uint64_t began = now();
	if (work.kind == KIND_ARITHMETIC)
	{
		s_sink = Arithmetic(ARITHMETIC_STEPS);
	}
	else
	{
		// The same block again and again: it stays in the cache it fits into.
		for (size_t done = 0; done < BYTES_PER_MEASUREMENT; done += work.blockBytes)
		{
			switch (work.kind)
			{
				case KIND_FILL:
					memset(work.to[index], static_cast<int>(done >> 10), work.blockBytes);
					break;
				case KIND_COPY:
					memcpy(work.to[index], work.from[index], work.blockBytes);
					break;
				default:
					WriteVertices(work.to[index], work.blockBytes);
					break;
			}
		}
	}
	work.took[index] = now() - began;

	work.cpu[index] = CurrentCpu();
	work.thread[index] = CurrentThread();
#ifdef __vita__
	work.priority[index] = sceKernelGetThreadCurrentPriority();
	work.cpuMask[index] = sceKernelGetThreadCpuAffinityMask(work.thread[index]);
#else
	work.priority[index] = 0;
	work.cpuMask[index] = 0;
#endif
}

//-----------------------------------------------------------------------------
double MegabytesPerSecond(uint64_t microseconds)
{
	return microseconds != 0 ? static_cast<double>(BYTES_PER_MEASUREMENT) / 1048576.0 * 1000000.0 / microseconds : 0.0;
}

//-----------------------------------------------------------------------------
uint64_t Alone(Work &work, Kind kind, size_t blockBytes)
{
	work.kind = kind;
	work.blockBytes = blockBytes;
	Pass(&work, 0);
	return work.took[0];
}

//-----------------------------------------------------------------------------
/// Both passes at the same time, one by this thread and one by the helper. False if the
/// helper did not take one in any of the tries.
bool Together(Work &work, Kind kind, size_t blockBytes)
{
	work.kind = kind;
	work.blockBytes = blockBytes;

	for (uint32_t tries = 0; tries < TRIES_FOR_TWO_THREADS; ++tries)
	{
		VitaHelper::forEach(2, Pass, &work);
		if (work.thread[0] != work.thread[1])
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
/// Writes what this thread and the helper measured when each made one pass at the same time.
void LogTogether(Work &work, Kind kind, const char *what, uint64_t alone)
{
	if (!Together(work, kind, LARGEST_BLOCK))
	{
		fprintf(stderr, "[machine] two at once, %s: the helper thread never took its part\n", what);
		return;
	}

	const int mine = work.thread[0] == CurrentThread() ? 0 : 1;
	const int helper = 1 - mine;

	if (kind == KIND_ARITHMETIC)
	{
		fprintf(stderr, "[machine] two at once, %s: this thread %.1f ms on core %d, the helper %.1f ms on core %d (alone %.1f ms); "
			"the helper has priority %d and may use cores 0x%x\n", what,
			work.took[mine] / 1000.0, work.cpu[mine], work.took[helper] / 1000.0, work.cpu[helper], alone / 1000.0,
			work.priority[helper], static_cast<unsigned int>(work.cpuMask[helper]));
	}
	else
	{
		fprintf(stderr, "[machine] two at once, %s: this thread %.0f MB/s on core %d, the helper %.0f MB/s on core %d (alone %.0f MB/s)\n", what,
			MegabytesPerSecond(work.took[mine]), work.cpu[mine], MegabytesPerSecond(work.took[helper]), work.cpu[helper],
			MegabytesPerSecond(alone));
	}
}

} // namespace

//-----------------------------------------------------------------------------
void runMachineTest()
{
	Work work = {};
	for (int i = 0; i < 2; ++i)
	{
		work.from[i] = static_cast<uint8_t *>(malloc(LARGEST_BLOCK));
		work.to[i] = static_cast<uint8_t *>(malloc(LARGEST_BLOCK));
	}

	if (work.from[0] != nullptr && work.to[0] != nullptr && work.from[1] != nullptr && work.to[1] != nullptr)
	{
		// The system hands out memory when it is first written to. That is not what is measured.
		for (int i = 0; i < 2; ++i)
		{
			memset(work.from[i], 0x5a, LARGEST_BLOCK);
			memset(work.to[i], 0xa5, LARGEST_BLOCK);
		}

#ifdef __vita__
		fprintf(stderr, "[machine] clocks: processor %d MHz, bus %d MHz, graphics %d MHz\n",
			scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
#endif

		static const size_t blocks[] = { 8 * 1024, 64 * 1024, 512 * 1024, LARGEST_BLOCK };
		uint64_t aloneLargest[3] = { 0, 0, 0 };
		for (const size_t blockBytes : blocks)
		{
			const uint64_t fill = Alone(work, KIND_FILL, blockBytes);
			const uint64_t copy = Alone(work, KIND_COPY, blockBytes);
			const uint64_t vertices = Alone(work, KIND_VERTICES, blockBytes);
			fprintf(stderr, "[machine] alone, blocks of %u KiB: fill %.0f MB/s, copy %.0f MB/s, vertices %.0f MB/s\n",
				static_cast<unsigned int>(blockBytes / 1024), MegabytesPerSecond(fill), MegabytesPerSecond(copy), MegabytesPerSecond(vertices));
			aloneLargest[0] = fill;
			aloneLargest[1] = copy;
			aloneLargest[2] = vertices;
		}

		const uint64_t arithmetic = Alone(work, KIND_ARITHMETIC, LARGEST_BLOCK);
		fprintf(stderr, "[machine] alone, arithmetic: %.1f ms; this thread has priority %d and may use cores 0x%x\n",
			arithmetic / 1000.0, work.priority[0], static_cast<unsigned int>(work.cpuMask[0]));

		LogTogether(work, KIND_ARITHMETIC, "arithmetic", arithmetic);
		LogTogether(work, KIND_FILL, "fill", aloneLargest[0]);
		LogTogether(work, KIND_COPY, "copy", aloneLargest[1]);
		LogTogether(work, KIND_VERTICES, "vertices", aloneLargest[2]);
	}
	else
	{
		fprintf(stderr, "[machine] no memory for the test\n");
	}

	for (int i = 0; i < 2; ++i)
	{
		free(work.from[i]);
		free(work.to[i]);
	}

	fflush(stderr);
}

} // namespace VitaDiagnostics
