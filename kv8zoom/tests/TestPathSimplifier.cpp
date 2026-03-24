////////////////////////////////////////////////////////////////////////////////
// kv8zoom/tests/TestPathSimplifier.cpp
////////////////////////////////////////////////////////////////////////////////

#include "PathSimplifier.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace kv8zoom;

static TrailPoint MakePt(double lat, double lon, double alt = 0.0, int64_t ts = 0)
{
    return {ts, lat, lon, alt};
}

// A perfectly straight line of N equally-spaced points.
// After simplification with any epsilon > 0, only the endpoints should remain.
static void TestStraightLine()
{
    PathSimplifier ps;
    const int N = 100;
    for (int i = 0; i < N; ++i)
        ps.Append(MakePt(48.0 + i * 0.0001, 2.3522, 0.0, i));

    auto result = ps.Simplify(5.0 /*metres*/);
    // At least start and end must be present.
    assert(result.size() >= 2);
    assert(result.front().lat_deg == 48.0);
    assert(result.back().lat_deg  == 48.0 + (N - 1) * 0.0001);
    // All intermediate points should be culled for a straight line.
    assert(result.size() < (size_t)N);
}

// A path with one large detour: the detour point must survive simplification.
static void TestDetourPreserved()
{
    PathSimplifier ps;
    // Start
    ps.Append(MakePt(0.0, 0.0,  0.0, 0));
    // Detour ~1 km north of the direct line
    ps.Append(MakePt(0.009, 0.005, 0.0, 1)); // ~1 km north detour
    // End
    ps.Append(MakePt(0.0, 0.01, 0.0, 2));

    // With epsilon = 10 m, the detour (>>10 m from the direct line) must be kept.
    auto result = ps.Simplify(10.0);
    assert(result.size() == 3);
}

// Tiny epsilon: all points kept.
static void TestEpsilonZeroKeepsAll()
{
    PathSimplifier ps;
    for (int i = 0; i < 10; ++i)
        ps.Append(MakePt(i * 0.0001, 0.0, 0.0, i));

    auto result = ps.Simplify(0.0);
    assert(result.size() == 10);
}

// Empty simplifier returns empty result.
static void TestEmpty()
{
    PathSimplifier ps;
    auto result = ps.Simplify(5.0);
    assert(result.empty());
}

// Single point returns one-point result.
static void TestSinglePoint()
{
    PathSimplifier ps;
    ps.Append(MakePt(1.0, 2.0));
    auto result = ps.Simplify(5.0);
    assert(result.size() == 1);
}

// maxOut cap is respected.
static void TestMaxOutCap()
{
    PathSimplifier ps;
    for (int i = 0; i < 1000; ++i)
        ps.Append(MakePt(i * 0.00001, i * 0.00001, 0.0, i));

    auto result = ps.Simplify(0.0, /*maxOut=*/50);
    assert(result.size() <= 50);
}

int main()
{
    TestStraightLine();
    TestDetourPreserved();
    TestEpsilonZeroKeepsAll();
    TestEmpty();
    TestSinglePoint();
    TestMaxOutCap();
    fprintf(stderr, "TestPathSimplifier: all pass\n");
    return 0;
}
