# PS Vita port

This directory holds everything that is specific to running Command & Conquer Generals
on the PlayStation Vita. The game code itself is the cross-platform tree of this
repository (GeneralsX, based on TheSuperHackers' modernisation of EA's GPL release); the
Vita build reuses its POSIX code paths and replaces the parts that need a desktop.

## Status

**On a console the game starts and reaches the main menu: the menus, their texts and the
pointer work. The training mission can be played: its opening sequence plays, units can
be commanded and fight, a game can be saved, and the mission and the game can be left
again. It runs at about 16 frames per second there, slower in a battle. Units, buildings
and roads look right; effects such as explosions and smoke and the water have not been
checked since they were last reported wrong. Music, sound effects and voices play (see
"Sound").** Everything behind that is unverified; expect it to fail
and to need fixes guided by the log file and core dumps.

| Area | State |
| --- | --- |
| Engine, game logic, WW3D renderer | compile unchanged for ARM (Cortex-A9, 32 bit, hard float) |
| Graphics | Direct3D 8 implemented on vitaGL's fixed function pipeline (`d3d8/`); menus and 3D scenes render, with the approximations listed under "How the graphics layer works" |
| Input | touch screen, sticks and buttons mapped to mouse and keyboard (`Device/`); works in the menus |
| Mouse pointer | a plain arrow drawn by `VitaMouse`; the game's animated `.ani` cursors are not loaded, and the retail data has no other pointer images |
| Audio | sound effects, speech and music through miniaudio (`Device/VitaAudioManager`, `Device/VitaAudioLoader`), behind the build option `VITA_AUDIO`; heard on a console, see "Sound" for what is still open |
| Video (intro, campaign movies) | Bink through FFmpeg when `tools/build-ffmpeg.sh` has been run, see "Movies"; skipped without it |
| Network play | sockets are wired up through newlib, untested |
| Zero Hour | not built yet; only the base game target `g_generals` has a Vita package |

## Layout

| Path | Contents |
| --- | --- |
| `d3d8/` | `IDirect3D8` / `IDirect3DDevice8` on vitaGL. Replaces `d3d8.dll`, as DXVK does on Linux. `VitaD3D8RenderThread` and `VitaD3D8GL.h` are the optional second thread for the calls to vitaGL. |
| `Device/` | `VitaGameEngine`, `VitaMouse`, `VitaKeyboard`, `VitaInput`, `VitaAudioManager` with `VitaAudioLoader`, and the silent audio and video stand-ins |
| `Main/` | `main()`: heap size, CPU clocks, network start-up, working directory, command line |
| `compat/` | the few POSIX functions newlib lacks on the Vita (`ioctl`, `getifaddrs`, `fnmatch`, `uname`, ...), the diagnostics written to the log, `VitaMachineTest`, which measures the console itself, and `VitaHelper`, a thread the game's thread shares loops with. `include/VitaD3D8Tags.h` is what the engine can tell the Direct3D layer about a resource |
| `sce_sys/` | LiveArea images and the script that draws them |
| `tools/` | scripts that turn a core dump or the log of a failed run into function names; `tools/audiotest/` runs the sound's loading code on a PC, `tools/renderthread_test.cpp` the render thread's stream, `tools/helper_test.cpp` the helper thread, `tools/frustum_test.cpp` and `tools/matrix_test.cpp` the NEON code of the view tests and of the bones (with `tools/neon_on_pc/`, the intrinsics in plain C++), `tools/d3d8test/` the whole Direct3D layer against a stand-in for vitaGL; `tools/samples.py` and `tools/time_table.py` read what the measurements write |
| `CMakeLists.txt` | the targets above and `vita_package()`, which produces the `.vpk` |
| `FixMiniaudio.cmake` | makes the corrected copy of `miniaudio.h` that is compiled, see "Sound" |

Outside this directory the port consists of `cmake/vita.cmake`, the `vita` preset in
`CMakePresets.json` and small `__vita__` / `VITA` branches next to the existing Linux and
macOS ones.

## Building

