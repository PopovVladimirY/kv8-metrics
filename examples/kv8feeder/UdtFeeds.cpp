////////////////////////////////////////////////////////////////////////////////
// kv8log/examples/kv8feeder/UdtFeeds.cpp
//
// UDT example feed threads:
//
//   RunWeatherStation -- flat, 7-field weather sample @ 1 Hz
//   RunAerialPlatform -- bumblebee-aerobatics UAV telemetry @ 1000 Hz
//
// Both use tight_wait (sleep + spin) so the emission rate stays accurate
// even under OS timer quantisation.
////////////////////////////////////////////////////////////////////////////////

#include "UdtFeeds.h"

#include <chrono>
#include <cmath>
#include <thread>

#ifdef _WIN32
#  include <intrin.h>   // _mm_pause
#endif

// ── Tight wait (copy of the helper used in main.cpp) ─────────────────────────
// Sleeps to within 200 µs of the deadline then spin-loops precisely.
// For intervals <= 200 µs the entire wait is a spin-loop.
namespace {

static void tight_wait(std::chrono::steady_clock::time_point deadline)
{
    using Clock = std::chrono::steady_clock;
    using us    = std::chrono::microseconds;
    constexpr us kSpinMargin{200};
    const auto sleep_tp = deadline - kSpinMargin;
    if (Clock::now() < sleep_tp)
        std::this_thread::sleep_until(sleep_tp);
    while (Clock::now() < deadline)
    {
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#endif
    }
}

} // anonymous namespace

// ── Thread: Environment/WeatherStation @ 1 Hz ───────────────────────────────
// Produces slow-moving sinusoidal values mimicking real sensor trends.
// All fields fit in a flat 28-byte packed struct.
void RunWeatherStation(const std::atomic<bool>& stop)
{
    KV8_UDT_FEED(wx, WeatherStation, "Environment/WeatherStation");

    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::seconds(1);
    auto next = Clock::now();
    uint64_t n = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        next += interval;
        tight_wait(next);
        ++n;

        // Slow-changing index.  Each unit = 1 second.
        const float t = static_cast<float>(n) * 0.01f;

        Kv8UDT_WeatherStation s;
        s.temperature_c = 20.0f  + 10.0f * sinf(t);
        s.humidity_pct  = 62.0f  + 18.0f * sinf(t * 0.7f  + 1.0f);
        s.pressure_hpa  = 1013.0f + 5.0f * sinf(t * 0.3f);
        s.wind_speed_ms =  8.0f  +  6.0f * sinf(t * 1.3f  + 2.0f);
        s.wind_dir_deg  = fmodf(s.wind_speed_ms * 20.0f + t * 15.0f, 360.0f);
        s.rain_mm_h     = (s.wind_speed_ms > 12.0f)
                          ? (s.wind_speed_ms - 12.0f) * 3.0f
                          : 0.0f;
        s.uv_index      = 5.0f + 4.0f * sinf(t * 0.5f);

        KV8_UDT_ADD(wx, s);
    }
}

