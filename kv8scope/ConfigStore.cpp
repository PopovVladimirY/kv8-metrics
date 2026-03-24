// kv8scope -- Kv8 Software Oscilloscope
// ConfigStore.cpp -- Load/save ScopeConfig as JSON via nlohmann/json.

#include "ConfigStore.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// nlohmann/json serialization (ADL free functions)
// ---------------------------------------------------------------------------

static void to_json(json& j, const ScopeConfig& c)
{
    j = json{
        // Connection
        {"brokers",            c.sBrokers},
        {"security_protocol",  c.sSecurityProtocol},
        {"sasl_mechanism",     c.sSaslMechanism},
        {"username",           c.sUsername},
        {"password",           c.sPassword},
        {"session_poll_ms",    c.iSessionPollMs},
        {"meta_stabilize_ms", c.iMetaStabilizeMs},
        // UI
        {"theme",              c.sTheme},
        {"font_face",          c.sFontFace},
        {"font_size",          c.iFontSize},
        {"default_time_win",   c.dDefaultTimeWin},
        {"realtime_time_win",  c.dRealtimeTimeWin},
        {"max_points_per_trace", c.iMaxPointsPerTrace},
        {"viz_mode",           c.sVizMode},
        {"cross_bar",          c.bCrossBar},
        // Runtime
        {"recent_sessions",    c.recentSessions},
        {"counter_viz_modes",  c.counterVizModes},
        {"table_column_widths", c.tableColumnWidths},
        // Liveness
        {"liveness", {
            {"heartbeat_live_s",      c.liveness.iHeartbeatLiveS},
            {"heartbeat_dead_s",      c.liveness.iHeartbeatDeadS},
            {"data_live_s",           c.liveness.iDataLiveS},
            {"data_dead_s",           c.liveness.iDataDeadS},
            {"heartbeat_interval_ms", c.liveness.iHeartbeatIntervalMs}
        }}
    };
}

static void from_json(const json& j, ScopeConfig& c)
{
    // Use value() with defaults so that missing keys keep the struct default.
    c.sBrokers          = j.value("brokers",             c.sBrokers);
    c.sSecurityProtocol = j.value("security_protocol",   c.sSecurityProtocol);
    c.sSaslMechanism    = j.value("sasl_mechanism",      c.sSaslMechanism);
    c.sUsername          = j.value("username",            c.sUsername);
    c.sPassword          = j.value("password",            c.sPassword);
    c.iSessionPollMs    = j.value("session_poll_ms",     c.iSessionPollMs);
    c.iMetaStabilizeMs  = j.value("meta_stabilize_ms",   c.iMetaStabilizeMs);

    c.sTheme            = j.value("theme",               c.sTheme);
    c.sFontFace         = j.value("font_face",           c.sFontFace);
    c.iFontSize         = j.value("font_size",           c.iFontSize);
    c.dDefaultTimeWin   = j.value("default_time_win",    c.dDefaultTimeWin);
    c.dRealtimeTimeWin  = j.value("realtime_time_win",   c.dRealtimeTimeWin);
    c.iMaxPointsPerTrace = j.value("max_points_per_trace", c.iMaxPointsPerTrace);
    c.sVizMode          = j.value("viz_mode",            c.sVizMode);
    c.bCrossBar         = j.value("cross_bar",           c.bCrossBar);

    if (j.contains("recent_sessions") && j["recent_sessions"].is_array())
    {
        c.recentSessions = j["recent_sessions"].get<std::vector<std::string>>();
    }

    if (j.contains("counter_viz_modes") && j["counter_viz_modes"].is_object())
    {
        c.counterVizModes =
            j["counter_viz_modes"]
            .get<std::map<std::string, std::map<std::string, std::string>>>();
    }

    if (j.contains("table_column_widths") && j["table_column_widths"].is_object())
    {
        c.tableColumnWidths =
            j["table_column_widths"]
            .get<std::map<std::string, std::vector<float>>>();
    }

    if (j.contains("liveness") && j["liveness"].is_object())
    {
        const auto& lv = j["liveness"];
        c.liveness.iHeartbeatLiveS      = lv.value("heartbeat_live_s",      c.liveness.iHeartbeatLiveS);
        c.liveness.iHeartbeatDeadS      = lv.value("heartbeat_dead_s",      c.liveness.iHeartbeatDeadS);
        c.liveness.iDataLiveS           = lv.value("data_live_s",           c.liveness.iDataLiveS);
        c.liveness.iDataDeadS           = lv.value("data_dead_s",           c.liveness.iDataDeadS);
        c.liveness.iHeartbeatIntervalMs = lv.value("heartbeat_interval_ms", c.liveness.iHeartbeatIntervalMs);
    }
}

// ---------------------------------------------------------------------------
// ConfigStore
// ---------------------------------------------------------------------------

ConfigStore::ConfigStore(const std::string& sPath)
    : m_sPath(sPath)
{
}

bool ConfigStore::Load()
{
    std::ifstream ifs(m_sPath);
    if (!ifs.is_open())
    {
        fprintf(stderr, "ConfigStore: %s not found, using defaults.\n",
                m_sPath.c_str());
        return false;
    }

    try
    {
        json j = json::parse(ifs);
        from_json(j, m_config);
        m_bDirty = false;
        fprintf(stderr, "ConfigStore: loaded %s\n", m_sPath.c_str());
        return true;
    }
    catch (const json::exception& e)
    {
        fprintf(stderr, "ConfigStore: parse error in %s: %s\n",
                m_sPath.c_str(), e.what());
        // Keep defaults on parse error.
        return false;
    }
}

bool ConfigStore::Save()
{
    json j;
    to_json(j, m_config);

    std::ofstream ofs(m_sPath);
    if (!ofs.is_open())
    {
        fprintf(stderr, "ConfigStore: cannot write %s\n", m_sPath.c_str());
        return false;
    }

    ofs << j.dump(4) << "\n";
    m_bDirty = false;
    fprintf(stderr, "ConfigStore: saved %s\n", m_sPath.c_str());
    return true;
}