Requirements: [VitaSDK](https://vitasdk.org) with the packages `vitaGL`, `zlib`,
`freetype`, `libpng` and `glm`, CMake 3.28 or newer and Ninja. The `VITASDK` environment
variable has to point at the SDK.

```sh
cmake --preset vita
cmake --build build/vita --target g_generals.vpk-vpk
```

The package is written to `build/vita/Generals/Code/Main/g_generals.vpk`.

Two libraries are better built here than taken from the SDK, each by a script in `tools/`
that puts them where the vita preset looks first; without them the game builds with the
SDK's vitaGL and without movies:

- `tools/build-vitagl.sh build/vita-vitagl`: the SDK's vitaGL, the same commit built the
  same way (`make NO_DEBUG=1`), with three patches from `patches/`:
  - `vitagl-light-range.patch` gives a point light a range beyond which it gives no light
    (`GL_LIGHT_RANGE_VGL`), as Direct3D does; without it the flashes of guns and
    explosions light the far sides of buildings.
  - `vitagl-texcoord-gen.patch` lets the fixed function shaders compute texture
    coordinates from the camera space position, normal or reflection vector
    (`vglTexCoordGen`), as Direct3D's `D3DTSS_TCI_CAMERASPACE*` modes do; vitaGL has no
    texture coordinate generation of its own. It also keeps the compiled fixed function
    shaders in a folder of their own (`vglSetFfpShaderCachePath`, here
    `ux0:data/Generals/shader_cache`): vitaGL keeps them in `ux0:data/shader_cache/v31`,
    which other vitaGL applications share and which is named only after vitaGL's version,
    and it took shaders compiled from the unpatched sources from there.
  - `vitagl-specular-power.patch` keeps the lighting from computing `pow(0, 0)` for the
    specular term, which the GPU makes NaN. vitaGL computes it for every light, also when
    the material has no specular color, and with the shininess of 0 that most of the
    game's materials have, faces lit by the sun but turned away from the camera came out
    white; walls and fences flickered while the camera turned.

  `VitaD3D8State.cpp` and `VitaD3D8Device.cpp` use what the patches add when the vitaGL
  header has it. A change of the patches' shader sources has to raise
  `FFP_SHADER_CACHE_PATCH` (in `vitagl-texcoord-gen.patch`, raised by later patches), or
  the shaders compiled before are taken from the cache.
- `tools/build-ffmpeg.sh build/vita-ffmpeg`: FFmpeg with the Bink decoders, see "Movies".

On Windows both run in MSYS2 with its MinGW compiler; the head of each script says how.

Build options of interest (`cmake --preset vita -D<name>=<value>`):

| Option | Default | Meaning |
| --- | --- | --- |
| `VITA_NEWLIB_HEAP_MB` | 300 | Size of the C heap. vitaGL gets what is left of the 365 MiB of main memory (about 20 MiB are taken by the executable and the stacks), in addition to the GPU's own memory. |
| `VITA_TITLE_ID` | GENR00001 | Title id of the package |
| `VITA_FRAME_PROFILE` | OFF | Compiles in what the frame profile needs: the engine's own timed functions (`[time]` lines) and the list of every combination of fixed function settings the game draws with (`[d3d8] state` lines). Both stay silent until `VITA_PROFILE` is set at run time, see below. Rebuilds everything. |
| `VITA_AUDIO` | OFF | Builds the game with sound, see "Sound". Fetches miniaudio 0.11.23. |
| `VITA_PGO` | OFF | Profile guided optimization, `GENERATE` or `USE`. See "Profile guided optimization". Rebuilds everything. |
| `VITA_CALL_PROFILE` | OFF | A build for measuring: every function marks that it was entered, for the sampler. See "Finding out where the time goes". Rebuilds everything; use a build directory of its own. |

## Installing

1. The console needs HENkaku/Ensō and `libshacccg.suprx`, which vitaGL uses to compile its
   shaders at run time (the usual requirement of vitaGL homebrew).
2. Install `g_generals.vpk` with VitaShell.
3. Copy the contents of an installed copy of the game (the directory with `INI.big`,
   `Textures.big`, `W3D.big`, ...) to `ux0:data/Generals/`. The game data is not part of
   this repository and has to come from a copy you own.

The game writes its options, saved games and the log to `ux0:data/Generals/UserData/`.
The log is `ux0:data/Generals/UserData/vita_log.txt`; it is the first thing to look at
when something does not work. The log of the run before is kept next to it as
`vita_log_previous.txt`, because the game is easily started once more after it has
ended on its own.

Extra command line arguments can be put into `ux0:data/Generals/vita_args.txt`, separated
by whitespace. The port always starts the game with `-xres 960 -yres 544 -noshellmap
-noEALogo`. A word of the form `+NAME=VALUE` in that file sets an environment variable
instead; the platform layer reads its switches for experiments that way:

| Variable | Effect |
| --- | --- |
| `VITA_PROFILE` | measures where the time of a frame goes and writes it to the log every ten seconds; costs a few percent of the frame itself |
| `VITA_TOUCH_SNAP_RADIUS` | how far, in panel pixels, a tap may miss a unit and still count as a tap on it (default 24, 0 switches the help off) |
| `VITA_D3D8_TOP_MIP_ONLY` | compressed textures are uploaded without their mip levels |
| `VITA_NO_AUDIO` | a build with sound runs silent, to compare frame times |
| `VITA_AUDIO_CACHE_MB` | memory for decoded sound effects (default 40) |
| `VITA_RENDER_THREAD` | `0` has the game's thread make the calls to vitaGL itself, as it did before there was a render thread (see "The render thread") |
| `VITA_BUFFERED_READS` | large reads go through the C library's buffer again, a kilobyte per request (see the notes at the end) |
| `VITA_SAMPLE` | in a measuring build (`VITA_CALL_PROFILE`): samples per second of the game's thread, written to `vita_samples.bin` at the end (see "Finding out where the time goes") |
| `VITA_CATCHUP` | `0`: one step of the logic per picture, the game in slow motion where the console cannot draw 30 pictures per second (see "The speed of the game") |
| `VITA_AB` | half of the steps of the logic run without what is being tried, for the `[bench]` lines (see "Finding out where the time goes") |
| `VITA_MEMTEST` | measures the console's memory and cores before the game starts (see "What the console is good at") |
| `VITA_PRELOAD` | what a mission's units and buildings need is loaded with the mission, not when first shown (`m_preloadAssets` of the engine). Tried once: the training mission then stood at 95 % for a long time, and the game ended with an exception when units were selected. The log of that run was lost, so the reason is not known; running out of memory is the likely one. |

## Sound

A build with `-DVITA_AUDIO=ON` plays the game's sound effects, speech and music. On a
console, music, effects and voices have been heard and sound right. The first version
stopped the game for two minutes of a twelve minute mission while it opened sound files
on the game's thread; what is described here is the second, which has run on a PC only
(`tools/audiotest/`) and still has to show on a console that those stops are gone.

`Device/VitaAudioManager` derives from the engine's `MiniAudioManager`, the audio back end
of the Linux and macOS builds, and replaces what the console needs done differently:

- **Output.** miniaudio knows no audio device of the Vita. Its engine mixes without one
  (48 kHz stereo) and a thread hands 1024 frames at a time to `sceAudioOutOutput()`, which
  blocks until the console has taken them. Mixing took 4 ms of the 21 ms a block lasts in
  the training mission, on a core the game does not use.
- **Nothing waits on the game's thread.** One read from the memory card takes a
  millisecond or more whatever its size, a WAV header is a dozen reads, and whoever asks
  miniaudio's resource manager for anything waits until its decoding thread has finished
  what it is decoding. `Device/VitaAudioLoader` does all of that on a thread of its own.
  A sound whose file is not in memory when the game asks for it is a stand-in: a sound
  that plays nothing and takes what a sound is told (volume, position, when to start).
  The real sound takes its place, set up the same, when the file is there. An effect
  that would start more than 0.7 s late is not played.
- **Sound effects** are read from the card in one piece each, decoded whole (16 bit PCM
  and IMA ADPCM in WAV files, by miniaudio's decoder) and kept in memory: 40 MiB, of which
  a mission uses 30; `+VITA_AUDIO_CACHE_MB=n` changes the limit. A sound of an effect that
  is in memory costs no more than the sound itself. miniaudio's resource manager is not
  used for them.
- **Music and speech** are decoded while they play, by miniaudio's resource manager (MP3
  and PCM). The loader opens them and lets go of them, because both wait for the decoding
  thread. They read through file callbacks with a 64 KiB buffer.
- **Files.** The archive directory belongs to the game's thread. The manager looks a file
  up there and names it `>archive|offset|size|name`; the loader reads that part of the
  archive by position through descriptors of its own.
- **What follows a sound.** `MiniAudioManager` lets go of every sound when it ends. A
  third of the game's sound events are loops (engines, weapons, ambience), most of them a
  sequence of files with a file to begin and a file to end with. The manager moves an
  event on as the original Miles code did: the rounds of a loop, attack and decay, a pause
  between rounds, the music starting over, counted for the scripts that wait for it. A
  sound effect that is told to stop plays to its end and is followed by its decay.
- **Loops without gaps.** The game looks at its sounds once per frame, and a frame takes
  60 to 100 ms. What follows a sound effect is therefore made ready 300 ms before the
  effect ends and given the time to start at. miniaudio starts a sound with the first
  piece of a block that begins at or after that time; the engine mixes in pieces of 256
  frames, which puts the start within 3 ms of the end before it.
- **Pausing** holds the sounds that are playing and drops what was asked for but has not
  begun, as the Miles code did. `MiniAudioManager` stops its sound groups instead, which
  also silences what starts during the pause: the game pauses its sound for a message
  that has a sound and a voice of its own.
- **Positional sounds.** Their position follows whatever they are attached to; a sound
  whose owner is gone is treated as stopped, and one that has moved out of its range is let
  go of (whoever plays it starts it again in range, as with Miles). The listener's "up" is
  set to z, the game's; with miniaudio's default the listener, who looks along y, has no
  left and right.

Lines of the log start with `[audio]`. With `VITA_PROFILE` set, the log also says what
mixing a block costs and how many sounds had to wait for their file and for how long, and
has the timers `Audio_loadSound` and `Audio_playingList` in its `[time]` lines.

miniaudio is pinned to release 0.11.23. Its resource manager writes to memory it has
freed when a sound that it decodes into memory is let go of while it is still loading.
`FixMiniaudio.cmake` explains the two lines and makes the corrected copy of the header
that is compiled; the fetched release is not changed, and with another release the build
stops there on purpose. With two decoding threads the same happened even when corrected,
so there is one. Since sound effects no longer go through the resource manager, the
manager does not come near those lines any more; the correction stays for whoever does.

`tools/audiotest/` runs the loader and miniaudio on a PC against the game's archives:
`stresstest.cpp` makes and lets go of sounds as fast as it can and looks for writes to
freed memory, `assumptions.cpp` checks what the manager relies on (an effect plays at its
own speed, a stand-in hands on what it was told, a stopped sound goes on where it was).
Their first lines say how to build and run them. Both should be run again before the
pinned release or the way sounds are created and let go of changes.

Everything that sounds at one moment is added up, and the sum can be larger than what
the output takes. Cut off there it rasps, which a dozen explosions at the end of a
mission did. The thread that hands the blocks to the console turns a block down as a
whole when its loudest sample does not fit and lets the following ones up again over half
a second (`VitaAudioManager::outputThread()`). When that happened, or when mixing a
block took more than three quarters of the time the block lasts, a line in the log says
so about every ten seconds. At the end of the training mission (run41) up to 181 of the
480 blocks of ten seconds were too loud, by up to 2.1 times, which had been cut off before;
mixing took 14 ms of the 21 ms a block lasts at most, so the output never ran dry.

Not done: the random pitch of sound effects and pausing a single sound. The sound of a
movie goes through the stream the engine keeps for movies, see "Movies".

## The language of the game

There is no registry on the console, and `GeneralsX/registry.ini` in the game's directory
has no language unless somebody writes one there. The game then takes the language of the
first of its language archives it finds, in the order of `tryAutoDetectLanguage()` in
`Common/System/registry.cpp`: English before German before German2. The German release
on Steam keeps its texts in `German2.big` and the voices in `AudioGerman2.big` and
`SpeechGerman2.big`. Its cuts (soldiers that speak like robots, other names for units, a
GLA mission left out) are all in those files and in `INI.big` and `Maps.big`; the code has
no switch for them.

A German patch that lifts the cuts was found to bring German texts, voices and speech as
`English.big`, `AudioEnglish.big` and `SpeechEnglish.big`, with a new `INI.big` (voices,
sound effects, campaign), `Maps.big` (the GLA mission) and `PatchData.big`, and to leave
the program alone. Copied into the game's directory on the memory card those take the
English place in the order above and are used without anything else to do; deleting the
three English archives and putting back the other three files goes back.

## Movies

The game's movies are Bink files: the intro (`sizzle_review.bik`, 800x600 at 30 frames
per second with sound) and the EA logo in `Data/<language>/Movies`, and in `Data/Movies`
one for the loading screen of each mission of a campaign (800x600 at 15 frames per second,
53 seconds, no sound; two GLA missions have 27 seconds at 30 with sound) and placeholders
of a second for the victories. The game looks for the language folder of the language it
runs in (see above): with the German patch that is `Data/English/Movies`, a copy of
`Data/german2/Movies`. The Vita starts with `-noEALogo`, a switch of this port that leaves
out the logo and plays the intro; `-nologo` in `vita_args.txt` leaves out both.

