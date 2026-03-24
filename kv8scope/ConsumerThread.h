// kv8scope -- Kv8 Software Oscilloscope
// ConsumerThread.h -- Background Kafka consumer thread for one session.
//
// Owns an IKv8Consumer, subscribes to all data topics in the session,
// decodes Kv8TelValue messages, converts QPC timestamps via
// TimeConverter, and pushes TelemetrySample into per-counter
// SpscRingBuffer instances.
//
// The render thread drains the ring buffers each frame (P2.3).

#pragma once

#include "SpscRingBuffer.h"
#include "TimeConverter.h"

#include <kv8/Kv8Types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kv8 { class IKv8Consumer; }
class AnnotationStore;
class ConfigStore;
class StatsEngine;

class ConsumerThread
{
public:
    /// @param meta     Session metadata (topics, counters, timer anchors).
    /// @param pConfig  Application config (broker address, credentials).
    ConsumerThread(const kv8::SessionMeta& meta,
                   const ConfigStore*       pConfig);
    ~ConsumerThread();

    // Non-copyable, non-movable.
    ConsumerThread(const ConsumerThread&)            = delete;
    ConsumerThread& operator=(const ConsumerThread&) = delete;
    ConsumerThread(ConsumerThread&&)                 = delete;
    ConsumerThread& operator=(ConsumerThread&&)      = delete;

    /// Launch the background thread.  Call only once.
    void Start();

    /// Signal the thread to stop after the current PollBatch cycle.
    void RequestStop();

    /// Block until the thread has exited.  Call after RequestStop().
    void Join();

    /// True once Run() has fully completed (consumer destroyed, thread about
    /// to exit).  Safe to call from any thread (relaxed atomic).  When this
    /// returns true, Join() is guaranteed to return instantly.
    bool IsDone() const
    { return m_bDone.load(std::memory_order_acquire); }

    /// Attach a StatsEngine to receive full-duration accumulator updates.
    /// Must be called before Start().  pStats may be nullptr (disables feeding).
    void SetStatsEngine(StatsEngine* pStats) { m_pStatsEngine = pStats; }

    /// Attach an AnnotationStore and the annotation topic name.
    /// When set, the consumer thread performs an initial ConsumeTopicFromBeginning
    /// on the topic, then subscribes for live updates.  Messages are forwarded
    /// to pStore->PushFromKafka() and drained on the render thread via DrainPending().
    /// Must be called before Start().  pStore may be nullptr (disables annotation loading).
    void SetAnnotationStore(AnnotationStore* pStore, std::string sTopic)
    {
        m_pAnnotationStore = pStore;
        m_sAnnotationTopic = std::move(sTopic);
    }

    /// Set the ._ctl topic for counter enable/disable command reception.
    /// Must be called before Start().  Empty string disables this feature.
    void SetCtlTopic(std::string sTopic) { m_sCtlTopic = std::move(sTopic); }

    /// Drain pending counter state events into the caller-supplied callback.
    /// Call from the main/render thread each frame.
    void DrainCounterStateEvents(
        const std::function<void(uint16_t wid, bool bEnabled)>& cb);

    // ---- Ring buffer access (called from the render / main thread) ------

    /// Get the ring buffer for a counter identified by (groupHash, counterID).
    /// Returns nullptr if the counter is not known.
    SpscRingBuffer<TelemetrySample>* GetRingBuffer(uint32_t dwHash,
                                                   uint16_t wCounterID);

    /// Enumerate all (dwHash, wCounterID) pairs that have ring buffers.
    std::vector<std::pair<uint32_t, uint16_t>> GetCounterKeys() const;

    // ---- Metrics (relaxed atomic reads, safe from any thread) -----------

    /// Total messages consumed since Start().
    uint64_t GetTotalMessages() const
    { return m_nTotalMessages.load(std::memory_order_relaxed); }

    /// Wall-clock timestamp (Unix epoch seconds) of the most recent sample.
    /// Used for online detection: if (now - GetLastMessageTime()) < 10 s
    /// the session is considered Online.
    double GetLastMessageTime() const
    { return m_dLastMsgTime.load(std::memory_order_relaxed); }

