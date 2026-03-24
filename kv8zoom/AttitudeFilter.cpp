////////////////////////////////////////////////////////////////////////////////
// kv8zoom/AttitudeFilter.cpp
////////////////////////////////////////////////////////////////////////////////

#include "AttitudeFilter.h"

#include <cmath>

namespace kv8zoom {

AttitudeFilter::AttitudeFilter(int window_ms) noexcept
    : m_windowUs(static_cast<int64_t>(window_ms) * 1000LL)
{
}

void AttitudeFilter::AddSample(int64_t ts_us,
                               double qw, double qx, double qy, double qz) noexcept
{
    // Normalise incoming quaternion to guard against producer drift.
    const double n = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
    if (n > 1e-9) {
        const double inv = 1.0 / n;
        qw *= inv; qx *= inv; qy *= inv; qz *= inv;
    }
    m_buf.push_back({ts_us, qw, qx, qy, qz});

    // Prune samples that have fallen outside the window.
    while (m_buf.size() > 1 && (ts_us - m_buf.front().ts_us) > m_windowUs)
        m_buf.pop_front();
}

void AttitudeFilter::AddSample(const AttFrame& f) noexcept
{
    AddSample(f.ts_us, f.data.qw, f.data.qx, f.data.qy, f.data.qz);
}

void AttitudeFilter::GetFiltered(int64_t ts_us,
                                 double& rw, double& rx, double& ry, double& rz) const noexcept
{
    if (m_buf.empty()) {
        rw = 1.0; rx = ry = rz = 0.0;
        return;
    }
    if (m_buf.size() == 1) {
        rw = m_buf.front().qw; rx = m_buf.front().qx;
        ry = m_buf.front().qy; rz = m_buf.front().qz;
        return;
    }

    const auto& first = m_buf.front();
    const auto& last  = m_buf.back();

    const int64_t span = last.ts_us - first.ts_us;
    double t = 0.5; // default: midpoint of window
    if (span > 0) {
        t = static_cast<double>(ts_us - first.ts_us) / static_cast<double>(span);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }

    Slerp(first.qw, first.qx, first.qy, first.qz,
          last.qw,  last.qx,  last.qy,  last.qz,
          t, rw, rx, ry, rz);
}

// static
void AttitudeFilter::Slerp(double  aw, double  ax, double  ay, double  az,
                            double  bw, double  bx, double  by, double  bz,
                            double  t,
                            double& rw, double& rx, double& ry, double& rz) noexcept
{
    // Ensure the short arc is taken.
    double dot = aw*bw + ax*bx + ay*by + az*bz;
    if (dot < 0.0) {
        bw = -bw; bx = -bx; by = -by; bz = -bz;
        dot = -dot;
    }

    // For very small angles fall back to normalised LERP.
    static constexpr double kThreshold = 0.9995;
    if (dot > kThreshold) {
        rw = aw + t * (bw - aw);
        rx = ax + t * (bx - ax);
        ry = ay + t * (by - ay);
        rz = az + t * (bz - az);
    } else {
        const double theta  = std::acos(dot);
        const double sinInv = 1.0 / std::sin(theta);
        const double s0     = std::sin((1.0 - t) * theta) * sinInv;
        const double s1     = std::sin(t           * theta) * sinInv;
        rw = s0 * aw + s1 * bw;
        rx = s0 * ax + s1 * bx;
        ry = s0 * ay + s1 * by;
        rz = s0 * az + s1 * bz;
    }

    // Re-normalise to prevent accumulation errors in long sessions.
    const double n = std::sqrt(rw*rw + rx*rx + ry*ry + rz*rz);
    if (n > 1e-9) {
        const double inv = 1.0 / n;
        rw *= inv; rx *= inv; ry *= inv; rz *= inv;
    }
}

} // namespace kv8zoom
