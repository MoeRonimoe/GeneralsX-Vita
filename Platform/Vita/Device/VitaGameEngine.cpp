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

// VitaGameEngine.cpp /////////////////////////////////////////////////////////
// GameEngine for the PS Vita, see VitaGameEngine.h.
///////////////////////////////////////////////////////////////////////////////

#include "VitaGameEngine.h"

#include "VitaInput.h"
#include "VitaSilentAudioManager.h"

#include "Common/GameAudio.h"
#include "Common/GlobalData.h"
#include "Common/Radar.h"
#include "GameClient/ParticleSys.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"

#if defined(VITA_AUDIO)
#include "VitaAudioManager.h"
#elif defined(SAGE_USE_MINIAUDIO)
#include "MiniAudioDevice/MiniAudioManager.h"
#elif defined(SAGE_USE_OPENAL)
#include "OpenALAudioDevice/OpenALAudioManager.h"
#endif

#include <psp2/kernel/processmgr.h>

#include <VitaDiagnostics.h>
#include <VitaHelper.h>
#include <VitaPacing.h>

#include <algorithm>
#include <cstdlib>

//-----------------------------------------------------------------------------
VitaGameEngine::VitaGameEngine()
{
	// The application always has focus while it runs; suspending freezes the whole process.
	m_isActive = TRUE;
}

//-----------------------------------------------------------------------------
VitaGameEngine::~VitaGameEngine()
{
}

//-----------------------------------------------------------------------------
void VitaGameEngine::init()
{
	VitaInput::init();
	GameEngine::init();

	// "+VITA_PRELOAD=1" in vita_args.txt: the engine can load what the units and buildings
	// of a mission will need while the mission loads, instead of when it is first shown,
	// which stops the game for a moment each time. It is a debugging option in the engine
	// and off by default; here it trades loading time and memory for those stops.
	if (getenv("VITA_PRELOAD") != nullptr && TheWritableGlobalData != nullptr)
		TheWritableGlobalData->m_preloadAssets = TRUE;
}

//-----------------------------------------------------------------------------
void VitaGameEngine::reset()
{
	GameEngine::reset();
}

//-----------------------------------------------------------------------------
/// The average of the times without the slowest of them, in milliseconds. Sorts the times.
static double averageWithoutSlowest(uint32_t *times, UnsignedInt count, UnsignedInt &left)
{
	// One in thirty: what happens more often than that is part of the game's work.
	const UnsignedInt dropped = (count + 15) / 30;
	left = count - dropped;
	if (left == 0)
		return 0.0;

	std::sort(times, times + count);
	uint64_t sum = 0;
	for (UnsignedInt i = 0; i < left; ++i)
		sum += times[i];
	return sum / 1000.0 / left;
}

