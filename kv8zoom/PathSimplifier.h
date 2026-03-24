////////////////////////////////////////////////////////////////////////////////
// kv8zoom/PathSimplifier.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <vector>

namespace kv8zoom {

/// A 3-D geographic point stored in the trail buffer.
struct TrailPoint {
    int64_t ts_us;
    double  lat_deg;
    double  lon_deg;
    double  alt_m;
};

/// Douglas-Peucker trail simplifier for geographic paths.
///
/// Points are appended in real-time as new NAV frames arrive. The Simplify()
/// method returns a reduced subset of points whose cross-track deviation from
/// the original path is no greater than epsilon_m metres.
///
/// The internal buffer is bounded to kMaxPoints. When the capacity is
/// reached, the oldest half of the points is dropped (ring-buffer semantics
/// are not needed because the Simplify step filters the trail anyway).
///
/// All methods must be called from the uWS event-loop thread.
class PathSimplifier
{
public:
    static constexpr size_t kMaxPoints = 65536;

    PathSimplifier() = default;

    /// Append a new trail point.
    void Append(const TrailPoint& p);

    /// Return a simplified copy of the trail with at most maxOut points.
    /// Epsilon is given in metres. Computes cross-track deviation in ENU space.
    std::vector<TrailPoint> Simplify(double epsilon_m, size_t maxOut = 4096) const;

    size_t Size() const noexcept { return m_pts.size(); }
    void   Clear() noexcept     { m_pts.clear(); }

    const std::vector<TrailPoint>& Raw() const noexcept { return m_pts; }

private:
    // Recursive Douglas-Peucker.  Marks points to keep in `keep` array.
    static void DouglasPeucker(const std::vector<TrailPoint>& pts,
                               size_t start, size_t end,
                               double epsilon_m,
                               std::vector<bool>& keep);

    // Great-circle cross-track distance from point `p` to the line segment
    // defined by `a` and `b`, in metres (flat-earth approximation).
    static double CrossTrackDist(const TrailPoint& a,
                                 const TrailPoint& b,
                                 const TrailPoint& p) noexcept;

    std::vector<TrailPoint> m_pts;
};

} // namespace kv8zoom
