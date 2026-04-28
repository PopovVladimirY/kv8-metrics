////////////////////////////////////////////////////////////////////////////////
// Kv8Consumer.cpp -- IKv8Consumer implementation
//
// All librdkafka usage is confined to this translation unit.
// No rdkafka types appear in any public header.
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <librdkafka/rdkafka.h>

#include <kv8/IKv8Consumer.h>
#include "UdtSchemaParser.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <unordered_set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kv8 {

////////////////////////////////////////////////////////////////////////////////
// Internal helpers
////////////////////////////////////////////////////////////////////////////////

// Safely drain and destroy a Kafka consumer.
//
// rd_kafka_destroy() can block indefinitely on Windows when the broker
// thread is stuck in recv() inside thrd_join(INFINITE).  We work around
// this by:
//   1. Unassigning all partitions and draining pending messages.
//   2. Running consumer_close() + destroy() in a short-lived detached thread.
//   3. Waiting up to 3 seconds.  On timeout we return without crashing
//      (the OS will reclaim the thread when the process exits).
static void SafeConsumerDestroy(rd_kafka_t *rk)
{
    if (!rk) return;

    // Unassign partitions to stop new fetches immediately.
    rd_kafka_topic_partition_list_t *cur = nullptr;
    if (rd_kafka_assignment(rk, &cur) == RD_KAFKA_RESP_ERR_NO_ERROR && cur)
    {
        rd_kafka_error_t *e = rd_kafka_incremental_unassign(rk, cur);
        if (e)
        {
            fprintf(stderr, "[KV8] incremental_unassign (destroy): %s\n", rd_kafka_error_string(e));
            rd_kafka_error_destroy(e);
        }
        rd_kafka_topic_partition_list_destroy(cur);
    }
    else
    {
        // Fallback for older brokers/protocols.
        rd_kafka_assign(rk, nullptr);
    }

    // Drain messages already in the local queue.
    rd_kafka_message_t *pMsg;
    while ((pMsg = rd_kafka_consumer_poll(rk, 100)) != nullptr)
        rd_kafka_message_destroy(pMsg);

    auto pDone = std::make_shared<std::atomic<bool>>(false);
    std::thread([rk, pDone]() {
        rd_kafka_consumer_close(rk);
        rd_kafka_destroy(rk);
        pDone->store(true);
    }).detach();

    for (int i = 0; i < 60; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (pDone->load()) return;
    }
    fprintf(stderr, "[KV8] WARN: rd_kafka_destroy() did not complete in 3s -- skipping.\n");
}

// Apply standard SASL/security settings to a Kafka conf object.
// Returns false (and prints to stderr) if any setting is rejected.
static bool ApplySecurityConf(rd_kafka_conf_t    *pConf,
                              const Kv8Config    &cfg,
                              char               *errstr,
                              size_t              errlen)
{
    auto set = [&](const char *k, const char *v) -> bool {
        if (rd_kafka_conf_set(pConf, k, v, errstr, errlen) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "[KV8] conf_set(%s): %s\n", k, errstr);
            return false;
        }
        return true;
    };

    if (!set("bootstrap.servers",  cfg.sBrokers.c_str()))       return false;
    if (!set("security.protocol",  cfg.sSecurityProto.c_str())) return false;

    if (cfg.sSecurityProto.find("sasl") != std::string::npos)
    {
        if (!set("sasl.mechanism", cfg.sSaslMechanism.c_str())) return false;
        if (!set("sasl.username",  cfg.sUser.c_str()))          return false;
        if (!set("sasl.password",  cfg.sPass.c_str()))          return false;
    }
    return true;
}

// Generate a unique consumer group ID with a given prefix.
// CR-04: Use a random 64-bit suffix instead of a millisecond wall-clock
// timestamp.  Two consumers started within the same millisecond (common in
// automated tests) previously generated the same group ID, causing them to
// join the same consumer group and split topic partitions silently.
static std::string MakeGroupID(const std::string &prefix,
                               const std::string &override = "")
{
    if (!override.empty()) return override;
    std::random_device rd;
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t rnd = dist(rd);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s-%016llx", prefix.c_str(),
             (unsigned long long)rnd);
    return std::string(buf);
}

////////////////////////////////////////////////////////////////////////////////
// Registry record parsing (internal -- not exposed in headers)
////////////////////////////////////////////////////////////////////////////////

static void ParseRegistryRecord(const void                         *pPayload,
                                size_t                              cbPayload,
                                const std::string                  &sChannel,
                                std::map<std::string, SessionMeta> &sessions,
                                std::set<std::string>              &deletedPrefixes,
                                std::map<uint32_t, std::string>    &schemaCache)
{
    if (cbPayload < sizeof(KafkaRegistryRecord)) return;

    const KafkaRegistryRecord *r =
        reinterpret_cast<const KafkaRegistryRecord*>(pPayload);

    if (r->wVersion != KV8_REGISTRY_VERSION)
    {
        fprintf(stderr, "[KV8] Skipping registry record with unknown version %u (expected %u)\n",
                (unsigned)r->wVersion, (unsigned)KV8_REGISTRY_VERSION);
        return;
    }
    if (cbPayload < sizeof(KafkaRegistryRecord) + r->wNameLen + r->wTopicLen)
        return;

    const char *pVar = reinterpret_cast<const char*>(r + 1);
    std::string sName(pVar, r->wNameLen);
    std::string sTopic(pVar + r->wNameLen, r->wTopicLen);

    // Tombstone: the session prefix is stored in sName; the topic is empty.
    if (r->wCounterID == KV8_CID_DELETED)
    {
        if (!sName.empty())
            deletedPrefixes.insert(sName);
        return;
    }

    // Schema record: sTopic carries the full JSON schema text (not a Kafka topic).
    // Store it in schemaCache keyed by dwHash = FNV-32(schema_json).
    // Schema records are written before their associated UDT feed records so the
    // JSON is always present in schemaCache when the feed counter record arrives.
    if (r->wCounterID == KV8_CID_SCHEMA)
    {
        schemaCache[r->dwHash] = sTopic;
        return;
    }

    if (sTopic.empty()) return;

    // ── Derive session prefix from the topic name ──────────────────────────
    // Log topic  : <channel>.<sessionID>._log         -> prefix before "._log"
    // Data topic : <channel>.<sessionID>.d.XXXXXXXX   -> prefix before ".d."
    // UDT topic  : <channel>.<sessionID>.u.XXXXXXXX.Y -> prefix before ".u."
    std::string sPrefix;
    if (r->wCounterID == KV8_CID_LOG)
    {
        size_t pos = sTopic.rfind("._log");
        if (pos == std::string::npos) return;
        sPrefix = sTopic.substr(0, pos);
    }
    else
    {
        size_t pos = sTopic.rfind(".d.");
        if (pos == std::string::npos) pos = sTopic.rfind(".u.");
        if (pos == std::string::npos) return;
        sPrefix = sTopic.substr(0, pos);
    }

    // Prefix must start with "<sChannel>."
    if (sPrefix.size() <= sChannel.size() + 1 ||
        sPrefix.substr(0, sChannel.size() + 1) != (sChannel + "."))
        return;

    // Ensure session entry exists.
    if (sessions.find(sPrefix) == sessions.end())
    {
        SessionMeta sm;
        sm.sSessionPrefix = sPrefix;
        sm.sSessionID     = sPrefix.substr(sChannel.size() + 1);
        sm.sLogTopic      = sPrefix + "._log";
        sm.sControlTopic  = sPrefix + "._ctl";
        sessions[sPrefix] = std::move(sm);
    }
    SessionMeta &sm = sessions[sPrefix];

    if (r->wCounterID == KV8_CID_LOG)
    {
        sm.sName = sName;
        return;
    }

    if (r->wCounterID == KV8_CID_GROUP)
    {
        sm.hashToGroup[r->dwHash]        = sName;
        sm.topicToGroupName[sTopic]      = sName;
        sm.topicToGroupHash[sTopic]      = r->dwHash;
        sm.dataTopics.insert(sTopic);
        if (r->qwTimerFrequency > 0) sm.topicToFrequency[sTopic]   = r->qwTimerFrequency;
        if (r->qwTimerValue     > 0) sm.topicToTimerValue[sTopic]  = r->qwTimerValue;
        if (r->dwTimeHi || r->dwTimeLo)
        {
            sm.topicToTimeHi[sTopic] = r->dwTimeHi;
            sm.topicToTimeLo[sTopic] = r->dwTimeLo;
        }
        return;
    }

    // Regular counter record (scalar or UDT feed).
    CounterMeta cm;
    cm.sName       = sName;
    cm.wCounterID  = r->wCounterID;
    cm.wFlags      = r->wFlags;
    cm.dbMin       = r->dbMin;
    cm.dbAlarmMin  = r->dbAlarmMin;
    cm.dbMax       = r->dbMax;
    cm.dbAlarmMax  = r->dbAlarmMax;
    cm.sDataTopic  = sTopic;

    // Populate UDT-specific fields when KV8_FLAG_UDT is set.
    if (r->wFlags & KV8_FLAG_UDT)
    {
        cm.bIsUdtFeed   = true;
        cm.dwSchemaHash = r->dwHash;
        auto it = schemaCache.find(r->dwHash);
        if (it != schemaCache.end())
            cm.sSchemaJson = it->second;
    }

    sm.hashToCounters[r->dwHash].push_back(cm);
    sm.dataTopics.insert(sTopic);
    // Per-counter layout: each counter owns its own topic; record the mapping.
    sm.topicToCounter[sTopic] = cm;
}

