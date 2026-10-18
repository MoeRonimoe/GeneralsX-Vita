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

// GeneralsX @performance Claude 13/10/2026 The tests of a sphere and of a box against the six planes
// of a frustum, four planes at a time, for processors with NEON.
//
// A scene asks for every one of its objects whether its sphere is in the view, and for every mesh
// of those whether its box is. One plane at a time that is twelve function calls and sixty
// multiplications and additions per object; on the PS Vita's processor the question took 3 ms of
// every frame for the 1900 objects of a map.
//
// The numbers are worked out in the order the tests for a single plane work them out
// (colmathplane.h, colmathplane.cpp), so the answers are the same; only a number too small to be
// represented normally is taken as zero by NEON. This file uses nothing of the library so that it
// can be compiled and compared with those tests elsewhere: Platform/Vita/tools/frustum_test.cpp.
//
// "Greater" is asked as "greater in size, and positive" throughout. The compiler turns the plain
// comparisons of two vectors of numbers (vcgtq_f32) into one comparison per number unless it is
// allowed to be careless with arithmetic, which this code base does not allow; the comparison of
// sizes (vcagtq_f32) it leaves as the one instruction it is.

#pragma once

#include <arm_neon.h>

/// What CollisionMath::OverlapType calls OUTSIDE, INSIDE and OVERLAPPED.
enum
{
	FRUSTUM_NEON_OUTSIDE = 0x01,
	FRUSTUM_NEON_INSIDE = 0x02,
	FRUSTUM_NEON_OVERLAPPED = 0x08
};

/// True if any of the eight flags is set, each of which is all ones or all zeros.
static inline bool Frustum_Neon_Any(uint32x4_t a, uint32x4_t b)
{
	const uint32x4_t either = vorrq_u32(a, b);
	const uint32x2_t half = vorr_u32(vget_low_u32(either), vget_high_u32(either));
	return vget_lane_u64(vreinterpret_u64_u32(half), 0) != 0;
}

/// True if all of the eight flags are set.
static inline bool Frustum_Neon_All(uint32x4_t a, uint32x4_t b)
{
	const uint32x4_t both = vandq_u32(a, b);
	const uint32x2_t half = vand_u32(vget_low_u32(both), vget_high_u32(both));
	return vget_lane_u64(vreinterpret_u64_u32(half), 0) == ~static_cast<uint64_t>(0);
}

/// All ones where the number is negative, and where it is a zero with a minus sign.
static inline uint32x4_t Frustum_Neon_Negative(float32x4_t a)
{
	return vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(a), 31));
}

/// planes: six planes, each the three numbers of its normal and its distance.
/// sphere: the three numbers of the center and the radius, which must not be negative.
static inline int Frustum_Neon_Sphere(const float * planes, const float * sphere)
{
	// Planes 0 to 3, and planes 2 to 5: two of them twice, which changes nothing.
	const float32x4x4_t first = vld4q_f32(planes);
	const float32x4x4_t last = vld4q_f32(planes + 8);

	const float32x4_t x = vdupq_n_f32(sphere[0]);
	const float32x4_t y = vdupq_n_f32(sphere[1]);
	const float32x4_t z = vdupq_n_f32(sphere[2]);
	const float32x4_t radius = vdupq_n_f32(sphere[3]);

	// The distance from the plane: x * normal x + y * normal y + z * normal z - distance.
	const float32x4_t fromFirst = vsubq_f32(
		vmlaq_f32(vmlaq_f32(vmulq_f32(x, first.val[0]), y, first.val[1]), z, first.val[2]), first.val[3]);
	const float32x4_t fromLast = vsubq_f32(
		vmlaq_f32(vmlaq_f32(vmulq_f32(x, last.val[0]), y, last.val[1]), z, last.val[2]), last.val[3]);

	// Farther from the plane than the radius: in front of it, or behind it.
	const uint32x4_t clearFirst = vcagtq_f32(fromFirst, radius);
	const uint32x4_t clearLast = vcagtq_f32(fromLast, radius);
	const uint32x4_t behindFirst = Frustum_Neon_Negative(fromFirst);
	const uint32x4_t behindLast = Frustum_Neon_Negative(fromLast);

	if (Frustum_Neon_Any(vbicq_u32(clearFirst, behindFirst), vbicq_u32(clearLast, behindLast)))
		return FRUSTUM_NEON_OUTSIDE;

	if (Frustum_Neon_All(vandq_u32(clearFirst, behindFirst), vandq_u32(clearLast, behindLast)))
		return FRUSTUM_NEON_INSIDE;

	return FRUSTUM_NEON_OVERLAPPED;
}

