////////////////////////////////////////////////////////////////////////////////
// kv8zoom/DecimationController.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Config.h"
#include "Frames.h"

#include <cstdint>
#include <vector>

namespace kv8zoom {

/// Decides whether a frame should be forwarded to clients based on the current
/// zoom level and minimum-displacement constraint.
///
/// Zoom level is the standard slippy-map zoom (0 = whole-world, 22 = street).
/// Each zoom band maps to a maximum output rate (Hz). Below the minimum
/// displacement threshold, NAV frames are additionally suppressed even if the
/// interval has elapsed, preventing jitter when the vehicle is stationary.
///
/// All methods must be called from the uWS event-loop thread.
class DecimationController
{
public:
    explicit DecimationController(const DecimationConfig& cfg);

    /// Update the current zoom level. Called when the frontend sends a camera
    /// message. Zoom is clamped to [0, 22].
    void SetZoom(int zoom) noexcept;
    int  GetZoom() const noexcept { return m_zoom; }

    /// Return true when the NAV frame should be emitted. Updates internal state.
    bool ShouldEmitNav(const NavFrame& f, int64_t now_us) noexcept;

    /// Return true when the ATT frame should be emitted. Updates internal state.
    bool ShouldEmitAtt(const AttFrame& f, int64_t now_us) noexcept;

    /// Return true when the MOT frame should be emitted. Updates internal state.
    bool ShouldEmitMot(const MotFrame& f, int64_t now_us) noexcept;

    /// Return true when the WX frame should be emitted. Updates internal state.
    bool ShouldEmitWx(const WxFrame& f, int64_t now_us) noexcept;

private:
    // Approximate flat-earth distance in metres between two lat/lon pairs.
    static double HaversineDist(double lat1, double lon1,
                                double lat2, double lon2) noexcept;

    int64_t IntervalUs() const noexcept;

    struct PerFeed {
        int64_t last_emit_us = 0;
    };

    struct NavState : PerFeed {
        double last_lat = 0.0;
        double last_lon = 0.0;
        bool   has_ref  = false;
    };

    DecimationConfig m_cfg;
    int              m_zoom{15};
    NavState         m_nav;
    PerFeed          m_att;
    PerFeed          m_mot;
    PerFeed          m_wx;
};

} // namespace kv8zoom
