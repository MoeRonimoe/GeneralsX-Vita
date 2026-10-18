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

// VitaYuv.cpp ////////////////////////////////////////////////////////////////
// See VitaYuv.h.
//
// For a pixel, with Y, U and V from 0 to 255:
//   luma  = (Y - 16) * 74                       1.164 * 64
//   red   = luma + (V - 128) * 102              1.596 * 64
//   green = luma - (U - 128) * 25 - (V - 128) * 52      0.391 * 64, 0.813 * 64
//   blue  = luma + (U - 128) * 129              2.018 * 64
// each in 16 bits, the sum held at the largest and smallest 16 bit number as NEON's
// saturating additions do (only blue gets there, and it is 255 either way), then
// divided by 64 with rounding and held between 0 and 255.
///////////////////////////////////////////////////////////////////////////////

#include "VitaYuv.h"

#if defined(__ARM_NEON) || defined(VITA_YUV_NEON_ON_PC)
#include <arm_neon.h>
#define VITA_YUV_NEON 1
#endif

namespace VitaYuv
{

namespace
{

int saturate16(int value)
{
	return value > 32767 ? 32767 : (value < -32768 ? -32768 : value);
}

int toByte(int value)
{
	const int shifted = (value + 32) >> 6;
	return shifted > 255 ? 255 : (shifted < 0 ? 0 : shifted);
}

uint16_t pixel(int y, int u, int v)
{
	const int luma = (y - 16) * 74;
	const int red = toByte(saturate16(luma + (v - 128) * 102));
	const int green = toByte(saturate16(luma - ((u - 128) * 25 + (v - 128) * 52)));
	const int blue = toByte(saturate16(luma + (u - 128) * 129));
	return static_cast<uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

void convertRowPlain(const uint8_t *y, const uint8_t *u, const uint8_t *v, int from, int width, uint16_t *rgb)
{
	for (int x = from; x < width; ++x)
		rgb[x] = pixel(y[x], u[x / 2], v[x / 2]);
}

#ifdef VITA_YUV_NEON
// Eight pixels from their luma and the colour terms that belong to them.
uint16x8_t eightPixels(uint8x8_t y8, int16x8_t redTerm, int16x8_t greenTerm, int16x8_t blueTerm)
{
	const int16x8_t luma = vmulq_n_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), vdupq_n_s16(16)), 74);
	const uint8x8_t red = vqrshrun_n_s16(vqaddq_s16(luma, redTerm), 6);
	const uint8x8_t green = vqrshrun_n_s16(vqsubq_s16(luma, greenTerm), 6);
	const uint8x8_t blue = vqrshrun_n_s16(vqaddq_s16(luma, blueTerm), 6);

	// Each colour into the top of a 16 bit lane; then green and blue are shifted in below
	// the top five and the top eleven bits.
	uint16x8_t result = vsriq_n_u16(vshll_n_u8(red, 8), vshll_n_u8(green, 8), 5);
	return vsriq_n_u16(result, vshll_n_u8(blue, 8), 11);
}

void convertRowNeon(const uint8_t *y, const uint8_t *u, const uint8_t *v, int width, uint16_t *rgb)
{
	const int whole = width & ~15;
	for (int x = 0; x < whole; x += 16)
	{
		const uint8x16_t luma = vld1q_u8(y + x);
		const int16x8_t uc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(u + x / 2))), vdupq_n_s16(128));
		const int16x8_t vc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(v + x / 2))), vdupq_n_s16(128));

		// Every colour sample is for two pixels side by side.
		const int16x8x2_t redTerm = vzipq_s16(vmulq_n_s16(vc, 102), vmulq_n_s16(vc, 102));
		const int16x8_t greenOnce = vmlaq_n_s16(vmulq_n_s16(uc, 25), vc, 52);
		const int16x8x2_t greenTerm = vzipq_s16(greenOnce, greenOnce);
		const int16x8x2_t blueTerm = vzipq_s16(vmulq_n_s16(uc, 129), vmulq_n_s16(uc, 129));

		vst1q_u16(rgb + x, eightPixels(vget_low_u8(luma), redTerm.val[0], greenTerm.val[0], blueTerm.val[0]));
		vst1q_u16(rgb + x + 8, eightPixels(vget_high_u8(luma), redTerm.val[1], greenTerm.val[1], blueTerm.val[1]));
	}
	convertRowPlain(y, u, v, whole, width, rgb);
}
#endif

} // namespace

void convertToRgb565Plain(const uint8_t *y, int yPitch, const uint8_t *u, int uPitch, const uint8_t *v, int vPitch,
	int width, int height, uint16_t *rgb, int rgbPitch)
{
	for (int row = 0; row < height; ++row)
	{
		convertRowPlain(y + row * yPitch, u + (row / 2) * uPitch, v + (row / 2) * vPitch, 0, width,
			reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(rgb) + row * rgbPitch));
	}
}

void convertToRgb565(const uint8_t *y, int yPitch, const uint8_t *u, int uPitch, const uint8_t *v, int vPitch,
	int width, int height, uint16_t *rgb, int rgbPitch)
{
#ifdef VITA_YUV_NEON
	for (int row = 0; row < height; ++row)
	{
		convertRowNeon(y + row * yPitch, u + (row / 2) * uPitch, v + (row / 2) * vPitch, width,
			reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(rgb) + row * rgbPitch));
	}
#else
	convertToRgb565Plain(y, yPitch, u, uPitch, v, vPitch, width, height, rgb, rgbPitch);
#endif
}

} // namespace VitaYuv