// -- Thread: Aerial platform @ 1000 Hz ----------------------------------------
// Simulates a UAV performing "bumblebee aerobatics" -- erratic, darting flight
// within a confined territory (~25 m radius, 42-58 m altitude).
//
// The path uses superimposed Lissajous harmonics with incommensurable
// frequencies, producing a complex, never-exactly-repeating trajectory that
// captures the quick direction changes and altitude bobbing of insect flight.
//
// Attitude (heading, pitch, bank) is derived from the velocity vector and
// lateral acceleration (coordinated-turn model).  Body-frame angular rates
// and specific-force (accelerometer) readings are computed analytically.
//
// Telemetry is partitioned into three time-synchronised UDT feeds so each
// payload stays within KV8_UDT_MAX_PAYLOAD (240 B):
//
//   Aerial/Navigation -- position, velocity, nav status    (58 B)
//   Aerial/Attitude   -- orientation, angular rate, accel  (80 B)
//   Aerial/Motors     -- battery voltage + 4 motor RPMs    (20 B)
//
// All three are sent with the same captured Unix-epoch timestamp.
void RunAerialPlatform(const std::atomic<bool>& stop)
{
    KV8_UDT_FEED(aerial_nav,    AerialNav,    "Aerial/Navigation");
    KV8_UDT_FEED(aerial_att,    AerialAtt,    "Aerial/Attitude");
    KV8_UDT_FEED(aerial_motors, AerialMotors, "Aerial/Motors");

    using Clock    = std::chrono::steady_clock;
    const auto interval = std::chrono::microseconds(1000); // 1 kHz
    auto next = Clock::now();

    static const double kPi              = 3.14159265358979323846;
    static const double kDt              = 0.001;  // seconds per tick
    static const double kCenterLat       = 47.6062;
    static const double kCenterLon       = -122.3321;
    static const double kMetersPerDegLat = 111320.0;
    const double kMetersPerDegLon        = kMetersPerDegLat
                                           * cos(kCenterLat * kPi / 180.0);

    // ---- Bumblebee multi-harmonic Lissajous flight path --------------------
    // Each spatial axis is a sum of 4 sinusoids whose frequencies are mutually
    // incommensurable (no rational ratio), so the trajectory never exactly
    // repeats.  Amplitudes are chosen so the total excursion fits inside the
    // "bumblebee territory":
    //   Horizontal: ~25 m radius from centre
    //   Vertical:   ~42 -- 58 m altitude
    //
    // Harmonic roles:
    //   [0] slow patrol drift      (period ~20 s)
    //   [1] investigation loop     (period ~7-8 s)
    //   [2] darting manoeuvre       (period ~3-4 s)
    //   [3] insect-like jitter     (period ~1.5-2 s)

    struct Harm { double a; double w; double p; };

    static const Harm hx[] = {          // X position: SIN
        {12.0, 0.31, 0.00},            //   slow patrol
        { 7.0, 0.73, 0.90},            //   investigation loop
        { 4.0, 1.71, 2.10},            //   darting
        { 2.0, 3.67, 0.40},            //   jitter
    };
    static const Harm hy[] = {          // Y position: COS
        {11.0, 0.41, 0.00},
        { 7.0, 0.93, 1.30},
        { 4.0, 1.57, 0.70},
        { 2.0, 3.31, 1.90},
    };
    static const double kBaseAlt = 50.0;
    static const Harm hz[] = {          // Z position: SIN (centred on kBaseAlt)
        {4.0, 0.53, 0.00},
        {2.0, 1.19, 0.50},
        {1.5, 2.29, 1.70},
        {0.8, 4.07, 0.30},
    };

    static const int NX = static_cast<int>(sizeof(hx) / sizeof(hx[0]));
    static const int NY = static_cast<int>(sizeof(hy) / sizeof(hy[0]));
    static const int NZ = static_cast<int>(sizeof(hz) / sizeof(hz[0]));

    double t        = 0.0;
    float  battery  = 24.0f;
    double prev_hdg = 0.0;
    double prev_pit = 0.0;
    double prev_bnk = 0.0;
    bool   first    = true;

    while (!stop.load(std::memory_order_relaxed)) {
        next += interval;
        tight_wait(next);
        t += kDt;

        // ---- Position (metres, local tangent plane) -----------------------
        double px = 0.0, py = 0.0, pz = kBaseAlt;
        for (int i = 0; i < NX; ++i)
            px += hx[i].a * sin(hx[i].w * t + hx[i].p);
        for (int i = 0; i < NY; ++i)
            py += hy[i].a * cos(hy[i].w * t + hy[i].p);
        for (int i = 0; i < NZ; ++i)
            pz += hz[i].a * sin(hz[i].w * t + hz[i].p);

        // ---- Velocity (analytical first derivative) -----------------------
        double vx = 0.0, vy = 0.0, vz = 0.0;
        for (int i = 0; i < NX; ++i)
            vx +=  hx[i].a * hx[i].w * cos(hx[i].w * t + hx[i].p);
        for (int i = 0; i < NY; ++i)
            vy += -hy[i].a * hy[i].w * sin(hy[i].w * t + hy[i].p);
        for (int i = 0; i < NZ; ++i)
            vz +=  hz[i].a * hz[i].w * cos(hz[i].w * t + hz[i].p);

        // ---- Acceleration (analytical second derivative) ------------------
        double ax = 0.0, ay = 0.0, az = 0.0;
        for (int i = 0; i < NX; ++i)
            ax += -hx[i].a * hx[i].w * hx[i].w
                    * sin(hx[i].w * t + hx[i].p);
        for (int i = 0; i < NY; ++i)
            ay += -hy[i].a * hy[i].w * hy[i].w
                    * cos(hy[i].w * t + hy[i].p);
        for (int i = 0; i < NZ; ++i)
            az += -hz[i].a * hz[i].w * hz[i].w
                    * sin(hz[i].w * t + hz[i].p);

        // ---- Geodetic position -------------------------------------------
        const double lat = kCenterLat + py / kMetersPerDegLat;
        const double lon = kCenterLon + px / kMetersPerDegLon;
        const double alt = pz;

        // ---- Attitude from velocity vector (coordinated-turn model) ------
        // Guard: when horizontal speed is near zero (direction reversals),
        // hold previous heading to prevent discontinuous jumps.
        const double spd_xy  = sqrt(vx * vx + vy * vy);
        const double kMinSpd = 0.5; // m/s -- below this, hold heading/bank
        const double heading = (spd_xy > kMinSpd)
                              ? atan2(vy, vx) : prev_hdg;
        const double pitch   = (spd_xy > kMinSpd)
                              ? atan2(vz, spd_xy) : 0.0;

        // Bank (roll): lateral specific-force in a coordinated turn
        const double a_lat = (spd_xy > kMinSpd)
                           ? (vx * ay - vy * ax) / spd_xy : 0.0;
        double bank = atan2(a_lat, 9.81);
        const double kMaxBank = 70.0 * kPi / 180.0;
        if (bank >  kMaxBank) bank =  kMaxBank;
        if (bank < -kMaxBank) bank = -kMaxBank;

        // ---- Quaternion (ZYX Tait-Bryan: heading, pitch, bank) -----------
        const double ch = cos(heading * 0.5), sh = sin(heading * 0.5);
        const double cp = cos(pitch   * 0.5), sp = sin(pitch   * 0.5);
        const double cr = cos(bank    * 0.5), sr = sin(bank    * 0.5);

        const double qw = cr*cp*ch + sr*sp*sh;
        const double qx = sr*cp*ch - cr*sp*sh;
        const double qy = cr*sp*ch + sr*cp*sh;
        const double qz = cr*cp*sh - sr*sp*ch;

        // ---- Body angular rates (Euler-rate kinematics, ZYX) -------------
        double omega_x = 0.0, omega_y = 0.0, omega_z = 0.0;
        if (!first) {
            double dh = heading - prev_hdg;
            if (dh >  kPi) dh -= 2.0 * kPi;   // unwrap
            if (dh < -kPi) dh += 2.0 * kPi;
            const double hdot = dh             / kDt;
            const double pdot = (pitch - prev_pit) / kDt;
            const double rdot = (bank  - prev_bnk) / kDt;
            omega_x =  rdot - hdot * sin(pitch);
            omega_y =  pdot * cos(bank)  + hdot * sin(bank) * cos(pitch);
            omega_z = -pdot * sin(bank)  + hdot * cos(bank) * cos(pitch);

            // Clamp to physically reasonable limits (~720 deg/s)
            const double kMaxOmega = 12.56; // rad/s
            if (omega_x >  kMaxOmega) omega_x =  kMaxOmega;
            if (omega_x < -kMaxOmega) omega_x = -kMaxOmega;
            if (omega_y >  kMaxOmega) omega_y =  kMaxOmega;
            if (omega_y < -kMaxOmega) omega_y = -kMaxOmega;
            if (omega_z >  kMaxOmega) omega_z =  kMaxOmega;
            if (omega_z < -kMaxOmega) omega_z = -kMaxOmega;
        }
        first    = false;
        prev_hdg = heading;
        prev_pit = pitch;
        prev_bnk = bank;

        // ---- Body-frame specific force (accelerometer reading) -----------
        // Specific force = kinematic_accel + gravity  (Z-up convention)
        const double sfx_i = ax;
        const double sfy_i = ay;
        const double sfz_i = az + 9.81;

        // Rotate inertial -> body via R^T  (R built from the quaternion)
        const double r00 = 1.0 - 2.0*(qy*qy + qz*qz);
        const double r10 = 2.0*(qx*qy + qz*qw);
        const double r20 = 2.0*(qx*qz - qy*qw);
        const double r01 = 2.0*(qx*qy - qz*qw);
        const double r11 = 1.0 - 2.0*(qx*qx + qz*qz);
        const double r21 = 2.0*(qy*qz + qx*qw);
        const double r02 = 2.0*(qx*qz + qy*qw);
        const double r12 = 2.0*(qy*qz - qx*qw);
        const double r22 = 1.0 - 2.0*(qx*qx + qy*qy);

        const double sf_bx = r00*sfx_i + r10*sfy_i + r20*sfz_i;
        const double sf_by = r01*sfx_i + r11*sfy_i + r21*sfz_i;
        const double sf_bz = r02*sfx_i + r12*sfy_i + r22*sfz_i;

        // ---- Motor / power simulation ------------------------------------
        // Thrust factor: gravity-compensating baseline scaled by vertical
        // demand.  Aggressive manoeuvres push motor RPM higher.
        double thrust_f = (az + 9.81) / 9.81;
        if (thrust_f < 0.2) thrust_f = 0.2;
        if (thrust_f > 3.0) thrust_f = 3.0;
        const float base_rpm = 3200.0f * static_cast<float>(thrust_f);

        // Characteristic bumblebee "buzz" (~25 Hz visible ripple in scope)
        const float buzz =
            120.0f * sinf(static_cast<float>(t * 157.08));

        // Differential RPM for roll / pitch authority
        const float roll_d  = 200.0f * sinf(static_cast<float>(bank));
        const float pitch_d = 150.0f * sinf(static_cast<float>(pitch));

        // Battery: faster drain during aggressive flight
        const float intensity =
            static_cast<float>(fabs(a_lat) * 0.1 + fabs(vz) * 0.2);
        battery -= 0.00002f * (1.0f + intensity);
        if (battery < 12.0f) battery = 24.0f;

        // GPS quality: slight variation with time
        const uint8_t sats = static_cast<uint8_t>(
            10 + static_cast<int>(4.0 * sin(t * 0.1)));
        const float hdop =
            1.0f + 0.5f * static_cast<float>(sin(t * 0.23));

        // ---- Single shared timestamp for all three feeds -----------------
        KV8_TIME(ts_ns);

        // -- Aerial/Navigation ---------------------------------------------
        Kv8UDT_AerialNav nav{};
        nav.navigation.position.lat_deg      = lat;
        nav.navigation.position.lon_deg      = lon;
        nav.navigation.position.alt_m        = alt;
        nav.navigation.velocity_ms.x         = vx;
        nav.navigation.velocity_ms.y         = vy;
        nav.navigation.velocity_ms.z         = vz;
        nav.navigation.nav_status.gps_fix    = 3;
        nav.navigation.nav_status.sats       = sats;
        nav.navigation.nav_status.hdop       = hdop;
        nav.navigation.nav_status.baro_alt_m = static_cast<float>(alt);
        KV8_UDT_ADD_TS(aerial_nav, nav, ts_ns);

        // -- Aerial/Attitude -----------------------------------------------
        Kv8UDT_AerialAtt att{};
        att.attitude.orientation.w       = qw;
        att.attitude.orientation.x       = qx;
        att.attitude.orientation.y       = qy;
        att.attitude.orientation.z       = qz;
        att.attitude.angular_rate_rads.x = omega_x;
        att.attitude.angular_rate_rads.y = omega_y;
        att.attitude.angular_rate_rads.z = omega_z;
        att.attitude.accel_ms2.x         = sf_bx;
        att.attitude.accel_ms2.y         = sf_by;
        att.attitude.accel_ms2.z         = sf_bz;
        KV8_UDT_ADD_TS(aerial_att, att, ts_ns);

        // -- Aerial/Motors -------------------------------------------------
        Kv8UDT_AerialMotors motors{};
        motors.battery_v  = battery;
        auto clamp_rpm = [](float rpm) -> float {
            if (rpm <    0.0f) return    0.0f;
            if (rpm > 12000.0f) return 12000.0f;
            return rpm;
        };
        motors.motor1_rpm = clamp_rpm(base_rpm + buzz + roll_d + pitch_d);
        motors.motor2_rpm = clamp_rpm(base_rpm + buzz - roll_d + pitch_d);
        motors.motor3_rpm = clamp_rpm(base_rpm - buzz + roll_d - pitch_d);
        motors.motor4_rpm = clamp_rpm(base_rpm - buzz - roll_d - pitch_d);
        KV8_UDT_ADD_TS(aerial_motors, motors, ts_ns);
    }
}
