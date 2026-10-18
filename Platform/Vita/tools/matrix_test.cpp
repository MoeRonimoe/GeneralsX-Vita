// matrix_test.cpp ////////////////////////////////////////////////////////////
// Checks the product of two matrices that the PS Vita build uses for the bones
// of its models (WWMath/matrix3dneon.h, a row at a time) against the ones it
// replaces there (Matrix3D::mul() and Matrix3D::postMul() of matrix3d.h, a
// number at a time, copied below): every number of every product the same to
// the last bit, also where the result is one of the two matrices.
//
// The NEON code is compiled with neon_on_pc/arm_neon.h, which does in plain C++
// what the intrinsics do. From the repository root, in one line:
//   g++ -O2 -std=c++17 -ffp-contract=off -I Platform/Vita/tools/neon_on_pc
//       -I Core/Libraries/Source/WWVegas/WWMath Platform/Vita/tools/matrix_test.cpp
//       -o build/hosttest/matrix_test.exe
///////////////////////////////////////////////////////////////////////////////

#include "matrix3dneon.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

struct Vector4 { float X, Y, Z, W; };
struct Matrix { Vector4 Row[3]; };

// The library's products, a number at a time ------------------------------------
float submul(const Vector4 &row, float tmp1, float tmp2, float tmp3)
{
	return row.X * tmp1 + row.Y * tmp2 + row.Z * tmp3;
}

/// Matrix3D::mul(): result = A * B. result may be B.
void mul(Matrix &result, const Matrix &A, const Matrix &B)
{
	float tmp1, tmp2, tmp3;

	tmp1 = B.Row[0].X;
	tmp2 = B.Row[1].X;
	tmp3 = B.Row[2].X;

	result.Row[0].X = submul(A.Row[0], tmp1, tmp2, tmp3);
	result.Row[1].X = submul(A.Row[1], tmp1, tmp2, tmp3);
	result.Row[2].X = submul(A.Row[2], tmp1, tmp2, tmp3);

	tmp1 = B.Row[0].Y;
	tmp2 = B.Row[1].Y;
	tmp3 = B.Row[2].Y;

	result.Row[0].Y = submul(A.Row[0], tmp1, tmp2, tmp3);
	result.Row[1].Y = submul(A.Row[1], tmp1, tmp2, tmp3);
	result.Row[2].Y = submul(A.Row[2], tmp1, tmp2, tmp3);

	tmp1 = B.Row[0].Z;
	tmp2 = B.Row[1].Z;
	tmp3 = B.Row[2].Z;

	result.Row[0].Z = submul(A.Row[0], tmp1, tmp2, tmp3);
	result.Row[1].Z = submul(A.Row[1], tmp1, tmp2, tmp3);
	result.Row[2].Z = submul(A.Row[2], tmp1, tmp2, tmp3);

	tmp1 = B.Row[0].W;
	tmp2 = B.Row[1].W;
	tmp3 = B.Row[2].W;

	result.Row[0].W = submul(A.Row[0], tmp1, tmp2, tmp3) + A.Row[0].W;
	result.Row[1].W = submul(A.Row[1], tmp1, tmp2, tmp3) + A.Row[1].W;
	result.Row[2].W = submul(A.Row[2], tmp1, tmp2, tmp3) + A.Row[2].W;
}

/// Matrix3D::postMul(): self = self * that.
void postMul(Matrix &self, const Matrix &that)
{
	for (int row = 0; row < 3; ++row)
	{
		const float tmpX = submul(self.Row[row], that.Row[0].X, that.Row[1].X, that.Row[2].X);
		const float tmpY = submul(self.Row[row], that.Row[0].Y, that.Row[1].Y, that.Row[2].Y);
		const float tmpZ = submul(self.Row[row], that.Row[0].Z, that.Row[1].Z, that.Row[2].Z);
		const float tmpW = submul(self.Row[row], that.Row[0].W, that.Row[1].W, that.Row[2].W);

		self.Row[row].X = tmpX;
		self.Row[row].Y = tmpY;
		self.Row[row].Z = tmpZ;
		self.Row[row].W += tmpW;
	}
}

