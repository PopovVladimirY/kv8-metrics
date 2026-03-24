////////////////////////////////////////////////////////////////////////////////
// kv8zoom/Frames.h -- binary frame structs that mirror the packed UDT payloads
//                     produced by kv8feeder.
//
// Wire format (one Kafka message):
//   [kv8::Kv8UDTSample (16 bytes)]  <-- header, always skipped
//   [packed struct data  (N bytes)]  <-- decoded into Xpayload below
//
// Layout must match the flattened field order defined in
// examples/kv8feeder/UdtFeeds.h exactly (pack(1), no padding).
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstring>

namespace kv8zoom {

// ---------------------------------------------------------------------------
// AerialNav payload (58 bytes)
// ap.Navigation expands to: ap.GeoPos(3xf64) + ap.Vec3(3xf64) + ap.NavStatus(u8+u8+f32+f32)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct NavPayload
{
    // ap.GeoPos
    double  lat_deg;
    double  lon_deg;
    double  alt_m;
    // ap.Vec3 velocity_ms
    double  vx_ms;
    double  vy_ms;
    double  vz_ms;
    // ap.NavStatus
    uint8_t gps_fix;
    uint8_t sats;
    float   hdop;
    float   baro_alt_m;
};
#pragma pack(pop)
static_assert(sizeof(NavPayload) == 58, "NavPayload size mismatch vs wire format");

// ---------------------------------------------------------------------------
// AerialAtt payload (80 bytes)
// ap.Attitude expands to: ap.Quat(4xf64) + ap.Vec3 angular_rate(3xf64) + ap.Vec3 accel(3xf64)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct AttPayload
{
    // ap.Quat
    double qw, qx, qy, qz;
    // ap.Vec3 angular_rate_rads
    double rx, ry, rz;
    // ap.Vec3 accel_ms2
    double ax, ay, az;
};
#pragma pack(pop)
static_assert(sizeof(AttPayload) == 80, "AttPayload size mismatch vs wire format");

// ---------------------------------------------------------------------------
// AerialMotors payload (20 bytes)
// Flat 5 x f32
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct MotPayload
{
    float battery_v;
    float m1, m2, m3, m4;
};
#pragma pack(pop)
static_assert(sizeof(MotPayload) == 20, "MotPayload size mismatch vs wire format");

// ---------------------------------------------------------------------------
// Environment/WeatherStation payload (28 bytes)
// Flat 7 x f32
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct WxPayload
{
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
    float wind_speed_ms;
    float wind_dir_deg;
    float rain_mm_h;
    float uv_index;
};
#pragma pack(pop)
static_assert(sizeof(WxPayload) == 28, "WxPayload size mismatch vs wire format");

// ---------------------------------------------------------------------------
// Frames -- header adds ts_us (Kafka broker timestamp * 1000) to each payload
// ---------------------------------------------------------------------------

struct NavFrame
{
    int64_t    ts_us = 0;
    NavPayload data  = {};
};

struct AttFrame
{
    int64_t    ts_us = 0;
    AttPayload data  = {};
};

struct MotFrame
{
    int64_t    ts_us = 0;
    MotPayload data  = {};
};

struct WxFrame
{
    int64_t   ts_us = 0;
    WxPayload data  = {};
};

} // namespace kv8zoom
