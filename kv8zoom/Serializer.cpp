////////////////////////////////////////////////////////////////////////////////
// kv8zoom/Serializer.cpp
////////////////////////////////////////////////////////////////////////////////

#include "Serializer.h"

#include <nlohmann/json.hpp>

namespace kv8zoom {

std::string Serializer::Nav(const OutputFrame& f)
{
    nlohmann::json j;
    j["type"] = "nav";
    j["ts"]   = f.nav_ts_us;
    j["lat"]  = f.nav.lat_deg;
    j["lon"]  = f.nav.lon_deg;
    j["alt"]  = f.nav.alt_m;
    j["vx"]   = f.nav.vx_ms;
    j["vy"]   = f.nav.vy_ms;
    j["vz"]   = f.nav.vz_ms;
    j["fix"]  = f.nav.gps_fix;
    j["sats"] = f.nav.sats;
    j["hdop"] = f.nav.hdop;
    j["baro"] = f.nav.baro_alt_m;
    return j.dump();
}

std::string Serializer::Att(const OutputFrame& f)
{
    nlohmann::json j;
    j["type"] = "att";
    j["ts"]   = f.att_ts_us;
    j["qw"]   = f.att_qw;
    j["qx"]   = f.att_qx;
    j["qy"]   = f.att_qy;
    j["qz"]   = f.att_qz;
    j["rx"]   = f.att_rx;
    j["ry"]   = f.att_ry;
    j["rz"]   = f.att_rz;
    j["ax"]   = f.att_ax;
    j["ay"]   = f.att_ay;
    j["az"]   = f.att_az;
    return j.dump();
}

std::string Serializer::Mot(const OutputFrame& f)
{
    nlohmann::json j;
    j["type"] = "mot";
    j["ts"]   = f.mot_ts_us;
    j["batt"] = f.mot.battery_v;
    j["m"]    = {f.mot.m1, f.mot.m2, f.mot.m3, f.mot.m4};
    return j.dump();
}

std::string Serializer::Wx(const OutputFrame& f)
{
    nlohmann::json j;
    j["type"] = "wx";
    j["ts"]   = f.wx_ts_us;
    j["temp"] = f.wx.temperature_c;
    j["hum"]  = f.wx.humidity_pct;
    j["pres"] = f.wx.pressure_hpa;
    j["ws"]   = f.wx.wind_speed_ms;
    j["wd"]   = f.wx.wind_dir_deg;
    j["rain"] = f.wx.rain_mm_h;
    j["uv"]   = f.wx.uv_index;
    return j.dump();
}

std::string Serializer::Snapshot(const OutputFrame& f)
{
    nlohmann::json j;
    j["type"] = "snap";

    if (f.has_nav) {
        auto& n = j["nav"];
        n["ts"]   = f.nav_ts_us;
        n["lat"]  = f.nav.lat_deg;
        n["lon"]  = f.nav.lon_deg;
        n["alt"]  = f.nav.alt_m;
        n["vx"]   = f.nav.vx_ms;
        n["vy"]   = f.nav.vy_ms;
        n["vz"]   = f.nav.vz_ms;
        n["fix"]  = f.nav.gps_fix;
        n["sats"] = f.nav.sats;
        n["hdop"] = f.nav.hdop;
        n["baro"] = f.nav.baro_alt_m;
    }

    if (f.has_att) {
        auto& a = j["att"];
        a["ts"] = f.att_ts_us;
        a["qw"] = f.att_qw; a["qx"] = f.att_qx;
        a["qy"] = f.att_qy; a["qz"] = f.att_qz;
        a["rx"] = f.att_rx; a["ry"] = f.att_ry; a["rz"] = f.att_rz;
        a["ax"] = f.att_ax; a["ay"] = f.att_ay; a["az"] = f.att_az;
    }

    if (f.has_mot) {
        auto& m = j["mot"];
        m["ts"]   = f.mot_ts_us;
        m["batt"] = f.mot.battery_v;
        m["m"]    = {f.mot.m1, f.mot.m2, f.mot.m3, f.mot.m4};
    }

    if (f.has_wx) {
        auto& w = j["wx"];
        w["ts"]   = f.wx_ts_us;
        w["temp"] = f.wx.temperature_c;
        w["hum"]  = f.wx.humidity_pct;
        w["pres"] = f.wx.pressure_hpa;
        w["ws"]   = f.wx.wind_speed_ms;
        w["wd"]   = f.wx.wind_dir_deg;
        w["rain"] = f.wx.rain_mm_h;
        w["uv"]   = f.wx.uv_index;
    }

    return j.dump();
}

std::string Serializer::Trail(const std::vector<TrailPoint>& pts)
{
    nlohmann::json j;
    j["type"] = "trail";
    auto& arr = j["pts"];
    arr = nlohmann::json::array();
    for (const auto& p : pts) {
        nlohmann::json pt;
        pt["ts"]  = p.ts_us;
        pt["lat"] = p.lat_deg;
        pt["lon"] = p.lon_deg;
        pt["alt"] = p.alt_m;
        arr.push_back(std::move(pt));
    }
    return j.dump();
}

std::string Serializer::Meta(const std::string& channel, int feedCount, int zoom)
{
    nlohmann::json j;
    j["type"]    = "meta";
    j["channel"] = channel;
    j["feeds"]   = feedCount;
    j["zoom"]    = zoom;
    return j.dump();
}

std::string Serializer::SessionList(const std::vector<SessionInfo>& sessions,
                                    const std::string& currentId)
{
    nlohmann::json j;
    j["type"]    = "sessions";
    j["current"] = currentId;
    auto& list = j["list"] = nlohmann::json::array();
    for (const auto& s : sessions) {
        nlohmann::json item;
        item["id"]       = s.id;
        item["name"]     = s.name.empty() ? s.id : s.name;
        item["prefix"]   = s.prefix;
        item["ts_start"] = s.ts_ms;
        item["feeds"]    = s.feeds;
        item["is_live"]  = s.is_live;
        list.push_back(std::move(item));
    }
    return j.dump();
}

std::string Serializer::SessionSelected(const std::string& id, const std::string& name)
{
    nlohmann::json j;
    j["type"] = "session_selected";
    j["id"]   = id;
    j["name"] = name.empty() ? id : name;
    return j.dump();
}

std::string Serializer::Timeline(const TimelineInfo& t)
{
    nlohmann::json j;
    j["type"]       = "timeline";
    j["ts_start"]   = t.ts_start_us;
    j["ts_end"]     = t.ts_end_us;
    j["ts_current"] = t.ts_current_us;
    j["is_live"]    = t.is_live;
    j["is_paused"]  = t.is_paused;
    return j.dump();
}

std::string Serializer::Error(const std::string& msg)
{
    nlohmann::json j;
    j["type"] = "error";
    j["msg"]  = msg;
    return j.dump();
}

} // namespace kv8zoom