They are decoded with FFmpeg, through the engine's FFmpeg player
(`Core/GameEngineDevice/Source/VideoDevice/FFmpeg`), compiled for the console by
`Device/VitaFFmpegVideoPlayer.cpp`. The SDK's FFmpeg package has no Bink, so
`tools/build-ffmpeg.sh` builds FFmpeg 9.0.2 with nothing but the Bink demuxer, the Bink
video and audio decoders and libswscale into `build/vita-ffmpeg`, where the vita preset
finds it (`VITA_FFMPEG_DIR`); without it the build skips the movies. On Windows the script
runs in MSYS2, see its head. It builds with `-fno-short-enums` like the game: the
compiler's default makes enums as small as their values, and with FFmpeg built that way
the game read the frame counter of a movie from the wrong place in FFmpeg's structures and
waited for a frame that never came (a black screen until Start).

On the Vita a movie goes into a 16 bit texture (`W3DDisplay::createVideoBuffer()`), which
the Direct3D layer takes without converting it, and `compat/VitaYuv.cpp` converts the
frames to it with NEON (libswscale has no ARM code for that and took 25 ms for a frame;
`tools/yuv_test.cpp` checks the NEON code against plain code and against the exact
colours). A frame that is late by a frame or more is decoded but not shown, unless the last
one shown is a tenth of a second old, so a movie keeps pace with its sound. The log has
`[movie]` lines with the time of decoding and of converting a frame and how many frames
were left out.

The engine skipped the video player's update while the intro ran, and the FFmpeg player
starts the sound of a movie there; `GameClient::update()` now updates it during the intro
as well.

## Finding out where the time goes

Two runs that are played by hand differ by more than most changes to the code gain: the
median frame of the same build came out 5 ms apart. What can be compared are the
`[bench]` lines of the log. The engine runs one step of its logic per frame while it
cannot keep up with 30 per second, and until the player does something, the same steps of
a mission are the same work in every run. `VitaGameEngine` writes how long each stretch
of 300 logic frames took, from frame 150 to frame 2550. So: start the mission, leave the
controls alone for the first two minutes, and compare those lines between builds. A
frame in which a file is read or a window is built takes a second, which is 4 ms on the
average of its stretch and depends on the memory card; the number to compare is the
second one of the line, the average without the slowest thirtieth of the frames. Each
line ends with how many of the passes that were shared with the helper thread the helper
made.

Two runs of the same build still differ by about a millisecond per frame. For a change
that can be switched while the game runs there is a finer measurement: with `+VITA_AB=1`
in `vita_args.txt`, `VitaGameEngine::update()` switches what is being tried off for half
of the steps of the logic, and each `[bench]` line is followed by the time of the steps
with it and of those without. Frames near each other are nearly the same work, so the two
numbers differ by what was tried and by little else. The half is the steps whose number
has an odd count of one bits: every other step would put whatever the game does on every
second or fourth step into one half. What is switched is written in `update()`; at the
moment it is the helper thread.

**What the console is good at.** `+VITA_MEMTEST=1` has `compat/VitaMachineTest.cpp`
measure the console before the game starts, in a second. At 444 MHz:

| blocks of | fill | copy | write vertices |
| --- | --- | --- | --- |
| 8 KiB (first cache) | 2584 MB/s | 1431 MB/s | 359 MB/s |
| 512 KiB (second cache) | 2418 MB/s | 799 MB/s | 360 MB/s |
| 8 MiB (memory) | 460 MB/s | 195 MB/s | 190 MB/s |
| 8 MiB, two threads at once, each | 316 MB/s | 139 MB/s | 141 MB/s |

Two threads that only compute are as fast together as one alone: they have a core each.
Two threads that move memory get 1.4 times what one gets. So a megabyte that the game's
thread copies costs it 5 ms, whoever else is busy, and that is what its large loops cost:
the skins of a scene full of soldiers are 800 KiB of vertices, written in 4.4 ms and
then copied for the render thread in 4 ms more, and neither NEON nor a second core made
the writing faster (see the notes at the end). What helps is moving fewer bytes. The log
of a frame profile says how many the draw calls copy.

With `+VITA_PROFILE=1` in `vita_args.txt` the log gets two lines every ten seconds. The
`[d3d8]` line splits the average frame into draw calls (state translation, vertex arrays,
vitaGL), game logic, uploads and the rest. The `[time]` line lists the timed functions of
the engine with their milliseconds per frame, how often they ran and how many draw calls
were issued inside them; this needs a build with `VITA_FRAME_PROFILE=ON`. The times are
inclusive, a function counts in all the timed functions that call it. A `~` marks a
function that runs so often that only a sample of its calls is timed and the sum is
scaled up. Without the variable the log only has the frame rate and the number of draw
calls. `python Platform/Vita/tools/time_table.py vita_log.txt GameEngine_update
Mesh_rigid Mesh_rigid:d` prints chosen timers of all those lines as a table, with the
`[bench]` lines in between (`:d` the draw calls, `:n` the calls). Where the game stands
still for a message of the mission, the table has the same numbers line after line: the
time to draw one picture without any game logic, to a tenth of a millisecond.

