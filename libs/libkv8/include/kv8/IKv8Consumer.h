////////////////////////////////////////////////////////////////////////////////
// kv8/IKv8Consumer.h -- abstract consumer interface for kv8 telemetry sessions
//
// Instances are created exclusively through the static factory function
// IKv8Consumer::Create().  No librdkafka headers are exposed; callers are
// completely decoupled from the underlying transport library.
//
// Typical usage patterns
// ──────────────────────
// 1. kv8cli -- continuous channel monitor:
//
//      auto c = IKv8Consumer::Create(cfg);
//      c->Subscribe(channel + "._registry");
//      while (running) {
//          std::string newTopic;
//          c->Poll(200, [&](topic, payload, len, ts) {
//              if (topic ends "._registry")
//                  newTopic = decodeRegistry(payload, len);
//              else
//                  handleDataOrLog(topic, payload, len, ts);
//          });
//          if (!newTopic.empty()) c->Subscribe(newTopic);
//      }
//
// 2. kv8bridge -- session discovery then targeted replay:
//
//      auto c = IKv8Consumer::Create(cfg);
//      auto sessions = c->DiscoverSessions(channel);
//      for (auto &t : session.dataTopics) c->Subscribe(t);
//      c->Subscribe(session.sControlTopic);
//      while (running)
//          c->Poll(200, [&](topic, payload, len, ts) { bridge(topic, payload, ts); });
//
// 3. kv8verify -- point-in-time topic reads:
//
//      auto c = IKv8Consumer::Create(cfg);
//      c->ConsumeTopicFromBeginning(manifestTopic, 15000,
//          [&](payload, len, ts) { parseManifest(payload); });
//      c->ConsumeTopicFromBeginning(dataTopic, 35000,
//          [&](payload, len, ts) { verifyMessage(payload); });
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Kv8Types.h"

#include <functional>
#include <string_view>
#include <memory>

namespace kv8 {

/// Abstract consumer for kv8 telemetry sessions stored in Kafka.
///
/// Thread safety: all methods must be called from a single thread unless
/// the documentation for a specific method states otherwise.
class IKv8Consumer
{
public:
    virtual ~IKv8Consumer() = default;

    // ── Channel discovery ──────────────────────────────────────────────────────

    /// Scan all Kafka topics on the broker and return channel prefixes whose
    /// registry topic name starts with "kv8." (case-insensitive).
    ///
    /// A "channel" is identified by a topic named "<prefix>._registry".
    /// The returned strings are the <prefix> part, e.g. "kv8.myapp".
    ///
    /// Uses rd_kafka_metadata() -- no messages are consumed.
    ///
    /// @param timeoutMs  Broker metadata request timeout in milliseconds.
    /// @return           Sorted list of unique channel prefixes found.
    virtual std::vector<std::string> ListChannels(int timeoutMs = 5000) = 0;

    // ── Session discovery ──────────────────────────────────────────────────────

    /// Scan the channel-level registry topic (<sChannel>._registry) from offset 0
    /// to the first PARTITION_EOF and return all sessions discovered.
    ///
    /// Opens a dedicated temporary consumer internally; does not disturb the
    /// active Subscribe()/Poll() subscription state.
    ///
    /// @param sChannel  Channel prefix with slashes already replaced by dots.
    /// @return          Map of sessionPrefix -> SessionMeta (may be empty).
    virtual std::map<std::string, SessionMeta>
        DiscoverSessions(const std::string &sChannel) = 0;

    // ── Streaming subscription ─────────────────────────────────────────────────

    /// Add @p sTopic to the active consumer-group subscription list.
    /// Idempotent: calling with the same topic multiple times is safe.
    ///
    /// The rebalance callback seeks all newly assigned topic-partitions to
    /// OFFSET_BEGINNING the first time they are seen in this instance's
    /// lifetime, so late subscriptions do not miss historical data.
    ///
    /// Must NOT be called from within the onMessage callback of Poll().
    virtual void Subscribe(const std::string &sTopic) = 0;

