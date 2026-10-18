# PS Vita platform setup (VitaSDK + vitaGL).
# Included right after project() so the settings apply to every target.
#
# Configure with:  cmake --preset vita   (requires the VITASDK environment variable)

if(NOT VITA)
    return()
endif()

if(NOT DEFINED VITASDK OR NOT EXISTS "${VITASDK}/share/vita.cmake")
    message(FATAL_ERROR "VITASDK is not set or does not point to a VitaSDK installation")
endif()

# The VitaSDK toolchain reports CMAKE_SYSTEM_NAME "Generic". The Vita port reuses the POSIX
# code paths of the Linux/macOS port (CompatLib, StdDevice file system, FreeType text),
# which the build system selects with UNIX.
set(UNIX TRUE)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_TITLE_ID "GENR00001" CACHE STRING "Nine character title id of the Vita package")
set(VITA_APP_VERSION "01.00" CACHE STRING "Version stored in param.sfo (NN.NN)")
# The engine, its memory pools and the decoded assets all live on the C heap. vitaGL takes
# what is left of the application budget for its own texture and vertex pools.
#
# Of the 365 MiB of main memory an application with the extended budget gets, about 20 go
# to the executable and the thread stacks and 16 are left unclaimed (VITAGL_RAM_RESERVE).
# The heap gets most of the rest: vitaGL has the GPU's own memory besides, and it falls
# back to the heap when its pools are full, while the engine cannot use vitaGL's pools.
# With 220 MiB the heap was already 172 MiB deep when a mission started to load.
set(VITA_NEWLIB_HEAP_MB "300" CACHE STRING "Size of the newlib heap in MiB")
# The engine has timers in its major functions (USE_PERF_TIMER). With this option their
# share of an average frame is written to the log every few seconds. Changing it rebuilds
# everything.
option(VITA_FRAME_PROFILE "Write the time spent in the engine's timed functions to the log" OFF)
# A build for measuring where the game's thread spends its time. Every function says that it
# was entered (-pg; Platform/Vita/compat/VitaSampler.cpp has what they call), and with
# "+VITA_SAMPLE=250" in vita_args.txt a thread takes down where the game's thread is that
# often. It costs a quarter or more of the frame rate, so give it a build directory of its own:
#   cmake --preset vita -B build/vita-prof -DVITA_CALL_PROFILE=ON
option(VITA_CALL_PROFILE "Have every function mark that it was entered, for the sampler" OFF)
# Profile guided optimization, in two builds of the same sources in the same build directory:
#   GENERATE  counts how often every branch is taken. The game writes the counts to
#             UserData/pgo_profile.gcfn when it is left through its menu;
#             "arm-vita-eabi-gcov-tool merge-stream pgo_profile.gcfn" on the build machine
#             turns that file into the .gcda files next to the objects.
#   USE       lets the compiler lay out and inline the code according to those counts.
# See Platform/Vita/README.md.
option(VITA_AUDIO "Sound, through miniaudio and the console's audio output" OFF)
set(VITA_PGO "OFF" CACHE STRING "Profile guided optimization: OFF, GENERATE or USE")
set_property(CACHE VITA_PGO PROPERTY STRINGS OFF GENERATE USE)

add_compile_options(
    # arm-vita-eabi defaults to the smallest enum type that fits. The INI and Xfer code
    # writes enum fields through Int pointers, exactly as it does on x86.
    -fno-short-enums
    # The engine puns between float and int all over the place.
    -fno-strict-aliasing
    -fsigned-char
    -fno-math-errno
    -ffunction-sections
    -fdata-sections
    -Wno-psabi
)

