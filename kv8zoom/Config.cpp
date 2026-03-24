////////////////////////////////////////////////////////////////////////////////
// kv8zoom/Config.cpp
////////////////////////////////////////////////////////////////////////////////

#include "Config.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace kv8zoom {

Config Config::Defaults()
{
    return Config{};
}

Config Config::LoadFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("kv8zoom: cannot open config file: " + path);

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("kv8zoom: JSON parse error: ") + e.what());
    }

    Config cfg = Defaults();

    if (j.contains("kafka") && j["kafka"].is_object()) {
        auto& k = j["kafka"];
        if (k.contains("brokers"))          cfg.kafka.brokers          = k["brokers"].get<std::string>();
        if (k.contains("security_proto"))   cfg.kafka.security_proto   = k["security_proto"].get<std::string>();
        if (k.contains("sasl_mechanism"))   cfg.kafka.sasl_mechanism   = k["sasl_mechanism"].get<std::string>();
        if (k.contains("user"))             cfg.kafka.user             = k["user"].get<std::string>();
        if (k.contains("pass"))             cfg.kafka.pass             = k["pass"].get<std::string>();
        if (k.contains("group_id"))         cfg.kafka.group_id         = k["group_id"].get<std::string>();
        if (k.contains("channel"))          cfg.kafka.channel          = k["channel"].get<std::string>();
    }

    if (j.contains("bridge") && j["bridge"].is_object()) {
        auto& b = j["bridge"];
        if (b.contains("ws_port"))             cfg.bridge.ws_port             = b["ws_port"].get<uint16_t>();
        if (b.contains("ring_buffer_seconds")) cfg.bridge.ring_buffer_seconds = b["ring_buffer_seconds"].get<int>();
        if (b.contains("snapshot_interval_s")) cfg.bridge.snapshot_interval_s = b["snapshot_interval_s"].get<int>();
        if (b.contains("attitude_slerp_window_ms"))
            cfg.bridge.attitude_slerp_window_ms = b["attitude_slerp_window_ms"].get<int>();

        if (b.contains("decimation") && b["decimation"].is_object()) {
            auto& d = b["decimation"];
            if (d.contains("min_displacement_m"))
                cfg.bridge.decimation.min_displacement_m = d["min_displacement_m"].get<double>();
            if (d.contains("zoom_rates") && d["zoom_rates"].is_array()) {
                cfg.bridge.decimation.zoom_rates.clear();
                for (auto& entry : d["zoom_rates"]) {
                    ZoomRate zr;
                    if (entry.contains("zoom_max")) zr.zoom_max = entry["zoom_max"].get<int>();
                    if (entry.contains("rate_hz"))  zr.rate_hz  = entry["rate_hz"].get<int>();
                    cfg.bridge.decimation.zoom_rates.push_back(zr);
                }
            }
        }
    }

    return cfg;
}

} // namespace kv8zoom
