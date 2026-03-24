////////////////////////////////////////////////////////////////////////////////
// kv8zoom/Config.h -- runtime configuration structs parsed from kv8zoom.json
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kv8zoom {

struct KafkaConfig
{
    std::string              brokers   = "localhost:19092";
    std::string              security_proto = "sasl_plaintext";
    std::string              sasl_mechanism = "PLAIN";
    std::string              user      = "kv8producer";
    std::string              pass      = "kv8secret";
    std::string              group_id  = "kv8zoom";
    // Channel to scan for sessions (slashes replaced by dots internally).
    // Default matches kv8feeder default channel.
    std::string              channel   = "kv8log/kv8feeder";
};

struct ZoomRate
{
    int zoom_max = 99;
    int rate_hz  = 10;
};

struct DecimationConfig
{
    std::vector<ZoomRate> zoom_rates = {
        {10, 1}, {14, 5}, {17, 15}, {19, 30}, {99, 60}
    };
    double min_displacement_m = 0.5;
};

struct BridgeConfig
{
    uint16_t       ws_port              = 9001;
    int            ring_buffer_seconds  = 30;
    int            snapshot_interval_s  = 5;
    DecimationConfig decimation;
    int            attitude_slerp_window_ms = 100;
};

struct Config
{
    KafkaConfig  kafka;
    BridgeConfig bridge;

    /// Load from JSON file.  Throws std::runtime_error on fatal parse errors.
    static Config LoadFromFile(const std::string& path);

    /// Return defaults (used when no file is found and as base for merging).
    static Config Defaults();
};

} // namespace kv8zoom