////////////////////////////////////////////////////////////////////////////////
// Kv8ConsumerImpl
////////////////////////////////////////////////////////////////////////////////

class Kv8ConsumerImpl final : public IKv8Consumer
{
public:
    explicit Kv8ConsumerImpl(const Kv8Config &cfg);
    ~Kv8ConsumerImpl() override;

    std::vector<std::string> ListChannels(int timeoutMs = 5000) override;

    std::map<std::string, SessionMeta>
        DiscoverSessions(const std::string &sChannel)   override;

    void Subscribe (const std::string &sTopic)          override;

    void Poll(int timeoutMs,
              const std::function<void(std::string_view,
                                       const void *, size_t, int64_t)> &) override;

    int PollBatch(int maxMessages, int timeoutMs,
                  const std::function<void(std::string_view,
                                           const void *, size_t, int64_t)> &) override;

    void Stop() override { m_bStop.store(true); }

    void ConsumeTopicFromBeginning(
        const std::string &sTopic,
        int                hardTimeoutMs,
        const std::function<void(const void *, size_t, int64_t)> &onMessage) override;

    bool ReadLatestRecord(
        const std::string &sTopic,
        int hardTimeoutMs,
        const std::function<void(const void *, size_t, int64_t)> &onMessage) override;

    int64_t GetTopicLatestTimestampMs(
        const std::string &sTopic,
        int timeoutMs = 2000) override;

    void AssignAtTimestampMs(const std::vector<std::string> &topics,
                             int64_t tsMs,
                             int     timeoutMs = 3000) override;

    std::map<std::string, int64_t>
        GetTopicMessageCounts(const std::vector<std::string> &topics,
                              int timeoutMs = 5000) override;

    void DeleteSessionTopics(const SessionMeta &sm) override;

    bool CreateTopic(const std::string &sTopic,
                     int                numPartitions,
                     int                replicationFactor = 1,
                     int                timeoutMs         = 8000) override;

    void MarkSessionDeleted(const std::string &sChannel,
                            const SessionMeta &sm) override;

    void DeleteChannel(const std::string &sChannel) override;

private:
    // ── Create the main streaming consumer (subscribe-mode) ──────────────────
    bool InitStreaming();

    // ── Rebalance callback (static -- receives 'this' via opaque) ────────────
    static void RebalanceCb(rd_kafka_t                      *rk,
                            rd_kafka_resp_err_t               err,
                            rd_kafka_topic_partition_list_t  *parts,
                            void                             *opaque);

    Kv8Config             m_cfg;
    rd_kafka_t           *m_rk             = nullptr; // streaming consumer handle
    rd_kafka_queue_t     *m_pConsumerQueue = nullptr; // batch-dequeue queue (rd_kafka_queue_get_consumer)
    std::vector<rd_kafka_message_t *> m_msgBuf;        // pre-allocated pointer array for PollBatch

    std::atomic<bool>     m_bStop{false};

    // Topics currently subscribed (protected by m_mtxSubs)
    std::mutex            m_mtxSubs;
    std::vector<std::string> m_vSubscribed;

    // (topic, partition) pairs seen by the rebalance callback.
    // std::unordered_set<uint64_t> with FNV-1a key: O(1) average, zero allocation
    // per lookup after initial assignment (set itself is heap-allocated once).
    std::mutex                   m_mtxStarted;
    std::unordered_set<uint64_t> m_setStarted;
};

// ── Constructor / destructor ─────────────────────────────────────────────────

Kv8ConsumerImpl::Kv8ConsumerImpl(const Kv8Config &cfg) : m_cfg(cfg) {}

Kv8ConsumerImpl::~Kv8ConsumerImpl()
{
    if (m_pConsumerQueue)
    {
        rd_kafka_queue_destroy(m_pConsumerQueue);
        m_pConsumerQueue = nullptr;
    }
    if (m_rk)
    {
        SafeConsumerDestroy(m_rk);
        m_rk = nullptr;
    }
}

// ── Lazy init of streaming consumer ─────────────────────────────────────────

bool Kv8ConsumerImpl::InitStreaming()
{
    if (m_rk) return true; // already initialised

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();

    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return false;
    }

    auto set = [&](const char *k, const char *v) -> bool {
        if (rd_kafka_conf_set(pConf, k, v, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "[KV8] conf_set(%s): %s\n", k, errstr);
            return false;
        }
        return true;
    };

    std::string gid = MakeGroupID("kv8consumer", m_cfg.sGroupID);
    if (!set("group.id",                  gid.c_str()))  { rd_kafka_conf_destroy(pConf); return false; }
    if (!set("auto.offset.reset",         "earliest"))   { rd_kafka_conf_destroy(pConf); return false; }
    if (!set("enable.auto.commit",        "false"))      { rd_kafka_conf_destroy(pConf); return false; }
    if (!set("partition.assignment.strategy", "cooperative-sticky")) { rd_kafka_conf_destroy(pConf); return false; }
    // Reduce session/heartbeat timeouts so the coordinator assigns partitions
    // faster on the initial JoinGroup (default session.timeout=45s is excessive
    // for a local benchmark and makes the coordinator wait longer than needed).
    if (!set("session.timeout.ms",        "6000"))       { rd_kafka_conf_destroy(pConf); return false; }
    if (!set("heartbeat.interval.ms",     "2000"))       { rd_kafka_conf_destroy(pConf); return false; }
    // Keep fetch latency low so the broker ships partial responses quickly.
    // Default fetch.wait.max.ms=500 would make the consumer appear dead for
    // the first few seconds of a benchmark against a fast local producer.
    if (!set("fetch.wait.max.ms",         "5"))          { rd_kafka_conf_destroy(pConf); return false; }
    // Disable Nagle's algorithm (TCP_NODELAY) on the broker socket. On Linux
    // with a local Docker bridge this is a no-op, but on Windows the consumer
    // reaches Kafka via the WSL2/Hyper-V virtual NIC which is a real TCP stack.
    // Without TCP_NODELAY, Nagle holds the FetchRequest until a delayed-ACK
    // arrives (~200-500 ms on Windows), making each fetch round-trip ~1 second.
    if (!set("socket.nagle.disable",      "true"))       { rd_kafka_conf_destroy(pConf); return false; }
    // Receive as many messages per fetch response as the broker will send.
    if (!set("max.partition.fetch.bytes", "10485760"))   { rd_kafka_conf_destroy(pConf); return false; } // 10 MB
    // queued.min.messages is the target local queue depth.  100000 means
    // rdkafka tries to keep ~1 second of data (at 100 kHz) pre-fetched, then
    // hits fetch.queue.backoff.ms (default 1000 ms) before fetching more --
    // causing the observed ~1 s live-mode update interval on Windows.
    // 1000 messages keeps the queue shallow enough that the backoff is rarely
    // triggered while still amortising per-fetch overhead.
    if (!set("queued.min.messages",       "1000"))       { rd_kafka_conf_destroy(pConf); return false; }
    // fetch.queue.backoff.ms is the penalty applied when the internal queue
    // exceeds its threshold.  The default 1000 ms is the direct cause of the
    // 1-second live-mode graph stall.  Set to 0 so any overshoot is recovered
    // on the very next poll cycle instead of waiting a full second.
    if (!set("fetch.queue.backoff.ms",    "0"))          { rd_kafka_conf_destroy(pConf); return false; }
    // fetch.error.backoff.ms: default 500 ms -- applied on partition errors
    // (e.g. UNKNOWN_TOPIC for the ._ctl topic before it is created).  Reduce
    // so a transient topic-not-found on one partition does not stall others.
    if (!set("fetch.error.backoff.ms",    "100"))        { rd_kafka_conf_destroy(pConf); return false; }

    rd_kafka_conf_set_rebalance_cb(pConf, Kv8ConsumerImpl::RebalanceCb);
    rd_kafka_conf_set_opaque(pConf, this);

    m_rk = rd_kafka_new(RD_KAFKA_CONSUMER, pConf, errstr, sizeof(errstr));
    if (!m_rk) {
        fprintf(stderr, "[KV8] rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
        return false;
    }
    // pConf is now owned by m_rk
    rd_kafka_poll_set_consumer(m_rk);

    // Get the high-level consumer's internal queue.
    // rd_kafka_consume_batch_queue() drains up to N messages in a single
    // mutex lock/unlock -- far cheaper than N calls to rd_kafka_consumer_poll.
    m_pConsumerQueue = rd_kafka_queue_get_consumer(m_rk);
    if (!m_pConsumerQueue)
    {
        fprintf(stderr, "[KV8] rd_kafka_queue_get_consumer returned NULL\n");
        SafeConsumerDestroy(m_rk);
        m_rk = nullptr;
        return false;
    }
    // Pre-allocate the pointer array used by PollBatch -- avoids heap
    // allocation in the hot path.  65536 covers any realistic batch size.
    m_msgBuf.resize(65536);
    return true;
}

