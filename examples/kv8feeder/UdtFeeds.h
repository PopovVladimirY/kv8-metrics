////////////////////////////////////////////////////////////////////////////////
// kv8log/examples/kv8feeder/UdtFeeds.h
//
// UDT schema definitions and thread prototypes for the two example feeds:
//
//   Environment/WeatherStation  -- flat, 7 x f32 fields, 1 Hz
//   Aerial/Platform             -- hierarchical, 2-level nesting, 1000 Hz
//
// Include AFTER defining KV8_LOG_ENABLE (or leave it undefined for a
// silent build).  The compile-definition is applied per-target in
// CMakeLists.txt so no manual define is needed here.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "kv8log/KV8_UDT.h"

#include <atomic>

// ============================================================================
// WeatherStation -- flat schema, 7 x f32 @ 1 Hz
// ============================================================================

#define KV8_WS_FIELDS(F, FN, E, EN)                                           \
    FN(f32, temperature_c,  "Temperature (degC)",  -40.0f,  85.0f)            \
    FN(f32, humidity_pct,   "Humidity (%)",           0.0f, 100.0f)           \
    FN(f32, pressure_hpa,   "Pressure (hPa)",        870.0f, 1084.0f)         \
    FN(f32, wind_speed_ms,  "Wind Speed (m/s)",       0.0f,  60.0f)           \
    FN(f32, wind_dir_deg,   "Wind Dir (deg)",          0.0f, 360.0f)          \
    FN(f32, rain_mm_h,      "Rainfall (mm/h)",         0.0f, 200.0f)          \
    FN(f32, uv_index,       "UV Index",                0.0f,  16.0f)

KV8_UDT_DEFINE(WeatherStation, KV8_WS_FIELDS)

// ============================================================================
// Aerial platform feeds -- split into 3 time-synchronised UDTs @ 1000 Hz
//
// The platform telemetry is partitioned into 3 top-level UDTs so each fits
// comfortably within KV8_UDT_MAX_PAYLOAD (240 B).  All three are sent with
// the same captured Unix-epoch timestamp so kv8scope can correlate them:
//
//   AerialNav    (58 B) -- navigation: position, velocity, nav status
//   AerialAtt    (80 B) -- attitude:   orientation, angular rate, acceleration
//   AerialMotors (20 B) -- propulsion: battery voltage + 4 motor RPMs
//
// Sub-schema dependency order (leaf schemas before composites):
//
// Level 1 (leaf primitives):
//   ap.Vec3      -- generic 3D double vector
//   ap.Quat      -- unit quaternion (w, x, y, z)
//   ap.GeoPos    -- geodetic position (lat/lon/alt)
//   ap.NavStatus -- GPS fix quality + barometric altitude
//
// Level 2 (first-order composites):
//   ap.Navigation  -- embeds GeoPos, Vec3, NavStatus
//   ap.Attitude    -- embeds Quat, Vec3 x2
//
// Top-level:
//   AerialNav    -- embeds ap.Navigation (58 bytes)
//   AerialAtt    -- embeds ap.Attitude   (80 bytes)
//   AerialMotors -- flat, 5 x f32        (20 bytes)
// ============================================================================

// -- ap.Vec3 ------------------------------------------------------------------
#define KV8_AP_VEC3_FIELDS(F, FN, E, EN)                                      \
    FN(f64, x, "X", -1e4, 1e4)                                                \
    FN(f64, y, "Y", -1e4, 1e4)                                                \
    FN(f64, z, "Z", -1e4, 1e4)

KV8_UDT_DEFINE_NS(ap, Vec3, KV8_AP_VEC3_FIELDS)

// -- ap.Quat ------------------------------------------------------------------
#define KV8_AP_QUAT_FIELDS(F, FN, E, EN)                                      \
    FN(f64, w, "W", -1.0, 1.0)                                                \
    FN(f64, x, "X", -1.0, 1.0)                                                \
    FN(f64, y, "Y", -1.0, 1.0)                                                \
    FN(f64, z, "Z", -1.0, 1.0)

