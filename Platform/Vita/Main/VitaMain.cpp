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

// VitaMain.cpp ///////////////////////////////////////////////////////////////
// Entry point for the PS Vita. Replaces WinMain(): prepares the console
// (memory, clocks, network, working directory) and runs GameMain().
//
// The retail game data is expected in ux0:data/<game>/ (the contents of the
// installed game directory), where <game> is VITA_GAME_FOLDER: Generals for the
// base game, GeneralsZH for Zero Hour (vita_package() in Platform/Vita/CMakeLists.txt).
// Everything the game writes goes to ux0:data/<game>/UserData/. Extra command
// line arguments can be put into ux0:data/<game>/vita_args.txt, separated by
// whitespace.
///////////////////////////////////////////////////////////////////////////////

#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/Debug.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/version.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"

#include "VitaGameEngine.h"

#include <VitaDiagnostics.h>
#include <VitaPacing.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

// MEMORY ---------------------------------------------------------------------
// newlib sizes its heap from this symbol. The engine, its memory pools and all decoded
// assets live there; vitaGL takes the rest of the application budget for GPU memory.
extern "C"
{
	unsigned int _newlib_heap_size_user = VITA_NEWLIB_HEAP_MB * 1024U * 1024U;

	// Script evaluation and pathfinding recurse deeply; Windows gave the main thread 1 MiB.
	unsigned int sceUserMainThreadStackSize = 2U * 1024U * 1024U;
}

// GLOBALS the engine expects from the platform entry point ----------------------
int __argc = 0;					///< global argument count, read by CommandLine.cpp
char **__argv = nullptr;		///< global argument vector

HWND ApplicationHWnd = nullptr;	///< there is no window; the display is the device

const Char *g_csfFile = "data/%s/generals.csf";	///< string file per language
const Char *g_strFile = "data/Generals.str";	///< plain text fallback of the string file

extern Int GameMain();

#ifdef VITA_PGO_GENERATE
// libgcov of a freestanding toolchain turns the counters of one translation unit into the
// contents of its .gcda file and leaves writing them to the application.
extern "C"
{
	struct gcov_info;
	void __gcov_info_to_gcda(const gcov_info *info, void (*filenameFunction)(const char *, void *),
		void (*dumpFunction)(const void *, unsigned, void *), void *(*allocateFunction)(unsigned, void *), void *arg);
	void __gcov_filename_to_gcfn(const char *filename, void (*dumpFunction)(const void *, unsigned, void *), void *arg);

	// Start and end of the section the compiler registers the translation units in
	// (-fprofile-info-section=gcovinfo); the linker defines both for a section of that name.
	extern const gcov_info *const __start_gcovinfo[];
	extern const gcov_info *const __stop_gcovinfo[];
}
#endif