// ── Rebalance callback ───────────────────────────────────────────────────────

void Kv8ConsumerImpl::RebalanceCb(rd_kafka_t                      *rk,
                                   rd_kafka_resp_err_t               err,
                                   rd_kafka_topic_partition_list_t  *parts,
                                   void                             *opaque)
{
    Kv8ConsumerImpl *self = static_cast<Kv8ConsumerImpl*>(opaque);
    if (!self) return;

    if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS)
    {
        // For each assigned partition: seek to OFFSET_BEGINNING the first time
        // this (topic, partition) pair is seen.  Subsequent rebalances (caused
        // by adding new topics) leave already-consumption partitions untouched.
        std::lock_guard<std::mutex> lk(self->m_mtxStarted);
        for (int i = 0; i < parts->cnt; i++)
        {
            // FNV-1a hash of topic name XOR partition index — zero heap allocation.
            const char *pTopic = parts->elems[i].topic ? parts->elems[i].topic : "";
            uint64_t h = 14695981039346656037ULL;
            for (const char *p = pTopic; *p; ++p)
            {
                h ^= (uint8_t)*p;
                h *= 1099511628211ULL;
            }
            uint64_t key = h ^ (uint64_t(uint32_t(parts->elems[i].partition)) * 2654435761ULL);
            if (self->m_setStarted.find(key) == self->m_setStarted.end())
            {
                parts->elems[i].offset = RD_KAFKA_OFFSET_BEGINNING;
                self->m_setStarted.insert(key);
            }
        }
        rd_kafka_error_t *e = rd_kafka_incremental_assign(rk, parts);
        if (e)
        {
            fprintf(stderr, "[KV8] incremental_assign: %s\n", rd_kafka_error_string(e));
            rd_kafka_error_destroy(e);
        }
    }
    else if (err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS)
    {
        rd_kafka_error_t *e = rd_kafka_incremental_unassign(rk, parts);
        if (e)
        {
            fprintf(stderr, "[KV8] incremental_unassign: %s\n", rd_kafka_error_string(e));
            rd_kafka_error_destroy(e);
        }
    }
    else
    {
        // For cooperative protocol, clear assignments via incremental unassign.
        rd_kafka_error_t *e = rd_kafka_incremental_unassign(rk, parts);
        if (e)
        {
            fprintf(stderr, "[KV8] incremental_unassign (fallback): %s\n", rd_kafka_error_string(e));
            rd_kafka_error_destroy(e);
        }
    }
}

// ── Subscribe ────────────────────────────────────────────────────────────────

void Kv8ConsumerImpl::Subscribe(const std::string &sTopic)
{
    if (sTopic.empty()) return;
    if (!InitStreaming()) return;

    // CR-06: Single lock scope covers both the dedup check and the
    // rd_kafka_subscribe call, eliminating the TOCTOU window that existed
    // when two separate lock acquisitions were used.  rd_kafka_subscribe()
    // is documented as non-blocking in librdkafka; it queues the subscription
    // change to the background thread and returns immediately.
    std::lock_guard<std::mutex> lk(m_mtxSubs);
    for (auto &s : m_vSubscribed)
        if (s == sTopic) return; // already subscribed
    m_vSubscribed.push_back(sTopic);

    rd_kafka_topic_partition_list_t *pSubs =
        rd_kafka_topic_partition_list_new(static_cast<int>(m_vSubscribed.size()));
    for (auto &s : m_vSubscribed)
        rd_kafka_topic_partition_list_add(pSubs, s.c_str(), RD_KAFKA_PARTITION_UA);

    rd_kafka_resp_err_t e = rd_kafka_subscribe(m_rk, pSubs);
    rd_kafka_topic_partition_list_destroy(pSubs);
    if (e != RD_KAFKA_RESP_ERR_NO_ERROR)
        fprintf(stderr, "[KV8] resubscribe: %s\n", rd_kafka_err2str(e));
}

// ── Internal: dispatch one librdkafka message through the callback ───────────
// Returns true if a data message was dispatched, false on timeout / EOF / err.
static bool DispatchOne(
    rd_kafka_message_t *pMsg,
    const std::function<void(std::string_view, const void *, size_t, int64_t)> &onMessage)
{
    if (!pMsg) return false;

    if (pMsg->err)
    {
        if (pMsg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF &&
            pMsg->err != RD_KAFKA_RESP_ERR__TIMED_OUT)
        {
            fprintf(stderr, "[KV8] consumer error: %s: %s\n",
                    rd_kafka_err2str(pMsg->err),
                    rd_kafka_message_errstr(pMsg));
        }
        rd_kafka_message_destroy(pMsg);
        return false;
    }

    rd_kafka_timestamp_type_t tsType;
    int64_t tsMs = rd_kafka_message_timestamp(pMsg, &tsType);
    if (tsMs < 0) tsMs = 0;

    // Pass topic name as string_view -- zero-copy, valid until rd_kafka_message_destroy.
    onMessage(std::string_view(rd_kafka_topic_name(pMsg->rkt)),
              pMsg->payload, pMsg->len, tsMs);

    rd_kafka_message_destroy(pMsg);
    return true;
}

// ── Poll ─────────────────────────────────────────────────────────────────────

void Kv8ConsumerImpl::Poll(
    int timeoutMs,
    const std::function<void(std::string_view, const void *, size_t, int64_t)> &onMessage)
{
    if (m_bStop.load() || !m_rk) return;
    DispatchOne(rd_kafka_consumer_poll(m_rk, timeoutMs), onMessage);
}

// ── PollBatch ─────────────────────────────────────────────────────────────────

