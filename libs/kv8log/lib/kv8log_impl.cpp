////////////////////////////////////////////////////////////////////////////////
// kv8log/lib/kv8log_impl.cpp
//
// Runtime shared library: implements the plain-C kv8log_* ABI.
// Built as kv8log_runtime.so / kv8log_runtime.dll.
// Loaded at runtime by the facade (Runtime.cpp); never linked at build time.
//
// Architecture mirrors kv8probe: one IKv8Producer per open handle, writing
// KafkaRegistryRecord + Kv8TelValue messages to Kafka.
//
// Per-counter topic layout (matches kv8scope expectations):
//   registry : <channel>._registry
//   log      : <channel>.<sessionID>._log
//   group    : <channel>.<sessionID>.d.<XXXXXXXX>          (GROUP record only)
//   data     : <channel>.<sessionID>.d.<XXXXXXXX>.<CCCC>   (per counter)
//
//   XXXXXXXX = FNV-32 hex of the channel name
//   CCCC     = 4-hex counter wID
//
// Timer strategy:
//   The session anchor is captured in kv8log_open():
//     hpc_anchor   = QPC / CLOCK_MONOTONIC in nanoseconds
//     wall_anchor  = clock_gettime(REALTIME) / GetSystemTimePreciseAsFileTime
//
//   kv8log_add()    calls QPC internally, converts to session offset.
//   kv8log_add_ts() accepts nanoseconds since Unix epoch; converts via anchor.
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  ifdef KV8LOG_EXPORTS
#    define KV8LOG_API extern "C" __declspec(dllexport)
#  else
#    define KV8LOG_API extern "C" __declspec(dllimport)
#  endif
#else
#  include <time.h>
#  include <unistd.h>
#  define KV8LOG_API extern "C" __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>

// kv8 producer + consumer + types from the static libkv8.
#include <kv8/IKv8Producer.h>
#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>
#include <kv8util/Kv8AppUtils.h>
#include <kv8util/Kv8TopicUtils.h>

using namespace kv8;