The engine's timers are the `DECLARE_PERF_TIMER`/`USE_PERF_TIMER` pairs all over its
code, many of them commented out; `Common/PerfTimer.h` maps them to the log on the Vita.
WW3D cannot include that header and uses `WW3D_PROFILE_SCOPE` from `WW3D2/frameprofile.h`.

The timers only tell about the places somebody put one. A measuring build asks the other
way round:

```
cmake --preset vita -B build/vita-prof -DVITA_CALL_PROFILE=ON -DVITA_AUDIO=ON
cmake --build build/vita-prof --target g_generals.vpk-vpk
```

In it the compiler puts a call at the start of every function (`-pg`), and the few
instructions that call runs (`__gnu_mcount_nc` in `compat/VitaSampler.cpp`) leave the
function's address and the stack pointer where a thread of its own finds them. With
`+VITA_SAMPLE=250` in `vita_args.txt` that thread takes them down that many times per
second, with the code addresses on the game's stack. When the game is left through its
menu the samples are written to `ux0:data/Generals/UserData/vita_samples.bin`, and

```
python Platform/Vita/tools/samples.py vita_samples.bin g_generals.syms g_generals --frame-ms 80
```

lists the functions by their share of the samples, alone and with what they call. The
symbols are `arm-vita-eabi-nm -C --defined-only` of the program that ran. The build is a
quarter or more slower, and code that calls a lot looks slower in it than it is; the
script's own description says what else to keep in mind. The other threads run
functions with the same calls; what the calls run looks at the processor's thread
register and leaves a mark only for the game's thread.

Every sample carries the step of the mission's logic it was taken in. `--frames 450-1050`
takes the samples of those steps, `--still` those of steps that lasted a second or more,
which is the game standing still for a message and doing nothing but draw, and `--moving`
the others. What to trust: the list by a function's own time for functions that do real
work, and `--callers` for a function that is called directly. The shares "with what it
calls" are too large for some functions and too small for others, because the stack is
read without knowing its frames; the engine's timers are the better source for those.
And a small function that is called very often has a share that is mostly the call the
measuring build adds to it: the test of a sphere against a plane had 9 % of the samples of
a picture, and doing it four planes at a time took a tenth of a millisecond off the 3 ms
its caller is timed at. Before working on what the samples name, see what the timer
around it says, and add one if there is none.

The simpler way, stopping the game's thread from outside and reading its registers, is
not open to a program like this one: `sceKernelSuspendThreadForVM()` answers 0x80020008.

## Profile guided optimization

The compiler can arrange and inline the code according to how the game actually runs.
That takes two builds of the same sources in one build directory and a run on the console
in between:

```sh
cmake --preset vita -B build/vita-pgo -DVITA_PGO=GENERATE -DVITA_NEWLIB_HEAP_MB=280
cmake --build build/vita-pgo --target g_generals.vpk-vpk
# install, play for a few minutes, leave the game through its menu, then on the PC:
arm-vita-eabi-gcov-tool merge-stream pgo_profile.gcfn
cmake build/vita-pgo -DVITA_PGO=USE -DVITA_NEWLIB_HEAP_MB=300
cmake --build build/vita-pgo --target g_generals.vpk-vpk
```

The first build counts how often every branch is taken, which makes it slower and its
executable larger (hence the smaller heap). The counts are only written when the game
ends through "exit" in its menu, to `UserData/pgo_profile.gcfn`; closing it from the
console's home screen loses them. That build logs to `vita_log_pgo.txt`, so the log of the
last ordinary run survives. `gcov-tool` puts the counts next to the object files, under
the absolute paths of the build directory they were compiled in.

The SDK's `libgcov` is the freestanding one. It cannot write files, which is why
`VitaMain.cpp` does that, and it has no value profilers, which is why both builds pass
`-fno-profile-values`. A source file that changes after the counts were taken is compiled
without them for the functions whose control flow changed; the profile ages with the code
and has to be taken again from time to time.

All steps have been done once: eleven minutes of the training mission gave 3.5 MiB of
counts for 908 source files, and the code of the second build came out 13 % smaller than
that of an ordinary one (what never ran is optimized for size).

**It gained nothing that could be measured.** Played by hand, the mission never shows the
same scenes twice, so the two builds were compared by frame time against the number of
draw calls in each ten-second line of the log: 64.5 ms for the second build and 63.3 ms
for an ordinary one at 560 draw calls, with the lines of either scattering by 9 ms. Most of
a frame is spent in vitaGL and in loops that wait for memory, which the compiler's
knowledge of branches does not help. The ordinary build stays the one to use: it needs
no run on the console, and it does not slow down everything the measuring run did not
happen to reach. An effect of a few percent needs a scene that repeats exactly to be
seen at all.

## When it crashes

The console shows error C2-12828-1 and writes a core dump to
`ux0:data/psp2core-<...>-eboot.bin.psp2dmp`. `tools/parse_core.py` turns it into the
crashed thread, the faulting instruction and the stack; its header comment lists what it
needs. The dump can only be read with the unstripped ELF of exactly the build that is
installed, so keep `build/vita/Generals/Code/Main/g_generals` for every build that goes
onto a console.

When the game closes without that error and without a dump, the engine ended itself after
a C++ exception; it then leaves `UserData/GeneralsX/Generals/ReleaseCrashInfo.txt` behind.
The log names every exception in a `[throw]` line with its type, its value (the engine's
error codes are listed in `Common/Errors.h`) and the call chain. `tools/resolve_log.py`
turns the chain into function names, again with the symbols of exactly the installed
build. Lines starting with `[mem]` (after each engine subsystem has come up) and
`[d3d8] memory` (whenever the numbers moved by several megabytes) show how full the C
heap and vitaGL's pools were along the way. Every ten seconds a `[d3d8]` line gives the
frame rate and the number of draw calls.

While only the executable changes there is no need to reinstall the package: replacing
`ux0:app/GENR00001/eboot.bin` with the `eboot.bin` from the new `.vpk` is enough.

## Controls

| Control | Action |
| --- | --- |
| Front touch | move the pointer and hold the left mouse button (tap to click, drag to select) |
| Left stick | move the pointer |
| Cross / Circle | left / right mouse button |
| Right stick | scroll the map |
| D-pad up / down | zoom |
| D-pad left / right | rotate the camera |
| L / R | Ctrl / Shift |
| Triangle | select all units (Q) |
| Square | attack move (A) |
| Start | Escape |
| Select | jump to the last radar event (Space) |
| L + R + Select | switch between the game at its speed with fewer pictures and one step per picture, see "The speed of the game" |

The map does not scroll when the pointer touches an edge of the screen, whatever
Options.ini says (`LookAtTranslator::canScrollAtScreenEdge()`): the pointer rests at the
edges often when it is moved by the stick or the touch screen, and the right stick scrolls.

The game uses its classic mouse scheme: a left click on the ground or on an enemy gives
the selected units their order, a right click deselects. The mapping lives in
`Device/VitaInput.cpp`. It reads every sample the console has buffered since the last
frame, not only the current state, because a tap is shorter than a frame at low frame
rates.

