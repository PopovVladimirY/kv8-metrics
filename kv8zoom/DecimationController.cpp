////////////////////////////////////////////////////////////////////////////////
// kv8zoom/DecimationController.cpp
////////////////////////////////////////////////////////////////////////////////

#include "DecimationController.h"

#include <algorithm>
#include <cmath>

namespace kv8zoom {

static constexpr double kPi        = 3.14159265358979323846;
static constexpr double kDeg2Rad   = kPi / 180.0;
static constexpr double kEarthR_m  = 6371000.0;

DecimationController::DecimationController(const DecimationConfig& cfg)
    : m_cfg(cfg)
{
    // Ensure zoom_rates are sorted ascending by zoom_max so lookup is correct.
    auto& zr = m_cfg.zoom_rates;
    std::sort(zr.begin(), zr.end(), [](const ZoomRate& a, const ZoomRate& b) {
        return a.zoom_max < b.zoom_max;
    });
}

void DecimationController::SetZoom(int zoom) noexcept
{
    m_zoom = std::max(0, std::min(22, zoom));
}

int64_t DecimationController::IntervalUs() const noexcept
{
    // Walk the zoom_rates table and find the entry whose zoom_max >= current zoom.
    // The last entry acts as the catch-all for the highest zoom band.
    int rate_hz = 1;
    for (const auto& zr : m_cfg.zoom_rates) {
        rate_hz = zr.rate_hz;
        if (m_zoom <= zr.zoom_max) break;
    }
    if (rate_hz <= 0) rate_hz = 1;
    return 1000000LL / rate_hz;
}

// static
double DecimationController::HaversineDist(double lat1, double lon1,
                                           double lat2, double lon2) noexcept
{
    const double dlat = (lat2 - lat1) * kDeg2Rad;
    const double dlon = (lon2 - lon1) * kDeg2Rad;
    const double a    = std::sin(dlat / 2) * std::sin(dlat / 2)
                      + std::cos(lat1 * kDeg2Rad) * std::cos(lat2 * kDeg2Rad)
                      * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * kEarthR_m * std::asin(std::sqrt(a));
}

bool DecimationController::ShouldEmitNav(const NavFrame& f, int64_t now_us) noexcept
{
    const int64_t interval = IntervalUs();

    if (now_us - m_nav.last_emit_us < interval) {
        return false;
    }

    // Minimum displacement gate: suppress if the vehicle has not moved enough.
    if (m_nav.has_ref && m_cfg.min_displacement_m > 0.0) {
        const double dist = HaversineDist(
            f.data.lat_deg, f.data.lon_deg,
            m_nav.last_lat, m_nav.last_lon);
        if (dist < m_cfg.min_displacement_m) {
            // Still update the timer to avoid flood on the next movement.
            m_nav.last_emit_us = now_us;
            return false;
        }
    }

    m_nav.last_emit_us = now_us;
    m_nav.last_lat     = f.data.lat_deg;
    m_nav.last_lon     = f.data.lon_deg;
    m_nav.has_ref      = true;
    return true;
}

bool DecimationController::ShouldEmitAtt(const AttFrame& /*f*/, int64_t now_us) noexcept
{
    if (now_us - m_att.last_emit_us < IntervalUs()) return false;
    m_att.last_emit_us = now_us;
    return true;
}

bool DecimationController::ShouldEmitMot(const MotFrame& /*f*/, int64_t now_us) noexcept
{
    if (now_us - m_mot.last_emit_us < IntervalUs()) return false;
    m_mot.last_emit_us = now_us;
    return true;
}

bool DecimationController::ShouldEmitWx(const WxFrame& /*f*/, int64_t now_us) noexcept
{
    // Weather station updates are already infrequent; use half-rate max.
    const int64_t wxInterval = std::max(IntervalUs(), int64_t(2000000)); // min 2 s
    if (now_us - m_wx.last_emit_us < wxInterval) return false;
    m_wx.last_emit_us = now_us;
    return true;
}

} // namespace kv8zoom