//-----------------------------------------------------------------------------
/// Writes to the log how long stretches of 300 steps of the game's logic took to work out
/// and draw.
///
/// The engine runs one step of the logic per frame as long as it cannot keep up with 30 per
/// second, and until the player does something, the same steps of a mission or of the menu's
/// background battle are the same work in every run: the same scripts, the same camera,
/// the same objects. Two runs that are played by hand differ by more than most changes to
/// the code gain; the time for these stretches differs by what changed. So: start the
/// mission, leave the controls alone for two minutes, and compare the lines.
///
/// What is counted is the time inside GameEngine::update() of the frames in which the logic
/// took a step. That leaves out what the frame rate limit waits, and the frames of a pause:
/// the messages of the training mission stop the game until they are read.
///
/// The line also says how much of the work that is shared with the helper thread
/// (VitaHelper.h) the helper did. None means that it slept through all of it.
///
/// A frame in which a file is read or a window is built takes a second, and one such frame
/// is 4 ms on the average of its stretch. How long it takes depends on the memory card. So
/// the line also has the average without the slowest frames, which is the number to compare.
///
/// Two runs of the same build still differ by a millisecond per frame, which is as much as
/// most changes gain by now. With "+VITA_AB=1" in vita_args.txt half of the steps run
/// without what is being tried (see VitaGameEngine::update()), and a second line has the
/// time of the steps with it and of those without: frames near each other are nearly the
/// same work, so the two numbers differ by what was tried and by little else.
static void noteFrameForBenchmark(UnsignedInt frameBefore, uint64_t workMicroseconds, Bool alternating, Bool without)
{
	enum { FIRST_FRAME = 150, STRETCH_FRAMES = 300, STRETCHES = 8 };

	static Bool inShell = FALSE;
	static UnsignedInt stretchStartFrame = 0;
	static UnsignedInt stretchFrames = 0;
	static uint64_t stretchWork = 0;
	static uint32_t frameWork[STRETCH_FRAMES];
	static Bool frameWithout[STRETCH_FRAMES];

	if (TheGameLogic == nullptr || !TheGameLogic->isInGame())
	{
		stretchFrames = 0;
		return;
	}

	// Only a frame that was one step of the same game counts.
	const UnsignedInt frame = TheGameLogic->getFrame();
	const Bool shell = TheGameLogic->isInShellGame();
	if (shell != inShell || frame < frameBefore)
		stretchFrames = 0;
	inShell = shell;

	if (frame != frameBefore + 1 || frame <= FIRST_FRAME || frame > FIRST_FRAME + STRETCHES * STRETCH_FRAMES)
		return;

	unsigned int passes = 0;
	unsigned int helperPasses = 0;
	if (stretchFrames == 0)
	{
		// What a stretch that was given up had counted is not part of this one.
		stretchStartFrame = frameBefore;
		stretchWork = 0;
		VitaHelper::takePassCounts(passes, helperPasses);
		VitaHelper::takeLoopTimes();
	}
	if (stretchFrames < STRETCH_FRAMES)
	{
		frameWork[stretchFrames] = static_cast<uint32_t>(workMicroseconds);
		frameWithout[stretchFrames] = without;
	}
	++stretchFrames;
	stretchWork += workMicroseconds;

	if ((frame - FIRST_FRAME) % STRETCH_FRAMES == 0)
	{
		// A stretch that was entered in its middle, after a game was loaded, says nothing.
		if (stretchFrames == STRETCH_FRAMES)
		{
			VitaHelper::takePassCounts(passes, helperPasses);

			// The two halves first, because working out the figure for all sorts the times.
			static uint32_t times[STRETCH_FRAMES];
			UnsignedInt count[2] = { 0, 0 };
			UnsignedInt left[2] = { 0, 0 };
			double average[2] = { 0.0, 0.0 };
			for (Int half = 0; half < 2; ++half)
			{
				for (UnsignedInt i = 0; i < STRETCH_FRAMES; ++i)
				{
					if ((frameWithout[i] ? 1 : 0) == half)
						times[count[half]++] = frameWork[i];
				}
				average[half] = averageWithoutSlowest(times, count[half], left[half]);
			}

			UnsignedInt leftOfAll = 0;
			const double averageOfAll = averageWithoutSlowest(frameWork, STRETCH_FRAMES, leftOfAll);

			fprintf(stderr, "[bench] %s, logic frames %u to %u: %.1f ms per frame, %.1f without the slowest %u; helper made %u of %u passes\n",
				shell ? "menu background" : "mission", static_cast<unsigned int>(stretchStartFrame),
				static_cast<unsigned int>(frame), stretchWork / 1000.0 / stretchFrames, averageOfAll,
				static_cast<unsigned int>(STRETCH_FRAMES - leftOfAll), helperPasses, passes);

			const VitaHelper::LoopTimes loops = VitaHelper::takeLoopTimes();
			if (alternating && left[0] != 0 && left[1] != 0)
			{
				fprintf(stderr, "[bench]   %.2f ms per frame with what is being tried (%u frames), %.2f ms without (%u frames), each without its slowest; "
					"a pass of a shared loop took %.1f us with the helper and %.1f us without\n",
					average[0], static_cast<unsigned int>(left[0]), average[1], static_cast<unsigned int>(left[1]),
					loops.sharedPasses != 0 ? static_cast<double>(loops.sharedMicroseconds) / loops.sharedPasses : 0.0,
					loops.alonePasses != 0 ? static_cast<double>(loops.aloneMicroseconds) / loops.alonePasses : 0.0);
			}
			fflush(stderr);
		}
		stretchFrames = 0;
		stretchWork = 0;
	}
}

//-----------------------------------------------------------------------------
/// Writes to the log every ten seconds of a mission how fast the game ran and how many
/// pictures it showed: 30 steps of the logic per second are the game at its speed.
static void notePace(UnsignedInt frameBefore)
{
	const uint64_t PERIOD_US = 10 * 1000 * 1000;

	static uint64_t periodStart = 0;
	static UnsignedInt steps = 0;

	const Bool inMission = TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
	const uint64_t now = VitaDiagnostics::now();
	if (!inMission || periodStart == 0)
	{
		unsigned int drawn = 0;
		unsigned int leftOut = 0;
		VitaPacing::takeCounts(drawn, leftOut);
		periodStart = inMission ? now : 0;
		steps = 0;
		return;
	}

	if (TheGameLogic->getFrame() == frameBefore + 1)
		++steps;

	if (now - periodStart < PERIOD_US)
		return;

	unsigned int drawn = 0;
	unsigned int leftOut = 0;
	VitaPacing::takeCounts(drawn, leftOut);
	const double seconds = (now - periodStart) / 1000000.0;
	fprintf(stderr, "[pace] %.1f steps of the logic and %.1f pictures per second, %u pictures left out; catching up is %s\n",
		steps / seconds, drawn / seconds, leftOut, VitaPacing::isCatchUp() ? "on" : "off");
	fflush(stderr);

	periodStart = now;
	steps = 0;
}

