// The few NEON intrinsics the port's vector code uses, in plain C++, so that the code can be
// compiled and checked on a PC (see ../frustum_test.cpp and ../matrix_test.cpp). Each does
// what ARM documents the intrinsic of that name to do, one lane after the other. Not for a
// console.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

struct float32x4_t { float lane[4]; };
struct uint32x4_t { uint32_t lane[4]; };
struct int32x4_t { int32_t lane[4]; };
struct uint32x2_t { uint32_t lane[2]; };
struct uint64x1_t { uint64_t lane[1]; };
struct float32x4x4_t { float32x4_t val[4]; };

/// Sixteen numbers, the first of every four into val[0], the second into val[1], and so on.
static inline float32x4x4_t vld4q_f32(const float *from)
{
	float32x4x4_t result;
	for (int i = 0; i < 4; ++i)
		for (int k = 0; k < 4; ++k)
			result.val[k].lane[i] = from[4 * i + k];
	return result;
}

static inline float32x4_t vdupq_n_f32(float value) { return { { value, value, value, value } }; }
static inline uint32x4_t vdupq_n_u32(uint32_t value) { return { { value, value, value, value } }; }

static inline float32x4_t vld1q_f32(const float *from) { return { { from[0], from[1], from[2], from[3] } }; }
static inline uint32x4_t vld1q_u32(const uint32_t *from) { return { { from[0], from[1], from[2], from[3] } }; }
static inline float32x4_t vld1q_dup_f32(const float *from) { return { { from[0], from[0], from[0], from[0] } }; }

static inline void vst1q_f32(float *to, float32x4_t a)
{
	for (int i = 0; i < 4; ++i)
		to[i] = a.lane[i];
}

/// The bits of a where the mask has ones and the bits of b where it has zeros.
static inline float32x4_t vbslq_f32(uint32x4_t mask, float32x4_t a, float32x4_t b)
{
	float32x4_t r;
	for (int i = 0; i < 4; ++i)
	{
		uint32_t fromA;
		uint32_t fromB;
		memcpy(&fromA, &a.lane[i], sizeof(fromA));
		memcpy(&fromB, &b.lane[i], sizeof(fromB));
		const uint32_t bits = (fromA & mask.lane[i]) | (fromB & ~mask.lane[i]);
		memcpy(&r.lane[i], &bits, sizeof(bits));
	}
	return r;
}

#define NEON_ON_PC_FLOAT_OP(name, expression) \
	static inline float32x4_t name(float32x4_t a, float32x4_t b) \
	{ float32x4_t r; for (int i = 0; i < 4; ++i) { const float x = a.lane[i]; const float y = b.lane[i]; r.lane[i] = expression; } return r; }
NEON_ON_PC_FLOAT_OP(vaddq_f32, x + y)
NEON_ON_PC_FLOAT_OP(vsubq_f32, x - y)
NEON_ON_PC_FLOAT_OP(vmulq_f32, x * y)

static inline float32x4_t vnegq_f32(float32x4_t a)
{
	float32x4_t r;
	for (int i = 0; i < 4; ++i)
		r.lane[i] = -a.lane[i];
	return r;
}

/// a + b * c, the product rounded before the sum is: this is not a fused multiply-add.
static inline float32x4_t vmlaq_f32(float32x4_t a, float32x4_t b, float32x4_t c)
{
	float32x4_t r;
	for (int i = 0; i < 4; ++i)
	{
		volatile float product = b.lane[i] * c.lane[i];
		r.lane[i] = a.lane[i] + product;
	}
	return r;
}

/// Where a is greater in size than b.
static inline uint32x4_t vcagtq_f32(float32x4_t a, float32x4_t b)
{
	uint32x4_t r;
	for (int i = 0; i < 4; ++i)
		r.lane[i] = fabsf(a.lane[i]) > fabsf(b.lane[i]) ? 0xFFFFFFFFu : 0u;
	return r;
}

/// Shifts to the right and fills up with the sign.
static inline int32x4_t vshrq_n_s32(int32x4_t a, int bits)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i)
		r.lane[i] = a.lane[i] < 0 ? ~(~a.lane[i] >> bits) : a.lane[i] >> bits;
	return r;
}

/// a and not b.
static inline uint32x4_t vbicq_u32(uint32x4_t a, uint32x4_t b)
{
	uint32x4_t r;
	for (int i = 0; i < 4; ++i)
		r.lane[i] = a.lane[i] & ~b.lane[i];
	return r;
}

static inline int32x4_t vreinterpretq_s32_f32(float32x4_t a)
{
	int32x4_t r;
	memcpy(&r, &a, sizeof(r));
	return r;
}

static inline uint32x4_t vreinterpretq_u32_s32(int32x4_t a)
{
	uint32x4_t r;
	memcpy(&r, &a, sizeof(r));
	return r;
}

#define NEON_ON_PC_BIT_OP(name, type, lanes, operation) \
	static inline type name(type a, type b) \
	{ type r; for (int i = 0; i < lanes; ++i) r.lane[i] = a.lane[i] operation b.lane[i]; return r; }
NEON_ON_PC_BIT_OP(vorrq_u32, uint32x4_t, 4, |)
NEON_ON_PC_BIT_OP(vandq_u32, uint32x4_t, 4, &)
NEON_ON_PC_BIT_OP(veorq_u32, uint32x4_t, 4, ^)
NEON_ON_PC_BIT_OP(vorr_u32, uint32x2_t, 2, |)
NEON_ON_PC_BIT_OP(vand_u32, uint32x2_t, 2, &)