namespace
{

#if !defined(VITA_GAME_FOLDER) || !defined(VITA_GAME_CHECK_FILE)
#error "VITA_GAME_FOLDER and VITA_GAME_CHECK_FILE come from vita_package() (Platform/Vita/CMakeLists.txt)"
#endif
#define VITA_DATA_DIRECTORY "ux0:data/" VITA_GAME_FOLDER

const char *const GAME_DATA_PATH = VITA_DATA_DIRECTORY;
const char *const USER_DATA_PATH = VITA_DATA_DIRECTORY "/UserData";
#ifdef VITA_PGO_GENERATE
// The run that collects the profile keeps the log of the last ordinary run intact.
const char *const LOG_FILE_PATH = VITA_DATA_DIRECTORY "/UserData/vita_log_pgo.txt";
const char *const PREVIOUS_LOG_FILE_PATH = VITA_DATA_DIRECTORY "/UserData/vita_log_pgo_previous.txt";
const char *const PROFILE_DATA_PATH = VITA_DATA_DIRECTORY "/UserData/pgo_profile.gcfn";
#else
const char *const LOG_FILE_PATH = VITA_DATA_DIRECTORY "/UserData/vita_log.txt";
const char *const PREVIOUS_LOG_FILE_PATH = VITA_DATA_DIRECTORY "/UserData/vita_log_previous.txt";
#endif
const char *const EXTRA_ARGS_PATH = VITA_DATA_DIRECTORY "/vita_args.txt";
// A file the game's data has, to tell that it was copied to GAME_DATA_PATH.
const char *const GAME_CHECK_FILE = VITA_GAME_CHECK_FILE;
const char *const BUNDLED_FONTS_PATH = "app0:fonts";

// The network stack needs its own memory before the first socket is opened.
const int NET_MEMORY_SIZE = 1 * 1024 * 1024;

// Arguments every launch starts with. The panel resolution is fixed, and the animated
// 3D menu background costs more memory and frame time than the console can spare. The
// intro movie is played, the EA logo before it is not; "-nologo" in vita_args.txt leaves
// out both.
const char *const s_defaultArgs[] =
{
	"GeneralsVita",
	"-xres", "960",
	"-yres", "544",
	"-noshellmap",
	"-noEALogo",
};

enum { MAX_ARGS = 64 };
char *s_argv[MAX_ARGS + 1];
char s_extraArgs[2048];

CriticalSection s_critSec1;
CriticalSection s_critSec2;
CriticalSection s_critSec3;
CriticalSection s_critSec4;
CriticalSection s_critSec5;

//-----------------------------------------------------------------------------
/// Default arguments followed by the whitespace separated words of vita_args.txt.
void buildCommandLine()
{
	int argc = 0;
	for (const char *arg : s_defaultArgs)
		s_argv[argc++] = const_cast<char *>(arg);

	FILE *file = fopen(EXTRA_ARGS_PATH, "r");
	if (file != nullptr)
	{
		const size_t length = fread(s_extraArgs, 1, sizeof(s_extraArgs) - 1, file);
		fclose(file);
		s_extraArgs[length] = '\0';

		for (char *word = strtok(s_extraArgs, " \t\r\n"); word != nullptr && argc < MAX_ARGS; word = strtok(nullptr, " \t\r\n"))
		{
			// "+NAME=VALUE" sets an environment variable instead of being passed to the game.
			// The platform layer reads its switches for experiments on the console that way.
			char *equals = strchr(word, '=');
			if (word[0] == '+' && equals != nullptr)
			{
				*equals = '\0';
				setenv(word + 1, equals + 1, 1);
				fprintf(stderr, "vita_args.txt: %s = %s\n", word + 1, equals + 1);
				continue;
			}

			s_argv[argc++] = word;
		}
	}

	s_argv[argc] = nullptr;
	__argc = argc;
	__argv = s_argv;
}

//-----------------------------------------------------------------------------
/// The Vita starts applications at a reduced clock; the game needs everything it can get.
void raiseClocks()
{
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
}

//-----------------------------------------------------------------------------
/// Bring up the network stack. Failure only disables LAN play, so it is not fatal.
void startNetwork()
{
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

	static char netMemory[NET_MEMORY_SIZE];
	SceNetInitParam param;
	param.memory = netMemory;
	param.size = sizeof(netMemory);
	param.flags = 0;

	const int result = sceNetInit(&param);
	if (result < 0 && result != static_cast<int>(SCE_NET_ERROR_EBUSY))
		fprintf(stderr, "WARNING: sceNetInit failed: 0x%08X\n", static_cast<unsigned int>(result));

	sceNetCtlInit();
}

//-----------------------------------------------------------------------------
/// Tells the console every few seconds that it is in use, and the log every half minute how far
/// the logic has come, which times a replay check step by step.
int keepAwakeThread(SceSize, void *)
{
	const SceUInt64 start = sceKernelGetProcessTimeWide();
	for (unsigned int tick = 1;; ++tick)
	{
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
		sceKernelDelayThread(5 * 1000 * 1000);

		// Read without a lock: a frame number that is one step old does not matter here.
		if (tick % 6 == 0 && TheGameLogic != nullptr)
		{
			fprintf(stderr, "[main] headless: logic frame %u after %.0f s\n", static_cast<unsigned int>(TheGameLogic->getFrame()),
				(sceKernelGetProcessTimeWide() - start) / 1000000.0);
		}
	}
	return 0;
}

//-----------------------------------------------------------------------------
/// The frames of the game tell the console that it is in use (VitaGameEngine::serviceWindowsOS).
/// "-headless" (with "-replay <file>": check a replay without drawing it) has none, and the
/// console would dim its screen and go to sleep in the middle of the check.
void keepAwakeWhenHeadless()
{
	for (int i = 1; i < __argc; ++i)
	{
		if (strcmp(__argv[i], "-headless") != 0)
			continue;

		// Behind the game's threads (160): it has nothing urgent to do.
		const SceUID thread = sceKernelCreateThread("keep awake", keepAwakeThread, 191, 16 * 1024, 0, 0, nullptr);
		if (thread >= 0)
			sceKernelStartThread(thread, 0, nullptr);
		fprintf(stderr, "[main] headless: no frames are drawn, the console is kept awake\n");
		return;
	}
}

//-----------------------------------------------------------------------------
/// Send the engine's diagnostics to a file on the memory card; there is no console.
void openLog()
{
	// Created even when the game data is missing, so that the log can say so.
	mkdir(GAME_DATA_PATH, 0777);
	mkdir(USER_DATA_PATH, 0777);
	VitaDiagnostics::g_userDataPath = USER_DATA_PATH;

	// The log of the run before stays, under another name. Whoever starts the game again
	// after it ended on its own would otherwise have nothing left to tell why it did.
	remove(PREVIOUS_LOG_FILE_PATH);
	rename(LOG_FILE_PATH, PREVIOUS_LOG_FILE_PATH);

	// Both streams go into the file, and both append: a stream that writes at a position of its
	// own writes over what the other one wrote there (the replay checks print to stdout).
	FILE *created = fopen(LOG_FILE_PATH, "w");
	if (created != nullptr)
		fclose(created);

	if (freopen(LOG_FILE_PATH, "a", stderr) != nullptr)
		setvbuf(stderr, nullptr, _IOLBF, 0);

	if (freopen(LOG_FILE_PATH, "a", stdout) != nullptr)
		setvbuf(stdout, nullptr, _IOLBF, 0);
}

#ifdef VITA_PGO_GENERATE

struct ProfileStream
{
	FILE *file;
	size_t bytes;
	bool failed;
};

//-----------------------------------------------------------------------------
void writeProfileChunk(const void *data, unsigned size, void *arg)
{
	ProfileStream *stream = static_cast<ProfileStream *>(arg);
	if (fwrite(data, 1, size, stream->file) != size)
		stream->failed = true;
	stream->bytes += size;
}

//-----------------------------------------------------------------------------
void writeProfileFilename(const char *filename, void *arg)
{
	__gcov_filename_to_gcfn(filename, writeProfileChunk, arg);
}

//-----------------------------------------------------------------------------
void *allocateProfileMemory(unsigned size, void *arg)
{
	return malloc(size);
}

//-----------------------------------------------------------------------------
/// Write the counters of all translation units as one stream, the form that
/// "gcov-tool merge-stream" unpacks into .gcda files on the build machine.
void writeProfileData()
{
	ProfileStream stream = { fopen(PROFILE_DATA_PATH, "wb"), 0, false };
	if (stream.file == nullptr)
	{
		fprintf(stderr, "[pgo] cannot create %s\n", PROFILE_DATA_PATH);
		return;
	}

	// The stream is a few megabytes in small pieces; the memory card is slow at those.
	static char buffer[256 * 1024];
	setvbuf(stream.file, buffer, _IOFBF, sizeof(buffer));

	unsigned units = 0;
	for (const gcov_info *const *info = __start_gcovinfo; info != __stop_gcovinfo; ++info, ++units)
		__gcov_info_to_gcda(*info, writeProfileFilename, writeProfileChunk, allocateProfileMemory, &stream);

	if (fclose(stream.file) != 0)
		stream.failed = true;

	fprintf(stderr, "[pgo] %s: %u translation units, %u bytes%s\n", PROFILE_DATA_PATH, units,
		static_cast<unsigned>(stream.bytes), stream.failed ? ", WRITING FAILED" : "");
	fflush(stderr);
}

#endif // VITA_PGO_GENERATE

} // namespace