// ── FNV-32 ─────────────────────────────────────────────────────────────────
static uint32_t Fnv32(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

// ── ExtractSchemaName ────────────────────────────────────────────────────────
// Extracts the value of "name":"..." from a schema JSON string.
// Returns empty string if the field is absent or malformed.
static std::string ExtractSchemaName(const char* schema_json)
{
    if (!schema_json) return {};
    const char* p = strstr(schema_json, "\"name\":");
    if (!p)           p = strstr(schema_json, "'name':");
    if (!p) return {};
    p += 7; // skip ["name"]: or ['name']: -- both are 7 chars
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"' && *p != '\'') return {};
    const char q = *p++;  // opening quote
    const char* end = strchr(p, q);
    if (!end) return {};
    return std::string(p, static_cast<size_t>(end - p));
}

// ── ParseCtlMsg ─────────────────────────────────────────────────────────────
// Parse a ._ctl JSON message: {"v":1,"cmd":"ctr_state","wid":N,"enabled":BOOL,...}
// Returns true on success and sets outWid / outEnabled.
static bool ParseCtlMsg(const char* p, size_t len,
                         uint16_t& outWid, bool& outEnabled)
{
    const char* end = p + len;
    auto find = [p, end](const char* key) -> const char*
    {
        size_t klen = strlen(key);
        for (const char* q = p; q + klen <= end; ++q)
            if (memcmp(q, key, klen) == 0) return q + klen;
        return nullptr;
    };
    const char* pw = find("\"wid\":");
    const char* pe = find("\"enabled\":");
    if (!pw || !pe) return false;
    while (pw < end && (*pw == ' ' || *pw == '\t')) ++pw;
    while (pe < end && (*pe == ' ' || *pe == '\t')) ++pe;
    char* ep = nullptr;
    long wid = strtol(pw, &ep, 10);
    if (ep == pw || wid < 0 || wid > 0xFFFF) return false;
    if (pe + 4 <= end && memcmp(pe, "true", 4) == 0)
        outEnabled = true;
    else if (pe + 5 <= end && memcmp(pe, "false", 5) == 0)
        outEnabled = false;
    else
        return false;
    outWid = static_cast<uint16_t>(wid);
    return true;
}

// ── Extension header ────────────────────────────────────────────────────────
static inline uint32_t MakeExtHeader(uint32_t size)
{
    // type=2 (TEL_V2), subtype=2 (VALUE).
    // CR-15: field bits [31:10] carry the byte size of the packet (not bits).
    return 2u | (2u << 5) | (size << 10);
}

// ── Platform time primitives ────────────────────────────────────────────────
static uint64_t MonoNs()
{
#ifdef _WIN32
    // QueryPerformanceFrequency returns a value that is constant for the
    // lifetime of the process (Windows guarantees this since Vista).
    // Cache it in a file-scope static initialized exactly once.
    static const uint64_t s_freq = []() -> uint64_t {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    uint64_t c = static_cast<uint64_t>(cnt.QuadPart);
    return (c / s_freq) * 1000000000ULL + (c % s_freq) * 1000000000ULL / s_freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t WallNs()
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t ft100 = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // FILETIME epoch is 1601-01-01; Unix epoch offset is 116444736000000000 * 100ns.
    return (ft100 - 116444736000000000ULL) * 100ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static void GetFiletime(uint32_t& hi, uint32_t& lo)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    hi = ft.dwHighDateTime;
    lo = ft.dwLowDateTime;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = (uint64_t)ts.tv_sec  * 10000000ULL
                + (uint64_t)ts.tv_nsec / 100ULL
                + 116444736000000000ULL;
    hi = (uint32_t)(ft >> 32);
    lo = (uint32_t)(ft);
#endif
}

// CR-03: Read both clocks back-to-back with no voluntary yield between them
// to minimise the skew between the two anchor values.  Using a single helper
// removes the risk of unrelated code being inserted between the two reads.
struct AnchorPair { uint64_t mono_ns; uint64_t wall_ns; };
static AnchorPair CaptureAnchors()
{
    AnchorPair ap;
    ap.mono_ns = MonoNs();
    ap.wall_ns = WallNs();
    return ap;
}

// ── Registry helper ─────────────────────────────────────────────────────────
static void WriteRegistryRecord(IKv8Producer* p, const std::string& regTopic,
    uint32_t dwHash, uint16_t wCounterID, uint16_t wFlags,
    double dbMin, double dbMax,
    uint64_t qwTimerFrequency, uint64_t qwTimerValue,
    uint32_t dwTimeHi, uint32_t dwTimeLo,
    const std::string& sName, const std::string& sTopic)
{
    KafkaRegistryRecord rec{};
    rec.dwHash           = dwHash;
    rec.wCounterID       = wCounterID;
    rec.wFlags           = wFlags;
    rec.dbMin            = dbMin;
    rec.dbAlarmMin       = dbMin;
    rec.dbMax            = dbMax;
    rec.dbAlarmMax       = dbMax;
    rec.wNameLen         = (uint16_t)sName.size();
    rec.wTopicLen        = (uint16_t)sTopic.size();
    rec.wVersion         = KV8_REGISTRY_VERSION;
    rec.wPad             = 0;
    rec.qwTimerFrequency = qwTimerFrequency;
    rec.qwTimerValue     = qwTimerValue;
    rec.dwTimeHi         = dwTimeHi;
    rec.dwTimeLo         = dwTimeLo;

    std::vector<uint8_t> buf(sizeof(rec) + sName.size() + sTopic.size());
    memcpy(buf.data(),                              &rec,          sizeof(rec));
    memcpy(buf.data() + sizeof(rec),                sName.data(),  sName.size());
    memcpy(buf.data() + sizeof(rec) + sName.size(), sTopic.data(), sTopic.size());
    p->Produce(regTopic, buf.data(), buf.size(), nullptr, 0);
}

// ── Per-counter slot ────────────────────────────────────────────────────────
struct CounterSlot
{
    std::string  name;
    std::string  data_topic;
    double       mn = 0.0;
    double       mx = 1.0;
    uint16_t     id = 0;
    std::atomic<uint16_t> seq{0};
    // Pre-created rd_kafka_topic_t* (opaque here; typed as void* to avoid
    // exposing librdkafka headers).  Set once in kv8log_register_counter()
    // via IKv8Producer::CreateTopic(); destroyed in kv8log_close() before
    // the producer is torn down.  Never written after registration.
    void*        rkt = nullptr;

    CounterSlot() = default;
    CounterSlot(CounterSlot&& o) noexcept
        : name(std::move(o.name))
        , data_topic(std::move(o.data_topic))
        , mn(o.mn), mx(o.mx), id(o.id)
        , seq(o.seq.load(std::memory_order_relaxed))
        , rkt(o.rkt)
    {
        o.rkt = nullptr;  // ownership transferred; guard against double-destroy
    }
    CounterSlot& operator=(CounterSlot&&) = delete;
    CounterSlot(const CounterSlot&)       = delete;
    CounterSlot& operator=(const CounterSlot&) = delete;
};

// ── Session handle ──────────────────────────────────────────────────────────
struct Kv8LogSession
{
    std::unique_ptr<IKv8Producer> producer;

    // Timing anchor
    uint64_t wall_anchor_ns = 0;  // Unix epoch nanoseconds at open
    uint64_t mono_anchor_ns = 0;  // CLOCK_MONOTONIC nanoseconds at open
    // Timer stored as nanoseconds; qwTimer in messages = (mono_ns - mono_anchor_ns)
    // expressed in units of 1 ns  (frequency = 1e9).
    static const uint64_t kTimerFrequency = 1000000000ULL; // 1 GHz (1 tick = 1 ns)

    // Topic names
    std::string channel_name;
    std::string session_id;
    std::string reg_topic;
    std::string log_topic;
    std::string group_topic;

    uint32_t  group_hash = 0;

    std::mutex counters_mu;
    std::vector<CounterSlot> counters;
    // Atomic so kv8log_add() can do a lock-free bounds check (CR-02).
    // Incremented with release ordering AFTER the slot is appended to the
    // vector; kv8log_add() loads with acquire to establish happens-before.
    std::atomic<uint16_t> next_id{0};

    // Per-counter enabled flags (indexed by wCounterID).
    // Initialized to true by kv8log_open(); updated by CtrlConsumerThread
    // when a ctr_state command arrives on the ._ctl topic.
    // Hot-path read: single memory_order_relaxed atomic load per sample.
    std::atomic<bool> aEnabled[1024];

    // Control topic and consumer thread for bidirectional enable/disable.
    std::string        ctl_topic;  // <channel>.<sessionID>._ctl
    kv8::Kv8Config     ctl_cfg;    // broker config stored for CtrlConsumerThread
    std::thread        ctl_thread;
    std::atomic<bool>  ctl_stop{false};

    // Write the initial session registration records to Kafka.
    void WriteSessionHeader()
    {
        uint32_t dwTimeHi = 0, dwTimeLo = 0;
        GetFiletime(dwTimeHi, dwTimeLo);

        // 0. Pre-create the ._ctl topic BEFORE writing registry records.
        //    kv8scope discovers sessions by reading the registry topic; if the
        //    registry record lands before ._ctl exists, kv8scope's ConsumerThread
        //    subscribes to a non-existent topic and gets UNKNOWN_TOPIC errors.
        //    A null-payload record ("tombstone") reliably forces auto-creation.
        //    ParseAndQueueCtl / ParseCtlMsg both check cbData == 0 and return
        //    immediately, so this record has zero effect on consumer behaviour.
        if (!ctl_topic.empty())
        {
            producer->Produce(ctl_topic, nullptr, 0, nullptr, 0);
            producer->Flush(2000);
        }

        // 1. LOG record: session announcement
        WriteRegistryRecord(producer.get(), reg_topic,
            0, KV8_CID_LOG, 0,
            0.0, 0.0, 0, 0, 0, 0,
            channel_name, log_topic);

        // 2. GROUP record: timer anchors
        WriteRegistryRecord(producer.get(), reg_topic,
            group_hash, KV8_CID_GROUP, 0,
            0.0, 0.0,
            kTimerFrequency, mono_anchor_ns,
            dwTimeHi, dwTimeLo,
            channel_name, group_topic);

        producer->Flush(5000);

        // 3. session_open marker so RunCtlConsumerThread has a meaningful first
        //    record to consume (avoids EOF-at-start edge cases in some rdkafka
        //    configurations).  ParseCtlMsg ignores it (no "wid"/"enabled" fields).
        if (!ctl_topic.empty())
        {
            uint64_t tsMs = WallNs() / 1000000ULL;
            char msg[128];
            int n = snprintf(msg, sizeof(msg),
                "{\"v\":1,\"cmd\":\"session_open\",\"ts\":%llu}",
                static_cast<unsigned long long>(tsMs));
            if (n > 0 && n < static_cast<int>(sizeof(msg)))
                producer->Produce(ctl_topic, msg, static_cast<size_t>(n),
                                  nullptr, 0);
            producer->Flush(2000);
        }
    }
};

// ── RunCtlConsumerThread ────────────────────────────────────────────────────
// Listens on the ._ctl topic and updates per-counter enabled flags when
// kv8scope (or another tool) sends a ctr_state command.
static void RunCtlConsumerThread(Kv8LogSession* s)
{
    kv8::Kv8Config cfg = s->ctl_cfg;
    // Use a unique group ID per session instance so offsets are not shared.
    cfg.sGroupID = s->ctl_topic + ".c";

    auto pCons = kv8::IKv8Consumer::Create(cfg);
    if (!pCons)
    {
        fprintf(stderr, "[kv8log_rt] CtrlConsumer: IKv8Consumer::Create failed\n");
        return;
    }

    // Subscribe from the beginning (IKv8Consumer seeks to OFFSET_BEGINNING on
    // first partition assignment, so any prior commands for this session are
    // replayed on startup in the correct order).
    pCons->Subscribe(s->ctl_topic);

    while (!s->ctl_stop.load(std::memory_order_relaxed))
    {
        pCons->Poll(50,
            [s](std::string_view, const void* data, size_t len, int64_t)
            {
                uint16_t wid = 0;
                bool     bEn = true;
                if (ParseCtlMsg(static_cast<const char*>(data), len, wid, bEn) &&
                    wid < s->next_id.load(std::memory_order_acquire))
                {
                    s->aEnabled[wid].store(bEn, std::memory_order_relaxed);
                }
            });
    }

    pCons->Stop();
}

// ── kv8log_open ─────────────────────────────────────────────────────────────
KV8LOG_API
void* kv8log_open(const char* brokers, const char* channel,
                  const char* user, const char* pass)
{
    if (!brokers || !channel) return nullptr;

    Kv8Config cfg;
    cfg.sBrokers = brokers;

    // CR-10: Allow security protocol and SASL mechanism to be overridden via
    // environment variables so callers on TLS-only brokers are not forced to
    // patch the source.  Fall back to sasl_plaintext/PLAIN when absent.
    //   KV8_SECURITY_PROTO  e.g. "sasl_ssl", "ssl", "plaintext"
    //   KV8_SASL_MECHANISM  e.g. "SCRAM-SHA-256", "SCRAM-SHA-512"
    {
        const char *proto = getenv("KV8_SECURITY_PROTO");
        const char *mech  = getenv("KV8_SASL_MECHANISM");
        cfg.sSecurityProto = proto ? proto : "sasl_plaintext";
        cfg.sSaslMechanism = mech  ? mech  : "PLAIN";
    }
    cfg.sUser = user ? user : "kv8producer";
    cfg.sPass = pass ? pass : "kv8secret";

    // Sanitize channel name: replace '/' with '.' for Kafka topic compatibility.
    std::string chan(channel);
    for (char& c : chan) if (c == '/') c = '.';

    // Generate unique session ID before creating the producer so the heartbeat
    // topic name can be wired into Kv8Config for automatic startup.
    std::string full  = kv8util::GenerateTopicName("S");
    std::string sesId;
    {
        auto pos = full.find('.');
        sesId = (pos != std::string::npos) ? full.substr(pos + 1) : full;
    }

    // Wire heartbeat: producer starts the background thread automatically on
    // connect and sends a clean-shutdown marker when destroyed.
    cfg.sHeartbeatTopic      = chan + "." + sesId + ".hb";
    cfg.nHeartbeatIntervalMs = 3000;

    auto prod = IKv8Producer::Create(cfg);
    if (!prod) {
        fprintf(stderr, "[kv8log_rt] IKv8Producer::Create failed\n");
        return nullptr;
    }

    uint32_t hash = Fnv32(chan);
    char szHash[16];
    snprintf(szHash, sizeof(szHash), "%08X", hash);

    auto* s = new Kv8LogSession();
    s->producer       = std::move(prod);
    s->channel_name   = chan;
    s->session_id     = sesId;
    s->group_hash     = hash;
    s->reg_topic      = chan + "._registry";
    s->log_topic      = chan + "." + sesId + "._log";
    s->group_topic    = chan + "." + sesId + ".d." + szHash;

    // Capture timing anchor as a back-to-back pair (CR-03: single helper
    // eliminates the risk of unrelated code between the two clock reads).
    auto anchors = CaptureAnchors();
    s->mono_anchor_ns = anchors.mono_ns;
    s->wall_anchor_ns = anchors.wall_ns;

    // Pre-reserve counter slots to prevent push_back() from ever reallocating
    // the vector.  Reallocation after other threads hold CounterSlot references
    // (through kv8log_add) would be a data race.  1024 slots is well above any
    // realistic counter count; the reservation cost is negligible (one malloc).
    s->counters.reserve(1024);

    // Initialize all per-counter enabled flags to true (default: all on).
    for (auto& e : s->aEnabled)
        e.store(true, std::memory_order_relaxed);

    // Store broker config and control topic for CtrlConsumerThread.
    s->ctl_cfg   = cfg;
    s->ctl_topic = chan + "." + sesId + "._ctl";

    s->WriteSessionHeader();

    // Start the control consumer thread after the session header is written
    // so the producer is fully set up before the consumer thread starts.
    s->ctl_stop.store(false, std::memory_order_relaxed);
    s->ctl_thread = std::thread(RunCtlConsumerThread, s);

    return s;
}

// ── kv8log_close ───────────────────────────────────────────────────────────
KV8LOG_API
void kv8log_close(void* h)
{
    if (!h) return;
    auto* s = static_cast<Kv8LogSession*>(h);

    // Signal and join the control consumer thread before tearing down the
    // producer.  The thread must not outlive the Kv8LogSession it references.
    s->ctl_stop.store(true, std::memory_order_relaxed);
    if (s->ctl_thread.joinable())
        s->ctl_thread.join();

    // Destroy pre-created topic handles BEFORE flushing and destroying the
    // producer.  rd_kafka_topic_destroy() must be called before rd_kafka_destroy().
    for (auto& slot : s->counters) {
        if (slot.rkt) {
            s->producer->DestroyTopic(slot.rkt);
            slot.rkt = nullptr;
        }
    }
    s->producer->Flush(10000);
    delete s;
}

// ── kv8log_set_counter_enabled ─────────────────────────────────────────────
KV8LOG_API
void kv8log_set_counter_enabled(void* h, uint16_t id, int bEnabled)
{
    if (!h) return;
    auto* s = static_cast<Kv8LogSession*>(h);
    if (id >= s->next_id.load(std::memory_order_acquire)) return;

    bool bEn = (bEnabled != 0);
    s->aEnabled[id].store(bEn, std::memory_order_relaxed);

    // Publish to ._ctl so kv8scope stays in sync.
    if (s->ctl_topic.empty()) return;
    uint64_t tsMs = WallNs() / 1000000ULL;
    char msg[128];
    int n = snprintf(msg, sizeof(msg),
        "{\"v\":1,\"cmd\":\"ctr_state\",\"wid\":%u,\"enabled\":%s,\"ts\":%llu}",
        static_cast<unsigned>(id),
        bEn ? "true" : "false",
        static_cast<unsigned long long>(tsMs));
    if (n > 0 && n < static_cast<int>(sizeof(msg)))
        s->producer->Produce(s->ctl_topic, msg, static_cast<size_t>(n),
                              nullptr, 0);
}

// ── kv8log_register_counter ────────────────────────────────────────────────
KV8LOG_API
int kv8log_register_counter(void* h, const char* name,
                             double mn, double mx, uint16_t* out_id)
{
    if (!h || !name || !out_id) return -1;
    auto* s = static_cast<Kv8LogSession*>(h);

    // Phase 1 (under lock): assign ID and push the slot.
    // The vector is pre-reserved in kv8log_open(); push_back never reallocates,
    // so concurrent kv8log_add() calls for already-registered counters are safe.
    // next_id is incremented AFTER push_back with release ordering so that a
    // concurrent kv8log_add() loading next_id with acquire observes a fully
    // constructed slot (CR-02).
    uint16_t id;
    std::string data_topic;
    {
        std::lock_guard<std::mutex> lk(s->counters_mu);
        // Peek at the current id under the lock (relaxed: we hold the mutex).
        id = s->next_id.load(std::memory_order_relaxed);
        *out_id = id;

        char szId[16];
        snprintf(szId, sizeof(szId), "%04X", (unsigned)id);
        data_topic = s->group_topic + "." + szId;

        CounterSlot slot;
        slot.name       = name;
        slot.data_topic = data_topic;
        slot.mn         = mn;
        slot.mx         = mx;
        slot.id         = id;
        // slot.rkt stays nullptr here; set below outside the lock.
        s->counters.push_back(std::move(slot));
        // Publish AFTER push_back so any thread loading next_id with acquire
        // is guaranteed to see the slot at index id.
        s->next_id.fetch_add(1, std::memory_order_release);
    }

    // Phase 2 (outside lock): Kafka I/O -- no vector mutation.
    // Write registry record so kv8scope can discover the counter before
    // any data samples arrive.
    WriteRegistryRecord(s->producer.get(), s->reg_topic,
        s->group_hash, id, 1 /*enabled*/,
        mn, mx, 0, 0, 0, 0,
        name, data_topic);

    // Phase 3 (outside lock): pre-create the topic handle for the hot path.
    // CR-14: The call_once guarantee below applies only to the C++ facade
    // (Counter::EnsureRegistered).  Raw C API callers MUST NOT call
    // kv8log_add() for a given id until kv8log_register_counter() returns;
    // violating this contract silently drops one message (rkt is nullptr
    // until this line completes, and ProduceToTopic guards against nullptr).
    s->counters[id].rkt = s->producer->CreateTopic(data_topic);

    // Phase 4 (outside lock): flush registry record.
    s->producer->Flush(2000);

    return 0;
}

// ── kv8log_add ─────────────────────────────────────────────────────────────
KV8LOG_API
void kv8log_add(void* h, uint16_t id, double value)
{
    if (!h) return;
    auto* s = static_cast<Kv8LogSession*>(h);

    // Lock-free bounds check via atomic next_id (CR-02).
    // acquire load pairs with the release fetch_add in kv8log_register_counter,
    // ensuring the CounterSlot at index id is fully visible before we read it.
    if (id >= s->next_id.load(std::memory_order_acquire)) return;
    if (!s->aEnabled[id].load(std::memory_order_relaxed)) return;
    CounterSlot& slot = s->counters[id];

    Kv8TelValue pkt{};
    pkt.sCommonRaw.dwBits = MakeExtHeader((uint32_t)sizeof(Kv8TelValue));
    pkt.wID      = id;
    pkt.wSeqN    = slot.seq.fetch_add(1, std::memory_order_relaxed);
    pkt.qwTimer  = MonoNs() - s->mono_anchor_ns;
    pkt.dbValue  = value;

    uint16_t keyBE = (uint16_t)(((id & 0xFF) << 8) | ((id >> 8) & 0xFF));
    s->producer->ProduceToTopic(slot.rkt, &pkt, sizeof(pkt), &keyBE, sizeof(keyBE));
}

// ── kv8log_add_ts ──────────────────────────────────────────────────────────
KV8LOG_API
void kv8log_add_ts(void* h, uint16_t id, double value, uint64_t ts_ns)
{
    if (!h) return;
    auto* s = static_cast<Kv8LogSession*>(h);

    // Lock-free bounds check via atomic next_id (CR-02).
    if (id >= s->next_id.load(std::memory_order_acquire)) return;
    if (!s->aEnabled[id].load(std::memory_order_relaxed)) return;
    CounterSlot& slot = s->counters[id];

    // Convert Unix-epoch ts_ns to session-relative tick.
    uint64_t offset_ns = (ts_ns >= s->wall_anchor_ns) ? (ts_ns - s->wall_anchor_ns) : 0;

    Kv8TelValue pkt{};
    pkt.sCommonRaw.dwBits = MakeExtHeader((uint32_t)sizeof(Kv8TelValue));
    pkt.wID      = id;
    pkt.wSeqN    = slot.seq.fetch_add(1, std::memory_order_relaxed);
    pkt.qwTimer  = offset_ns;
    pkt.dbValue  = value;

    uint16_t keyBE = (uint16_t)(((id & 0xFF) << 8) | ((id >> 8) & 0xFF));
    s->producer->ProduceToTopic(slot.rkt, &pkt, sizeof(pkt), &keyBE, sizeof(keyBE));
}

// ── kv8log_flush ───────────────────────────────────────────────────────────
KV8LOG_API
void kv8log_flush(void* h, int timeout_ms)
{
    if (!h) return;
    auto* s = static_cast<Kv8LogSession*>(h);
    s->producer->Flush(timeout_ms);
}

// ── kv8log_monotonic_to_ns ─────────────────────────────────────────────────
KV8LOG_API
uint64_t kv8log_monotonic_to_ns(void* h, uint64_t mono_ns)
{
    if (!h) return 0;
    auto* s = static_cast<Kv8LogSession*>(h);
    // result = wall_anchor_ns + (mono_ns - mono_anchor_ns)
    if (mono_ns < s->mono_anchor_ns) return s->wall_anchor_ns;
    return s->wall_anchor_ns + (mono_ns - s->mono_anchor_ns);
}

// ── kv8log_register_udt_feed ───────────────────────────────────────────────
//
// Registers a UDT feed.  Two registry records are written:
//   1. KV8_CID_SCHEMA  -- carries the JSON schema (idempotent per hash).
//   2. Regular counter record with KV8_FLAG_UDT set -- carries the feed name
//      and its data topic so kv8scope can subscribe to sample messages.
//
// UDT feeds share the same ID space and aEnabled[] array as scalar counters.
// The hot path (kv8log_add_udt) is lock-free after registration.
KV8LOG_API
int kv8log_register_udt_feed(void* h, const char* display_name,
                              const char* schema_json, uint16_t* out_id)
{
    if (!h || !display_name || !schema_json || !out_id) return -1;
    auto* s = static_cast<Kv8LogSession*>(h);

    const uint32_t    schema_hash = Fnv32(std::string(schema_json));
    const std::string schema_name = ExtractSchemaName(schema_json);

    uint16_t    id;
    std::string data_topic;
    {
        std::lock_guard<std::mutex> lk(s->counters_mu);
        id = s->next_id.load(std::memory_order_relaxed);
        if (id >= 1024) return -2; // ID space exhausted
        *out_id = id;

        // UDT data topic: <channel>.<sessionID>.u.<schemaHash8hex>.<feedID4hex>
        char szHash[16], szId[16];
        snprintf(szHash, sizeof(szHash), "%08X", schema_hash);
        snprintf(szId,   sizeof(szId),   "%04X", static_cast<unsigned>(id));
        data_topic = s->channel_name + "." + s->session_id + ".u." + szHash + "." + szId;

        CounterSlot slot;
        slot.name       = display_name;
        slot.data_topic = data_topic;
        slot.mn         = 0.0;
        slot.mx         = 0.0;
        slot.id         = id;
        // slot.rkt set below outside the lock (same pattern as scalar counters)
        s->counters.push_back(std::move(slot));
        // Release ordering: pairs with acquire in kv8log_add_udt bounds check.
        s->next_id.fetch_add(1, std::memory_order_release);
    }

    // Write schema registry record (idempotent; consumers deduplicate by hash).
    // sTopic field carries the full JSON schema text so consumers can decode
    // payloads without a separate schema registry.
    WriteRegistryRecord(s->producer.get(), s->reg_topic,
        schema_hash, KV8_CID_SCHEMA, 0,
        0.0, 0.0, 0, 0, 0, 0,
        schema_name, std::string(schema_json));

    // Write UDT feed registry record: KV8_FLAG_UDT marks it as a UDT stream.
    // Bit 0 set (| 1u) mirrors what scalar counters do -- means "enabled at creation".
    WriteRegistryRecord(s->producer.get(), s->reg_topic,
        schema_hash, id, static_cast<uint16_t>(KV8_FLAG_UDT | 1u),
        0.0, 0.0, 0, 0, 0, 0,
        display_name, data_topic);

    // Pre-create topic handle for zero-alloc hot path (same as scalar counters).
    s->counters[id].rkt = s->producer->CreateTopic(data_topic);

    s->producer->Flush(2000);
    return 0;
}

// ── kv8log_register_udt_schema ────────────────────────────────────────────
//
// Registers a bare schema record (KV8_CID_SCHEMA) without creating a feed.
// Used to pre-register embedded sub-schemas before calling register_udt_feed
// for a nested UDT type.  Idempotent: consumers deduplicate by hash.
KV8LOG_API
int kv8log_register_udt_schema(void* h, const char* schema_json)
{
    if (!h || !schema_json) return -1;
    auto* s = static_cast<Kv8LogSession*>(h);

    const uint32_t    schema_hash = Fnv32(std::string(schema_json));
    const std::string schema_name = ExtractSchemaName(schema_json);

    WriteRegistryRecord(s->producer.get(), s->reg_topic,
        schema_hash, KV8_CID_SCHEMA, 0,
        0.0, 0.0, 0, 0, 0, 0,
        schema_name, std::string(schema_json));

    s->producer->Flush(2000);
    return 0;
}

// ── kv8log_add_udt ─────────────────────────────────────────────────────────
//
// Hot path: entirely lock-free.  Builds the 16-byte Kv8UDTSample header and
// appends packed_data on the stack, then hands the buffer to ProduceToTopic.
// Maximum message size: sizeof(Kv8UDTSample) + KV8_UDT_MAX_PAYLOAD = 256 B.
KV8LOG_API
void kv8log_add_udt(void* h, uint16_t feed_id,
                    const void* packed_data, uint16_t data_size)
{
    if (!h || !packed_data) return;
    auto* s = static_cast<Kv8LogSession*>(h);

    // Lock-free bounds check (CR-02): acquire pairs with release in register.
    if (feed_id >= s->next_id.load(std::memory_order_acquire)) return;
    if (!s->aEnabled[feed_id].load(std::memory_order_relaxed))  return;
    if (data_size > KV8_UDT_MAX_PAYLOAD) return;

    CounterSlot& slot = s->counters[feed_id];

    // Stack buffer: zero heap allocation on hot path.
    uint8_t buf[sizeof(Kv8UDTSample) + KV8_UDT_MAX_PAYLOAD];
    const uint16_t msg_size =
        static_cast<uint16_t>(sizeof(Kv8UDTSample) + data_size);

    Kv8UDTSample hdr{};
    // ExtHeader bits: type=2 (TEL_V2), subtype=KV8_TEL_TYPE_UDT=5, size=msg_size.
    hdr.sCommonRaw.dwBits =
        2u | (KV8_TEL_TYPE_UDT << 5) | (static_cast<uint32_t>(msg_size) << 10);
    hdr.wFeedID = feed_id;
    hdr.wSeqN   = slot.seq.fetch_add(1, std::memory_order_relaxed);
    hdr.qwTimer = MonoNs() - s->mono_anchor_ns;

    memcpy(buf,                         &hdr,        sizeof(hdr));
    memcpy(buf + sizeof(Kv8UDTSample),  packed_data, data_size);

    uint16_t keyBE = static_cast<uint16_t>(
        ((feed_id & 0xFFu) << 8) | ((feed_id >> 8) & 0xFFu));
    s->producer->ProduceToTopic(slot.rkt, buf, msg_size, &keyBE, sizeof(keyBE));
}

// ── kv8log_add_udt_ts ──────────────────────────────────────────────────────
//
// As kv8log_add_udt but accepts a caller-supplied Unix-epoch timestamp in ns
// (same contract as kv8log_add_ts for scalar counters).
KV8LOG_API
void kv8log_add_udt_ts(void* h, uint16_t feed_id,
                        const void* packed_data, uint16_t data_size,
                        uint64_t ts_ns)
{
    if (!h || !packed_data) return;
    auto* s = static_cast<Kv8LogSession*>(h);

    if (feed_id >= s->next_id.load(std::memory_order_acquire)) return;
    if (!s->aEnabled[feed_id].load(std::memory_order_relaxed))  return;
    if (data_size > KV8_UDT_MAX_PAYLOAD) return;

    CounterSlot& slot = s->counters[feed_id];

    // Convert Unix-epoch ts_ns to session-relative offset (same as kv8log_add_ts).
    uint64_t offset_ns = (ts_ns >= s->wall_anchor_ns)
                         ? (ts_ns - s->wall_anchor_ns) : 0;

    uint8_t buf[sizeof(Kv8UDTSample) + KV8_UDT_MAX_PAYLOAD];
    const uint16_t msg_size =
        static_cast<uint16_t>(sizeof(Kv8UDTSample) + data_size);

    Kv8UDTSample hdr{};
    hdr.sCommonRaw.dwBits =
        2u | (KV8_TEL_TYPE_UDT << 5) | (static_cast<uint32_t>(msg_size) << 10);
    hdr.wFeedID = feed_id;
    hdr.wSeqN   = slot.seq.fetch_add(1, std::memory_order_relaxed);
    hdr.qwTimer = offset_ns;

    memcpy(buf,                         &hdr,        sizeof(hdr));
    memcpy(buf + sizeof(Kv8UDTSample),  packed_data, data_size);

    uint16_t keyBE = static_cast<uint16_t>(
        ((feed_id & 0xFFu) << 8) | ((feed_id >> 8) & 0xFFu));
    s->producer->ProduceToTopic(slot.rkt, buf, msg_size, &keyBE, sizeof(keyBE));
}
