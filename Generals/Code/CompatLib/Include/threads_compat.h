/*
**	Command & Conquer Generals Zero Hour(tm)
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

// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
// Cross-platform threading primitives (Win32 critical sections for Linux)

#pragma once

#ifndef THREADS_COMPAT_H
#define THREADS_COMPAT_H

#ifdef _UNIX

// POSIX threads support
#include <pthread.h>

#if defined(__vita__)

// GeneralsX @performance Claude 13/10/2026 A lock for the PS Vita that costs nothing when nobody
// else wants it. The engine takes a critical section for every string it copies or lets go of and
// for every block it takes from or returns to a memory pool, thousands of times per frame, nearly
// always with no other thread in sight. Through a recursive pthread mutex each of those went
// into the thread library twice and looked up the calling thread there; samples of the game's
// thread showed that library's return addresses all over its stack. Here the owner is the
// value of the processor's thread register, which is the address of the thread's own storage
// and so differs from thread to thread, and taking a free lock is one compare-and-swap.

#ifdef __cplusplus
extern "C" {
#endif

int sceKernelDelayThread(unsigned int microseconds);

typedef struct _CRITICAL_SECTION_COMPAT {
    volatile unsigned int owner;    // thread register of the thread that holds it, 0 while free
    int ref_count;                  // how often that thread has entered; only the owner touches it
} CRITICAL_SECTION;

static inline unsigned int VitaThreadRegister(void) {
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

inline void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    cs->owner = 0;
    cs->ref_count = 0;
}

inline void DeleteCriticalSection(CRITICAL_SECTION *cs) {
    cs->owner = 0;
    cs->ref_count = 0;
}

inline void EnterCriticalSection(CRITICAL_SECTION *cs) {
    const unsigned int self = VitaThreadRegister();

    // Only this thread writes its own value there, so reading it back means it holds the lock.
    if (cs->owner != self) {
        unsigned int tries = 0;
        unsigned int expected = 0;
        while (!__atomic_compare_exchange_n(&cs->owner, &expected, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            expected = 0;

            // The holder is on another core and through in a moment, as a rule. If it was
            // taken off its core instead, it needs this one: sleeping lets a thread of any
            // priority run, which yielding would not.
            if (++tries >= 200) {
                sceKernelDelayThread(50);
                tries = 0;
            }
        }
    }

    cs->ref_count++;
}

inline void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (--cs->ref_count == 0)
        __atomic_store_n(&cs->owner, 0, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#else // __vita__

// Emulate Windows CRITICAL_SECTION with pthread_mutex_t
// Based on fighter19's CritSec implementation
typedef struct _CRITICAL_SECTION_COMPAT {
    pthread_mutex_t mutex;
    pthread_t owner;     // Thread ID of current owner (for recursive locking)
    int ref_count;       // Recursion depth counter
} CRITICAL_SECTION;

// Windows-style critical section API wrappers
#ifdef __cplusplus
extern "C" {
#endif

// Initialize a critical section (maps to pthread_mutex_init)
inline void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    
    // Enable recursive locking (Windows CRITICAL_SECTION is recursive by default)
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    cs->owner = 0;
    cs->ref_count = 0;
}

// Destroy a critical section (maps to pthread_mutex_destroy)
inline void DeleteCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_destroy(&cs->mutex);
    cs->owner = 0;
    cs->ref_count = 0;
}

// Enter a critical section (blocking lock)
inline void EnterCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_lock(&cs->mutex);
    cs->owner = pthread_self();
    cs->ref_count++;
}

// Leave a critical section (unlock)
inline void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    cs->ref_count--;
    
    if (cs->ref_count == 0) {
        cs->owner = 0;
        pthread_mutex_unlock(&cs->mutex);
    }
}

#ifdef __cplusplus
}
#endif

#endif // __vita__

#endif // _UNIX

#endif // THREADS_COMPAT_H