//-----------------------------------------------------------------------------
GameEngine *CreateGameEngine()
{
	return NEW VitaGameEngine;
}

//-----------------------------------------------------------------------------
int main(int argc, char *argv[])
{
	int exitcode = 1;

	raiseClocks();
	openLog();

	fprintf(stderr, "Command & Conquer Generals - PS Vita\n");
	VitaDiagnostics::install();
	VitaDiagnostics::logMemory("at start");

	if (chdir(GAME_DATA_PATH) != 0 || access(GAME_CHECK_FILE, F_OK) != 0)
	{
		fprintf(stderr, "FATAL: no game data in %s. Copy the contents of the installed game directory there.\n", GAME_DATA_PATH);
		fflush(stderr);
		sceKernelExitProcess(1);
		return 1;
	}

	// GlobalData builds the user data directory from this, as it does on Linux.
	setenv("XDG_DATA_HOME", USER_DATA_PATH, 1);

	// The fonts ship inside the package. This directory is searched first.
	setenv("GX_BUNDLE_FONTS", BUNDLED_FONTS_PATH, 1);

	startNetwork();
	buildCommandLine();
	keepAwakeWhenHeadless();

	// "+VITA_PATHFIND_LINE_CACHE=0": the pathfinder checks every cell on its lines again, as
	// the original did. For showing with a replay that skipping the repeated checks changes nothing.
	if (getenv("VITA_PATHFIND_LINE_CACHE") != nullptr && atoi(getenv("VITA_PATHFIND_LINE_CACHE")) == 0)
	{
		Pathfinder::setRepeatedLineChecksSkipped(FALSE);
		fprintf(stderr, "[main] the pathfinder checks every cell on its lines again\n");
	}

	// "+VITA_PROFILE=1" in vita_args.txt: measure where the time of a frame goes and write
	// it to the log every few seconds. Off by default, because measuring costs time itself.
	VitaDiagnostics::setProfiling(getenv("VITA_PROFILE") != nullptr);

	// "+VITA_SAMPLE=250": look where this thread is that many times per second, and write
	// what was seen to a file at the end.
	VitaDiagnostics::startSampling();

	// "+VITA_MEMTEST=1": measure what the console is good at before the game starts.
	if (getenv("VITA_MEMTEST") != nullptr)
		VitaDiagnostics::runMachineTest();

	// The game keeps its speed by leaving pictures out, see VitaPacing.h, unless
	// "+VITA_CATCHUP=0" asks for one step of the logic per picture.
	VitaPacing::setCatchUp(getenv("VITA_CATCHUP") == nullptr || atoi(getenv("VITA_CATCHUP")) != 0);

	try
	{
		TheAsciiStringCriticalSection = &s_critSec1;
		TheUnicodeStringCriticalSection = &s_critSec2;
		TheDmaCriticalSection = &s_critSec3;
		TheMemoryPoolCriticalSection = &s_critSec4;
		TheDebugLogCriticalSection = &s_critSec5;

		// The memory manager has to exist before the first NEW.
		initMemoryManager();

		// GameEngine::init() reads the version for the window title.
		TheVersion = NEW Version;

		CommandLine::parseCommandLineForStartup();

		exitcode = GameMain();
		fprintf(stderr, "GameMain() returned %d\n", exitcode);
		VitaDiagnostics::finishSampling();
		VitaDiagnostics::logMemory("at exit");
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "FATAL: unhandled exception: %s\n", e.what());
	}
	catch (...)
	{
		fprintf(stderr, "FATAL: unhandled exception\n");
	}

	delete TheVersion;
	TheVersion = nullptr;

	shutdownMemoryManager();

	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	fflush(stderr);

#ifdef VITA_PGO_GENERATE
	writeProfileData();
#endif

	// Skip the global destructors: the object pools they would walk are already gone,
	// exactly as with ExitProcess() on Windows.
	sceKernelExitProcess(exitcode);
	return exitcode;
}