int Kv8ConsumerImpl::PollBatch(
    int maxMessages,
    int timeoutMs,
    const std::function<void(std::string_view, const void *, size_t, int64_t)> &onMessage)
{
    if (m_bStop.load() || !m_rk || !m_pConsumerQueue || maxMessages <= 0) return 0;

    // rd_kafka_consume_batch_queue does NOT pump librdkafka's internal event
    // loop (fetch-pipeline timers, auto-commit, rebalance acknowledgement).
    // rd_kafka_consumer_poll() does.  Without this the background machinery
    // stalls periodically, causing the 0-recv seconds seen in throughput traces.
    // A zero-timeout poll processes only already-queued events -- never blocks.
    //
    // IMPORTANT: rd_kafka_consumer_poll can return a data message in addition
    // to processing events.  We must dispatch it through onMessage; discarding
    // the return value loses one message per PollBatch call.
    int nProcessed = 0;
    // CR-16: Renamed from pEvtMsg -- rd_kafka_consumer_poll can return a
    // regular data message, not only internal event control messages.
    rd_kafka_message_t *pPollMsg = rd_kafka_consumer_poll(m_rk, 0);
    if (pPollMsg)
    {
        if (DispatchOne(pPollMsg, onMessage)) ++nProcessed;
    }

    // rd_kafka_consume_batch_queue blocks up to timeoutMs waiting for the
    // first message, then returns up to rkmessages_size messages that are
    // already in the local queue -- all in a SINGLE mutex lock/unlock cycle.
    // The previous per-message rd_kafka_consumer_poll loop paid that mutex
    // cost once per message, which serialised throughput at high rates.
    const size_t batchSz = ((size_t)maxMessages < m_msgBuf.size())
                            ? (size_t)maxMessages
                            : m_msgBuf.size();
    const ssize_t n = rd_kafka_consume_batch_queue(m_pConsumerQueue,
                                                   timeoutMs,
                                                   m_msgBuf.data(),
                                                   batchSz);
    if (n <= 0) return nProcessed;

    for (ssize_t i = 0; i < n; ++i)
        if (DispatchOne(m_msgBuf[i], onMessage)) ++nProcessed;
    return nProcessed;
}

// ── ConsumeTopicFromBeginning ─────────────────────────────────────────────────

// CR-12: Query the actual partition count for a topic so we can assign all
// partitions rather than hard-coding partition 0.  Falls back to 1 on error.
static int32_t QueryPartitionCount(rd_kafka_t *rk,
                                    const std::string &sTopic,
                                    int timeoutMs)
{
    rd_kafka_topic_t *rkt = rd_kafka_topic_new(rk, sTopic.c_str(), nullptr);
    if (!rkt) return 1;

    const struct rd_kafka_metadata *meta = nullptr;
    rd_kafka_resp_err_t e = rd_kafka_metadata(rk, 0, rkt, &meta, timeoutMs);
    rd_kafka_topic_destroy(rkt);

    if (e != RD_KAFKA_RESP_ERR_NO_ERROR || !meta) return 1;

    int32_t cnt = (meta->topic_cnt > 0) ? meta->topics[0].partition_cnt : 1;
    rd_kafka_metadata_destroy(meta);
    return (cnt > 0) ? cnt : 1;
}

void Kv8ConsumerImpl::ConsumeTopicFromBeginning(
    const std::string &sTopic,
    int                hardTimeoutMs,
    const std::function<void(const void *, size_t, int64_t)> &onMessage)
{
    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();

    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return;
    }

    auto set = [&](const char *k, const char *v) -> bool {
        if (rd_kafka_conf_set(pConf, k, v, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "[KV8] ctb conf_set(%s): %s\n", k, errstr);
            return false;
        }
        return true;
    };

    std::string gid = MakeGroupID("kv8-ctb");
    if (!set("group.id",             gid.c_str()))    { rd_kafka_conf_destroy(pConf); return; }
    if (!set("enable.auto.commit",   "false"))         { rd_kafka_conf_destroy(pConf); return; }
    if (!set("enable.partition.eof", "true"))          { rd_kafka_conf_destroy(pConf); return; }
    // Short socket timeout so rd_kafka_destroy() completes promptly.
    if (!set("socket.timeout.ms",    "1500"))          { rd_kafka_conf_destroy(pConf); return; }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, pConf, errstr, sizeof(errstr));
    if (!rk) { fprintf(stderr, "[KV8] ctb rd_kafka_new: %s\n", errstr);
               rd_kafka_conf_destroy(pConf); return; }
    rd_kafka_poll_set_consumer(rk);

    // CR-12: Query actual partition count to avoid silently missing data on
    // partitions 1+ when the broker default num.partitions > 1.
    int32_t nParts = QueryPartitionCount(rk, sTopic, 3000);

    rd_kafka_topic_partition_list_t *pAssign =
        rd_kafka_topic_partition_list_new(nParts);
    for (int32_t p = 0; p < nParts; ++p)
        rd_kafka_topic_partition_list_add(pAssign, sTopic.c_str(), p)->offset =
            RD_KAFKA_OFFSET_BEGINNING;

    rd_kafka_resp_err_t eAssign = rd_kafka_assign(rk, pAssign);
    rd_kafka_topic_partition_list_destroy(pAssign);
    if (eAssign != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        fprintf(stderr, "[KV8] ctb assign: %s\n", rd_kafka_err2str(eAssign));
        SafeConsumerDestroy(rk);
        return;
    }

    // Track how many partitions have signalled EOF so we stop once all are done.
    int32_t nEof = 0;
    const auto tDeadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(hardTimeoutMs);

    while (nEof < nParts && std::chrono::steady_clock::now() < tDeadline)
    {
        rd_kafka_message_t *pMsg = rd_kafka_consumer_poll(rk, 200);
        if (!pMsg) continue;

        if (pMsg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            ++nEof;
            rd_kafka_message_destroy(pMsg);
            if (nEof >= nParts) break; // all partitions exhausted
            continue;
        }
        if (pMsg->err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            if (pMsg->err != RD_KAFKA_RESP_ERR__TIMED_OUT)
                fprintf(stderr, "[KV8] ctb error: %s\n", rd_kafka_err2str(pMsg->err));
            rd_kafka_message_destroy(pMsg);
            continue;
        }

        rd_kafka_timestamp_type_t tsType;
        int64_t tsMs = rd_kafka_message_timestamp(pMsg, &tsType);
        if (tsMs < 0) tsMs = 0;

        onMessage(pMsg->payload, pMsg->len, tsMs);
        rd_kafka_message_destroy(pMsg);
    }

    SafeConsumerDestroy(rk);
}

// ── ListChannels ─────────────────────────────────────────────────────────────

std::vector<std::string>
Kv8ConsumerImpl::ListChannels(int timeoutMs)
{
    std::vector<std::string> result;

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return result;
    }
    // CR-08: Cap socket.timeout.ms to 10 000 ms.  The previous formula
    // (timeoutMs * 2 + 3000) produced up to 123 s for a 60 s input,
    // causing rd_kafka_destroy() to block for that long on an unreachable
    // broker.  10 s is enough for any real network and keeps handle
    // teardown bounded.
    auto set = [&](const char *k, const char *v) {
        rd_kafka_conf_set(pConf, k, v, nullptr, 0);
    };
    {
        int sockTimeout = timeoutMs + 3000;
        if (sockTimeout > 10000) sockTimeout = 10000;
        char tsBuf[32];
        snprintf(tsBuf, sizeof(tsBuf), "%d", sockTimeout);
        set("socket.timeout.ms", tsBuf);
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rk)
    {
        fprintf(stderr, "[KV8] ListChannels rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
        return result;
    }

    // Allow the SASL handshake to complete before requesting metadata.
    // Without this, rd_kafka_metadata() absorbs the entire timeout on just
    // the TCP+SASL connection, leaving no time for the actual request.
    {
        auto tWarm = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < tWarm)
            rd_kafka_poll(rk, 100);
    }

    // Retry metadata once -- the first attempt may still time out on a slow
    // broker or newly-created handle.
    const struct rd_kafka_metadata *meta = nullptr;
    rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        err = rd_kafka_metadata(rk, 1 /*all_topics*/, nullptr, &meta, timeoutMs);
        if (err == RD_KAFKA_RESP_ERR_NO_ERROR) break;
        fprintf(stderr, "[KV8] ListChannels metadata (attempt %d): %s\n",
                attempt + 1, rd_kafka_err2str(err));
        if (attempt == 0)
            rd_kafka_poll(rk, 500); // brief pause before retry
    }

    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        rd_kafka_destroy(rk);
        return result;
    }

    // Accept any topic whose name ends with "._registry" and does not start
    // with "__" (Kafka-internal topics such as __consumer_offsets).
    // Removing the old hard-coded kv8. prefix filter means any channel
    // name (myapp.service, prod.telemetry, etc.) is discovered automatically.
    // CR-17: kSuffixLen computed from the literal so it stays in sync if
    // the suffix string is ever changed.
    static const char  *kSuffix    = "._registry";
    static const size_t kSuffixLen = sizeof("._registry") - 1u;

    for (int i = 0; i < meta->topic_cnt; ++i)
    {
        const char *name = meta->topics[i].topic;
        if (!name) continue;
        size_t n = strlen(name);
        // Must be long enough to have at least one channel character before ._registry
        if (n <= kSuffixLen) continue;
        // Skip Kafka-internal topics (__consumer_offsets, __transaction_state, etc.)
        if (name[0] == '_' && name[1] == '_') continue;
        // Check suffix
        if (strcmp(name + n - kSuffixLen, kSuffix) != 0) continue;
        // Strip ._registry to get channel prefix
        result.emplace_back(name, n - kSuffixLen);
    }

    rd_kafka_metadata_destroy(meta);
    rd_kafka_destroy(rk);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// ── DiscoverSessions ─────────────────────────────────────────────────────────

