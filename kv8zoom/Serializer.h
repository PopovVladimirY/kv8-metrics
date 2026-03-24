////////////////////////////////////////////////////////////////////////////////
// kv8zoom/Serializer.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "FrameAssembler.h"
#include "PathSimplifier.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kv8zoom {

/// Plain data describing a single discovered session. Used in the sessions
/// list message sent to newly connected clients.
struct SessionInfo {
    std::string id;       ///< sSessionID (e.g. "20240101T120000Z-AB12-CD34")
    std::string name;     ///< Human-readable name (sName, may be empty)
    std::string prefix;   ///< Full session prefix (sSessionPrefix)
    int64_t     ts_ms;    ///< Session start time, ms since Unix epoch (0 if unknown)
    int         feeds;    ///< Number of UDT feed topics found
    bool        is_live = false; ///< True when heartbeat shows KV8_HB_STATE_ALIVE
};

/// Converts frame data to JSON strings ready for WebSocket broadcast.
///
/// Every public method returns a compact JSON string without trailing newline.
/// No allocations are cached; each call constructs a fresh string.
struct Serializer {
    // ── Live telemetry messages ───────────────────────────────────────────────

    /// {"type":"nav", "ts":..., "lat":..., "lon":..., "alt":...,
    ///  "vx":..., "vy":..., "vz":...,
    ///  "fix":..., "sats":..., "hdop":..., "baro":...}
    static std::string Nav(const OutputFrame& f);

    /// {"type":"att", "ts":...,
    ///  "qw":..., "qx":..., "qy":..., "qz":...,
    ///  "rx":..., "ry":..., "rz":...,
    ///  "ax":..., "ay":..., "az":...}
    static std::string Att(const OutputFrame& f);

    /// {"type":"mot", "ts":..., "batt":..., "m":[m1,m2,m3,m4]}
    static std::string Mot(const OutputFrame& f);

    /// {"type":"wx", "ts":..., "temp":..., "hum":..., "pres":...,
    ///  "ws":..., "wd":..., "rain":..., "uv":...}
    static std::string Wx(const OutputFrame& f);

    // ── Session-level messages ────────────────────────────────────────────────

    /// Full snapshot with the latest values of all feeds. Sent on client
    /// connect and periodically as a keep-alive / re-sync mechanism.
    /// {"type":"snap", "nav":{...}, "att":{...}, "mot":{...}, "wx":{...}}
    static std::string Snapshot(const OutputFrame& f);

    /// Ground-track trail as a JSON array of {ts,lat,lon,alt} objects.
    /// {"type":"trail", "pts":[...]}
    static std::string Trail(const std::vector<TrailPoint>& pts);

    /// Connection/status metadata sent on connect.
    /// {"type":"meta", "channel":..., "feeds":..., "zoom":...}
    static std::string Meta(const std::string& channel, int feedCount, int zoom);

    /// List of all discovered sessions.
    /// {"type":"sessions", "list":[{"id":...,"name":...,"ts_start":...,"feeds":...}],
    ///  "current":"<activeId or empty>"}
    static std::string SessionList(const std::vector<SessionInfo>& sessions,
                                   const std::string& currentId);

    /// Broadcast after a session switch so clients know to reset their state.
    /// {"type":"session_selected", "id":..., "name":...}
    static std::string SessionSelected(const std::string& id, const std::string& name);

    /// Timeline info broadcast every second and on client connect.
    /// {"type":"timeline","ts_start":<us>,"ts_end":<us>,"ts_current":<us>,"is_live":<bool>}
    struct TimelineInfo {
        int64_t ts_start_us;   ///< Timestamp of first received frame (microseconds)
        int64_t ts_end_us;     ///< Timestamp of most recent received frame
        int64_t ts_current_us; ///< Timestamp of most recently emitted frame
        bool    is_live;       ///< True when streaming normally (not fast-forwarding)
        bool    is_paused = false; ///< True when server is suppressing live emission
    };
    static std::string Timeline(const TimelineInfo& t);

    /// Error notification.
    /// {"type":"error", "msg":...}
    static std::string Error(const std::string& msg);
};

} // namespace kv8zoom