static inline uint32x2_t vget_low_u32(uint32x4_t a) { return { { a.lane[0], a.lane[1] } }; }
static inline uint32x2_t vget_high_u32(uint32x4_t a) { return { { a.lane[2], a.lane[3] } }; }

static inline uint64x1_t vreinterpret_u64_u32(uint32x2_t a)
{
	uint64x1_t r;
	memcpy(&r, &a, sizeof(r));
	return r;
}

static inline uint64_t vget_lane_u64(uint64x1_t a, int lane) { return a.lane[lane]; }

static inline float32x4_t vreinterpretq_f32_u32(uint32x4_t a)
{
	float32x4_t r;
	memcpy(&r, &a, sizeof(r));
	return r;
}

static inline uint32x4_t vreinterpretq_u32_f32(float32x4_t a)
{
	uint32x4_t r;
	memcpy(&r, &a, sizeof(r));
	return r;
}

// Integers of 8 and 16 bits, for the colour conversion of movies (compat/VitaYuv.cpp,
// ../yuv_test.cpp).
struct uint8x8_t { uint8_t lane[8]; };
struct uint8x16_t { uint8_t lane[16]; };
struct int16x8_t { int16_t lane[8]; };
struct uint16x8_t { uint16_t lane[8]; };
struct int16x8x2_t { int16x8_t val[2]; };

static inline uint8x8_t vld1_u8(const uint8_t *from)
{
	uint8x8_t r;
	memcpy(r.lane, from, sizeof(r.lane));
	return r;
}

static inline uint8x16_t vld1q_u8(const uint8_t *from)
{
	uint8x16_t r;
	memcpy(r.lane, from, sizeof(r.lane));
	return r;
}

static inline void vst1q_u16(uint16_t *to, uint16x8_t a) { memcpy(to, a.lane, sizeof(a.lane)); }

static inline uint8x8_t vget_low_u8(uint8x16_t a)
{
	uint8x8_t r;
	memcpy(r.lane, a.lane, 8);
	return r;
}

static inline uint8x8_t vget_high_u8(uint8x16_t a)
{
	uint8x8_t r;
	memcpy(r.lane, a.lane + 8, 8);
	return r;
}

/// Widens each byte to 16 bits.
static inline uint16x8_t vmovl_u8(uint8x8_t a)
{
	uint16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = a.lane[i];
	return r;
}

static inline int16x8_t vreinterpretq_s16_u16(uint16x8_t a)
{
	int16x8_t r;
	memcpy(&r, &a, sizeof(r));
	return r;
}

static inline int16x8_t vdupq_n_s16(int16_t value)
{
	int16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = value;
	return r;
}

/// The lanes wrap around as in the hardware: the result is the low 16 bits.
static inline int16x8_t vsubq_s16(int16x8_t a, int16x8_t b)
{
	int16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = static_cast<int16_t>(static_cast<uint16_t>(a.lane[i] - b.lane[i]));
	return r;
}

static inline int16x8_t vmulq_n_s16(int16x8_t a, int16_t b)
{
	int16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = static_cast<int16_t>(static_cast<uint16_t>(a.lane[i] * b));
	return r;
}

/// a + b * c, in 16 bits that wrap around.
static inline int16x8_t vmlaq_n_s16(int16x8_t a, int16x8_t b, int16_t c)
{
	int16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = static_cast<int16_t>(static_cast<uint16_t>(a.lane[i] + b.lane[i] * c));
	return r;
}

static inline int16_t neonOnPcSaturate16(int value)
{
	return static_cast<int16_t>(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
}

/// Sums and differences held at the largest and the smallest 16 bit number.
static inline int16x8_t vqaddq_s16(int16x8_t a, int16x8_t b)
{
	int16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = neonOnPcSaturate16(a.lane[i] + b.lane[i]);
	return r;
}

static inline int16x8_t vqsubq_s16(int16x8_t a, int16x8_t b)
{
	int16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = neonOnPcSaturate16(a.lane[i] - b.lane[i]);
	return r;
}

/// Each lane shifted right with rounding, then held between 0 and 255.
static inline uint8x8_t vqrshrun_n_s16(int16x8_t a, int bits)
{
	uint8x8_t r;
	for (int i = 0; i < 8; ++i)
	{
		const int value = (a.lane[i] + (1 << (bits - 1))) >> bits;
		r.lane[i] = static_cast<uint8_t>(value > 255 ? 255 : (value < 0 ? 0 : value));
	}
	return r;
}

/// Each byte widened to 16 bits and shifted left.
static inline uint16x8_t vshll_n_u8(uint8x8_t a, int bits)
{
	uint16x8_t r;
	for (int i = 0; i < 8; ++i)
		r.lane[i] = static_cast<uint16_t>(a.lane[i] << bits);
	return r;
}

/// b shifted right into a, which keeps the bits the shift leaves empty.
static inline uint16x8_t vsriq_n_u16(uint16x8_t a, uint16x8_t b, int bits)
{
	uint16x8_t r;
	const uint16_t kept = static_cast<uint16_t>(~(0xFFFFu >> bits));
	for (int i = 0; i < 8; ++i)
		r.lane[i] = static_cast<uint16_t>((a.lane[i] & kept) | (b.lane[i] >> bits));
	return r;
}

/// The lanes of a and b alternating: a0 b0 a1 b1 ... into val[0] and val[1].
static inline int16x8x2_t vzipq_s16(int16x8_t a, int16x8_t b)
{
	int16x8x2_t r;
	for (int i = 0; i < 8; ++i)
	{
		r.val[i / 4].lane[(i % 4) * 2] = a.lane[i];
		r.val[i / 4].lane[(i % 4) * 2 + 1] = b.lane[i];
	}
	return r;
}