std::map<std::string, SessionMeta>
Kv8ConsumerImpl::DiscoverSessions(const std::string &sChannel)
{
    std::map<std::string, SessionMeta> sessions;
    std::set<std::string> deletedPrefixes;
    std::map<uint32_t, std::string> schemaCache;

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();

    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return sessions;
    }

    auto set = [&](const char *k, const char *v) -> bool {
        if (rd_kafka_conf_set(pConf, k, v, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "[KV8] disc conf_set(%s): %s\n", k, errstr);
            return false;
        }
        return true;
    };

    std::string gid = MakeGroupID("kv8-disc");
    if (!set("group.id",             gid.c_str()))    { rd_kafka_conf_destroy(pConf); return sessions; }
    if (!set("enable.auto.commit",   "false"))         { rd_kafka_conf_destroy(pConf); return sessions; }
    if (!set("enable.partition.eof", "true"))          { rd_kafka_conf_destroy(pConf); return sessions; }
    if (!set("socket.timeout.ms",    "1500"))          { rd_kafka_conf_destroy(pConf); return sessions; }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, pConf, errstr, sizeof(errstr));
    if (!rk)
    {
        fprintf(stderr, "[KV8] disc rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
        return sessions;
    }
    rd_kafka_poll_set_consumer(rk);

    // CR-12: Query actual partition count; fall back to 1 on error.
    std::string sRegTopic = sChannel + "._registry";
    int32_t nParts = QueryPartitionCount(rk, sRegTopic, 3000);

    rd_kafka_topic_partition_list_t *pAssign =
        rd_kafka_topic_partition_list_new(nParts);
    for (int32_t p = 0; p < nParts; ++p)
        rd_kafka_topic_partition_list_add(pAssign, sRegTopic.c_str(), p)->offset =
            RD_KAFKA_OFFSET_BEGINNING;
    rd_kafka_resp_err_t eAssign = rd_kafka_assign(rk, pAssign);
    rd_kafka_topic_partition_list_destroy(pAssign);
    if (eAssign != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        fprintf(stderr, "[KV8] disc assign: %s\n", rd_kafka_err2str(eAssign));
        SafeConsumerDestroy(rk);
        return sessions;
    }

    // CR-18: diagnostic messages go to stderr, not stdout.
    fprintf(stderr, "[KV8] Scanning registry: %s (%d partition(s)) ...\n",
            sRegTopic.c_str(), (int)nParts);

    uint64_t nRead = 0;
    int32_t  nEof  = 0;
    const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (nEof < nParts && std::chrono::steady_clock::now() < tDeadline)
    {
        rd_kafka_message_t *pMsg = rd_kafka_consumer_poll(rk, 200);
        if (!pMsg) continue;

        if (pMsg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            if (++nEof >= nParts)
                fprintf(stderr, "[KV8] Registry EOF after %llu record(s).\n",
                        (unsigned long long)nRead);
        }
        else if (pMsg->err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            fprintf(stderr, "[KV8] disc poll: %s\n", rd_kafka_err2str(pMsg->err));
        }
        else
        {
            ++nRead;
            ParseRegistryRecord(pMsg->payload, pMsg->len, sChannel, sessions, deletedPrefixes, schemaCache);
        }
        rd_kafka_message_destroy(pMsg);
    }

    if (nEof < nParts)
        fprintf(stderr, "[KV8] Registry scan timed out after %llu record(s).\n",
                (unsigned long long)nRead);

    SafeConsumerDestroy(rk);

    // ── Synthesize virtual scalar counters from UDT feed schemas ──────────────
    //
    // For each UDT feed (CounterMeta with bIsUdtFeed==true), parse the JSON schema
    // and append one virtual CounterMeta per leaf field into the same
    // hashToCounters bucket.  The virtual entries carry field offset + type so
    // ConsumerThread can decode them directly from the binary UDT payload.
    //
    // Also inject hashToGroup entries for schema hashes and copy timing metadata
    // so ConsumerThread::InitTimeConverters can build a TimeConverter for each
    // UDT data topic.
    if (!schemaCache.empty())
    {
        // Fixed-point parse: schemas can reference each other in any order.
        // Iterate until all schemas resolve or no further progress is made.
        std::map<std::string, ResolvedSchema> knownSchemas;      // name -> resolved
        std::map<uint32_t,    ResolvedSchema> hashToResolved;    // hash -> resolved
        {
            struct Pending { uint32_t hash; std::string json; };
            std::vector<Pending> remaining;
            for (const auto& kv : schemaCache)
                remaining.push_back({kv.first, kv.second});

            bool progress = true;
            while (progress && !remaining.empty())
            {
                progress = false;
                for (auto it = remaining.begin(); it != remaining.end(); )
                {
                    ResolvedSchema rs;
                    std::string err;
                    if (ParseUdtSchema(it->json.c_str(), knownSchemas, rs, err))
                    {
                        rs.dwHash = it->hash;
                        knownSchemas[rs.schema_name] = rs;
                        hashToResolved[it->hash]     = std::move(rs);
                        it = remaining.erase(it);
                        progress = true;
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            if (!remaining.empty())
            {
                fprintf(stderr, "[KV8] Warning: %zu UDT schema(s) could not be resolved "
                        "(missing dependency?).\n", remaining.size());
            }
        }

        for (auto& sm2 : sessions)
        {
            SessionMeta& sm = sm2.second;

            // Gather one timing anchor from the first available group topic.
            uint64_t fallbackFreq  = 0, fallbackTimer = 0;
            uint32_t fallbackHi    = 0, fallbackLo    = 0;
            for (const auto& kv : sm.topicToFrequency)
            {
                fallbackFreq = kv.second;
                const auto itT = sm.topicToTimerValue.find(kv.first);
                const auto itH = sm.topicToTimeHi.find(kv.first);
                const auto itL = sm.topicToTimeLo.find(kv.first);
                if (itT != sm.topicToTimerValue.end()) fallbackTimer = itT->second;
                if (itH != sm.topicToTimeHi.end())     fallbackHi    = itH->second;
                if (itL != sm.topicToTimeLo.end())     fallbackLo    = itL->second;
                break;
            }

            // Collect UDT feeds for this session before iterating
            // (we will append to the same container).
            struct UdtFeedInfo
            {
                uint32_t schemaHash;
                CounterMeta cm;
            };
            std::vector<UdtFeedInfo> udtFeeds;
            for (const auto& kv : sm.hashToCounters)
                for (const auto& cm : kv.second)
                    if (cm.bIsUdtFeed && !cm.sSchemaJson.empty())
                        udtFeeds.push_back({kv.first, cm});

            for (std::size_t fi = 0; fi < udtFeeds.size(); ++fi)
            {
                const uint32_t   schemaHash = udtFeeds[fi].schemaHash;
                const CounterMeta& feedCm  = udtFeeds[fi].cm;

                // Find the resolved schema for this feed.
                auto sit = hashToResolved.find(schemaHash);
                if (sit == hashToResolved.end())
                    continue;
                const ResolvedSchema& rs = sit->second;
                if (rs.fields.empty())
                    continue;

                // Add a hashToGroup entry so CounterTree can find this group.
                if (sm.hashToGroup.find(schemaHash) == sm.hashToGroup.end())
                    sm.hashToGroup[schemaHash] = feedCm.sName;

                // Copy timing metadata for the UDT data topic (same session clock).
                if (fallbackFreq > 0 &&
                    sm.topicToFrequency.find(feedCm.sDataTopic) == sm.topicToFrequency.end())
                {
                    sm.topicToFrequency[feedCm.sDataTopic]   = fallbackFreq;
                    sm.topicToTimerValue[feedCm.sDataTopic]  = fallbackTimer;
                    sm.topicToTimeHi[feedCm.sDataTopic]      = fallbackHi;
                    sm.topicToTimeLo[feedCm.sDataTopic]      = fallbackLo;
                }

                // Synthesize one virtual CounterMeta per flattened schema field.
                // wCounterID = 0x1000 + (feed_instance * 64) + field_index
                // (feed_instance is bounded by 64; field_index is bounded by 63).
                const uint16_t idBase =
                    static_cast<uint16_t>(0x1000u +
                        ((static_cast<unsigned>(fi) & 0x3Fu) << 6u));

                auto& bucket = sm.hashToCounters[schemaHash];
                for (std::size_t fIdx = 0; fIdx < rs.fields.size(); ++fIdx)
                {
                    const UdtSchemaField& f = rs.fields[fIdx];

                    CounterMeta virt;
                    virt.sName              = feedCm.sName + "/" + f.name;
                    virt.sDisplayName       = f.display_name;
                    virt.wCounterID         = static_cast<uint16_t>(idBase + (fIdx & 0x3Fu));
                    virt.wFlags             = 1u;  // enabled
                    virt.dbMin              = f.min_val;
                    virt.dbAlarmMin         = f.min_val;
                    virt.dbMax              = f.max_val;
                    virt.dbAlarmMax         = f.max_val;
                    virt.sDataTopic         = feedCm.sDataTopic;
                    virt.bIsUdtVirtualField = true;
                    virt.wUdtFieldOffset    = f.offset;
                    virt.nUdtFieldType      = static_cast<uint8_t>(f.type);
                    bucket.push_back(std::move(virt));
                }
            }
        }
    }

    // Filter out tombstoned sessions.
    for (const auto &prefix : deletedPrefixes)
        sessions.erase(prefix);

    return sessions;
}

// ── ReadLatestRecord ──────────────────────────────────────────────────────────

bool Kv8ConsumerImpl::ReadLatestRecord(
    const std::string &sTopic,
    int hardTimeoutMs,
    const std::function<void(const void *, size_t, int64_t)> &onMessage)
{
    // Step 1: Query the high-watermark offset using a temporary producer handle.
    // This avoids creating a consumer group just for a metadata query.
    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return false;
    }
    auto setQ = [&](const char *k, const char *v) {
        rd_kafka_conf_set(pConf, k, v, nullptr, 0);
    };
    setQ("socket.timeout.ms", "3000");

    rd_kafka_t *rkQ = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rkQ) { rd_kafka_conf_destroy(pConf); return false; }

    int64_t lo = 0, hi = 0;
    rd_kafka_resp_err_t wErr = rd_kafka_query_watermark_offsets(
        rkQ, sTopic.c_str(), 0, &lo, &hi,
        (std::min)(hardTimeoutMs, 3000));
    rd_kafka_destroy(rkQ);

    if (wErr != RD_KAFKA_RESP_ERR_NO_ERROR || hi <= 0)
        return false; // topic empty or not found

    // Step 2: Open a temporary assign-mode consumer positioned at (hi - 1).
    pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return false;
    }
    auto setC = [&](const char *k, const char *v) -> bool {
        if (rd_kafka_conf_set(pConf, k, v, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "[KV8] rlr conf_set(%s): %s\n", k, errstr);
            return false;
        }
        return true;
    };
    std::string gid = MakeGroupID("kv8-rlr");
    if (!setC("group.id",             gid.c_str()))  { rd_kafka_conf_destroy(pConf); return false; }
    if (!setC("enable.auto.commit",   "false"))       { rd_kafka_conf_destroy(pConf); return false; }
    if (!setC("enable.partition.eof", "true"))        { rd_kafka_conf_destroy(pConf); return false; }
    if (!setC("socket.timeout.ms",    "1500"))        { rd_kafka_conf_destroy(pConf); return false; }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, pConf, errstr, sizeof(errstr));
    if (!rk) { fprintf(stderr, "[KV8] rlr rd_kafka_new: %s\n", errstr);
               rd_kafka_conf_destroy(pConf); return false; }
    rd_kafka_poll_set_consumer(rk);

    rd_kafka_topic_partition_list_t *pAssign = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(pAssign, sTopic.c_str(), 0)->offset = hi - 1;
    rd_kafka_resp_err_t eAssign = rd_kafka_assign(rk, pAssign);
    rd_kafka_topic_partition_list_destroy(pAssign);
    if (eAssign != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        fprintf(stderr, "[KV8] rlr assign: %s\n", rd_kafka_err2str(eAssign));
        SafeConsumerDestroy(rk);
        return false;
    }

    bool bGot = false;
    const auto tDeadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(hardTimeoutMs);

    while (!bGot && std::chrono::steady_clock::now() < tDeadline)
    {
        rd_kafka_message_t *pMsg = rd_kafka_consumer_poll(rk, 200);
        if (!pMsg) continue;

        if (pMsg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            rd_kafka_message_destroy(pMsg);
            break; // topic had no message at offset (hi-1)
        }
        if (pMsg->err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            rd_kafka_message_destroy(pMsg);
            break;
        }

        rd_kafka_timestamp_type_t tsType;
        int64_t tsMs = rd_kafka_message_timestamp(pMsg, &tsType);
        if (tsMs < 0) tsMs = 0;

        onMessage(pMsg->payload, pMsg->len, tsMs);
        rd_kafka_message_destroy(pMsg);
        bGot = true;
    }

    SafeConsumerDestroy(rk);
    return bGot;
}