KV8_UDT_DEFINE_NS(ap, Quat, KV8_AP_QUAT_FIELDS)

// -- ap.GeoPos ----------------------------------------------------------------
#define KV8_AP_GEOPOS_FIELDS(F, FN, E, EN)                                    \
    FN(f64, lat_deg, "Latitude (deg)",   -90.0,   90.0)                       \
    FN(f64, lon_deg, "Longitude (deg)", -180.0,  180.0)                       \
    FN(f64, alt_m,   "Altitude (m)",    -500.0, 9000.0)

KV8_UDT_DEFINE_NS(ap, GeoPos, KV8_AP_GEOPOS_FIELDS)

// -- ap.NavStatus -------------------------------------------------------------
#define KV8_AP_NAVSTATUS_FIELDS(F, FN, E, EN)                                 \
    FN(u8,  gps_fix,    "GPS Fix",         0,      5)                         \
    FN(u8,  sats,       "Satellites",      0,     32)                         \
    FN(f32, hdop,       "HDOP",            0.5f,  99.0f)                      \
    FN(f32, baro_alt_m, "Baro Alt (m)", -500.0f, 9000.0f)

KV8_UDT_DEFINE_NS(ap, NavStatus, KV8_AP_NAVSTATUS_FIELDS)

// -- ap.Navigation ------------------------------------------------------------
// EN(prefix, Name, cname) emits type = "prefix.Name" in JSON, which is the
// same name used by KV8_UDT_DEFINE_NS.  Do NOT use E(ap_Vec3, ...) here --
// that would emit "ap_Vec3" which does not match the schema name "ap.Vec3".
#define KV8_AP_NAVIGATION_FIELDS(F, FN, E, EN)                                \
    EN(ap, GeoPos,    position)                                                \
    EN(ap, Vec3,      velocity_ms)                                            \
    EN(ap, NavStatus, nav_status)

KV8_UDT_DEFINE_NS(ap, Navigation, KV8_AP_NAVIGATION_FIELDS)

// -- ap.Attitude --------------------------------------------------------------
#define KV8_AP_ATTITUDE_FIELDS(F, FN, E, EN)                                  \
    EN(ap, Quat, orientation)                                                  \
    EN(ap, Vec3, angular_rate_rads)                                           \
    EN(ap, Vec3, accel_ms2)

KV8_UDT_DEFINE_NS(ap, Attitude, KV8_AP_ATTITUDE_FIELDS)

// -- AerialNav (top-level: navigation part) ----------------------------------
#define KV8_AERIAL_NAV_FIELDS(F, FN, E, EN)                                   \
    EN(ap, Navigation, navigation)

KV8_UDT_DEFINE(AerialNav, KV8_AERIAL_NAV_FIELDS)

// -- AerialAtt (top-level: attitude part) -------------------------------------
#define KV8_AERIAL_ATT_FIELDS(F, FN, E, EN)                                   \
    EN(ap, Attitude, attitude)

KV8_UDT_DEFINE(AerialAtt, KV8_AERIAL_ATT_FIELDS)

// -- AerialMotors (top-level: propulsion part) --------------------------------
#define KV8_AERIAL_MOTORS_FIELDS(F, FN, E, EN)                                \
    FN(f32, battery_v,  "Battery (V)",    6.0f,  25.2f)                       \
    FN(f32, motor1_rpm, "Motor 1 (RPM)",  0.0f, 12000.0f)                     \
    FN(f32, motor2_rpm, "Motor 2 (RPM)",  0.0f, 12000.0f)                     \
    FN(f32, motor3_rpm, "Motor 3 (RPM)",  0.0f, 12000.0f)                     \
    FN(f32, motor4_rpm, "Motor 4 (RPM)",  0.0f, 12000.0f)

KV8_UDT_DEFINE(AerialMotors, KV8_AERIAL_MOTORS_FIELDS)

// ============================================================================
// Thread entry points (defined in UdtFeeds.cpp)
// ============================================================================

void RunWeatherStation(const std::atomic<bool>& stop);
void RunAerialPlatform(const std::atomic<bool>& stop);
