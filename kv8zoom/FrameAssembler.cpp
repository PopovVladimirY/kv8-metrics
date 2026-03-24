////////////////////////////////////////////////////////////////////////////////
// kv8zoom/FrameAssembler.cpp
////////////////////////////////////////////////////////////////////////////////

#include "FrameAssembler.h"

#include <cstring>

namespace kv8zoom {

void FrameAssembler::UpdateNav(const NavFrame& f) noexcept
{
    std::memcpy(&m_cur.nav, &f.data, sizeof(NavPayload));
    m_cur.nav_ts_us = f.ts_us;
    m_cur.has_nav   = true;
}

void FrameAssembler::UpdateAtt(int64_t ts_us,
                               double qw, double qx, double qy, double qz,
                               const AttFrame& raw) noexcept
{
    m_cur.att_qw = qw; m_cur.att_qx = qx;
    m_cur.att_qy = qy; m_cur.att_qz = qz;
    m_cur.att_rx = raw.data.rx; m_cur.att_ry = raw.data.ry; m_cur.att_rz = raw.data.rz;
    m_cur.att_ax = raw.data.ax; m_cur.att_ay = raw.data.ay; m_cur.att_az = raw.data.az;
    m_cur.att_ts_us = ts_us;
    m_cur.has_att   = true;
}

void FrameAssembler::UpdateMot(const MotFrame& f) noexcept
{
    std::memcpy(&m_cur.mot, &f.data, sizeof(MotPayload));
    m_cur.mot_ts_us = f.ts_us;
    m_cur.has_mot   = true;
}

void FrameAssembler::UpdateWx(const WxFrame& f) noexcept
{
    std::memcpy(&m_cur.wx, &f.data, sizeof(WxPayload));
    m_cur.wx_ts_us = f.ts_us;
    m_cur.has_wx   = true;
}

} // namespace kv8zoom