int64_t Kv8ConsumerImpl::GetTopicLatestTimestampMs(
    const std::string &sTopic,
    int timeoutMs)
{
    int64_t tsResult = -1;
    ReadLatestRecord(sTopic, timeoutMs,
        [&](const void * /*pPayload*/, size_t /*cb*/, int64_t tsKafkaMs)
        {
            tsResult = tsKafkaMs;
        });
    return tsResult;
}

// ── AssignAtTimestampMs ───────────────────────────────────────────────────────

void Kv8ConsumerImpl::AssignAtTimestampMs(
    const std::vector<std::string> &topics,
    int64_t tsMs,
    int timeoutMs)
{
    if (topics.empty()) return;
    if (!InitStreaming()) return;

    // Build a partition list: one entry per topic, offset field = target tsMs.
    // rd_kafka_offsets_for_times() interprets the offset field as a timestamp
    // (ms since epoch) when it is >= 0, and returns the first message offset
    // at or after that timestamp.
    rd_kafka_topic_partition_list_t *tpl =
        rd_kafka_topic_partition_list_new((int)topics.size());
    for (const auto &t : topics)
        rd_kafka_topic_partition_list_add(tpl, t.c_str(), 0)->offset = tsMs;

    rd_kafka_resp_err_t err =
        rd_kafka_offsets_for_times(m_rk, tpl, timeoutMs);

    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        fprintf(stderr, "[KV8] AssignAtTimestampMs offsets_for_times: %s -- "
                        "falling back to OFFSET_BEGINNING\n",
                rd_kafka_err2str(err));
        for (int i = 0; i < tpl->cnt; ++i)
            tpl->elems[i].offset = RD_KAFKA_OFFSET_BEGINNING;
    }
    else
    {
        // offsets_for_times sets offset=-1 when no message exists at or after
        // the requested timestamp.  Treat that as OFFSET_END (live-only).
        for (int i = 0; i < tpl->cnt; ++i)
            if (tpl->elems[i].offset < 0)
                tpl->elems[i].offset = RD_KAFKA_OFFSET_END;

        fprintf(stderr, "[KV8] AssignAtTimestampMs: resolved %d topic(s)\n",
                tpl->cnt);
    }

    // Direct partition assignment -- no group coordination.
    err = rd_kafka_assign(m_rk, tpl);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        fprintf(stderr, "[KV8] AssignAtTimestampMs rd_kafka_assign: %s\n",
                rd_kafka_err2str(err));

    rd_kafka_topic_partition_list_destroy(tpl);

    // Track the assigned topics so that Stop() / Reset() know what was active.
    {
        std::lock_guard<std::mutex> lk(m_mtxSubs);
        for (const auto &t : topics)
        {
            bool found = false;
            for (auto &s : m_vSubscribed)
                if (s == t) { found = true; break; }
            if (!found) m_vSubscribed.push_back(t);
        }
    }
}