add_link_options(
    # The prebuilt SDK archives use short enums. None of the interfaces we call pass
    # enum-typed struct members, so the mixed attribute is only noise.
    -Wl,--no-enum-size-warning
    -Wl,--gc-sections
    # vita-elf-create appends Sony's module information (imports, exports, process
    # parameters) to the end of the code segment. The space for it is only what page
    # alignment happens to leave before the data segment, which is too little for the
    # number of system libraries this game imports. The linker script reserves more
    # when this symbol is defined. Without it the Windows build of vita-elf-create
    # crashes with heap corruption instead of reporting the overlap.
    -Wl,--defsym=__sce_headroom=0x4000
    # libstdc++ reaches the thread library through weak references, and a weak reference
    # does not pull a member out of libpthread.a. The engine links some thread functions
    # itself, which makes libstdc++ consider threads active, while the ones only libstdc++
    # uses stay unresolved and their calls become no-ops. The first guarded static local
    # then locks a mutex that pthread_once never created and the process dies before
    # main(). Requesting the symbols makes the linker take them from the archive.
    -Wl,--undefined=pthread_once
    -Wl,--undefined=pthread_cond_init
    -Wl,--undefined=pthread_cond_signal
    -Wl,--undefined=pthread_cond_broadcast
    -Wl,--undefined=pthread_cond_wait
    -Wl,--undefined=pthread_cond_timedwait
    -Wl,--undefined=pthread_cond_destroy
    -Wl,--undefined=sched_yield
)

# newlib hides the POSIX declarations (strdup, strcasecmp, fileno, ...) in strict ISO mode.
# _UNIX selects the POSIX branch of the GameSpy SDK, which only recognises Linux and macOS.
add_compile_definitions(_GNU_SOURCE _UNIX)

# GameLogic keeps the state of the last 64 frames in memory to explain multiplayer
# desyncs, in buffers of 72 MiB that it allocates at start-up. GameDefines.h switches this
# on whenever the macro is undefined, so RTS_BUILD_OPTION_DEEP_CRC=OFF in the preset is
# not enough to keep it out.
add_compile_definitions(DEEP_CRC_TO_MEMORY=0)

# The engine blends the two key frames a raw animation is between (WWDefines.h). The retail
# game did not: it shows the key frame the time falls into, with code that reads the
# channels directly. The blend is a quaternion interpolation and two lookups per channel for
# every animated bone on every frame, and at the frame rates of this console the time moves
# on by more than a key frame from one frame to the next, so there is nothing in between to
# show. Changing it rebuilds everything.
add_compile_definitions(WW3D_ENABLE_RAW_ANIM_INTERPOLATION=0)

if(VITA_FRAME_PROFILE)
    add_compile_definitions(VITA_FRAME_PROFILE)
endif()

if(VITA_CALL_PROFILE)
    add_compile_options(-pg)
    add_compile_definitions(VITA_CALL_PROFILE)
endif()

if(VITA_PGO STREQUAL "GENERATE")
    # The SDK's libgcov is the freestanding one: it has no code to write files and none of
    # the value profilers (indirect calls, time of first run), so those are left out, and
    # the records are registered in a section that Platform/Vita/Main/VitaMain.cpp walks.
    # The counters are plain increments; the few lost to races between threads do not matter.
    add_compile_options(
        -fprofile-generate
        -fno-profile-values
        -fprofile-update=single
        -fprofile-info-section=gcovinfo
    )
    add_compile_definitions(VITA_PGO_GENERATE)
    add_link_options(-fprofile-arcs)
elseif(VITA_PGO STREQUAL "USE")
    add_compile_options(
        -fprofile-use
        -fno-profile-values
        # Counters lost to races make some counts inconsistent; let the compiler smooth them.
        -fprofile-correction
        # Sources without counts (never run, or changed since) are compiled as usual.
        -Wno-missing-profile
        -Wno-error=coverage-mismatch
    )
elseif(NOT VITA_PGO STREQUAL "OFF")
    message(FATAL_ERROR "VITA_PGO must be OFF, GENERATE or USE, not '${VITA_PGO}'")
endif()

# <sys/ioctl.h> and <dlfcn.h>, which newlib lacks. Implemented by the vita_compat library.
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/Platform/Vita/compat/include")

message(STATUS "PS Vita build: VITASDK=${VITASDK}, title id ${VITA_TITLE_ID}")