A fingertip is several times as wide as a soldier on the screen, and the engine picks
what is under one exact pixel. A tap that hits nothing is therefore moved onto the nearest
infantry or vehicle within three millimetres, provided the engine's own picking finds
that unit at the new point. Dragging (a selection box) and the pointer moved with the
stick are not affected.

## The speed of the game

The game's logic takes 30 steps for a second of the game, and the engine takes at most one
step per frame it draws. The console draws about 20 frames per second in an ordinary scene
and 13 in a crowded one, so everything moves at two thirds to two fifths of its speed, and
the voices of a mission, which run by the clock, are done before what they talk about.

The game keeps its speed instead (`compat/VitaPacing`) unless `+VITA_CATCHUP=0` is in
`vita_args.txt`; both shoulder buttons held and Select switch between the two while
playing. The logic
is then owed all the time that passes (`GameEngine::canUpdateRegularGameLogic()`), and
while it is half a step or more behind, `W3DDisplay::draw()` leaves the next picture out,
never two in a row. A frame without a picture takes less than half the time of one with:
everything but the drawing still happens in it, the way the engine's own fast time does
it. Where a picture takes 45 ms that comes to 30 steps and 15 pictures per second. The steps
and what they work out are the same either way, only when they are taken differs.

On the console (run40) playing was found more pleasant that way, and the scenes a mission
shows by itself were not: with sixty soldiers in view a frame without a picture still
takes 37 ms, so leaving out every other picture gave 15 to 19 steps at 7 to 9 pictures
per second where it had been 13 and 13, and a camera that moves shows every missing
picture. So no picture is left out while the mission's script moves the camera, while
the picture is between black bars or while the player's input is switched off, and none
where the pictures would come to be more than a tenth of a second apart on average: a
crowded scene gets two or three pictures between those left out, or all of them
(`VitaPacing::leaveOutPicture()`).

Played that way (run41) the mix was found good, and is the default since. A `[pace]` line
in the log says every ten seconds of a mission how many steps and pictures per second
there were.

What is still felt is the pause when something appears for the first time: the cargo
plane and the paratroopers of the training mission are some thirty models and animations
that are read from the memory card when they are first needed, each in 5 to 20 ms, and
built then, which stops the game for a few seconds at the start of that scene.

## How the graphics layer works

The engine keeps calling Direct3D 8. `d3d8/` records the state it sets and translates it
into fixed function OpenGL right before each draw call, one group of state at a time and
only for the groups that changed. The conventions that differ between the two APIs
(clip space depth range, pixel centres, winding, vertex colour byte order, texture
matrix layout) are bridged in `d3d8/VitaD3D8State.cpp`, which documents each of them.

The device reports the capabilities of a card without programmable shaders and with two
texture stages, so the engine takes the fixed function paths it has for such hardware.
Not offered, and reported as unavailable: vertex and pixel shaders, cube and volume
textures, rendering into textures. Calls outside the implemented set fail and are named
once in the log (`[d3d8] not supported: ...`).

Known approximations:

- Texture coordinates generated from the camera space position, normal or reflection
  vector are exact with the vitaGL of `tools/build-vitagl.sh`. With the SDK's vitaGL they
  are approximated: the object space position or normal goes through the texture matrix
  as two component coordinates, which is exact for layers projected straight down onto
  the terrain (shroud, cloud shadows) and visibly wrong for environment mapping.
- Spot lights are treated as point lights. The range of a light is only applied with the
  vitaGL of `tools/build-vitagl.sh`.
- Fog on pretransformed vertices is not applied.

## The render thread

The game does everything on one thread; the console has three cores for it. Of a frame
of 64 ms in the training mission, 17 ms were spent in this layer's draw calls, 12.5 ms of
that inside vitaGL. A second thread makes all calls to vitaGL: the game's thread writes
each call into a stream (`d3d8/VitaD3D8RenderThread`) and goes on, the render thread
reads the stream and makes the calls in order. vitaGL is still used by one thread only,
from `vglInit` on. On a console this took 10 ms off the game's frame (median 74 instead
of 85 ms with sound and the frame profile on); the render thread spent 14 ms per frame
inside vitaGL, and the game waited for it 0.9 ms per frame, for the three or four calls
it has to wait for. `+VITA_RENDER_THREAD=0` switches the thread off.
`tools/renderthread_test.cpp` runs the stream on a PC with functions that check what
they get in place of vitaGL.

In a second step the translation of Direct3D state to OpenGL went to the render thread
as well. With the thread, the game's thread spent as long writing the single calls to
vitaGL into the stream as vitaGL then took to make some of them: forty entries per draw
call, 6 ms per frame. Now the device is two halves (`VitaD3D8Device.h` says which member
is whose). The methods of `IDirect3DDevice8` keep the state as the engine has set it, to
answer the `Get` calls and to tell whether a `Set` call changes anything, and write
every change and every draw call into the stream as what it is: "render state 27 is 1",
"draw these indices". `VitaD3D8State.cpp` runs where the stream is read. It keeps the
state as it was when the call it is making was written, translates what changed and
calls vitaGL directly; that file does not include `VitaD3D8GL.h`.

- `d3d8/VitaD3D8GL.h` turns the `gl...()` calls of the files that include it into
  entries of the stream by redefining their names. Those are the calls the game's thread
  still makes itself: buffer and texture uploads, and creating and deleting both. Every
  vitaGL function used there has to be listed; one that is not would be called by the
  game's thread while the render thread is inside vitaGL. The header says which four are
  left out on purpose.
- What the render thread's half of the device runs must not write to the stream or wait
  for it (`Defer`, `Sync`, `Keep`): it would wait for itself.
- Textures are the one thing the two halves share, and the render thread is behind: the
  engine may have let go of a texture while draw calls with it are still in the stream.
  So the render thread is not given the texture but what it needs of it
  (`Device::BoundTexture`: the GL texture, whether it has mip levels, and the block with
  the sampler state last applied to it), with the first draw call that uses the stage.
  The game's thread uploads a texture that was written to before it writes the draw call
  (`Device::SendTextures`). The block with the sampler state is freed by an entry in the
  stream, which the render thread reaches after everything that used the texture.
- `tools/d3d8test/` builds the layer on a PC against a stand-in for vitaGL that keeps
  everything the layer sets and writes down what it adds up to at each draw call, for a
  random but repeatable sequence of the calls the game makes. `run.sh` compares the
  trace with and without the render thread, `compare.sh` the traces of two versions of
  the layer. The split into two halves was made against it: 30 sequences of 7000 draw
  calls each come out the same as before the split. Run it after changing the layer.
  With `D3D8TEST_SLOW_DRAWS=400` in the environment every draw call of the stand-in takes
  that many microseconds, which keeps the game's thread a frame ahead of the one that
  draws: memory that is written over before it was drawn from shows as vertices that hash
  to something else. A layer that was broken that way on purpose is caught by it.
- What a call points to has to be there when the call is made, which is later than when
  it was written. Small things (a matrix, a color) are copied into the entry. Vertices and
  indices that are drawn from system memory are copied into the stream by `Draw*()`
  (`RenderThread::Keep()`): the engine fills its dynamic buffers again right after the
  draw call. Buffer uploads are copied up to a megabyte.
