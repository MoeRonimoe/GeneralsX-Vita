// yuv_test.cpp ///////////////////////////////////////////////////////////////
// Checks the colour conversion of movie frames on the PS Vita (compat/VitaYuv.cpp):
//   - the NEON code gives every pixel the same bits as the code without NEON, for
//     pictures of many widths and heights, odd ones included, and for every Y, U
//     and V;
//   - both are within one step of each 5 or 6 bit colour of the exact BT.601
//     studio range formulas.
//
// The NEON code is compiled with neon_on_pc/arm_neon.h. From the repository root,
// in one line:
//   g++ -O2 -std=c++17 -DVITA_YUV_NEON_ON_PC -I Platform/Vita/tools/neon_on_pc
//       -I Platform/Vita/compat/include Platform/Vita/tools/yuv_test.cpp
//       Platform/Vita/compat/VitaYuv.cpp -o build/hosttest/yuv_test.exe
///////////////////////////////////////////////////////////////////////////////

#include "VitaYuv.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{

uint32_t s_random = 12345;

uint8_t randomByte()
{
	s_random = s_random * 1103515245u + 12345u;
	return static_cast<uint8_t>(s_random >> 16);
}

struct Picture
{
	int width;
	int height;
	std::vector<uint8_t> y, u, v;
	int yPitch, uvPitch;
};

Picture makePicture(int width, int height)
{
	Picture p;
	p.width = width;
	p.height = height;
	p.yPitch = width + 3;
	p.uvPitch = (width + 1) / 2 + 5;
	p.y.resize(p.yPitch * height);
	p.u.resize(p.uvPitch * ((height + 1) / 2));
	p.v.resize(p.uvPitch * ((height + 1) / 2));
	for (uint8_t &b : p.y) b = randomByte();
	for (uint8_t &b : p.u) b = randomByte();
	for (uint8_t &b : p.v) b = randomByte();
	return p;
}

int compareNeonWithPlain(const Picture &p)
{
	const int pitch = p.width * 2 + 6;
	std::vector<uint8_t> neon(pitch * p.height, 0xAB), plain(pitch * p.height, 0xAB);
	VitaYuv::convertToRgb565(p.y.data(), p.yPitch, p.u.data(), p.uvPitch, p.v.data(), p.uvPitch, p.width, p.height,
		reinterpret_cast<uint16_t *>(neon.data()), pitch);
	VitaYuv::convertToRgb565Plain(p.y.data(), p.yPitch, p.u.data(), p.uvPitch, p.v.data(), p.uvPitch, p.width, p.height,
		reinterpret_cast<uint16_t *>(plain.data()), pitch);
	int differences = 0;
	for (size_t i = 0; i < neon.size(); ++i)
		if (neon[i] != plain[i])
			++differences;
	return differences;
}

double clampColour(double value)
{
	return value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value);
}

} // namespace

int main()
{
	int failures = 0;

	// NEON against plain, many sizes.
	for (int width = 1; width <= 70; ++width)
	{
		for (int height = 1; height <= 5; ++height)
		{
			const int differences = compareNeonWithPlain(makePicture(width, height));
			if (differences != 0)
			{
				printf("FAIL %dx%d: %d bytes differ between NEON and plain\n", width, height, differences);
				++failures;
			}
		}
	}
	if (compareNeonWithPlain(makePicture(800, 600)) != 0)
	{
		printf("FAIL 800x600: NEON and plain differ\n");
		++failures;
	}

	// Every Y, U and V: 256 values of Y along a row of 256 pixels, U and V the same for
	// each pair of pixels, so the whole cube is 256 x 256 rows of pictures 256 wide.
	int worst[3] = { 0, 0, 0 };
	std::vector<uint8_t> y(256), u(128), v(128);
	std::vector<uint16_t> neon(256), plain(256);
	for (int ui = 0; ui < 256; ++ui)
	{
		for (int vi = 0; vi < 256; ++vi)
		{
			for (int i = 0; i < 256; ++i)
				y[i] = static_cast<uint8_t>(i);
			// Pixel x uses colour sample x / 2; fill so that the pair (2k, 2k+1) gets U = ui, V = vi.
			for (int i = 0; i < 128; ++i)
			{
				u[i] = static_cast<uint8_t>(ui);
				v[i] = static_cast<uint8_t>(vi);
			}
			VitaYuv::convertToRgb565(y.data(), 256, u.data(), 128, v.data(), 128, 256, 1, neon.data(), 512);
			VitaYuv::convertToRgb565Plain(y.data(), 256, u.data(), 128, v.data(), 128, 256, 1, plain.data(), 512);
			for (int i = 0; i < 256; ++i)
			{
				if (neon[i] != plain[i])
				{
					if (failures < 20)
						printf("FAIL Y %d U %d V %d: NEON 0x%04x, plain 0x%04x\n", i, ui, vi, neon[i], plain[i]);
					++failures;
				}
				const double luma = 1.164 * (i - 16);
				const double exact[3] = {
					clampColour(luma + 1.596 * (vi - 128)),
					clampColour(luma - 0.391 * (ui - 128) - 0.813 * (vi - 128)),
					clampColour(luma + 2.018 * (ui - 128)) };
				const int got[3] = { (plain[i] >> 11) & 31, (plain[i] >> 5) & 63, plain[i] & 31 };
				const int bits[3] = { 5, 6, 5 };
				for (int c = 0; c < 3; ++c)
				{
					const int expected = static_cast<int>(exact[c]) >> (8 - bits[c]);
					const int error = std::abs(got[c] - expected);
					if (error > worst[c])
						worst[c] = error;
				}
			}
		}
	}
	printf("largest difference to the exact formulas in steps of the 16 bit colours: red %d, green %d, blue %d\n",
		worst[0], worst[1], worst[2]);
	if (worst[0] > 1 || worst[1] > 1 || worst[2] > 1)
	{
		printf("FAIL: more than one step away from the exact colours\n");
		++failures;
	}

	printf(failures == 0 ? "all checks passed\n" : "%d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
