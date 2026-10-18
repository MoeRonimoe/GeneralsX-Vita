// frustum_test.cpp ///////////////////////////////////////////////////////////
// Checks the tests of a sphere and a box against a frustum that the PS Vita build
// uses (WWMath/colmathfrustumneon.h, four planes at a time) against the ones it
// replaces there (colmathfrustum.cpp with colmathplane.h and colmathplane.cpp,
// one plane at a time, copied below): the same answer for every sphere and box,
// also for those that touch a plane exactly.
//
// The NEON code is compiled with neon_on_pc/arm_neon.h, which does in plain C++
// what the intrinsics do. From the repository root, in one line:
//   g++ -O2 -std=c++17 -ffp-contract=off -I Platform/Vita/tools/neon_on_pc
//       -I Core/Libraries/Source/WWVegas/WWMath Platform/Vita/tools/frustum_test.cpp
//       -o build/hosttest/frustum_test.exe
///////////////////////////////////////////////////////////////////////////////

#include "colmathfrustumneon.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

const float COINCIDENCE_EPSILON = 0.000001f;

enum { POS = 0x01, NEG = 0x02, ON = 0x04, BOTH = 0x08, OUTSIDE = POS, INSIDE = NEG, OVERLAPPED = BOTH };

struct Vector3 { float X, Y, Z; };
struct Plane { Vector3 N; float D; };
struct Sphere { Vector3 Center; float Radius; };
struct Box { Vector3 Center; Vector3 Extent; };

