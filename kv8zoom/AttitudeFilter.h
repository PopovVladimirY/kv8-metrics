////////////////////////////////////////////////////////////////////////////////
// kv8zoom/AttitudeFilter.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Frames.h"

#include <cstdint>
#include <deque>

namespace kv8zoom {

/// SLERP-based attitude smoother.
///
/// Maintains a sliding window of raw quaternion samples. When queried, it
/// interpolates between the oldest sample in the window and the newest at the
/// fractional position corresponding to the query timestamp, producing a
/// temporally smooth result.
///
/// If the window contains fewer than two samples a passthrough is returned.
/// All methods must be called from the same thread.
class AttitudeFilter
{
public:
    explicit AttitudeFilter(int window_ms) noexcept;

    /// Add a raw sample. Automatically prunes samples older than window_ms.
    void AddSample(int64_t ts_us, double qw, double qx, double qy, double qz) noexcept;
    void AddSample(const AttFrame& f) noexcept;

    /// Return a smoothed quaternion [w, x, y, z] at the given timestamp.
    /// Writes the result into the four output doubles. The output is normalised.
    void GetFiltered(int64_t ts_us,
                     double& qw, double& qx, double& qy, double& qz) const noexcept;

private:
    struct Sample {
        int64_t ts_us;
        double  qw, qx, qy, qz;
    };

    // Spherical linear interpolation between q0 and q1 at parameter t in [0,1].
    static void Slerp(double  aw, double  ax, double  ay, double  az,
                      double  bw, double  bx, double  by, double  bz,
                      double  t,
                      double& rw, double& rx, double& ry, double& rz) noexcept;

    int64_t          m_windowUs;
    std::deque<Sample> m_buf;
};

} // namespace kv8zoom