//-----------------------------------------------------------------------------
void VitaGameEngine::update()
{
	serviceWindowsOS();

	const UnsignedInt frameBefore = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;

	// "+VITA_AB=1": half of the steps of the logic run without what is being tried, for
	// noteFrameForBenchmark(). What is being tried is whatever is switched here; it has to
	// be something that leaves the picture and the game as they are.
	//
	// Which half: the steps whose number has an odd count of one bits. Taking every other
	// step would put everything the game does on every second, fourth or eighth step into
	// one of the halves; this order has as many of each in both.
	static const Bool alternating = getenv("VITA_AB") != nullptr && atoi(getenv("VITA_AB")) != 0;
	const Bool without = alternating && (__builtin_popcount(frameBefore) & 1) != 0;
	if (alternating)
		VitaHelper::setEnabled(!without);

	const uint64_t start = VitaDiagnostics::now();
	GameEngine::update();
	const uint64_t work = VitaDiagnostics::now() - start;
	noteFrameForBenchmark(frameBefore, work, alternating, without);
	notePace(frameBefore);

	// How long frames with and without a picture take decides how many pictures may be left
	// out (VitaPacing.h). Only frames in which the logic took a step are frames of a game
	// that runs.
	VitaPacing::noteFrame(work / 1000000.0f, TheGameLogic != nullptr && TheGameLogic->isInGame()
		&& !TheGameLogic->isInShellGame() && TheGameLogic->getFrame() == frameBefore + 1);
}

//-----------------------------------------------------------------------------
void VitaGameEngine::serviceWindowsOS()
{
	// The console dims its screen and then goes to sleep when nobody touches it for a
	// while, which a game with cutscenes and battles to watch must not let happen. This
	// starts all of its timers over.
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

	// The samples of the sampler say whether they were taken in a mission or in the menu,
	// whose background is a game as well, and in which step of the mission's logic: that
	// tells the stretches of a mission apart, and a number that stays is a game that stands
	// still. A mission that is still being loaded has not counted a frame yet.
	VitaDiagnostics::g_sampleTag = TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()
		&& TheGameLogic->getFrame() > 1 ? TheGameLogic->getFrame() : 0;

	VitaInput::poll();
}

//-----------------------------------------------------------------------------
GameLogic *VitaGameEngine::createGameLogic()
{
	return NEW W3DGameLogic;
}

//-----------------------------------------------------------------------------
GameClient *VitaGameEngine::createGameClient()
{
	return NEW W3DGameClient;
}

//-----------------------------------------------------------------------------
ModuleFactory *VitaGameEngine::createModuleFactory()
{
	return NEW W3DModuleFactory;
}

//-----------------------------------------------------------------------------
ThingFactory *VitaGameEngine::createThingFactory()
{
	return NEW W3DThingFactory;
}

//-----------------------------------------------------------------------------
FunctionLexicon *VitaGameEngine::createFunctionLexicon()
{
	return NEW W3DFunctionLexicon;
}

//-----------------------------------------------------------------------------
LocalFileSystem *VitaGameEngine::createLocalFileSystem()
{
	return NEW StdLocalFileSystem;
}

//-----------------------------------------------------------------------------
ArchiveFileSystem *VitaGameEngine::createArchiveFileSystem()
{
	return NEW StdBIGFileSystem;
}

//-----------------------------------------------------------------------------
Radar *VitaGameEngine::createRadar(Bool dummy)
{
	if (dummy)
		return NEW RadarDummy;

	return NEW W3DRadar;
}

//-----------------------------------------------------------------------------
WebBrowser *VitaGameEngine::createWebBrowser()
{
	// The embedded browser is Windows COM; the game runs without one.
	return nullptr;
}

//-----------------------------------------------------------------------------
ParticleSystemManager *VitaGameEngine::createParticleSystemManager(Bool dummy)
{
	if (dummy)
		return NEW ParticleSystemManagerDummy;

	return NEW W3DParticleSystemManager;
}

//-----------------------------------------------------------------------------
AudioManager *VitaGameEngine::createAudioManager(Bool dummy)
{
#if defined(VITA_AUDIO)
	// "+VITA_NO_AUDIO=1" in vita_args.txt runs the game silent, to compare frame times.
	if (!dummy && getenv("VITA_NO_AUDIO") == nullptr)
		return NEW VitaAudioManager;
#elif defined(SAGE_USE_MINIAUDIO)
	if (!dummy)
		return NEW MiniAudioManager;
#elif defined(SAGE_USE_OPENAL)
	if (!dummy)
		return NEW OpenALAudioManager;
#endif

	// Headless run, or a build without an audio backend: the game runs silent.
	return NEW VitaSilentAudioManager;
}