// ── GetTopicMessageCounts ─────────────────────────────────────────────────────

std::map<std::string, int64_t>
Kv8ConsumerImpl::GetTopicMessageCounts(const std::vector<std::string> &topics,
                                       int timeoutMs)
{
    std::map<std::string, int64_t> result;
    if (topics.empty()) return result;

    // Use a temporary producer handle for watermark queries.
    // Producers can query broker watermarks without consuming data.
    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        for (auto &t : topics) result[t] = -1LL;
        return result;
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rk)
    {
        fprintf(stderr, "[KV8] GetTopicMessageCounts rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
        for (auto &t : topics) result[t] = -1LL;
        return result;
    }

    for (const auto &sTopic : topics)
    {
        // Kv8 uses single-partition topics.  Query partition 0.
        // For completeness, also probe partition 1 if partition 0 succeeds,
        // but single-partition is the common case.
        int64_t total = 0;
        bool    ok    = true;

        // CR-09: Guard the partition scan with a maximum to prevent an
        // unbounded loop on clusters with many partitions.  All kv8 topics
        // are created with a single partition; 64 is generous.
        static constexpr int32_t kMaxPartitions = 64;
        for (int32_t part = 0; part < kMaxPartitions; ++part)
        {
            int64_t lo = 0, hi = 0;
            rd_kafka_resp_err_t err =
                rd_kafka_query_watermark_offsets(rk, sTopic.c_str(), part, &lo, &hi, timeoutMs);

            if (err == RD_KAFKA_RESP_ERR_NO_ERROR)
            {
                total += (hi > lo) ? (hi - lo) : 0;
                if (part == 0)
                {
                    // Quick check: if partition 1 doesn't exist, stop here.
                    // We test it by trying partition 1 in the next iteration.
                    continue;
                }
                continue; // keep going until unknown partition
            }
            else if (err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
                     err == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART)
            {
                if (part == 0) ok = false; // topic itself not found
                break; // no more partitions
            }
            else
            {
                if (part == 0) ok = false;
                break;
            }
        }

        result[sTopic] = ok ? total : -1LL;
    }

    rd_kafka_destroy(rk);
    return result;
}

// ── CreateTopic ───────────────────────────────────────────────────────────────

bool Kv8ConsumerImpl::CreateTopic(const std::string &sTopic,
                                   int                numPartitions,
                                   int                replicationFactor,
                                   int                timeoutMs)
{
    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    { rd_kafka_conf_destroy(pConf); return false; }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rk)
    { fprintf(stderr, "[KV8] CreateTopic rd_kafka_new: %s\n", errstr);
      rd_kafka_conf_destroy(pConf); return false; }

    rd_kafka_AdminOptions_t *pOpts =
        rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_CREATETOPICS);
    rd_kafka_AdminOptions_set_request_timeout(pOpts, timeoutMs, errstr, sizeof(errstr));

    rd_kafka_NewTopic_t *pNew =
        rd_kafka_NewTopic_new(sTopic.c_str(), numPartitions, replicationFactor,
                              errstr, sizeof(errstr));
    if (!pNew)
    { fprintf(stderr, "[KV8] NewTopic_new: %s\n", errstr);
      rd_kafka_AdminOptions_destroy(pOpts); rd_kafka_destroy(rk); return false; }

    rd_kafka_queue_t *pQ = rd_kafka_queue_new(rk);
    rd_kafka_CreateTopics(rk, &pNew, 1, pOpts, pQ);

    bool bOk = false;
    rd_kafka_event_t *pEv = nullptr;
    auto tDeadline = std::chrono::steady_clock::now()
                   + std::chrono::milliseconds(timeoutMs + 2000);
    while (!pEv && std::chrono::steady_clock::now() < tDeadline)
        pEv = rd_kafka_queue_poll(pQ, 200);

    if (pEv)
    {
        if (rd_kafka_event_type(pEv) == RD_KAFKA_EVENT_CREATETOPICS_RESULT)
        {
            size_t cnt = 0;
            const rd_kafka_CreateTopics_result_t *pRes =
                rd_kafka_event_CreateTopics_result(pEv);
            const rd_kafka_topic_result_t **ppRes =
                rd_kafka_CreateTopics_result_topics(pRes, &cnt);
            for (size_t i = 0; i < cnt; i++)
            {
                rd_kafka_resp_err_t e = rd_kafka_topic_result_error(ppRes[i]);
                if (e == RD_KAFKA_RESP_ERR_NO_ERROR ||
                    e == RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS)
                    bOk = true;
                else
                    fprintf(stderr, "[KV8] CreateTopic '%s': %s\n",
                            rd_kafka_topic_result_name(ppRes[i]),
                            rd_kafka_topic_result_error_string(ppRes[i]));
            }
        }
        rd_kafka_event_destroy(pEv);
    }
    else
    {
        fprintf(stderr, "[KV8] CreateTopic timed out.\n");
    }

    rd_kafka_NewTopic_destroy(pNew);
    rd_kafka_AdminOptions_destroy(pOpts);
    rd_kafka_queue_destroy(pQ);
    rd_kafka_destroy(rk);
    return bOk;
}

// ── DeleteSessionTopics ───────────────────────────────────────────────────────

void Kv8ConsumerImpl::DeleteSessionTopics(const SessionMeta &sm)
{
    // Collect topics: all data topics + log + control.
    std::vector<std::string> topics;
    for (auto &t : sm.dataTopics) topics.push_back(t);
    topics.push_back(sm.sLogTopic);
    topics.push_back(sm.sControlTopic);

    printf("[KV8] Deleting %zu topic(s) for session '%s'...\n",
           topics.size(), sm.sSessionID.c_str());

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();

    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return;
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rk) { fprintf(stderr, "[KV8] del rd_kafka_new: %s\n", errstr);
               rd_kafka_conf_destroy(pConf); return; }

    rd_kafka_AdminOptions_t *pOpts =
        rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_DELETETOPICS);
    rd_kafka_AdminOptions_set_request_timeout(pOpts, 15000, errstr, sizeof(errstr));

    std::vector<rd_kafka_DeleteTopic_t*> vDel;
    vDel.reserve(topics.size());
    for (auto &t : topics) vDel.push_back(rd_kafka_DeleteTopic_new(t.c_str()));

    rd_kafka_queue_t *pQ = rd_kafka_queue_new(rk);
    rd_kafka_DeleteTopics(rk, vDel.data(), vDel.size(), pOpts, pQ);

    // Wait up to 20 s for the result.
    rd_kafka_event_t *pEv = nullptr;
    auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!pEv && std::chrono::steady_clock::now() < tDeadline)
        pEv = rd_kafka_queue_poll(pQ, 500);

    if (pEv)
    {
        if (rd_kafka_event_type(pEv) == RD_KAFKA_EVENT_DELETETOPICS_RESULT)
        {
            const rd_kafka_DeleteTopics_result_t *pRes =
                rd_kafka_event_DeleteTopics_result(pEv);
            size_t cnt = 0;
            const rd_kafka_topic_result_t **ppRes =
                rd_kafka_DeleteTopics_result_topics(pRes, &cnt);
            for (size_t i = 0; i < cnt; i++)
            {
                rd_kafka_resp_err_t e = rd_kafka_topic_result_error(ppRes[i]);
                const char *name = rd_kafka_topic_result_name(ppRes[i]);
                if (e == RD_KAFKA_RESP_ERR_NO_ERROR)
                    printf("  Deleted : %s\n", name);
                else if (e == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART)
                    printf("  Absent  : %s  (already gone)\n", name);
                else
                    fprintf(stderr, "  Failed (%s): %s\n",
                            name,
                            rd_kafka_topic_result_error_string(ppRes[i]));
            }
        }
        rd_kafka_event_destroy(pEv);
    }
    else
    {
        fprintf(stderr, "[KV8] DeleteTopics timed out.\n");
    }

    for (auto *d : vDel) rd_kafka_DeleteTopic_destroy(d);
    rd_kafka_AdminOptions_destroy(pOpts);
    rd_kafka_queue_destroy(pQ);
    rd_kafka_destroy(rk);
}