- The copy of the vertices is not needed if the buffer is written by the rules Direct3D
  has for dynamic buffers: a lock with `D3DLOCK_DISCARD` gives up everything, a lock with
  `D3DLOCK_NOOVERWRITE` leaves alone what was drawn. Then a discarding lock moves the
  buffer to memory that no draw call still points into (`BufferStorage::TakeUnusedBlock()`:
  the buffer goes round a few blocks, each free again when the frame it was drawn in is
  on the screen), and draw calls point into the buffer. Not all of the game keeps the
  rules: the trees lock their buffer with `D3DLOCK_DISCARD` and write only the trees that
  moved. So the engine says which buffer does, with `SetPrivateData()` and the tag in
  `VitaD3D8Tags.h`, and so far that is WW3D's own dynamic vertex buffer, which carries the
  skins, the particles and everything two-dimensional. A lock of such a buffer that does
  not fit the rules is waited for (`VertexBuffer::PrepareLock()`); the engine has one, for
  skins that did not fit the buffer at the first go.
- Calls that return something are made by the render thread while the game's thread
  waits (`RenderThread::Sync()`): the first upload of a texture, creating buffers, asking
  vitaGL for its free memory. Waiting means waiting for everything written before, a
  millisecond or more once the render thread also translates the state. The game writes
  to about three small textures per frame (shroud, radar, text); their new pixels are
  copied into the stream instead (`Texture::ReplacePixelsLater()`, up to 1 MiB). That
  includes the first upload of a texture: the game makes two new textures per frame for
  text that changed, and their names come from a handful taken from vitaGL ahead of time.
  The log says which uploads are still waited for.
- Direct3D has vertex colors as B, G, R, A and vitaGL reads R, G, B, A. For buffer objects
  the colors are reordered in the copy that is uploaded. For vertices drawn from system
  memory (dynamic buffers, `Draw*UP`) the render thread reorders them in the copy the draw
  call brought along (`Device::KeepVertices()`, `swapColors`). They used to be reordered by
  the game's thread in the buffer at `Unlock()`, for everything that was locked: the tread
  marks on the ground lock 20000 vertices on every frame to write a few. In a buffer that
  is drawn from without a copy the render thread reorders them where they are, once for
  each lock (`VertexBuffer::CommitDirect()`); such a buffer is locked for what is written.
- The game may be one frame ahead of what is on the screen. If the frame before the one
  it has just finished is not shown yet, it waits in `Present()`.
- The render thread looks for more work for 1.5 ms before it goes to sleep, and then
  sleeps in naps of a quarter of a millisecond, looking again after each. The game's
  thread does not wake it: that was a call into the system each time, a millisecond per
  frame when it was measured. The log says how often per frame the thread slept.
- With the thread, the times of the `[d3d8]` line are those of writing the calls down; a
  second line says how long the render thread spent making them.

## Notes for whoever continues

- What the second set of samples showed (run29), after the first had been dealt with:
  - A ninth of a frame was under `HLodClass::Update_Sub_Object_Transforms()`. The game set
    the transform of every model it drew on every frame, which makes the model work out
    its bones, sub-objects and bounding volumes again, for buildings that never move as
    for anything else. `W3DModelDraw::doDrawModule()` now leaves a transform alone that
    is what it was. Animated models update when they are rendered regardless, and
    `Capture_Bone()`/`Release_Bone()` now invalidate the hierarchy themselves, which they
    left to that per-frame call.
  - Every mesh set its four lights again, nearly always the sun, as for the mesh before.
    `DX8Wrapper::Set_Light_Environment()` now keeps what it made the lights from and
    returns when it is asked for the same again and nobody has set a light since. Who sets
    the device's lights past the render state has to say so (`EnvironmentLightCount = -1`):
    the translucent objects of `SortingRendererClass` did not, and until that was found
    walls and fences took their lights at the start of the next frame and flickered.
  - In this layer: the vertex colors and the waking of the render thread, see "The render
    thread".
- What the first samples of the game's thread showed (run26, training mission), none of
  it in a place that had a timer:
  - The replay recorder flushed its file on every frame, because the camera of each frame
    is one of the messages it writes; on the memory card a flush takes 4 ms.
    `RecorderClass::updateRecord()` now flushes every two seconds on the Vita.
  - The engine takes a critical section for every string it copies or frees and for
    every block of its memory pools, thousands of times per frame. Through a recursive
    pthread mutex that went into the thread library twice each time. `threads_compat.h`
    has a lock for the Vita that is one compare-and-swap when nobody else holds it.
  - `GameClient::update()` asked for the shroud status of every object on every frame
    (1800 objects, each a chain of pointers into cold memory). Now it asks for the ones
    whose drawable is marked (`Drawable::markShroudStatusStale()`): the status became
    invalid, the object entered or left the partition system, somebody else set the flag,
    or the object was seen clear less than five seconds ago. This is in the game's own
    code, for all platforms, and it is the one change of this kind that could show as
    wrong fog: an object that stays hidden or stays visible means a way of changing the
    status was missed.
  - Still open: the D3D layer's
    own calls are 6 ms of the game's thread even now that they only write to the stream;
    `TerrainLogic::getWaterHandle()` walks all polygon triggers for every question about
    water; models and textures that are first shown during play are read then.
- Skins (the infantry) are the one kind of mesh whose vertices are computed on every
  frame. On the Vita `DX8SkinFVFCategoryContainer::Render()` has them transformed straight
  into the vertex buffer by `MeshModelClass::compose_deformed_vertex_buffer()`, which uses
  NEON there: a Cortex-A9 takes about five cycles for each of the 33 multiplications and
  additions of a vertex otherwise, and the compiler may not use NEON for them by itself
  without `-ffast-math`, which is not allowed in this code base.
- The game has three cores: one runs the game, one the render thread, and the third did
  nothing. `compat/VitaHelper` is a thread for it. `VitaHelper::forEach()` runs a loop
  whose passes do not depend on each other on the calling thread and the helper together;
  each takes the next pass with one compare-and-swap until none are left. The helper
  looks for the next loop for a millisecond after its last one and then waits for a
  semaphore, which the game's thread signals when it opens a loop and the helper has said
  that it sleeps: a few calls into the system per frame. The skins are the first loop
  that uses it, one pass per skin. What a pass may do is narrow: the engine's allocators,
  strings and reference counts belong to the game's thread, and so does everything that
  is worked out on first use. That is why `Render()` asks for each model's vertex normals
  before it shares the skins out. `tools/helper_test.cpp` runs the helper on a PC. Waking
  the helper takes the time of a few skins, so a set of fewer than 24 is not shared.
- The engine blends raw animations between their key frames, which the retail game did
  not (`WW3D_ENABLE_RAW_ANIM_INTERPOLATION` in `WWDefines.h`). `cmake/vita.cmake` switches
  that off, for poses that at 15 to 20 frames per second are more than a key frame apart
  anyway.
- Those two together (run33) gained nothing that the `[bench]` lines could show: the two
  stretches with the most soldiers came out 0.8 and 0.9 ms shorter, the others 0.6 ms
  longer on average, and two runs of one build differ by as much. The helper made 36 to
  62 % of the passes, so it was not asleep; the samples had simply promised more than
  was there. A sample names the function that was entered last, not the one that is
  running: what a caller does between two calls counts for the function that has just
  returned. `Fast_Slerp()` had 1.9 % of the samples that way, with the matrix its caller
  builds from the result.
- Why sharing the skins gained so little was measured next (run34, run35): frames with
  and without the helper side by side, and the console itself, see "What the console is
  good at". Writing the vertices of a skin is not arithmetic but 44 bytes per vertex into
  memory that is in no cache, and two cores write 1.4 times what one writes. A set of 60
  skins took 40 us per skin shared and 57 us alone, a frame with many soldiers about a
  millisecond less; a set of 8 took longer shared than alone. The same explains why NEON
  changed little.
