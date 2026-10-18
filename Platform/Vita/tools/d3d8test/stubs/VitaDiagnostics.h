// What the Direct3D 8 layer uses of compat/include/VitaDiagnostics.h, for building it on a
// PC (see ../driver.cpp). mockgl.cpp has the functions.
#pragma once

#include <cstddef>
#include <cstdint>

namespace VitaDiagnostics
{

struct MemoryState
{
	size_t heapUsed;
	size_t heapGrown;
	size_t heapLimit;
};

MemoryState getMemoryState();

enum TimeSlot
{
	TIME_GAME_LOGIC,
	TIME_SLOT_COUNT
};

uint64_t now();

extern bool g_profiling;

inline uint64_t profileStart() { return g_profiling ? now() : 0; }

extern uint32_t g_drawCalls;

uint64_t takeTime(TimeSlot slot);

void logNamedTimers(uint32_t frames);

} // namespace VitaDiagnostics