    /// Number of samples dropped because a ring buffer was full.
    uint64_t GetDropCount() const
    { return m_nDropped.load(std::memory_order_relaxed); }

    /// True once the consumer has connected to Kafka and subscribed.
    bool IsConnected() const
    { return m_bConnected.load(std::memory_order_relaxed); }

private:
    void Run();                ///< Thread entry point.
    void InitTimeConverters(); ///< Build per-topic TimeConverter instances.
    void InitRingBuffers();    ///< Pre-allocate per-counter ring buffers.

    /// Composite map key from group hash + counter ID.
    static uint64_t MakeKey(uint32_t dwHash, uint16_t wID)
    {
        return (static_cast<uint64_t>(dwHash) << 16) | wID;
    }

    // ---- Immutable after construction -----------------------------------
    kv8::SessionMeta   m_meta;
    const ConfigStore* m_pConfig;

    /// Pre-computed per-topic context for the hot path.
    /// Linear scan is faster than map lookup for the small number of topics
    /// in a typical session (<20).
    struct TopicCtx
    {
        std::string   sTopic;
        TimeConverter converter;
        uint32_t      dwHash = 0;
    };
    std::vector<TopicCtx> m_topicCtx;

    /// Per-counter ring buffers.  Key = MakeKey(dwHash, wCounterID).
    /// Written by the consumer thread (Push), read by the renderer (Pop).
    std::map<uint64_t, std::unique_ptr<SpscRingBuffer<TelemetrySample>>>
        m_ringBuffers;

    // ---- Thread state ---------------------------------------------------
    std::thread       m_thread;
    std::atomic<bool> m_bStop{false};
    std::atomic<bool> m_bDone{false};
    std::atomic<bool> m_bConnected{false};

    // ---- Metrics (written by consumer thread, read by main thread) ------
    std::atomic<uint64_t> m_nTotalMessages{0};
    std::atomic<double>   m_dLastMsgTime{0.0};
    std::atomic<uint64_t> m_nDropped{0};

    /// StatsEngine pointer -- set before Start(), read on consumer thread.
    /// Plain pointer (no ownership); lifetime managed by ScopeWindow.
    StatsEngine* m_pStatsEngine = nullptr;

    /// AnnotationStore pointer -- set before Start(), read on consumer thread.
    /// Plain pointer (no ownership); lifetime managed by ScopeWindow.
    AnnotationStore* m_pAnnotationStore = nullptr;
    std::string      m_sAnnotationTopic;

    // ---- Counter enable/disable state (._ctl topic) ---------------------

    /// Topic name for counter control commands (<sessionPrefix>._ctl).
    /// Empty = feature disabled.
    std::string m_sCtlTopic;

    struct CounterStateEvent { uint16_t wid; bool bEnabled; };
    std::mutex                     m_ctlMu;
    std::vector<CounterStateEvent> m_pendingCtl;

    /// Parse a ._ctl JSON payload and push a CounterStateEvent.
    /// Thread-safe (acquires m_ctlMu).
    void ParseAndQueueCtl(const void* pData, size_t cbData);

    /// Pre-built per-topic UDT field decode context (hot path).
    /// Key = UDT data topic string; value = ordered list of fields to decode.
    struct UdtFieldDecodeCtx
    {
        uint64_t nKey;        ///< MakeKey(schemaHash, wCounterID) for ring buffer lookup
        uint32_t dwHash;      ///< Schema hash (group) -- for StatsEngine
        uint16_t wCounterID;  ///< Virtual counter ID  -- for StatsEngine
        uint16_t wOffset;     ///< Byte offset within packed UDT payload
        uint8_t  nFieldType;  ///< Kv8UdtFieldType cast to uint8_t
    };
    std::unordered_map<std::string, std::vector<UdtFieldDecodeCtx>> m_udtTopicFields;

    /// Ring buffer capacity per counter (entries).
    /// 2^20 = ~1M entries = 16 MB per counter -- holds ~10 seconds of data
    /// at 100K msg/s, far exceeding the inter-frame drain interval.
    static constexpr uint64_t kRingCapacity = 1u << 20;
};