- What the third set of samples showed (run37), with nothing above 5 % any more:
  - Whether a sphere is in the view was asked for each of the 1900 objects of the map one
    plane at a time, and whether a box is for every mesh of those that are, through twelve
    function calls each: a tenth of the samples of a picture. `WWMath/colmathfrustumneon.h`
    asks four planes at a time. `tools/frustum_test.cpp` compares it on a PC with the tests
    it replaces: the same answers for seven million spheres and boxes. The compiler turns
    `vcgtq_f32()` into four single comparisons unless it may be careless with arithmetic;
    the file says how it asks instead.
  - `Drawable::updateDrawable()` runs for every drawable on every frame and wrote three of
    its members whether they changed or not, and asked every object whether it is dead to
    stop a glow that nearly none has. A write makes the processor carry the memory back out
    of its cache. Now it writes what changes and asks when there is a glow.
  - In this layer, per draw call: the layout of the vertex format was worked out again and
    copied into the stream (92 bytes), and the textures of the stages were looked at again.
    The layout comes from a table now (`GetCachedLayout()`), and the stages are looked at
    when a texture was set or written or a color operation changed (`SendTextures()`).
  - What those three measured (run38): the picture of a standing game went from 25.5 to
    24.4 ms and the stretches with many soldiers lost 0.8 and 1.5 ms. Nearly all of it is
    the draw calls of this layer, 3.7 instead of 5.0 ms for a thousand of them. The view
    tests gained a tenth of a millisecond, see above for why the samples had promised more.
  - The bones of a model are two products of matrices per bone, in single numbers. In the
    stretches with many soldiers `Matrix3D::Multiply()` was the largest entry of all, and
    the timer around the bones of animated models has 3 to 6 ms. `HTreeClass` now takes
    those products a row at a time with NEON (`WWMath/matrix3dneon.h`);
    `tools/matrix_test.cpp` compares them on a PC with `Matrix3D::mul()` and `postMul()`:
    the same numbers to the last bit for three million products. Only what is drawn uses
    it. The game's logic keeps the other code, because it has to come out the same on
    every platform.
  - `W3DView_updateView`, 2 ms for a standing picture and up to 8 ms with many soldiers,
    is not the view: `W3DView::update()` ends by calling `Drawable::draw()` for every
    drawable in the view, which sets up turrets, gun barrels, muzzle flashes and shadows
    again each time. Turrets and barrels are bones that the game "controls" on every frame
    with the transform they have, and `Animatable3DObjClass::Control_Bone()` told the model
    that its hierarchy had changed whatever it was given: every vehicle and every gun
    emplacement in the view worked out all its bones, parts and bounds on every frame.
    `HTreeClass::Control_Bone()` now says whether it changed anything.
  - What those two measured (run39): the bones of an animated model take 50 instead of
    55 us, half a millisecond of a frame with many soldiers and nothing that the frame
    times of the other stretches show. Most of a bone's time is not the two products but
    reading its animation and the calls around them. The turrets changed nothing in the
    training mission, which has a handful of vehicles in view; a battle of tanks is where
    it would show, and that has not been measured.
  - Where this leaves the frame, with the timers on: 45 ms in an ordinary scene and 73 to
    79 ms with sixty soldiers, of which the game's logic is 12 and 20 to 24 ms. From the
    measuring changes of run35 to run39 the three rounds of changes took 1, 3 and 3 ms off
    the first three stretches. What is left is spread over everything the engine does per
    object, per mesh and per drawable; no change of a day's work is in sight that takes
    more than a millisecond.
  - Seen and left: the terrain works out the light of every explosion and muzzle flash per
    vertex with four calls per light (`doTheDynamicLight()`); every object that moves walks
    the list of all trigger areas, and so does every question about water
    (`PolygonTrigger::pointInTrigger()`); the scene walks all its objects a second time to
    find the few that are not drawn through the lists of the first walk; every object has
    the sun added to its lights anew (`LightEnvironmentClass::Add_Light()`); the game walks
    all 1900 drawables of a map three times per frame and the scene its 1900 objects twice.
- `lstrcpyn()` was mapped to `strncpy()` for all platforms that are not Windows, in
  `string_compat.h` of both games and in WWLib's `osdep.h`. The Windows function copies
  one character less than it is told and always ends the string; `strncpy()` does
  neither. The asset managers make the file name of "Model.Part" with it (copy up to the
  dot, append ".w3d"), so on these platforms the name kept the dot and went on with
  whatever was in the buffer before. The file was never found, nothing remembered that,
  and every request for such a part asked the memory card again under a new name. It is a
  function with the Windows behaviour now.
- With a frame profile the log has a `[file]` line every ten seconds: how many requests
  the engine made to its file system per frame, the names that took the most time and the
  names asked for most often. Models and textures that are loaded when first shown are
  the usual entries; a name that shows up every time is something being looked for over
  and over.
- A request to the memory card takes a third of a millisecond however little it asks for.
  newlib's `fread()` makes one for every buffer it fills, and the engine opens its files
  with a buffer of one kilobyte (`BUFSIZ`). Every file in an archive is read whole
  through such a stream, so a texture of a megabyte took 300 ms, and the engine opens a
  texture two or three times. `LocalFile::read()` now reads 4 KiB or more in one request,
  around the buffer. That took the time a run spends in slow file requests from 73 to
  27 seconds; a megabyte still takes 90 ms.
- Half of those 27 seconds went into reading files again that had just been read: WW3D
  opens every texture three times in a row, and an open reads the whole file out of the
  archive. `RAMFile::openFromArchive()` keeps the last files it read (8 of them, 8 MiB)
  and copies from there.

- The console dims its screen and goes to sleep when nobody touches it for a while,
  also in the middle of a cutscene. `VitaGameEngine::serviceWindowsOS()` starts those
  timers over on every frame (`sceKernelPowerTick`).
- The Vita compiler defaults to the smallest enum type that fits. The build passes
  `-fno-short-enums` because the engine writes enum fields through `Int` pointers.
- `vita-elf-create` needs room after the code segment for Sony's module information.
  `cmake/vita.cmake` reserves it with `__sce_headroom`; without it the Windows build of
  the tool crashes with heap corruption instead of reporting the overlap.
- libstdc++ refers to the thread library through weak symbols, which do not pull members
  out of `libpthread.a`. `cmake/vita.cmake` requests them explicitly; without that the
  first guarded static local crashes before `main()` (null mutex in `__cxa_guard_acquire`).
- A shift by a count outside 0..31 is undefined in C++ and differs in practice: x86 and
  ARM64 take the count modulo 32, the Vita's 32-bit ARM shifts everything out and yields
  0. The retail code tested death types and veterancy levels with `1 << (type - 1)`,
  which is a shift by -1 for the first value of each (normal death, no promotion). On the
  Vita no death module applied to such a unit: killed infantry kept standing, wrecks and
  barrels neither changed nor exploded. `Damage.h` and `GameCommon.h` now reduce the
  count explicitly. Any other shift that relies on the wrap-around would fail the same
  quiet way.
- The engine checks the result of almost none of its Direct3D calls. A lock that fails
  leaves it writing through an uninitialised pointer, so a data abort in texture or
  terrain code is usually explained by a `[d3d8]` line shortly before it in the log.
- GeneralsX draws the terrain from 256 pixel tiles in an atlas 8192 pixels wide. The
  Vita's GPU takes textures up to 4096 pixels, and the tiles of one map would need
  hundreds of megabytes, so `TileData.h` keeps the original 64 pixel tiles and the 2048
  pixel atlas for this platform.
