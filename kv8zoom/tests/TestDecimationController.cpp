////////////////////////////////////////////////////////////////////////////////
// kv8zoom/tests/TestDecimationController.cpp
////////////////////////////////////////////////////////////////////////////////

#include "DecimationController.h"

#include <cassert>
#include <cstdio>

using namespace kv8zoom;

static DecimationConfig MakeConfig()
{
    DecimationConfig cfg;
    cfg.min_displacement_m = 1.0;
    cfg.zoom_rates = {
        {3,  1},
        {7,  5},
        {12, 15},
        {17, 30},
        {22, 100}
    };
    return cfg;
}

static NavFrame MakeNav(double lat, double lon)
{
    NavFrame f{};
    f.ts_us           = 0;
    f.data.lat_deg    = lat;
    f.data.lon_deg    = lon;
    return f;
}

// Zoom 0 -> rate 1 Hz -> interval 1 000 000 us
static void TestZoomRateMapping()
{
    DecimationController dc(MakeConfig());
    dc.SetZoom(0);

    NavFrame f = MakeNav(48.8566, 2.3522);

    int64_t now = 0;
    // First emit should always pass (last_emit = 0, now = 0, interval = 0 initially)
    // Actually (0 - 0) >= interval(1000000 us) is false for 0...
    // Let's advance now by the full interval.
    now = 1000001;
    assert(dc.ShouldEmitNav(f, now));

    // Immediately after: not enough time has passed.
    assert(!dc.ShouldEmitNav(f, now + 1));

    // After another full interval.
    assert(dc.ShouldEmitNav(f, now + 1000001));
}

// Zoom 22 -> rate 100 Hz -> interval 10 000 us
static void TestHighZoomRate()
{
    DecimationController dc(MakeConfig());
    dc.SetZoom(22);

    NavFrame f = MakeNav(0.0, 0.0);
    int64_t now = 10001;
    assert(dc.ShouldEmitNav(f, now));
    // 5 000 us later: interval is 10 000 us -> should not emit
    assert(!dc.ShouldEmitNav(f, now + 5000));
    // 10 001 us later: should emit
    assert(dc.ShouldEmitNav(f, now + 10001));
}

// Displacement gate: vehicle stationary -> no emission even after interval
static void TestDisplacementGate()
{
    DecimationController dc(MakeConfig());
    dc.SetZoom(10); // 15 Hz -> 66 667 us

    // First point establishes the reference.
    NavFrame f1 = MakeNav(48.8566, 2.3522);
    int64_t now = 70000; // > interval
    assert(dc.ShouldEmitNav(f1, now));

    // Same position: displacement = 0 < 1 m -> suppress.
    NavFrame f2 = MakeNav(48.8566, 2.3522);
    now += 70000;
    assert(!dc.ShouldEmitNav(f2, now));

    // Move ~100 m north.
    NavFrame f3 = MakeNav(48.8575, 2.3522); // ~100 m north
    now += 70000;
    assert(dc.ShouldEmitNav(f3, now));
}

static void TestSetZoomClamping()
{
    DecimationController dc(MakeConfig());
    dc.SetZoom(-5);  assert(dc.GetZoom() == 0);
    dc.SetZoom(100); assert(dc.GetZoom() == 22);
    dc.SetZoom(10);  assert(dc.GetZoom() == 10);
}

int main()
{
    TestZoomRateMapping();
    TestHighZoomRate();
    TestDisplacementGate();
    TestSetZoomClamping();
    fprintf(stderr, "TestDecimationController: all pass\n");
    return 0;
}
