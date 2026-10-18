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

// VitaYuv.h //////////////////////////////////////////////////////////////////
// The colours of a movie frame for the screen, on the PS Vita.
//
// A movie of the game decodes to planar YUV with the colour halved in both
// directions (4:2:0), and the console shows it from a texture of 16 bit pixels
// (RGB 5:6:5). FFmpeg's libswscale has no ARM code for that pair and took 25 ms
// for a frame of 800x600, most of the 33 ms a frame of the intro has. This does
// it with NEON, 16 pixels at a time, in integers: studio range BT.601, as
// libswscale assumes for these movies, with the factors in 1/64.
//
// Tested on a PC against libswscale and against itself without NEON:
// Platform/Vita/tools/yuv_test.cpp.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>

namespace VitaYuv
{

/// Converts width x height pixels. The pitches are in bytes; u and v have a sample for
/// every 2x2 pixels. Rows of an odd width or height end in their last colour sample.
void convertToRgb565(const uint8_t *y, int yPitch, const uint8_t *u, int uPitch, const uint8_t *v, int vPitch,
	int width, int height, uint16_t *rgb, int rgbPitch);

/// The same a pixel at a time, without NEON: what the NEON code must match bit for bit.
void convertToRgb565Plain(const uint8_t *y, int yPitch, const uint8_t *u, int uPitch, const uint8_t *v, int vPitch,
	int width, int height, uint16_t *rgb, int rgbPitch);

} // namespace VitaYuv