- The terrain is drawn from a square window of the map, 129 cells on a side, that
  follows the camera. The scripted camera of a cutscene kept that size at a low pitch
  and placed the window by an estimate made for the larger window of the low player
  camera (TheSuperHackers' change), so trees and buildings at the far corners hung in
  the sky. Now that camera places the window on the cells it can see
  (`HeightMapRenderObjClass::findVisibleArea()`), and its view grows the window by tiles
  of 32 cells up to 257 when it sees more. The search covers the whole view between the
  lowest and the highest ground of the map, up to the distance the largest window holds:
  the game's own search starts from where the corners of the view meet the lowest ground,
  which a camera close to the ground looking up (the first scene of the first training
  mission) meets far away or not at all, and the ground around it went missing. The search
  looks at cells several apart, so the first 640 units of the view are included whole: a
  slope right beside the camera falls between those cells. The window
  stays grown until the player has the camera again, because each change of size rebuilds
  all of the terrain's vertices, 0.4 to 0.6 s on the console. The log has a `[terrain]`
  line for each growth with the time that took. The whole window is drawn on every frame,
  seen or not, so a larger window costs the GPU in proportion.
- The surface of a texture level shares the lifetime and the reference count of its
  texture, as in Direct3D. The D3DX replacement in CompatLib depends on it when it
  builds mip chains (`D3DXFilterTexture`).
- Measured with the 300 MiB heap: 48 MiB are taken before `main()` by the engine's
  preallocated memory pools, the game client adds 31 MiB, and the first mission another
  56 MiB. GeneralsX's buffers for explaining multiplayer desyncs took 72 MiB on top; the
  build switches them off (`DEEP_CRC_TO_MEMORY=0` in `cmake/vita.cmake`). vitaGL fills its
  share of main memory (about 26 MiB) while starting and takes part of the GPU memory.
- A process can have 64 files open, and about 20 are the game's `.big` archives. FreeType
  keeps the file of every face open, and the engine creates a face per font, size and
  weight, so the font files are read into memory once instead (`render2dsentence.cpp`).
  Anything else that holds files open per object will hit the same limit; the symptom is
  that opening unrelated files starts to fail.
- Seeking in a file that is being written is very slow on the memory card. `XferSave`
  seeks twice per block of a saved game, which made a 4 MiB save take minutes; on the
  Vita it writes into a memory stream and puts the file on the card in one piece.
- The engine locks a whole vertex buffer to change a few vertices (terrain: whenever the
  view scrolls or a light moves) and expects the rest to stay, also in write-only
  buffers. Every buffer therefore has a system memory copy, kept in Direct3D's byte
  order; what goes to vitaGL is a copy of the changed range with the vertex colors
  reordered.
- vitaGL builds the mip levels of a 5551 texture with the bit layout of 1555, which turns
  them into noise. A1R5G5B5 textures with mip maps (the terrain) are uploaded as 32 bit,
  X1R5G5B5 as 565.
- vitaGL remembers the buffer object of the last color pointer even while the color array
  is disabled, and with lighting it reads the constant ambient material from that buffer
  (the bytes of its first vertex taken for four floats). Lit meshes came out tinted
  green. `SetVertexArrays()` resets the color pointer whenever the vertex color is not
  used. The same misreading remains for a lit draw whose vertex color replaces only the
  diffuse or only the emissive material color; the game has not been seen to do that.
- vitaGL recalculates the normal matrix only when the first draw call after a change of
  the model view matrix is a lit one, and never marks it as changed: it reaches the shader
  only together with all other uniforms, after a change of shader or once per frame. Every
  mesh of a run with the same kind of state was therefore lit with the orientation of the
  first one, and the lighting of whole groups of objects (all trees on screen) jumped
  whenever effects such as muzzle flashes changed the order of the draw calls.
  `ApplyState()` calls `glUseProgram(0)`, vitaGL's public way of marking all uniforms as
  changed, before the first lit draw call with a new model view matrix, and loads the
  matrix again if an unlit draw call has used it first.
- vitaGL drops a material color that is set while that color follows the vertex color
  (`GL_COLOR_MATERIAL`), and leaves the last vertex color in it once that ends.
  `ApplyLighting()` therefore switches color material first and sets the material after
  it. The other way round, a mesh drawn right after one with vertex colors lost its
  material, and objects flickered between lit and unlit with the draw order.
- vitaGL lets the GPU reorder the blocks of a compressed texture level
  (`sceGxmTransferCopy`), which completes some time after the call returns. When a
  texture is uploaded level by level, vitaGL grows the texture's memory for each further
  level at once; if that moves the block, the pending copy writes into freed memory and
  corrupts the allocator of the GPU memory pool (seen as a data abort inside SceLibKernel
  during a later texture upload). Waiting with `sceGxmTransferFinish()` after every level
  cures the crash, but the small levels still came out black or as noise on the console:
  with `VITA_D3D8_TOP_MIP_ONLY` set the woven patterns on distant surfaces and most black
  specks were gone. `Texture::UploadCompressed()` therefore only lets vitaGL reserve the
  levels (a null data pointer) and writes the blocks into the texture's memory itself, in
  the GPU's order (`CopyBlocksSwizzled()`). Sizes that are not a power of two still take
  vitaGL's path.
- vitaGL generates the mip levels of uncompressed textures itself and ignores the data
  it is given for them.
- The engine sets world matrix, material and all light sources again for every object it
  draws, mostly to the values they already have. `Device::SetTransform()`,
  `SetMaterial()` and `SetLight()` compare with what is set and do nothing for a repeat;
  the light sources have a dirty group of their own, apart from the material, because
  handing them to OpenGL needs the view matrix loaded and costs some thirty calls.
- The base game refilled the vertex buffers of all road types on every frame, walking the
  whole road list eight times per type (25 ms per frame in the training mission). It now
  does what Zero Hour does and refills them only when a segment comes into view or
  leaves it (`W3DRoadBuffer::visibilityChanged()`), and skips road types without a
  visible segment.
- Since TheSuperHackers' PR #1579 the time of animations runs on with every drawn frame,
  and `HTreeClass::Anim_Update_Without_Interpolation()` returned without working out
  anything on a frame without sync time, so that raw animations step with the logic. A
  model whose animation had just been set stayed in the pose it had, the base pose for a
  new one. The logic takes launch bones from such a new model and keeps them: the Scud
  Storm in the first USA mission launched its missiles level into its surroundings,
  from missiles that had not risen. That happens whenever a frame is drawn without a
  logic step before the bones are first needed, which the PS Vita's pacing does all the
  time. `Animatable3DObjClass::Raw_Anim_Update()` now holds the frame the hierarchy last
  showed on such a frame, but always works the hierarchy out; an animation that is set
  is shown from its own frame at once.
- vitaGL copies a whole buffer object on every partial update of one that was drawn
  recently. Dynamic vertex and index buffers therefore stay in system memory and are
  drawn from there.
- For vertex arrays in system memory vitaGL copies everything from the start of the
  arrays up to the highest index of a draw call, once per vertex attribute. The engine
  draws its sorted translucent geometry as many small ranges of one large dynamic buffer,
  so that buffer was copied over and over: an explosion cost more than 100 ms per frame
  in vitaGL alone. `DrawIndexedPrimitive()` now starts the arrays at the lowest vertex a
  call uses and moves the indices down to match (`RebaseIndices()`).
