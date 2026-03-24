////////////////////////////////////////////////////////////////////////////////
// kv8zoom/tests/TestAttitudeFilter.cpp
////////////////////////////////////////////////////////////////////////////////

#include "AttitudeFilter.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace kv8zoom;

static bool NearlyEqual(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) < eps;
}

static bool IsUnitQuaternion(double qw, double qx, double qy, double qz, double eps = 1e-9)
{
    double n = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
    return std::abs(n - 1.0) < eps;
}

// With a single sample the filter should return that sample unchanged.
static void TestSingleSample()
{
    AttitudeFilter f(100);
    f.AddSample(0, 1.0, 0.0, 0.0, 0.0);
    double qw, qx, qy, qz;
    f.GetFiltered(0, qw, qx, qy, qz);
    assert(NearlyEqual(qw, 1.0));
    assert(NearlyEqual(qx, 0.0));
    assert(NearlyEqual(qy, 0.0));
    assert(NearlyEqual(qz, 0.0));
}

// Output of SLERP must always be a unit quaternion.
static void TestOutputIsUnit()
{
    AttitudeFilter f(200);
    // Two distinct orientations.
    f.AddSample(0,       1.0,  0.0,  0.0, 0.0);        // identity
    f.AddSample(100000,  0.0,  1.0,  0.0, 0.0);        // 180 deg around X

    double qw, qx, qy, qz;
    f.GetFiltered(50000, qw, qx, qy, qz);              // midpoint
    assert(IsUnitQuaternion(qw, qx, qy, qz, 1e-9));
}

// SLERP at t=0 should be close to q0, at t=1 close to q1.
static void TestSlerpEndpoints()
{
    AttitudeFilter f(200000); // 200 ms window in us
    // q0 = identity
    f.AddSample(0, 1.0, 0.0, 0.0, 0.0);
    // q1 = 90 deg around Z: (cos45, 0, 0, sin45)
    const double s = std::sqrt(2.0) / 2.0;
    f.AddSample(200000, s, 0.0, 0.0, s);

    double qw, qx, qy, qz;
    // At t=0 (query == first ts)
    f.GetFiltered(0, qw, qx, qy, qz);
    assert(IsUnitQuaternion(qw, qx, qy, qz));
    // qw should be close to 1 (identity)
    assert(qw > 0.9);

    // At t=1 (query == last ts)
    f.GetFiltered(200000, qw, qx, qy, qz);
    assert(IsUnitQuaternion(qw, qx, qy, qz));
    // qz should be close to sin45
    assert(std::abs(qz) > 0.7);
}

// Samples older than the window should be pruned.
static void TestWindowPruning()
{
    AttitudeFilter f(100); // 100 ms window

    // Add a sample at t=0
    f.AddSample(0, 1.0, 0.0, 0.0, 0.0);
    // Add a sample 200 ms later; the first should be pruned.
    const double s = std::sqrt(2.0) / 2.0;
    f.AddSample(200000, s, 0.0, 0.0, s);
    // Add another sample 250 ms.
    f.AddSample(250000, 1.0, 0.0, 0.0, 0.0);

    // GetFiltered should return a unit quaternion (no assert on exact value,
    // just verify it doesn't crash and output is normalised).
    double qw, qx, qy, qz;
    f.GetFiltered(225000, qw, qx, qy, qz);
    assert(IsUnitQuaternion(qw, qx, qy, qz, 1e-9));
}

// Unnormalised input quaternion should be normalised internally.
static void TestNormalisesInput()
{
    AttitudeFilter f(100);
    // Scale an identity quaternion by 5 -- should still come out as identity.
    f.AddSample(0, 5.0, 0.0, 0.0, 0.0);
    double qw, qx, qy, qz;
    f.GetFiltered(0, qw, qx, qy, qz);
    assert(NearlyEqual(qw, 1.0, 1e-9));
}

int main()
{
    TestSingleSample();
    TestOutputIsUnit();
    TestSlerpEndpoints();
    TestWindowPruning();
    TestNormalisesInput();
    fprintf(stderr, "TestAttitudeFilter: all pass\n");
    return 0;
}
