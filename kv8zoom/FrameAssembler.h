////////////////////////////////////////////////////////////////////////////////
// kv8zoom/FrameAssembler.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Frames.h"

#include <cstdint>

namespace kv8zoom {

/// Combines the latest samples from each feed type into a single coherent
/// snapshot that can be serialised and broadcast to connected clients.
///
/// Fields are updated independently as frames arrive; staleness is tracked
/// at per-feed granularity via the has_* flags.
///
/// All methods must be called from the uWS event-loop thread.
struct OutputFrame {
    // Navigation
    NavPayload nav;
    int64_t    nav_ts_us = 0;
    bool       has_nav   = false;

    // Attitude (SLERP-smoothed)
    double  att_qw = 1.0, att_qx = 0.0, att_qy = 0.0, att_qz = 0.0;
    double  att_rx = 0.0, att_ry = 0.0, att_rz = 0.0;
    double  att_ax = 0.0, att_ay = 0.0, att_az = 0.0;
    int64_t att_ts_us = 0;
    bool    has_att   = false;

    // Motors
    MotPayload mot;
    int64_t    mot_ts_us = 0;
    bool       has_mot   = false;

    // Weather
    WxPayload wx;
    int64_t   wx_ts_us = 0;
    bool      has_wx   = false;
};

class FrameAssembler
{
public:
    FrameAssembler() = default;

    void UpdateNav(const NavFrame& f) noexcept;
    void UpdateAtt(int64_t ts_us,
                   double qw, double qx, double qy, double qz,
                   const AttFrame& raw) noexcept;
    void UpdateMot(const MotFrame& f) noexcept;
    void UpdateWx(const WxFrame& f) noexcept;

    /// True once at least one NAV and one ATT frame have been received.
    bool HasMinimumData() const noexcept { return m_cur.has_nav && m_cur.has_att; }

    const OutputFrame& Latest() const noexcept { return m_cur; }

private:
    OutputFrame m_cur;
};

} // namespace kv8zoom