// ── MarkSessionDeleted ────────────────────────────────────────────────────────

// CR-11: Delivery-report state for the tombstone record.
// Used so we can confirm the tombstone actually reached the broker,
// rather than relying on rd_kafka_flush() which returns success when the
// queue drains by timeout (message silently discarded).
struct TombstoneDR {
    std::atomic<bool>   done{false};
    rd_kafka_resp_err_t err{RD_KAFKA_RESP_ERR_NO_ERROR};
};
static void TombstoneDRCb(rd_kafka_t * /*rk*/,
                           const rd_kafka_message_t *pMsg,
                           void *opaque)
{
    auto *r  = static_cast<TombstoneDR*>(opaque);
    r->err   = pMsg->err;
    r->done.store(true, std::memory_order_release);
}

void Kv8ConsumerImpl::MarkSessionDeleted(const std::string &sChannel,
                                         const SessionMeta &sm)
{
    // Build tombstone record: fixed header + session-prefix bytes, no topic bytes.
    std::vector<uint8_t> buf(sizeof(KafkaRegistryRecord) + sm.sSessionPrefix.size());
    KafkaRegistryRecord *rec = reinterpret_cast<KafkaRegistryRecord*>(buf.data());
    memset(rec, 0, sizeof(KafkaRegistryRecord));
    rec->wCounterID = KV8_CID_DELETED;
    rec->wVersion   = KV8_REGISTRY_VERSION;
    rec->wNameLen   = (uint16_t)sm.sSessionPrefix.size();
    rec->wTopicLen  = 0;
    memcpy(rec + 1, sm.sSessionPrefix.data(), sm.sSessionPrefix.size());

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        fprintf(stderr, "[KV8] MarkSessionDeleted: security conf failed\n");
        return;
    }

    // CR-11: Install DR callback so we know whether the tombstone was
    // actually committed by the broker, not just queued locally.
    TombstoneDR drResult;
    rd_kafka_conf_set_opaque(pConf, &drResult);
    rd_kafka_conf_set_dr_msg_cb(pConf, TombstoneDRCb);

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rk)
    {
        fprintf(stderr, "[KV8] MarkSessionDeleted rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
        return;
    }

    std::string sRegTopic = sChannel + "._registry";
    rd_kafka_resp_err_t err = rd_kafka_producev(
        rk,
        RD_KAFKA_V_TOPIC(sRegTopic.c_str()),
        RD_KAFKA_V_VALUE(buf.data(), buf.size()),
        RD_KAFKA_V_PARTITION(0),
        RD_KAFKA_V_END);

    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        fprintf(stderr, "[KV8] MarkSessionDeleted produce: %s\n", rd_kafka_err2str(err));
        rd_kafka_destroy(rk);
        return;
    }

    // Poll until the DR callback fires (tombstone delivered/failed) or timed out.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10000);
    while (!drResult.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        rd_kafka_poll(rk, 100);

    rd_kafka_destroy(rk);

    if (!drResult.done.load(std::memory_order_relaxed))
        err = RD_KAFKA_RESP_ERR__TIMED_OUT;
    else
        err = drResult.err;

    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        fprintf(stderr, "[KV8] MarkSessionDeleted delivery failed: %s\n",
                rd_kafka_err2str(err));
    else
        // CR-18: diagnostic messages go to stderr, not stdout.
        fprintf(stderr, "[KV8] Tombstone delivered for '%s' in %s.\n",
                sm.sSessionPrefix.c_str(), sRegTopic.c_str());
}

// ── DeleteChannel ─────────────────────────────────────────────────────────────

void Kv8ConsumerImpl::DeleteChannel(const std::string &sChannel)
{
    // Match all topics belonging to this channel: those starting with "<sChannel>."
    std::string sDotPrefix = sChannel + ".";

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();
    if (!ApplySecurityConf(pConf, m_cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return;
    }
    rd_kafka_conf_set(pConf, "socket.timeout.ms", "25000", nullptr, 0);

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!rk)
    {
        fprintf(stderr, "[KV8] DeleteChannel rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
        return;
    }

    // Warm up the SASL connection.
    {
        auto tWarm = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < tWarm)
            rd_kafka_poll(rk, 100);
    }

    const struct rd_kafka_metadata *meta = nullptr;
    rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        err = rd_kafka_metadata(rk, 1 /*all_topics*/, nullptr, &meta, 10000);
        if (err == RD_KAFKA_RESP_ERR_NO_ERROR) break;
        fprintf(stderr, "[KV8] DeleteChannel metadata (attempt %d): %s\n",
                attempt + 1, rd_kafka_err2str(err));
        if (attempt == 0) rd_kafka_poll(rk, 500);
    }

    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        rd_kafka_destroy(rk);
        return;
    }

    // Collect all topics whose name starts with "<sChannel>."
    std::vector<std::string> topics;
    for (int i = 0; i < meta->topic_cnt; ++i)
    {
        const char *name = meta->topics[i].topic;
        if (!name) continue;
        if (strncmp(name, sDotPrefix.c_str(), sDotPrefix.size()) == 0)
            topics.push_back(name);
    }
    rd_kafka_metadata_destroy(meta);

    if (topics.empty())
    {
        printf("[KV8] No topics found for channel '%s'.\n", sChannel.c_str());
        rd_kafka_destroy(rk);
        return;
    }

    printf("[KV8] Deleting %zu topic(s) for channel '%s'...\n",
           topics.size(), sChannel.c_str());

    rd_kafka_AdminOptions_t *pOpts =
        rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_DELETETOPICS);
    rd_kafka_AdminOptions_set_request_timeout(pOpts, 15000, errstr, sizeof(errstr));

    std::vector<rd_kafka_DeleteTopic_t*> vDel;
    vDel.reserve(topics.size());
    for (auto &t : topics) vDel.push_back(rd_kafka_DeleteTopic_new(t.c_str()));

    rd_kafka_queue_t *pQ = rd_kafka_queue_new(rk);
    rd_kafka_DeleteTopics(rk, vDel.data(), vDel.size(), pOpts, pQ);

    rd_kafka_event_t *pEv = nullptr;
    auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!pEv && std::chrono::steady_clock::now() < tDeadline)
        pEv = rd_kafka_queue_poll(pQ, 500);

    if (pEv)
    {
        if (rd_kafka_event_type(pEv) == RD_KAFKA_EVENT_DELETETOPICS_RESULT)
        {
            const rd_kafka_DeleteTopics_result_t *pRes =
                rd_kafka_event_DeleteTopics_result(pEv);
            size_t cnt = 0;
            const rd_kafka_topic_result_t **ppRes =
                rd_kafka_DeleteTopics_result_topics(pRes, &cnt);
            for (size_t i = 0; i < cnt; i++)
            {
                rd_kafka_resp_err_t e = rd_kafka_topic_result_error(ppRes[i]);
                const char *name = rd_kafka_topic_result_name(ppRes[i]);
                if (e == RD_KAFKA_RESP_ERR_NO_ERROR)
                    printf("  Deleted : %s\n", name);
                else if (e == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART)
                    printf("  Absent  : %s  (already gone)\n", name);
                else
                    fprintf(stderr, "  Failed (%s): %s\n",
                            name, rd_kafka_topic_result_error_string(ppRes[i]));
            }
        }
        rd_kafka_event_destroy(pEv);
    }
    else
    {
        fprintf(stderr, "[KV8] DeleteChannel: admin request timed out.\n");
    }

    for (auto *d : vDel) rd_kafka_DeleteTopic_destroy(d);
    rd_kafka_AdminOptions_destroy(pOpts);
    rd_kafka_queue_destroy(pQ);
    rd_kafka_destroy(rk);
}

////////////////////////////////////////////////////////////////////////////////
// Factory
////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IKv8Consumer> IKv8Consumer::Create(const Kv8Config &cfg)
{
    return std::make_unique<Kv8ConsumerImpl>(cfg);
}

} // namespace kv8
