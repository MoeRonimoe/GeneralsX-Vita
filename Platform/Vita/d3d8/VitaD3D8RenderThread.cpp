/*
**	Direct3D 8 on vitaGL
**	Part of the PS Vita port of Command & Conquer Generals.
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

// VitaD3D8RenderThread.cpp ///////////////////////////////////////////////////
// See VitaD3D8RenderThread.h.
//
// The stream is a chain of blocks of memory. The game's thread writes entries
// one after the other and says from time to time how many there are; the render
// thread runs them up to that number. Each of the two has its own place in the
// chain, and the count is all they share while they work. A block goes back to
// the game's thread when the frame it was read in has been shown, so that data
// kept in it is there for every call of that frame.
///////////////////////////////////////////////////////////////////////////////

#include "VitaD3D8RenderThread.h"

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <thread>
#endif

namespace VitaD3D8
{
namespace RenderThread
{

bool g_queueing = false;
Writer g_writer = { nullptr, nullptr, 0, 0 };

namespace
{

/// A piece of the stream. The entries follow it in memory.
struct Block
{
	Block *next;
	size_t capacity;	///< bytes for entries
};

// The usual size of a block. A frame of the game fills a few of them, most of it with
// vertices that are drawn from system memory.
const size_t BLOCK_BYTES = 256 * 1024;

// Blocks of the usual size that are kept for the next frames instead of freed.
const size_t MAX_SPARE_BLOCKS = 12;

// vitaGL builds and compiles its shaders on the thread that draws.
const size_t THREAD_STACK_BYTES = 2 * 1024 * 1024;

// How long the render thread looks for more entries before it goes to sleep. The game
// writes a draw call every 100 microseconds or so while it draws, with gaps of a
// millisecond where it works out what to draw next, and the core this thread looks on has
// nothing else to do. During the game's logic, 20 ms without a draw call, it sleeps.
const uint64_t LOOK_BEFORE_SLEEP_US = 1500;

// It sleeps in naps and looks again after each. Nobody wakes it: being woken was a call
// into the system for the game's thread each time, a millisecond per frame in all when
// that was measured. The naps get longer when nothing comes for a while, as in a menu.
const uint32_t SHORT_NAP_US = 250;
const uint32_t LONG_NAP_US = 2000;
const uint32_t SHORT_NAPS = 80;

// Entries after which the game's thread says how many there are even if nothing was
// drawn, so that long runs of other calls do not pile up.
const uint32_t PUBLISH_AFTER_ENTRIES = 256;

// THE GAME'S THREAD ------------------------------------------------------------
Block *s_writeBlock = nullptr;	///< the block g_writer writes into
uint32_t s_framesWritten = 0;
Waits s_waits = {};

// BOTH -----------------------------------------------------------------------
bool s_running = false;
std::atomic<uint32_t> s_published(0);		///< entries the render thread may run
std::atomic<uint32_t> s_framesShown(0);
std::atomic<uint64_t> s_busyTime(0);
std::atomic<uint32_t> s_sleeps(0);

pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_done = PTHREAD_COND_INITIALIZER;	///< the game's thread waits for the render thread

pthread_mutex_t s_spareLock = PTHREAD_MUTEX_INITIALIZER;
Block *s_spareBlocks = nullptr;
size_t s_spareCount = 0;

// THE RENDER THREAD ----------------------------------------------------------
Block *s_readBlock = nullptr;
size_t s_readOffset = 0;
uint32_t s_executed = 0;
Block *s_readBlocks = nullptr;	///< blocks read to their end since the last frame was shown

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
void Nap(uint32_t microseconds)
{
#ifdef __vita__
	sceKernelDelayThread(microseconds);
#else
	std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
#endif
}

//-----------------------------------------------------------------------------
Entry *EntryAt(Block *block, size_t offset)
{
	return reinterpret_cast<Entry *>(reinterpret_cast<unsigned char *>(block + 1) + offset);
}

//-----------------------------------------------------------------------------
/// Has the game's thread write into a block from its start.
void WriteInto(Block *block)
{
	s_writeBlock = block;
	g_writer.next = reinterpret_cast<unsigned char *>(block + 1);
	g_writer.limit = g_writer.next + block->capacity - sizeof(Entry);
}

//-----------------------------------------------------------------------------
Block *TakeBlock(size_t capacity)
{
	Block *block = nullptr;

	if (capacity == BLOCK_BYTES)
	{
		pthread_mutex_lock(&s_spareLock);
		if (s_spareBlocks != nullptr)
		{
			block = s_spareBlocks;
			s_spareBlocks = block->next;
			--s_spareCount;
		}
		pthread_mutex_unlock(&s_spareLock);
	}

	if (block == nullptr)
	{
		block = static_cast<Block *>(malloc(sizeof(Block) + capacity));
		if (block == nullptr)
		{
			// There is no going on without the stream, and no drawing without going on.
			fprintf(stderr, "[d3d8] out of memory for %u bytes of draw calls\n", static_cast<unsigned int>(capacity));
			fflush(stderr);
			abort();
		}
		block->capacity = capacity;
	}

	block->next = nullptr;
	return block;
}

//-----------------------------------------------------------------------------
void ReturnBlock(Block *block)
{
	pthread_mutex_lock(&s_spareLock);
	const bool keep = block->capacity == BLOCK_BYTES && s_spareCount < MAX_SPARE_BLOCKS;
	if (keep)
	{
		block->next = s_spareBlocks;
		s_spareBlocks = block;
		++s_spareCount;
	}
	pthread_mutex_unlock(&s_spareLock);

	if (!keep)
		free(block);
}

//-----------------------------------------------------------------------------
void WakeGameThread()
{
	pthread_mutex_lock(&s_lock);
	pthread_cond_broadcast(&s_done);
	pthread_mutex_unlock(&s_lock);
}

//-----------------------------------------------------------------------------
/// Run by the render thread after the call that shows a frame.
void FrameShown()
{
	// Everything that pointed into these blocks has been drawn.
	while (s_readBlocks != nullptr)
	{
		Block *block = s_readBlocks;
		s_readBlocks = block->next;
		ReturnBlock(block);
	}

	s_framesShown.fetch_add(1);
	WakeGameThread();
}

//-----------------------------------------------------------------------------
void *ThreadMain(void *)
{
	for (;;)
	{
		const uint32_t published = s_published.load();
		if (published == s_executed)
		{
			// Nothing to do. More usually comes within the time of a draw call.
			const uint64_t began = Now();
			bool more = false;
			while (!more)
			{
				for (int i = 0; i < 64 && !more; ++i)
					more = s_published.load() != s_executed;
				if (!more && Now() - began > LOOK_BEFORE_SLEEP_US)
					break;
			}

			if (!more)
			{
				s_sleeps.fetch_add(1, std::memory_order_relaxed);
				for (uint32_t naps = 0; s_published.load() == s_executed; ++naps)
					Nap(naps < SHORT_NAPS ? SHORT_NAP_US : LONG_NAP_US);
			}
			continue;
		}

		const uint64_t began = Now();
		while (s_executed != published)
		{
			Entry *entry = EntryAt(s_readBlock, s_readOffset);
			++s_executed;

			if (entry->size == 0)
			{
				// The end of a block. It is kept until the frame is shown.
				Block *finished = s_readBlock;
				s_readBlock = finished->next;
				s_readOffset = 0;
				finished->next = s_readBlocks;
				s_readBlocks = finished;
				continue;
			}

			s_readOffset += entry->size;
			if (entry->run != nullptr)
				entry->run(entry);
		}
		s_busyTime.fetch_add(Now() - began);
	}

	return nullptr;
}

} // namespace

//-----------------------------------------------------------------------------
bool Start()
{
	if (s_running)
		return true;

	// On unless "+VITA_RENDER_THREAD=0" asks for the calls to be made by the game's thread.
	const char *wanted = getenv("VITA_RENDER_THREAD");
	if (wanted != nullptr && atoi(wanted) == 0)
		return false;

	WriteInto(TakeBlock(BLOCK_BYTES));
	g_writer.written = 0;
	g_writer.publishAt = PUBLISH_AFTER_ENTRIES;
	s_readBlock = s_writeBlock;
	s_readOffset = 0;

	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, THREAD_STACK_BYTES);
	pthread_t thread;
	const bool created = pthread_create(&thread, &attributes, ThreadMain, nullptr) == 0;
	pthread_attr_destroy(&attributes);

	if (!created)
	{
		fprintf(stderr, "[d3d8] no render thread; drawing on the game's thread\n");
		free(s_writeBlock);
		s_writeBlock = nullptr;
		s_readBlock = nullptr;
		g_writer = Writer();
		return false;
	}

	s_running = true;
	g_queueing = true;
	return true;
}

//-----------------------------------------------------------------------------
bool IsRunning()
{
	return s_running;
}

//-----------------------------------------------------------------------------
Entry *AllocateSlow(size_t size)
{
	// What was written before this is complete, which the entry made here is not yet.
	if (g_writer.written - s_published.load(std::memory_order_relaxed) >= PUBLISH_AFTER_ENTRIES)
		Publish();

	// Every block ends with an entry that says so.
	if (size > static_cast<size_t>(g_writer.limit - g_writer.next))
	{
		Block *next = TakeBlock(size + sizeof(Entry) > BLOCK_BYTES ? size + sizeof(Entry) : BLOCK_BYTES);
		s_writeBlock->next = next;

		Entry *end = reinterpret_cast<Entry *>(g_writer.next);
		end->run = nullptr;
		end->size = 0;
		end->reserved = 0;
		++g_writer.written;

		WriteInto(next);

		// That entry counts, and Allocate() only looks for the count it is told to.
		if (g_writer.written == g_writer.publishAt)
			Publish();
	}

	Entry *entry = reinterpret_cast<Entry *>(g_writer.next);
	entry->run = nullptr;
	entry->size = static_cast<uint32_t>(size);
	entry->reserved = 0;
	g_writer.next += size;
	++g_writer.written;
	return entry;
}

//-----------------------------------------------------------------------------
void Publish()
{
	if (!g_queueing || s_published.load(std::memory_order_relaxed) == g_writer.written)
		return;

	// The render thread finds them by itself, at the latest after its nap.
	s_published.store(g_writer.written);
	g_writer.publishAt = g_writer.written + PUBLISH_AFTER_ENTRIES;
}

//-----------------------------------------------------------------------------
const void *Keep(const void *data, size_t size)
{
	if (!g_queueing || data == nullptr || size == 0)
		return data;

	Entry *entry = Allocate(sizeof(Entry) + size);
	memcpy(entry + 1, data, size);
	return entry + 1;
}

//-----------------------------------------------------------------------------
void RunSync(void (*function)(void *context), void *context)
{
	if (!g_queueing)
	{
		function(context);
		return;
	}

	const uint64_t began = Now();
	std::atomic<bool> done(false);
	std::atomic<bool> *finished = &done;
	Defer([function, context, finished]
	{
		// The game's thread waits, and what the function calls is to be done here and now.
		g_queueing = false;
		function(context);
		g_queueing = true;

		finished->store(true);
		WakeGameThread();
	});
	Publish();

	// Most of what is waited for takes less time than going to sleep does.
	for (int i = 0; i < 4000 && !done.load(); ++i)
	{
	}

	if (!done.load())
	{
		pthread_mutex_lock(&s_lock);
		while (!done.load())
			pthread_cond_wait(&s_done, &s_lock);
		pthread_mutex_unlock(&s_lock);
	}

	++s_waits.calls;
	s_waits.callTime += Now() - began;
}

//-----------------------------------------------------------------------------
void EndFrame()
{
	if (!g_queueing)
		return;

	Defer([] { FrameShown(); });
	++s_framesWritten;
	Publish();

	// The render thread may still be drawing the frame before this one. If it is, the
	// game has nothing to gain by getting further ahead.
	if (s_framesWritten - s_framesShown.load() > 1)
	{
		const uint64_t began = Now();
		pthread_mutex_lock(&s_lock);
		while (s_framesWritten - s_framesShown.load() > 1)
			pthread_cond_wait(&s_done, &s_lock);
		pthread_mutex_unlock(&s_lock);
		s_waits.frameTime += Now() - began;
	}
}

//-----------------------------------------------------------------------------
uint32_t FrameBeingWritten()
{
	return s_framesWritten;
}

//-----------------------------------------------------------------------------
bool IsFrameDone(uint32_t frame)
{
	return !g_queueing || static_cast<int32_t>(s_framesShown.load() - frame) > 0;
}

//-----------------------------------------------------------------------------
uint64_t TakeBusyTime()
{
	return s_busyTime.exchange(0);
}

//-----------------------------------------------------------------------------
Waits TakeWaits()
{
	Waits waits = s_waits;
	waits.sleeps = s_sleeps.exchange(0);
	s_waits = Waits();
	return waits;
}

} // namespace RenderThread
} // namespace VitaD3D8
