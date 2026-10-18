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

// GeneralsX @performance Claude 13/10/2026 The product of two matrices of the kind Matrix3D holds
// (three rows of four numbers, the fourth row being 0 0 0 1), a row at a time, for processors
// with NEON.
//
// The bones of a model are two such products per bone on every frame the model moves, 36
// multiplications and 27 additions each in single numbers; in a scene with many soldiers that was
// the largest single entry among the samples of the PS Vita's frames.
//
// Every number of the result is worked out in the order Matrix3D::mul() works it out
// (matrix3d.h), so it is the same number; only one too small to be represented normally is taken
// as zero by NEON. This file uses nothing of the library so that it can be compiled and compared
// with Matrix3D::mul() elsewhere: Platform/Vita/tools/matrix_test.cpp. It is meant for what is
// drawn. The game's logic has to come out the same on all platforms and keeps the other code.

#pragma once

#include <arm_neon.h>

/// result = a * b. All three are twelve numbers, row after row; result may be a or b.
static inline void Matrix3D_Neon_Multiply(const float * a, const float * b, float * result)
{
	static const uint32_t fourth[4] = { 0u, 0u, 0u, 0xFFFFFFFFu };
	const uint32x4_t isFourth = vld1q_u32(fourth);

	const float32x4_t b0 = vld1q_f32(b);
	const float32x4_t b1 = vld1q_f32(b + 4);
	const float32x4_t b2 = vld1q_f32(b + 8);

	// A row of the result is x * row 0 of b + y * row 1 of b + z * row 2 of b, with x, y, z and w
	// the numbers of the row of a; its fourth number gets w added. A row of a is read before the
	// row of the result is written, which is what lets result be a.
	float32x4_t row = vmlaq_f32(vmlaq_f32(vmulq_f32(vld1q_dup_f32(a), b0), vld1q_dup_f32(a + 1), b1), vld1q_dup_f32(a + 2), b2);
	float32x4_t moved = vaddq_f32(row, vld1q_dup_f32(a + 3));
	vst1q_f32(result, vbslq_f32(isFourth, moved, row));

	row = vmlaq_f32(vmlaq_f32(vmulq_f32(vld1q_dup_f32(a + 4), b0), vld1q_dup_f32(a + 5), b1), vld1q_dup_f32(a + 6), b2);
	moved = vaddq_f32(row, vld1q_dup_f32(a + 7));
	vst1q_f32(result + 4, vbslq_f32(isFourth, moved, row));

	row = vmlaq_f32(vmlaq_f32(vmulq_f32(vld1q_dup_f32(a + 8), b0), vld1q_dup_f32(a + 9), b1), vld1q_dup_f32(a + 10), b2);
	moved = vaddq_f32(row, vld1q_dup_f32(a + 11));
	vst1q_f32(result + 8, vbslq_f32(isFourth, moved, row));
}
