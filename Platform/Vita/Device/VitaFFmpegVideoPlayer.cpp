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

// VitaFFmpegVideoPlayer.cpp //////////////////////////////////////////////////
// The engine's FFmpeg video player, built for the PS Vita with the FFmpeg of
// tools/build-ffmpeg.sh, which decodes the game's Bink files.
//
// The player hands the sound of a movie to the miniaudio stream of the audio
// manager when SAGE_USE_MINIAUDIO is defined. The Vita build defines it for this
// file only: for the rest of the engine it would also select FFmpeg for decoding
// every sound, which the console does with miniaudio's own decoders
// (Device/VitaAudioLoader.cpp) and which this FFmpeg cannot do.
///////////////////////////////////////////////////////////////////////////////

#define SAGE_USE_MINIAUDIO
#include "../../../Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp"
