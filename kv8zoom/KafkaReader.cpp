////////////////////////////////////////////////////////////////////////////////
// kv8zoom/KafkaReader.cpp
////////////////////////////////////////////////////////////////////////////////

#include "KafkaReader.h"

#include <kv8/Kv8Types.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

namespace kv8zoom {

// Display names as defined in the kv8feeder UDT feed registration.
static constexpr const char* kNavName = "Aerial/Navigation";
static constexpr const char* kAttName = "Aerial/Attitude";
static constexpr const char* kMotName = "Aerial/Motors";
static constexpr const char* kWxName  = "WeatherStation";

// â”€â”€ Consumer lifecycle â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

KafkaReader::KafkaReader(const KafkaConfig& cfg) : m_cfg(cfg)
{
    CreateConsumer();
}

void KafkaReader::CreateConsumer()
{
    kv8::Kv8Config kCfg;
    kCfg.sBrokers       = m_cfg.brokers;
    kCfg.sSecurityProto = m_cfg.security_proto;
    kCfg.sSaslMechanism = m_cfg.sasl_mechanism;
    kCfg.sUser          = m_cfg.user;
    kCfg.sPass          = m_cfg.pass;
    kCfg.sGroupID       = m_cfg.group_id;
    m_consumer = kv8::IKv8Consumer::Create(kCfg);
}

void KafkaReader::Reset()
{
    // Called only when the consumer thread is stopped.
    m_topicToFeed.clear();
    m_navRing.clear();
    m_attRing.clear();
    m_motRing.clear();
    m_wxRing.clear();
    m_consumer.reset();
    CreateConsumer();
}

// â”€â”€ Session discovery â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::map<std::string, kv8::SessionMeta> KafkaReader::DiscoverAllSessions()
{
    const std::string sanitized = kv8::Kv8SanitizeChannel(m_cfg.channel);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::map<std::string, kv8::SessionMeta> sessions;

    while (std::chrono::steady_clock::now() < deadline) {
        sessions = m_consumer->DiscoverSessions(sanitized);
        if (!sessions.empty()) break;
        fprintf(stderr, "[KafkaReader] No sessions on '%s', retrying...\n", sanitized.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (sessions.empty())
        fprintf(stderr, "[KafkaReader] Timed out: no sessions found on '%s'\n", sanitized.c_str());
    else
        fprintf(stderr, "[KafkaReader] Found %zu session(s) on '%s'\n",
                sessions.size(), sanitized.c_str());

    return sessions;
}

// â”€â”€ Session subscription â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

int KafkaReader::SubscribeToSession(const kv8::SessionMeta& meta)
{
    int subscribed = 0;

    for (auto& [hash, counters] : meta.hashToCounters) {
        for (auto& cm : counters) {
            if (!cm.bIsUdtFeed) continue;

            FeedType ft;
            if      (cm.sName == kNavName) ft = FeedType::Nav;
            else if (cm.sName == kAttName) ft = FeedType::Att;
            else if (cm.sName == kMotName) ft = FeedType::Mot;
            else if (cm.sName == kWxName)  ft = FeedType::Wx;
            else continue;

            if (m_topicToFeed.count(cm.sDataTopic)) continue; // already subscribed
            m_topicToFeed[cm.sDataTopic] = ft;
            m_consumer->Subscribe(cm.sDataTopic);
            ++subscribed;

            fprintf(stderr, "[KafkaReader] Subscribed '%s' -> '%s'\n",
                    cm.sName.c_str(), cm.sDataTopic.c_str());
        }
    }

    if (subscribed == 0)
        fprintf(stderr, "[KafkaReader] Warning: no matching UDT feeds in session '%s'\n",
                meta.sSessionID.c_str());

    return subscribed;
}

int KafkaReader::SubscribeToSessionFromTimestampMs(const kv8::SessionMeta& meta,
                                                   int64_t seekMs,
                                                   int     timeoutMs)
{
    // Collect the feed topics and map them first (same logic as SubscribeToSession).
    std::vector<std::string> topics;
    for (auto& [hash, counters] : meta.hashToCounters) {
        for (auto& cm : counters) {
            if (!cm.bIsUdtFeed) continue;

            FeedType ft;
            if      (cm.sName == kNavName) ft = FeedType::Nav;
            else if (cm.sName == kAttName) ft = FeedType::Att;
            else if (cm.sName == kMotName) ft = FeedType::Mot;
            else if (cm.sName == kWxName)  ft = FeedType::Wx;
            else continue;

            if (m_topicToFeed.count(cm.sDataTopic)) continue;
            m_topicToFeed[cm.sDataTopic] = ft;
            topics.push_back(cm.sDataTopic);

            fprintf(stderr, "[KafkaReader] Assigning '%s' -> '%s' at ts=%lldms\n",
                    cm.sName.c_str(), cm.sDataTopic.c_str(), (long long)seekMs);
        }
    }

    if (topics.empty()) {
        fprintf(stderr, "[KafkaReader] Warning: no matching UDT feeds in session '%s'\n",
                meta.sSessionID.c_str());
        return 0;
    }

    m_consumer->AssignAtTimestampMs(topics, seekMs, timeoutMs);
    return (int)topics.size();
}

std::map<std::string, kv8::SessionMeta> KafkaReader::QuickDiscover()
{
    kv8::Kv8Config kCfg;
    kCfg.sBrokers       = m_cfg.brokers;
    kCfg.sSecurityProto = m_cfg.security_proto;
    kCfg.sSaslMechanism = m_cfg.sasl_mechanism;
    kCfg.sUser          = m_cfg.user;
    kCfg.sPass          = m_cfg.pass;
    kCfg.sGroupID       = m_cfg.group_id + ".discover";
    auto tmp = kv8::IKv8Consumer::Create(kCfg);
    return tmp->DiscoverSessions(kv8::Kv8SanitizeChannel(m_cfg.channel));
}

std::map<std::string, bool>
KafkaReader::QuickCheckLiveness(const std::map<std::string, kv8::SessionMeta>& sessions,
                                int timeoutMsPerSession)
{
    kv8::Kv8Config kCfg;
    kCfg.sBrokers       = m_cfg.brokers;
    kCfg.sSecurityProto = m_cfg.security_proto;
    kCfg.sSaslMechanism = m_cfg.sasl_mechanism;
    kCfg.sUser          = m_cfg.user;
    kCfg.sPass          = m_cfg.pass;
    kCfg.sGroupID       = m_cfg.group_id + ".liveness";
    auto tmp = kv8::IKv8Consumer::Create(kCfg);

    std::map<std::string, bool> result;
    for (const auto& [prefix, meta] : sessions) {
        const std::string hbTopic = meta.sSessionPrefix + ".hb";
        bool alive = false;
        tmp->ReadLatestRecord(hbTopic, timeoutMsPerSession,
            [&](const void* p, size_t n, int64_t /*tsMs*/) {
                if (n >= sizeof(kv8::HbRecord)) {
                    kv8::HbRecord rec;
                    std::memcpy(&rec, p, sizeof(rec));
                    const int64_t nowMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                    // Live = ALIVE state AND heartbeat is recent (< 10 s = ~3x interval).
                    alive = (rec.bVersion == kv8::KV8_HB_VERSION
                          && rec.bState  == kv8::KV8_HB_STATE_ALIVE
                          && (nowMs - rec.tsUnixMs) < 10000LL);
                }
            });
        result[prefix] = alive;
        fprintf(stderr, "[KafkaReader] Liveness '%s': %s\n",
                meta.sSessionID.c_str(), alive ? "LIVE" : "offline");
    }
    return result;
}

int64_t KafkaReader::GetTopicLatestTimestampMs(const std::string& topic, int timeoutMs)
{
    return m_consumer->GetTopicLatestTimestampMs(topic, timeoutMs);
}

// â”€â”€ Consumer loop â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void KafkaReader::PollOnce(int timeoutMs)
{
    m_consumer->PollBatch(256, timeoutMs,
        [this](std::string_view sTopic,
               const void*      pPayload,
               size_t           cbPayload,
               int64_t          tsKafkaMs)
        {
            OnMessage(sTopic, pPayload, cbPayload, tsKafkaMs);
        });
}

void KafkaReader::RequestStop()
{
    m_stop.store(true, std::memory_order_relaxed);
    m_consumer->Stop();
}

void KafkaReader::OnMessage(std::string_view sTopic,
                            const void*      pPayload,
                            size_t           cbPayload,
                            int64_t          tsKafkaMs)
{
    static constexpr size_t kHdrSize = sizeof(kv8::Kv8UDTSample);
    if (cbPayload < kHdrSize) return;

    auto it = m_topicToFeed.find(std::string(sTopic));
    if (it == m_topicToFeed.end()) return;

    const uint8_t* data     = static_cast<const uint8_t*>(pPayload) + kHdrSize;
    const size_t   dataSize = cbPayload - kHdrSize;
    const int64_t  tsUs     = tsKafkaMs * 1000LL;

    switch (it->second) {

    case FeedType::Nav:
        if (dataSize >= sizeof(NavPayload)) {
            NavFrame f;
            f.ts_us = tsUs;
            std::memcpy(&f.data, data, sizeof(NavPayload));
            m_navRing.push(f);
        }
        break;

    case FeedType::Att:
        if (dataSize >= sizeof(AttPayload)) {
            AttFrame f;
            f.ts_us = tsUs;
            std::memcpy(&f.data, data, sizeof(AttPayload));
            m_attRing.push(f);
        }
        break;

    case FeedType::Mot:
        if (dataSize >= sizeof(MotPayload)) {
            MotFrame f;
            f.ts_us = tsUs;
            std::memcpy(&f.data, data, sizeof(MotPayload));
            m_motRing.push(f);
        }
        break;

    case FeedType::Wx:
        if (dataSize >= sizeof(WxPayload)) {
            WxFrame f;
            f.ts_us = tsUs;
            std::memcpy(&f.data, data, sizeof(WxPayload));
            m_wxRing.push(f);
        }
        break;
    }
}

} // namespace kv8zoom
