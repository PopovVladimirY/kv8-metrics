////////////////////////////////////////////////////////////////////////////////
// kv8zoom/tests/TestSerializer.cpp
////////////////////////////////////////////////////////////////////////////////

#include "Serializer.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <string>

using namespace kv8zoom;

static OutputFrame MakeFrame()
{
    OutputFrame f;
    // NAV
    f.nav.lat_deg    = 48.8566;
    f.nav.lon_deg    =  2.3522;
    f.nav.alt_m      = 120.5;
    f.nav.vx_ms      =  1.0;
    f.nav.vy_ms      =  2.0;
    f.nav.vz_ms      = -0.5;
    f.nav.gps_fix    =  3;
    f.nav.sats       = 12;
    f.nav.hdop       =  0.8f;
    f.nav.baro_alt_m = 118.0f;
    f.nav_ts_us      = 1000000;
    f.has_nav        = true;
    // ATT
    f.att_qw = 1.0; f.att_qx = 0.0; f.att_qy = 0.0; f.att_qz = 0.0;
    f.att_rx = 0.01; f.att_ry = 0.02; f.att_rz = 0.0;
    f.att_ax = 0.0; f.att_ay = 0.0; f.att_az = -9.81;
    f.att_ts_us = 1000000;
    f.has_att   = true;
    // MOT
    f.mot.battery_v = 14.8f;
    f.mot.m1 = 0.5f; f.mot.m2 = 0.5f; f.mot.m3 = 0.5f; f.mot.m4 = 0.5f;
    f.mot_ts_us = 1000000;
    f.has_mot   = true;
    // WX
    f.wx.temperature_c = 22.0f;
    f.wx.humidity_pct  = 55.0f;
    f.wx.pressure_hpa  = 1013.0f;
    f.wx.wind_speed_ms = 3.5f;
    f.wx.wind_dir_deg  = 180.0f;
    f.wx.rain_mm_h     = 0.0f;
    f.wx.uv_index      = 3.0f;
    f.wx_ts_us = 1000000;
    f.has_wx   = true;
    return f;
}

static void TestNavMessage()
{
    OutputFrame f = MakeFrame();
    std::string s = Serializer::Nav(f);
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "nav");
    assert(j["lat"].get<double>() == f.nav.lat_deg);
    assert(j["lon"].get<double>() == f.nav.lon_deg);
    assert(j["alt"].get<double>() == f.nav.alt_m);
    assert(j["fix"].get<int>()    == f.nav.gps_fix);
    assert(j["sats"].get<int>()   == f.nav.sats);
}

static void TestAttMessage()
{
    OutputFrame f = MakeFrame();
    std::string s = Serializer::Att(f);
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "att");
    assert(j["qw"].get<double>() == 1.0);
    assert(j["az"].get<double>() == f.att_az);
}

static void TestMotMessage()
{
    OutputFrame f = MakeFrame();
    std::string s = Serializer::Mot(f);
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "mot");
    assert(j["batt"].get<float>() == f.mot.battery_v);
    assert(j["m"].size() == 4);
}

static void TestWxMessage()
{
    OutputFrame f = MakeFrame();
    std::string s = Serializer::Wx(f);
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "wx");
    assert(j["temp"].get<float>() == f.wx.temperature_c);
}

static void TestSnapshot()
{
    OutputFrame f = MakeFrame();
    std::string s = Serializer::Snapshot(f);
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "snap");
    assert(j.contains("nav"));
    assert(j.contains("att"));
    assert(j.contains("mot"));
    assert(j.contains("wx"));
    assert(j["nav"]["lat"].get<double>() == f.nav.lat_deg);
}

static void TestTrailMessage()
{
    std::vector<TrailPoint> pts = {
        {0,      48.0, 2.0, 100.0},
        {1000,   48.1, 2.1, 110.0},
        {2000,   48.2, 2.2, 120.0}
    };
    std::string s = Serializer::Trail(pts);
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "trail");
    assert(j["pts"].size() == 3);
    assert(j["pts"][0]["lat"].get<double>() == 48.0);
}

static void TestMetaMessage()
{
    std::string s = Serializer::Meta("kv8log.kv8feeder", 4, 15);
    auto j = nlohmann::json::parse(s);
    assert(j["type"]    == "meta");
    assert(j["feeds"]   == 4);
    assert(j["zoom"]    == 15);
    assert(j["channel"] == "kv8log.kv8feeder");
}

static void TestErrorMessage()
{
    std::string s = Serializer::Error("test error");
    auto j = nlohmann::json::parse(s);
    assert(j["type"] == "error");
    assert(j["msg"]  == "test error");
}

int main()
{
    TestNavMessage();
    TestAttMessage();
    TestMotMessage();
    TestWxMessage();
    TestSnapshot();
    TestTrailMessage();
    TestMetaMessage();
    TestErrorMessage();
    fprintf(stderr, "TestSerializer: all pass\n");
    return 0;
}
