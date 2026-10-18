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

// VitaD3D8RenderThread.h /////////////////////////////////////////////////////
// A second thread that makes the calls to vitaGL.
//
// The game does everything on one thread and the console has three cores for it.
// What this layer spends inside vitaGL, a fifth of a frame, does not depend on
// anything the game does next, so it can be done while the game goes on: the
// thread the game runs on writes each call into a stream instead of making it,
// and a thread that does nothing else reads the stream and makes the calls, in
// the order they were written.
//
// All calls to vitaGL are made by that thread, from the first to the last; vitaGL
// is used by one thread as before, only by another one. The calls that return
// something, or that take data too large to copy, are made by it as well, while
// the game's thread waits for them.
//
// What a call points to has to be there when the call is made, which is later
// than when it was written. Keep() copies data into the stream for that.
//
// The game may be one frame ahead of what is on the screen and no more.
//
// This file knows nothing about vitaGL; VitaD3D8GL.h puts the calls on top of it.
// tools/renderthread_test.cpp runs it on a PC.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace VitaD3D8
{
namespace RenderThread
{

/// The beginning of every entry of the stream.
struct Entry
{
	void (*run)(Entry *entry);	///< what the render thread does with the entry; null for data and for the end of a block
	uint32_t size;				///< of the whole entry in bytes; 0 at the end of a block
	uint32_t reserved;
};

/// True while calls are to be written to the stream: the thread runs and is not in the
/// middle of something it does for the game's thread, which waits and reads this then.
extern bool g_queueing;

/// Data up to this size is copied into the stream; a call with more is waited for.
enum { MAX_KEPT_BYTES = 1024 * 1024 };

/// Starts the thread, unless the environment says not to ("+VITA_RENDER_THREAD=0" in
/// vita_args.txt). Without the thread every call is made where it is written.
bool Start();
bool IsRunning();

/// Where the game's thread writes. Nothing else touches this; it is in the header so that
/// writing an entry is a few instructions and no call. The game writes ten thousand of them
/// per frame.
struct Writer
{
	unsigned char *next;	///< where the next entry goes
	unsigned char *limit;	///< entries end here; behind it is room for the entry that ends the block
	uint32_t written;		///< entries written so far
	uint32_t publishAt;		///< the count at which the render thread is told of them even if nothing was drawn
};
extern Writer g_writer;

/// Allocate() when the block is full or it is time to tell the render thread.
Entry *AllocateSlow(size_t size);

/// Space for an entry of the given size in the stream, with its size filled in. Only to be
/// called while calls are being queued.
inline Entry *Allocate(size_t size)
{
	size = (size + 7) & ~static_cast<size_t>(7);

	Writer &writer = g_writer;
	if (size > static_cast<size_t>(writer.limit - writer.next) || writer.written == writer.publishAt)
		return AllocateSlow(size);

	Entry *entry = reinterpret_cast<Entry *>(writer.next);
	entry->run = nullptr;
	entry->size = static_cast<uint32_t>(size);
	entry->reserved = 0;
	writer.next += size;
	++writer.written;
	return entry;
}

/// Lets the render thread see what was written. Called after everything that draws.
void Publish();

/// Copies data into the stream and returns where it is there, for a call that is
/// written right after. Returns the data itself when calls are not being queued.
const void *Keep(const void *data, size_t size);

/// Has the render thread call the function and waits until it has. Called from the
/// render thread itself, or without one, it just calls.
void RunSync(void (*function)(void *context), void *context);

/// Called after the call that shows the frame: waits if the frame before it is not on
/// the screen yet.
void EndFrame();

/// The number of the frame the game's thread is writing calls for.
uint32_t FrameBeingWritten();

/// True once the render thread has made every call of the frame with that number. Without
/// a render thread every call is made when it is written.
bool IsFrameDone(uint32_t frame);

/// Microseconds the render thread spent making calls since the last time this was asked.
uint64_t TakeBusyTime();

/// What the game's thread spent waiting for the render thread since the last time this
/// was asked: for calls it had to wait for, and for the frame before the one it finished.
struct Waits
{
	uint32_t calls;
	uint64_t callTime;	///< microseconds
	uint64_t frameTime;
	uint32_t sleeps;	///< times the render thread found nothing to do for so long that it went to sleep
};
Waits TakeWaits();

//-----------------------------------------------------------------------------
/// Has the render thread run something, later. The function object is copied into the
/// stream and must not refer to anything that may be gone by then.
template <typename Function>
inline void Defer(Function function)
{
	if (!g_queueing)
	{
		function();
		return;
	}

	struct Deferred
	{
		Entry entry;
		Function function;
	};
	static_assert(std::is_trivially_destructible<Function>::value, "nothing destroys what is in the stream");
	static_assert(alignof(Deferred) <= 8, "entries are aligned to 8 bytes");

	Deferred *deferred = reinterpret_cast<Deferred *>(Allocate(sizeof(Deferred)));
	new (&deferred->function) Function(function);
	deferred->entry.run = [](Entry *entry) { reinterpret_cast<Deferred *>(entry)->function(); };
}

//-----------------------------------------------------------------------------
/// Keeps a parameter from having a say in what the template's arguments are: those come
/// from the function alone, and what is passed is converted to them as in a plain call.
template <typename Type>
struct AsDeclared
{
	typedef Type type;
};

/// Has the render thread call a function with arguments that are plain values.
template <typename... Arguments>
inline void Call(void (*function)(Arguments...), typename AsDeclared<Arguments>::type... arguments)
{
	if (!g_queueing)
	{
		function(arguments...);
		return;
	}

	Defer([function, arguments...] { function(arguments...); });
}

//-----------------------------------------------------------------------------
/// Has the render thread run something and waits for it. The function object may refer
/// to anything of the caller's.
template <typename Function>
inline void Sync(Function &&function)
{
	if (!g_queueing)
	{
		function();
		return;
	}

	typedef typename std::remove_reference<Function>::type Plain;
	RunSync([](void *context) { (*static_cast<Plain *>(context))(); }, const_cast<void *>(static_cast<const void *>(&function)));
}

} // namespace RenderThread
} // namespace VitaD3D8