    /// Poll for at most one message, waiting up to @p timeoutMs milliseconds.
    ///
    /// When a data message arrives, @p onMessage is invoked synchronously
    /// before Poll() returns.  Calling Subscribe() inside the callback is
    /// not permitted -- collect topic names and subscribe after Poll() returns.
    ///
    /// Timeout and PARTITION_EOF are silently ignored (normal conditions).
    ///
    /// @param timeoutMs  Maximum wait time in milliseconds.
    /// @param onMessage  (topic, payload, payloadLen, kafkaTimestampMs)
    virtual void Poll(int timeoutMs,
                      const std::function<void(std::string_view   sTopic,
                                               const void         *pPayload,
                                               size_t              cbPayload,
                                               int64_t             tsKafkaMs)> &onMessage) = 0;

    /// Signal the consumer to stop.  Subsequent Poll() calls return immediately.
    /// Safe to call from any thread (e.g. a signal handler).
    virtual void Stop() = 0;

    /// Drain up to @p maxMessages messages from the rdkafka queue in a single
    /// call.  Blocks on the first message for up to @p timeoutMs milliseconds,
    /// then drains any additional already-queued messages with timeout=0.
    ///
    /// Use this instead of a Poll()-per-message loop when the producer rate is
    /// high: one PollBatch(N) call amortises per-call overhead across N messages
    /// and allows the consumer thread to keep up with a fast producer.
    ///
    /// @param maxMessages  Upper bound on messages processed in this call.
    /// @param timeoutMs    Max wait for the first message (milliseconds).
    /// @param onMessage    Same callback signature as Poll().
    /// @return             Number of messages processed (0 if nothing arrived).
    virtual int PollBatch(
        int maxMessages,
        int timeoutMs,
        const std::function<void(std::string_view   sTopic,
                                 const void         *pPayload,
                                 size_t              cbPayload,
                                 int64_t             tsKafkaMs)> &onMessage) = 0;

    // ── Point-in-time topic reads ──────────────────────────────────────────────

    /// Read all messages from @p sTopic starting from offset BEGINNING, up to
    /// the first PARTITION_EOF or @p hardTimeoutMs milliseconds elapsed.
    ///
    /// Opens a dedicated temporary consumer (assign-mode, not subscribe-mode)
    /// so the active Subscribe()/Poll() subscription is not modified.
    ///
    /// Intended for manifest reads (kv8verify) and similar one-shot reads.
    ///
    /// @param sTopic        Kafka topic to read.
    /// @param hardTimeoutMs Wall-clock deadline in milliseconds.
    /// @param onMessage     (payload, payloadLen, kafkaTimestampMs)
    virtual void ConsumeTopicFromBeginning(
        const std::string &sTopic,
        int                hardTimeoutMs,
        const std::function<void(const void *pPayload,
                                 size_t      cbPayload,
                                 int64_t     tsKafkaMs)> &onMessage) = 0;

    // ── Point-in-time tail reads ───────────────────────────────────────────────

    /// Read the single newest message from @p sTopic and invoke @p onMessage.
    ///
    /// Queries the broker for the high-watermark offset of partition 0, then
    /// opens a temporary assign-mode consumer positioned at (high - 1) to
    /// retrieve just that one message.
    ///
    /// Returns true if exactly one message was delivered to @p onMessage.
    /// Returns false when the topic is empty, does not exist, or the broker
    /// cannot be reached within @p hardTimeoutMs milliseconds.
    ///
    /// The active Subscribe()/Poll() subscription is not modified.
    ///
    /// @param sTopic        Kafka topic to tail (single-partition assumed).
    /// @param hardTimeoutMs Deadline shared across watermark query + consume.
    /// @param onMessage     (payload, payloadLen, kafkaTimestampMs)
    virtual bool ReadLatestRecord(
        const std::string &sTopic,
        int hardTimeoutMs,
        const std::function<void(const void *pPayload,
                                 size_t      cbPayload,
                                 int64_t     tsKafkaMs)> &onMessage) = 0;

    /// Return the Kafka timestamp (milliseconds since Unix epoch) of the
    /// newest message in @p sTopic, or -1 if the topic is empty / unreachable.
    ///
    /// Implemented as a thin wrapper around ReadLatestRecord() -- it reads
    /// the record and extracts the broker-assigned timestamp.
    ///
    /// @param sTopic     Kafka topic to query.
    /// @param timeoutMs  Maximum time to wait, in milliseconds.
    virtual int64_t GetTopicLatestTimestampMs(
        const std::string &sTopic,
        int timeoutMs = 2000) = 0;