// The library's tests, one plane at a time -----------------------------------
float Dot_Product(const Vector3 &a, const Vector3 &b)
{
	return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

int Overlap_Test(const Plane &plane, const Sphere &sphere)
{
	float dist = Dot_Product(sphere.Center, plane.N) - plane.D;
	if (dist > sphere.Radius)
		return POS;
	if (dist < -sphere.Radius)
		return NEG;
	return BOTH;
}

int Overlap_Test(const Plane &plane, const Vector3 &point)
{
	float delta = Dot_Product(point, plane.N) - plane.D;
	if (delta > COINCIDENCE_EPSILON)
		return POS;
	if (delta < -COINCIDENCE_EPSILON)
		return NEG;
	return ON;
}

bool Fast_Is_Float_Positive(const float &val)
{
	uint32_t bits;
	memcpy(&bits, &val, sizeof(bits));
	return !(bits & 0x80000000u);
}

int Overlap_Test(const Plane &plane, const Box &box)
{
	Vector3 posfarpt;
	posfarpt.X = Fast_Is_Float_Positive(plane.N.X) ? box.Extent.X : -box.Extent.X;
	posfarpt.Y = Fast_Is_Float_Positive(plane.N.Y) ? box.Extent.Y : -box.Extent.Y;
	posfarpt.Z = Fast_Is_Float_Positive(plane.N.Z) ? box.Extent.Z : -box.Extent.Z;

	Vector3 negfarpt = { -posfarpt.X, -posfarpt.Y, -posfarpt.Z };
	posfarpt.X += box.Center.X;
	posfarpt.Y += box.Center.Y;
	posfarpt.Z += box.Center.Z;
	negfarpt.X += box.Center.X;
	negfarpt.Y += box.Center.Y;
	negfarpt.Z += box.Center.Z;
	if (Overlap_Test(plane, negfarpt) == POS)
		return POS;
	if (Overlap_Test(plane, posfarpt) == NEG)
		return NEG;
	return BOTH;
}

template <class Shape>
int Overlap_Test(const Plane *planes, const Shape &shape)
{
	int mask = 0;
	for (int i = 0; i < 6; i++)
	{
		int result = Overlap_Test(planes[i], shape);
		if (result == OUTSIDE)
			return OUTSIDE;
		mask |= result;
	}

	if (mask == INSIDE)
		return INSIDE;
	return OVERLAPPED;
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

/// Six planes around a point, facing outward like the planes of a view, at odd angles or,
/// one time in four, along the axes, where a sphere can touch a plane exactly.
void MakeFrustum(Plane *planes)
{
	const bool axes = Next() % 4 == 0;
	const Vector3 middle = { Between(-500.0f, 500.0f), Between(-500.0f, 500.0f), Between(-100.0f, 100.0f) };
	for (int i = 0; i < 6; ++i)
	{
		Vector3 normal = { 0.0f, 0.0f, 0.0f };
		if (axes)
		{
			(&normal.X)[i / 2] = (i & 1) ? -1.0f : 1.0f;
			// A zero with a sign, which the test for boxes looks at.
			if (Next() % 3 == 0)
				(&normal.X)[(i / 2 + 1) % 3] = -0.0f;
		}
		else
		{
			normal.X = Between(-1.0f, 1.0f);
			normal.Y = Between(-1.0f, 1.0f);
			normal.Z = Between(-1.0f, 1.0f);
			const float length = sqrtf(Dot_Product(normal, normal));
			if (length < 0.001f)
			{
				normal.X = 1.0f;
			}
			else
			{
				normal.X /= length;
				normal.Y /= length;
				normal.Z /= length;
			}
		}

		planes[i].N = normal;
		planes[i].D = (axes ? floorf(Dot_Product(normal, middle)) + static_cast<float>(Next() % 300)
			: Dot_Product(normal, middle) + Between(0.0f, 300.0f));
	}
}

} // namespace

//-----------------------------------------------------------------------------
int main(int argc, char **argv)
{
	const unsigned int frustums = argc > 1 ? static_cast<unsigned int>(atoi(argv[1])) : 200000;
	unsigned int different = 0;
	unsigned int counts[2][9] = {};
	unsigned int touching = 0;
	unsigned int tested = 0;

	static_assert(sizeof(Plane) == 16 && sizeof(Sphere) == 16 && sizeof(Box) == 24, "the tests read these as numbers in a row");

	for (unsigned int number = 0; number < frustums; ++number)
	{
		Plane planes[6];
		MakeFrustum(planes);
		const bool whole = Next() % 4 == 0;

		for (int shape = 0; shape < 24; ++shape)
		{
			// Whole numbers put spheres and corners onto planes that lie along the axes.
			Sphere sphere;
			sphere.Center.X = whole ? static_cast<float>(static_cast<int>(Next() % 1600) - 800) : Between(-800.0f, 800.0f);
			sphere.Center.Y = whole ? static_cast<float>(static_cast<int>(Next() % 1600) - 800) : Between(-800.0f, 800.0f);
			sphere.Center.Z = whole ? static_cast<float>(static_cast<int>(Next() % 400) - 200) : Between(-200.0f, 200.0f);
			sphere.Radius = whole ? static_cast<float>(Next() % 200) : Between(0.0f, 200.0f);

			Box box;
			box.Center = sphere.Center;
			box.Extent.X = whole ? static_cast<float>(Next() % 150) : Between(0.0f, 150.0f);
			box.Extent.Y = whole ? static_cast<float>(Next() % 150) : Between(0.0f, 150.0f);
			box.Extent.Z = whole ? static_cast<float>(Next() % 150) : Between(0.0f, 150.0f);

			const int expectedSphere = Overlap_Test(planes, sphere);
			const int gotSphere = Frustum_Neon_Sphere(&planes[0].N.X, &sphere.Center.X);
			const int expectedBox = Overlap_Test(planes, box);
			const int gotBox = Frustum_Neon_Box(&planes[0].N.X, &box.Center.X, COINCIDENCE_EPSILON);
			++counts[0][expectedSphere];
			++counts[1][expectedBox];
			++tested;

			// How many of the spheres lie against a plane exactly, where "greater" and "greater
			// or equal" give different answers.
			for (int i = 0; i < 6; ++i)
			{
				const float dist = Dot_Product(sphere.Center, planes[i].N) - planes[i].D;
				if (dist == sphere.Radius || dist == -sphere.Radius)
				{
					++touching;
					break;
				}
			}

			if (expectedSphere != gotSphere || expectedBox != gotBox)
			{
				if (different < 10)
					printf("frustum %u: sphere %d instead of %d, box %d instead of %d\n", number, gotSphere, expectedSphere, gotBox, expectedBox);
				++different;
			}
		}
	}

	printf("%u spheres: %u outside, %u inside, %u across a plane; %u touch a plane exactly\n", tested,
		counts[0][OUTSIDE], counts[0][INSIDE], counts[0][OVERLAPPED], touching);
	printf("%u boxes: %u outside, %u inside, %u across a plane\n", tested, counts[1][OUTSIDE], counts[1][INSIDE], counts[1][OVERLAPPED]);
	if (different != 0)
	{
		printf("%u of them DIFFERENT\n", different);
		return 1;
	}

	printf("the same answers\n");
	return 0;
}
