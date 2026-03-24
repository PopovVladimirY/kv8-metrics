////////////////////////////////////////////////////////////////////////////////
// kv8zoom/KafkaReader.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Config.h"
#include "Frames.h"
#include "SpscRingBuffer.h"

#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace kv8zoom {

enum class FeedType : uint8_t { Nav, Att, Mot, Wx };

/// Discovers kv8feeder UDT feed topics and decodes binary packets into typed
/// SPSC ring buffers. Runs on a dedicated consumer thread.
class KafkaReader
{
public:
    static constexpr size_t kNavRingSize = 32768; // 32 s at 1 kHz
    static constexpr size_t kAttRingSize = 32768;
    static constexpr size_t kMotRingSize = 32768;
    static constexpr size_t kWxRingSize  = 64;

    explicit KafkaReader(const KafkaConfig& cfg);
    ~KafkaReader() = default;

    // Scan Kafka for all sessions on the configured channel.
    // Retries for up to 10 s. Does NOT subscribe to any topics.
    // Returns empty map on timeout.
    std::map<std::string, kv8::SessionMeta> DiscoverAllSessions();

    // Subscribe to the UDT feeds of a specific session.
    // Returns the number of feed topics subscribed.
    int SubscribeToSession(const kv8::SessionMeta& meta);

    // Like SubscribeToSession but positions each topic at the offset
    // corresponding to @p seekMs (milliseconds since Unix epoch) using
    // rd_kafka_offsets_for_times.  No historical replay -- first messages
    // delivered are at or after @p seekMs.
    // Returns the number of feed topics assigned.
    int SubscribeToSessionFromTimestampMs(const kv8::SessionMeta& meta,
                                          int64_t seekMs,
                                          int     timeoutMs = 3000);

    // Tear down the consumer, recreate it, and clear all ring buffers.
    // Call only when the consumer thread is stopped.
    void Reset();

    // Drain one batch of Kafka messages (call from the consumer thread).
    void PollOnce(int timeoutMs = 100);

    // Signal the consumer to stop. Thread-safe.
    void RequestStop();
    bool StopRequested() const noexcept { return m_stop.load(std::memory_order_relaxed); }
    // Clear the stop flag before restarting after Reset().
    void ClearStop() noexcept { m_stop.store(false, std::memory_order_relaxed); }

    // Single-shot session discovery using a fresh temporary consumer.
    // Thread-safe: does not touch m_consumer. No retry on empty result.
    std::map<std::string, kv8::SessionMeta> QuickDiscover();

    // Check the heartbeat topic of each discovered session to determine liveness.
    // Opens a dedicated temporary consumer -- thread-safe, does not touch m_consumer.
    // A session is live when its latest HbRecord has bState == KV8_HB_STATE_ALIVE.
    // timeoutMsPerSession: max wait per heartbeat record read.
    std::map<std::string, bool>
        QuickCheckLiveness(const std::map<std::string, kv8::SessionMeta>& sessions,
                           int timeoutMsPerSession = 1500);

    // Query Kafka for the broker-assigned timestamp (ms since epoch) of the newest
    // message in 'topic'. Returns -1 on failure.
    // MUST be called only when the kafka consumer thread is not running.
    int64_t GetTopicLatestTimestampMs(const std::string& topic, int timeoutMs = 3000);

    // Ring buffer accessors (written by consumer thread, read by uWS thread).
    SpscRingBuffer<NavFrame, kNavRingSize>& NavRing() noexcept { return m_navRing; }
    SpscRingBuffer<AttFrame, kAttRingSize>& AttRing() noexcept { return m_attRing; }
    SpscRingBuffer<MotFrame, kMotRingSize>& MotRing() noexcept { return m_motRing; }
    SpscRingBuffer<WxFrame,  kWxRingSize>&  WxRing()  noexcept { return m_wxRing;  }

private:
    void OnMessage(std::string_view sTopic,
                   const void*      pPayload,
                   size_t           cbPayload,
                   int64_t          tsKafkaMs);
    void CreateConsumer();

    KafkaConfig m_cfg;
    std::unique_ptr<kv8::IKv8Consumer> m_consumer;
    std::unordered_map<std::string, FeedType> m_topicToFeed;

    SpscRingBuffer<NavFrame, kNavRingSize> m_navRing;
    SpscRingBuffer<AttFrame, kAttRingSize> m_attRing;
    SpscRingBuffer<MotFrame, kMotRingSize> m_motRing;
    SpscRingBuffer<WxFrame,  kWxRingSize>  m_wxRing;

    alignas(64) std::atomic<bool> m_stop{false};
};

} // namespace kv8zoom