    // ── Timestamp-based direct assignment ────────────────────────────────────

    /// Position the streaming consumer at the partition offsets that correspond
    /// to the messages with Kafka broker timestamps >= @p tsMs.
    ///
    /// Uses rd_kafka_offsets_for_times() to resolve timestamps, then
    /// rd_kafka_assign() to directly assign each topic-partition-0 at the
    /// returned offset.  Bypasses consumer-group coordination entirely, so
    /// the first PollBatch() call after this returns messages at or after
    /// @p tsMs with zero historical replay.
    ///
    /// If a topic has no message at or after @p tsMs, its partition is assigned
    /// at OFFSET_END (only future messages).  If the lookup times out, the
    /// partition falls back to OFFSET_BEGINNING.
    ///
    /// Must be called while the consumer thread is stopped (i.e. after Reset()
    /// and before starting the poll thread).
    ///
    /// @param topics     Data topics to assign (partition 0 assumed for all).
    /// @param tsMs       Target timestamp in milliseconds since Unix epoch.
    /// @param timeoutMs  Broker request timeout for offset resolution.
    virtual void AssignAtTimestampMs(const std::vector<std::string> &topics,
                                     int64_t tsMs,
                                     int     timeoutMs = 3000) = 0;

    // ── Topic statistics ───────────────────────────────────────────────────────

    /// Query the approximate message count for each topic in @p topics.
    ///
    /// Uses broker watermark offsets (high - low per partition, summed across
    /// all partitions).  The query is a metadata call -- no message data is
    /// transferred.  Accurate for non-compacted topics; approximate for
    /// compacted or time-deleted ones.
    ///
    /// Returns a map of topic -> message count.
    /// Topics that could not be queried within @p timeoutMs are mapped to -1.
    ///
    /// @param topics     Topics to query.
    /// @param timeoutMs  Per-topic broker query timeout in milliseconds.
    virtual std::map<std::string, int64_t>
        GetTopicMessageCounts(const std::vector<std::string> &topics,
                              int timeoutMs = 5000) = 0;

    // ── Session deletion ───────────────────────────────────────────────────────

    /// Delete all Kafka topics that belong to @p sm (data topics + _log + _ctl)
    /// using the Kafka Admin API.  Requires the broker ACL to allow topic deletion.
    ///
    /// @param sm  Session metadata returned by DiscoverSessions().
    virtual void DeleteSessionTopics(const SessionMeta &sm) = 0;

    /// Write a tombstone KV8_CID_DELETED record to <sChannel>._registry marking
    /// this session as deleted.  Future calls to DiscoverSessions() will skip it.
    ///
    /// @param sChannel  Channel prefix (e.g. "kv8.myapp").
    /// @param sm        Session metadata returned by DiscoverSessions().
    virtual void MarkSessionDeleted(const std::string &sChannel,
                                    const SessionMeta &sm) = 0;

    /// Delete ALL Kafka topics belonging to this channel: every topic whose name
    /// starts with "<sChannel>." (discovered via broker metadata), plus the
    /// <sChannel>._registry topic itself.
    ///
    /// After this call the channel is completely gone from the broker.
    ///
    /// @param sChannel  Channel prefix (e.g. "kv8.myapp").
    virtual void DeleteChannel(const std::string &sChannel) = 0;

    /// Create a Kafka topic with the given number of partitions.
    ///
    /// Idempotent: if the topic already exists the call returns true silently.
    /// More partitions = more parallel fetch streams = higher consumer throughput.
    ///
    /// @param sTopic            Topic name.
    /// @param numPartitions     Number of partitions to create.
    /// @param replicationFactor Replication factor (use 1 for single-broker).
    /// @param timeoutMs         Admin API request timeout in milliseconds.
    /// @return                  true on success or topic-already-exists.
    virtual bool CreateTopic(const std::string &sTopic,
                             int                numPartitions,
                             int                replicationFactor = 1,
                             int                timeoutMs         = 8000) = 0;

    // ── Factory ────────────────────────────────────────────────────────────────

    /// Create and return a new consumer.
    /// Returns nullptr only on a fatal Kafka configuration error (very rare).
    static std::unique_ptr<IKv8Consumer> Create(const Kv8Config &cfg);
};

} // namespace kv8