// ------------------------------------------------------------------------------
uint32_t s_seed = 1;

uint32_t Next()
{
	s_seed ^= s_seed << 13;
	s_seed ^= s_seed >> 17;
	s_seed ^= s_seed << 5;
	return s_seed;
}

float Between(float low, float high)
{
	return low + (high - low) * ((Next() & 0xFFFFFF) / 16777215.0f);
}

/// A rotation with a position, as the bones are, or numbers of any size, with zeros of both
/// signs and ones among them.
Matrix MakeMatrix()
{
	Matrix matrix;
	float *numbers = &matrix.Row[0].X;

	if (Next() % 3 != 0)
	{
		const float angle = Between(-3.2f, 3.2f);
		const float tilt = Between(-1.6f, 1.6f);
		const float scale = Next() % 4 == 0 ? Between(0.1f, 8.0f) : 1.0f;
		matrix.Row[0] = { scale * cosf(angle), -scale * sinf(angle) * cosf(tilt), scale * sinf(angle) * sinf(tilt), Between(-900.0f, 900.0f) };
		matrix.Row[1] = { scale * sinf(angle), scale * cosf(angle) * cosf(tilt), -scale * cosf(angle) * sinf(tilt), Between(-900.0f, 900.0f) };
		matrix.Row[2] = { 0.0f, scale * sinf(tilt), scale * cosf(tilt), Between(-90.0f, 90.0f) };
	}
	else
	{
		for (int i = 0; i < 12; ++i)
			numbers[i] = Between(-1.0f, 1.0f) * powf(10.0f, static_cast<float>(static_cast<int>(Next() % 13) - 6));
	}

	for (int i = 0; i < 12; ++i)
	{
		switch (Next() % 40)
		{
			case 0: numbers[i] = 0.0f; break;
			case 1: numbers[i] = -0.0f; break;
			case 2: numbers[i] = 1.0f; break;
			case 3: numbers[i] = -1.0f; break;
			default: break;
		}
	}

	return matrix;
}

bool Same(const Matrix &a, const Matrix &b)
{
	return memcmp(&a, &b, sizeof(Matrix)) == 0;
}

} // namespace

//-----------------------------------------------------------------------------
int main(int argc, char **argv)
{
	const unsigned int products = argc > 1 ? static_cast<unsigned int>(atoi(argv[1])) : 2000000;
	unsigned int different = 0;

	static_assert(sizeof(Matrix) == 12 * sizeof(float), "the product reads a matrix as twelve numbers in a row");

	for (unsigned int number = 0; number < products; ++number)
	{
		const Matrix a = MakeMatrix();
		const Matrix b = MakeMatrix();

		// Into a third matrix.
		Matrix expected;
		mul(expected, a, b);
		Matrix got;
		Matrix3D_Neon_Multiply(&a.Row[0].X, &b.Row[0].X, &got.Row[0].X);
		bool same = Same(expected, got);

		// Into the first: what postMul() does.
		Matrix expectedInA = a;
		postMul(expectedInA, b);
		Matrix gotInA = a;
		Matrix3D_Neon_Multiply(&gotInA.Row[0].X, &b.Row[0].X, &gotInA.Row[0].X);
		same = same && Same(expectedInA, gotInA) && Same(expectedInA, expected);

		// Into the second, which mul() allows.
		Matrix expectedInB = b;
		mul(expectedInB, a, expectedInB);
		Matrix gotInB = b;
		Matrix3D_Neon_Multiply(&a.Row[0].X, &gotInB.Row[0].X, &gotInB.Row[0].X);
		same = same && Same(expectedInB, gotInB);

		if (!same)
		{
			if (different < 10)
				printf("product %u differs\n", number);
			++different;
		}
	}

	printf("%u products, each into a third matrix, into the first and into the second\n", products);
	if (different != 0)
	{
		printf("%u of them DIFFERENT\n", different);
		return 1;
	}

	printf("the same numbers to the last bit\n");
	return 0;
}