/// The corners of a box that are farthest along and against the normals of four planes, and how
/// far they are from those planes.
static inline void Frustum_Neon_Box_Planes(const float32x4x4_t & planes, const float * box,
	float32x4_t & nearest, float32x4_t & farthest)
{
	const uint32x4_t sign = vdupq_n_u32(0x80000000u);

	// The extent with the sign of the normal: the corner farthest along the normal, from the center.
	const float32x4_t alongX = vreinterpretq_f32_u32(
		veorq_u32(vreinterpretq_u32_f32(vdupq_n_f32(box[3])), vandq_u32(vreinterpretq_u32_f32(planes.val[0]), sign)));
	const float32x4_t alongY = vreinterpretq_f32_u32(
		veorq_u32(vreinterpretq_u32_f32(vdupq_n_f32(box[4])), vandq_u32(vreinterpretq_u32_f32(planes.val[1]), sign)));
	const float32x4_t alongZ = vreinterpretq_f32_u32(
		veorq_u32(vreinterpretq_u32_f32(vdupq_n_f32(box[5])), vandq_u32(vreinterpretq_u32_f32(planes.val[2]), sign)));

	const float32x4_t centerX = vdupq_n_f32(box[0]);
	const float32x4_t centerY = vdupq_n_f32(box[1]);
	const float32x4_t centerZ = vdupq_n_f32(box[2]);

	const float32x4_t farX = vaddq_f32(alongX, centerX);
	const float32x4_t farY = vaddq_f32(alongY, centerY);
	const float32x4_t farZ = vaddq_f32(alongZ, centerZ);
	const float32x4_t nearX = vaddq_f32(vnegq_f32(alongX), centerX);
	const float32x4_t nearY = vaddq_f32(vnegq_f32(alongY), centerY);
	const float32x4_t nearZ = vaddq_f32(vnegq_f32(alongZ), centerZ);

	nearest = vsubq_f32(
		vmlaq_f32(vmlaq_f32(vmulq_f32(nearX, planes.val[0]), nearY, planes.val[1]), nearZ, planes.val[2]), planes.val[3]);
	farthest = vsubq_f32(
		vmlaq_f32(vmlaq_f32(vmulq_f32(farX, planes.val[0]), farY, planes.val[1]), farZ, planes.val[2]), planes.val[3]);
}

/// planes: six planes, each the three numbers of its normal and its distance.
/// box: the three numbers of the center and the three of the extent.
/// epsilon: how far from a plane a point has to be to count as on one side of it; not negative.
static inline int Frustum_Neon_Box(const float * planes, const float * box, float epsilon)
{
	const float32x4x4_t first = vld4q_f32(planes);
	const float32x4x4_t last = vld4q_f32(planes + 8);

	float32x4_t nearestFirst;
	float32x4_t farthestFirst;
	float32x4_t nearestLast;
	float32x4_t farthestLast;
	Frustum_Neon_Box_Planes(first, box, nearestFirst, farthestFirst);
	Frustum_Neon_Box_Planes(last, box, nearestLast, farthestLast);

	// Outside if even the nearest corner is in front of a plane.
	const float32x4_t apart = vdupq_n_f32(epsilon);
	if (Frustum_Neon_Any(vbicq_u32(vcagtq_f32(nearestFirst, apart), Frustum_Neon_Negative(nearestFirst)),
			vbicq_u32(vcagtq_f32(nearestLast, apart), Frustum_Neon_Negative(nearestLast))))
		return FRUSTUM_NEON_OUTSIDE;

	// Inside if the farthest corner is behind every plane.
	if (Frustum_Neon_All(vandq_u32(vcagtq_f32(farthestFirst, apart), Frustum_Neon_Negative(farthestFirst)),
			vandq_u32(vcagtq_f32(farthestLast, apart), Frustum_Neon_Negative(farthestLast))))
		return FRUSTUM_NEON_INSIDE;

	return FRUSTUM_NEON_OVERLAPPED;
}
