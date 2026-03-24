////////////////////////////////////////////////////////////////////////////////
// kv8zoom/PathSimplifier.cpp
////////////////////////////////////////////////////////////////////////////////

#include "PathSimplifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kv8zoom {

static constexpr double kPi       = 3.14159265358979323846;
static constexpr double kDeg2Rad  = kPi / 180.0;
static constexpr double kEarthR_m = 6371000.0;

// Convert geographic point to ENU (metres) relative to a reference point.
static void ToENU(const TrailPoint& ref, const TrailPoint& p,
                  double& ex, double& ny, double& up) noexcept
{
    const double lat0 = ref.lat_deg * kDeg2Rad;
    const double cosLat = std::cos(lat0);
    ex = kEarthR_m * cosLat * (p.lon_deg - ref.lon_deg) * kDeg2Rad;
    ny = kEarthR_m             * (p.lat_deg - ref.lat_deg) * kDeg2Rad;
    up = p.alt_m - ref.alt_m;
}

// static
double PathSimplifier::CrossTrackDist(const TrailPoint& a,
                                      const TrailPoint& b,
                                      const TrailPoint& p) noexcept
{
    // Compute bx, by, bz (B relative to A in ENU).
    double bx, by, bz;
    ToENU(a, b, bx, by, bz);
    // Compute px, py, pz (P relative to A in ENU).
    double px, py, pz;
    ToENU(a, p, px, py, pz);

    // Segment length squared.
    const double len2 = bx*bx + by*by + bz*bz;
    if (len2 < 1e-12) {
        // Degenerate segment: return distance from a to p.
        return std::sqrt(px*px + py*py + pz*pz);
    }

    // Project p onto the line through a-b, clamp to [0,1].
    double t = (px*bx + py*by + pz*bz) / len2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    const double dx = px - t * bx;
    const double dy = py - t * by;
    const double dz = pz - t * bz;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

// static
void PathSimplifier::DouglasPeucker(const std::vector<TrailPoint>& pts,
                                    size_t start, size_t end,
                                    double epsilon_m,
                                    std::vector<bool>& keep)
{
    if (end <= start + 1) return; // nothing to do

    double maxDist  = 0.0;
    size_t maxIdx   = start;

    for (size_t i = start + 1; i < end; ++i) {
        const double d = CrossTrackDist(pts[start], pts[end], pts[i]);
        if (d > maxDist) {
            maxDist = d;
            maxIdx  = i;
        }
    }

    if (maxDist > epsilon_m) {
        keep[maxIdx] = true;
        DouglasPeucker(pts, start,  maxIdx, epsilon_m, keep);
        DouglasPeucker(pts, maxIdx, end,    epsilon_m, keep);
    }
}

void PathSimplifier::Append(const TrailPoint& p)
{
    m_pts.push_back(p);

    // When the buffer is full, drop the oldest half to amortise the cost.
    if (m_pts.size() >= kMaxPoints) {
        const size_t half = kMaxPoints / 2;
        m_pts.erase(m_pts.begin(), m_pts.begin() + static_cast<ptrdiff_t>(half));
    }
}

std::vector<TrailPoint> PathSimplifier::Simplify(double epsilon_m, size_t maxOut) const
{
    if (m_pts.empty())  return {};
    if (m_pts.size() == 1) return m_pts;

    const size_t n = m_pts.size();
    std::vector<bool> keep(n, false);
    keep[0]     = true;
    keep[n - 1] = true;

    DouglasPeucker(m_pts, 0, n - 1, epsilon_m, keep);

    std::vector<TrailPoint> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) result.push_back(m_pts[i]);

    // If the result still exceeds maxOut, stride-subsample the kept points.
    if (result.size() > maxOut) {
        const size_t stride = result.size() / maxOut + 1;
        std::vector<TrailPoint> sub;
        sub.reserve(maxOut);
        for (size_t i = 0; i < result.size(); i += stride)
            sub.push_back(result[i]);
        if (sub.empty() || sub.back().ts_us != result.back().ts_us)
            sub.push_back(result.back()); // always include the tail
        result = std::move(sub);
    }

    return result;
}

} // namespace kv8zoom
