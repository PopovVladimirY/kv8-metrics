# kv8util & libkv8 API Reference

This guide covers the two C++ libraries that form the Kv8 telemetry-over-Kafka
toolkit:

| Library | Purpose | Namespace |
|---------|---------|-----------|
| **libkv8** | Core Kafka abstraction -- producer, consumer, types | `kv8` |
| **kv8util** | Application utilities -- timer, stats, signal handling | `kv8util` |

Together they provide everything needed to build Kv8 telemetry consumers and
producers without any direct exposure to librdkafka.

---

## Table of contents

1. [Quick start](#1-quick-start)
2. [libkv8 -- Core API](#2-libkv8----core-api)
   - [Kv8Config](#kv8config)
   - [IKv8Producer](#ikv8producer)
   - [IKv8Consumer](#ikv8consumer)
   - [On-wire types](#on-wire-types)
   - [Data model types](#data-model-types)
   - [Utility helpers](#utility-helpers)
3. [kv8util -- Application utilities](#3-kv8util----application-utilities)
   - [Kv8Timer](#kv8timer)
   - [Kv8BenchMsg](#kv8benchmsg)
   - [Kv8Stats](#kv8stats)
   - [Kv8TopicUtils](#kv8topicutils)
   - [Kv8AppUtils](#kv8apputils)
4. [Usage examples](#4-usage-examples)
   - [Minimal producer](#41-minimal-producer)
   - [Minimal consumer](#42-minimal-consumer)
   - [Session discovery and replay](#43-session-discovery-and-replay)
   - [High-throughput benchmark](#44-high-throughput-benchmark-pattern)
   - [Point-in-time verification](#45-point-in-time-verification)
   - [Maintenance operations](#46-maintenance-operations)
5. [Consumer best practices](#5-consumer-best-practices----zero-gap-reception)

---

## 1. Quick start

### Include paths

```
libs/libkv8/include/kv8/      -- IKv8Consumer.h, IKv8Producer.h, Kv8Types.h
libs/kv8util/include/kv8util/    -- Kv8Timer.h, Kv8BenchMsg.h, Kv8Stats.h, ...
```

### CMake linkage

```cmake
target_link_libraries(my_tool PRIVATE kv8 kv8util)
```

### Minimal includes

```cpp
#include <kv8/IKv8Producer.h>
#include <kv8/IKv8Consumer.h>
#include <kv8util/Kv8AppUtils.h>
```

---

## 2. libkv8 -- Core API

Everything in `<kv8/*.h>`.  No librdkafka types are exposed.

### Kv8Config

Defined in `<kv8/Kv8Types.h>`.  Passed to every factory function.

```cpp
struct Kv8Config
{
    std::string sBrokers       = "localhost:19092";
    std::string sSecurityProto = "sasl_plaintext";
    std::string sSaslMechanism = "PLAIN";
    std::string sUser          = "kv8producer";
    std::string sPass          = "kv8secret";
    std::string sGroupID;   // empty = auto-generated unique group
};
```

| Field | Description |
|-------|-------------|
| `sBrokers` | Comma-separated `host:port` list of Kafka bootstrap brokers. |
| `sSecurityProto` | `plaintext`, `sasl_plaintext`, or `sasl_ssl`. |
| `sSaslMechanism` | `PLAIN`, `SCRAM-SHA-256`, or `SCRAM-SHA-512`. |
| `sUser` / `sPass` | SASL credentials. |
| `sGroupID` | Consumer group ID.  Leave empty for auto-generated unique group. |

Use `kv8util::BuildKv8Config()` for a one-liner construction from CLI args.

---

### IKv8Producer

Defined in `<kv8/IKv8Producer.h>`.  Abstract interface; instances created via
the static factory.

#### Factory

```cpp
static std::unique_ptr<IKv8Producer> IKv8Producer::Create(const Kv8Config &cfg);
```

Returns `nullptr` only on fatal Kafka configuration error.

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| **Produce** | `bool Produce(const string &sTopic, const void *pPayload, size_t cbPayload, const void *pKey = nullptr, size_t cbKey = 0)` | Enqueue one message.  Payload and key are copied internally.  Returns `false` when the internal queue is full. |
| **Flush** | `void Flush(int timeoutMs = 10000)` | Block until all enqueued messages are delivered or timeout.  Pass `0` for a non-blocking poll of delivery reports. |
| **GetDeliveryFailures** | `int64_t GetDeliveryFailures() const` | Cumulative delivery-report failures (broker could not accept). |
| **GetDeliverySuccess** | `int64_t GetDeliverySuccess() const` | Cumulative successfully delivered messages. |

**Thread safety:** `Produce()` and `Flush()` must be called from a single
thread.

---

### IKv8Consumer

Defined in `<kv8/IKv8Consumer.h>`.  The most feature-rich interface.

#### Factory

```cpp
static std::unique_ptr<IKv8Consumer> IKv8Consumer::Create(const Kv8Config &cfg);
```

#### Channel & session discovery

| Method | Signature | Description |
|--------|-----------|-------------|
| **ListChannels** | `vector<string> ListChannels(int timeoutMs = 5000)` | Scan broker metadata for all `kv8.*` channel prefixes that have a `._registry` topic. |
| **DiscoverSessions** | `map<string, SessionMeta> DiscoverSessions(const string &sChannel)` | Read the channel's `._registry` topic from offset 0 and return all sessions found.  Uses a temporary internal consumer -- does not disturb active subscriptions. |

#### Streaming subscription

| Method | Signature | Description |
|--------|-----------|-------------|
| **Subscribe** | `void Subscribe(const string &sTopic)` | Add a topic to the live subscription.  Idempotent.  New partitions are seeked to `OFFSET_BEGINNING` automatically.  Must not be called from inside a Poll callback. |
| **Poll** | `void Poll(int timeoutMs, const function<void(string_view sTopic, const void *pPayload, size_t cbPayload, int64_t tsKafkaMs)> &onMessage)` | Wait for up to one message.  Callback fires synchronously. |
| **PollBatch** | `int PollBatch(int maxMessages, int timeoutMs, const function<...> &onMessage)` | Drain up to `maxMessages` in one call.  Blocks on the first message for `timeoutMs`, then drains queued messages with timeout=0.  Returns number of messages processed.  **Preferred for high-throughput consumers.** |
| **Stop** | `void Stop()` | Signal the consumer to stop.  Subsequent `Poll`/`PollBatch` calls return immediately.  Thread-safe (callable from signal handlers). |

#### Point-in-time reads

| Method | Signature | Description |
|--------|-----------|-------------|
| **ConsumeTopicFromBeginning** | `void ConsumeTopicFromBeginning(const string &sTopic, int hardTimeoutMs, const function<void(const void*, size_t, int64_t)> &onMessage)` | Read all messages from offset 0 until `PARTITION_EOF` or timeout.  Uses a dedicated temporary consumer.  Ideal for verification and manifest reads. |

#### Topic administration

| Method | Signature | Description |
|--------|-----------|-------------|
| **GetTopicMessageCounts** | `map<string, int64_t> GetTopicMessageCounts(const vector<string> &topics, int timeoutMs = 5000)` | Approximate message count per topic via broker watermarks.  Topics that fail to query map to `-1`. |
| **CreateTopic** | `bool CreateTopic(const string &sTopic, int numPartitions, int replicationFactor = 1, int timeoutMs = 8000)` | Idempotent topic creation.  More partitions = higher parallel throughput. |
| **DeleteSessionTopics** | `void DeleteSessionTopics(const SessionMeta &sm)` | Delete all topics belonging to a session (data + log + control). |
| **MarkSessionDeleted** | `void MarkSessionDeleted(const string &sChannel, const SessionMeta &sm)` | Write a tombstone record to `._registry` so future `DiscoverSessions()` skips this session. |
| **DeleteChannel** | `void DeleteChannel(const string &sChannel)` | Delete every topic starting with `<channel>.` plus the `._registry` itself. |

**Thread safety:** All methods must be called from a single thread, except
`Stop()` which is safe from any thread.

---

### On-wire types

Defined in `<kv8/Kv8Types.h>` with `#pragma pack(push, 2)`.

#### Kv8PacketHeader

```cpp
struct Kv8PacketHeader { uint32_t dwBits; };
```

Encodes type (5 bits), sub-type (5 bits), and payload size in the remaining
bits.  Use the extraction helpers:

```cpp
uint32_t Kv8GetExtType(Kv8PacketHeader h);
uint32_t Kv8GetExtSubtype(Kv8PacketHeader h);
uint32_t Kv8GetExtSize(Kv8PacketHeader h);
```

#### Kv8TelValue

The most common message payload (22 bytes, pack(2)):

```cpp
struct Kv8TelValue
{
    Kv8PacketHeader sCommonRaw;  // extension header
    uint16_t wID;         // counter ID
    uint16_t wSeqN;       // rolling sequence number (wraps at 65535)
    uint64_t qwTimer;     // producer QPC tick
    double   dbValue;     // sampled value
};
```

#### KafkaRegistryRecord

Variable-length record stored in `._registry` topics:

| Field | Type | Description |
|-------|------|-------------|
| `dwHash` | `uint32_t` | FNV-32 hash of the channel name |
| `wCounterID` | `uint16_t` | Counter ID; `0xFFFF` = group; `0xFFFE` = log; `0xFFFD` = deleted |
| `wFlags` | `uint16_t` | Bit 0: counter enabled at creation |
| `dbMin`, `dbAlarmMin`, `dbMax`, `dbAlarmMax` | `double` | Counter thresholds |
| `wNameLen`, `wTopicLen` | `uint16_t` | Byte lengths of the variable-length UTF-8 tail |
| `wVersion` | `uint16_t` | Must equal `KV8_REGISTRY_VERSION` (currently 2) |
| `qwTimerFrequency` | `uint64_t` | Producer QPC frequency (group records only) |
| `qwTimerValue` | `uint64_t` | Producer QPC anchor tick (group records only) |
| `dwTimeHi`, `dwTimeLo` | `uint32_t` | FILETIME anchor (group records only) |

The fixed header is followed by `[wNameLen bytes UTF-8][wTopicLen bytes UTF-8]`.

**Record types by `wCounterID`:**

| Constant | Value | Meaning |
|----------|-------|---------|
| `KV8_CID_GROUP` | `0xFFFF` | Group-level record (channel/telemetry info) |
| `KV8_CID_LOG` | `0xFFFE` | Session log-topic announcement |
| `KV8_CID_DELETED` | `0xFFFD` | Tombstone (session was deleted) |
| Other | 0..N | Individual counter definition |

---

### Data model types

#### CounterMeta

```cpp
struct CounterMeta
{
    std::string sName;
    uint16_t    wCounterID;
    uint16_t    wFlags;
    double      dbMin, dbAlarmMin, dbMax, dbAlarmMax;
    std::string sDataTopic;
};
```

#### SessionMeta

Aggregated session metadata built by `DiscoverSessions()`:

| Field | Type | Description |
|-------|------|-------------|
| `sSessionID` | `string` | e.g. `20260217T142301Z-A3F2-7B01` |
| `sSessionPrefix` | `string` | `<channel>.<sessionID>` |
| `sName` | `string` | Human-readable session name |
| `sLogTopic` | `string` | `<prefix>._log` |
| `sControlTopic` | `string` | `<prefix>._control` |
| `hashToGroup` | `map<uint32_t, string>` | Registry hash to group display name |
| `hashToCounters` | `map<uint32_t, vector<CounterMeta>>` | Hash to counter list |
| `dataTopics` | `set<string>` | All data topic names |
| `topicToCounter` | `map<string, CounterMeta>` | Per-counter topic layout lookup |
| `topicToGroupName` | `map<string, string>` | Topic to group display name |
| `topicToFrequency` | `map<string, uint64_t>` | QPC frequency per data topic |
| `topicToTimerValue` | `map<string, uint64_t>` | QPC anchor tick per data topic |
| `topicToTimeHi/Lo` | `map<string, uint32_t>` | FILETIME anchor per data topic |

---

### Utility helpers

All in `<kv8/Kv8Types.h>`, namespace `kv8`.

| Function | Description |
|----------|-------------|
| `Kv8SanitizeChannel(string)` | Replace `/` with `.` in a channel prefix.  Mirrors ClKafka producer-side sanitization. |
| `Kv8ExtractHashFromTopic(string_view, uint32_t&)` | Parse the 8-hex-digit channel ID from a `.d.XXXXXXXX` data topic name. |
| `Kv8FormatKafkaTimestamp(int64_t tsMs)` | Format a broker timestamp as ISO 8601 string. |

---

## 3. kv8util -- Application utilities

Everything in `<kv8util/*.h>`, namespace `kv8util`.

### Kv8Timer

`<kv8util/Kv8Timer.h>` -- cross-platform high-resolution timing.

| Function | Description |
|----------|-------------|
| `TimerInit()` | **Must be called once** at startup.  Captures QPC-to-wall-clock offset. |
| `TimerNow()` | Current high-resolution tick (QPC on Windows, `CLOCK_MONOTONIC` ns on Linux). |
| `TicksToNs(uint64_t ticks)` | Convert tick delta to nanoseconds. |
| `QpcToWallMs(uint64_t tick)` | Convert an absolute tick to Unix-epoch milliseconds.  Pure integer math, no syscall. |
| `WallMs()` | Current wall-clock milliseconds since Unix epoch. |

All functions are `static inline` for zero-overhead inlining on a hot path.

### Kv8BenchMsg

`<kv8util/Kv8BenchMsg.h>` -- 24-byte benchmark payload (pack(8)):

```cpp
struct BenchMsg
{
    uint64_t qSendTick;    // QPC tick at Produce() entry
    int64_t  tSendWallMs;  // wall-clock ms at Produce() entry
    uint64_t nSeq;         // 0-based sequence number
};
```

Embeds two clocks so the consumer can compute all latency splits
(dispatch, producer-to-broker, broker-to-consumer, end-to-end) without
cross-clock conversion.

### Kv8Stats

`<kv8util/Kv8Stats.h>` -- statistics computation and report writing.

| Function | Description |
|----------|-------------|
| `ComputeStats(vector<double> &v)` | Sort in-place, return `Stats` with min/max/mean/median/stddev/percentiles. |
| `PrintStatsBlock(FILE*, title, Stats&, unit, count)` | Write a formatted percentile table. |
| `PrintHistogram(FILE*, sorted, unit, nBuckets=20)` | Write an ASCII histogram with 20 buckets by default. |

The `Stats` struct contains: `dMin`, `dMax`, `dMean`, `dMedian`, `dStdDev`,
`dP50`, `dP75`, `dP90`, `dP95`, `dP99`, `dP999`.

### Kv8TopicUtils

`<kv8util/Kv8TopicUtils.h>` -- topic name generation.

| Function | Description |
|----------|-------------|
| `GenerateTopicName(prefix)` | `"<prefix>.YYYYMMDDTHHMMSSz-XXXXXX"` with a sub-second hex suffix for uniqueness. |
| `NowUTC()` | Current UTC time as `"YYYY-MM-DDTHH:MM:SSZ"`. |

```cpp
struct ProgressRow
{
    int     tSec;
    int64_t nSent, nRecv, nQueueFull;
    double  sendRateMps, recvRateMps;
};
```

### Kv8AppUtils

`<kv8util/Kv8AppUtils.h>` -- cross-platform application building blocks.

#### AppSignal

Cooperative shutdown via Ctrl+C / SIGINT / SIGTERM.  Meyer's singleton -- no
file-scope globals.

```cpp
kv8util::AppSignal::Install();              // once in main()
while (kv8util::AppSignal::IsRunning())     // main loop condition
    { /* ... */ }
kv8util::AppSignal::RequestStop();          // programmatic stop
```

| Method | Description |
|--------|-------------|
| `Install()` | Register OS signal handlers.  Call once before the main loop. |
| `IsRunning()` | `true` while the application should keep running. |
| `RequestStop()` | Request cooperative stop.  Thread-safe. |

#### CheckEscKey

```cpp
bool CheckEscKey();   // true when Esc (0x1B) detected on stdin
```

Non-blocking.  Call from one thread only.

#### BuildKv8Config

```cpp
kv8::Kv8Config BuildKv8Config(
    const string &sBrokers,
    const string &sSecurityProto,
    const string &sSaslMechanism,
    const string &sUser,
    const string &sPass,
    const string &sGroupID = "");
```

One-liner `Kv8Config` construction from CLI parameters.

---

## 4. Usage examples

### 4.1 Minimal producer

Write 1000 telemetry samples to a topic.

```cpp
#include <kv8/IKv8Producer.h>
#include <kv8/Kv8Types.h>
#include <kv8util/Kv8Timer.h>
#include <kv8util/Kv8TopicUtils.h>
#include <cstdio>

int main()
{
    kv8util::TimerInit();

    kv8::Kv8Config cfg;
    cfg.sBrokers = "localhost:19092";
    cfg.sSecurityProto = "sasl_plaintext";
    cfg.sSaslMechanism = "PLAIN";
    cfg.sUser = "kv8producer";
    cfg.sPass = "kv8secret";

    auto producer = kv8::IKv8Producer::Create(cfg);
    if (!producer) {
        fprintf(stderr, "Failed to create producer\n");
        return 1;
    }

    std::string topic = kv8util::GenerateTopicName("myapp");
    printf("Publishing to %s\n", topic.c_str());

    for (int i = 0; i < 1000; ++i)
    {
        kv8::Kv8TelValue val{};
        val.wID      = 0;
        val.wSeqN    = (uint16_t)(i & 0xFFFF);
        val.qwTimer  = kv8util::TimerNow();
        val.dbValue  = (double)i * 0.1;

        uint32_t key = 0; // partition key
        if (!producer->Produce(topic, &val, sizeof(val), &key, sizeof(key)))
        {
            producer->Flush(100); // back-pressure: flush and retry
            producer->Produce(topic, &val, sizeof(val), &key, sizeof(key));
        }

        // Flush periodically to avoid unbounded queue growth.
        if ((i & 0xFFF) == 0xFFF)
            producer->Flush(0);
    }

    producer->Flush(30000); // wait for all messages to reach the broker
    printf("Done. Delivered %" PRId64 " messages\n",
           producer->GetDeliverySuccess());
    return 0;
}
```

**Key points:**
- `Produce()` returns `false` when the internal queue is full; back off with
  `Flush(100)` then retry.
- Call `Flush(0)` periodically to pump delivery reports without blocking.
- Call `Flush(30000)` at the end to confirm all messages are delivered.

---

### 4.2 Minimal consumer

Subscribe to a topic and print every telemetry value.

```cpp
#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>
#include <kv8util/Kv8AppUtils.h>
#include <cstdio>
#include <cstring>

int main()
{
    kv8util::AppSignal::Install();

    kv8::Kv8Config cfg;
    cfg.sBrokers = "localhost:19092";
    cfg.sSecurityProto = "sasl_plaintext";
    cfg.sSaslMechanism = "PLAIN";
    cfg.sUser = "kv8producer";
    cfg.sPass = "kv8secret";

    auto consumer = kv8::IKv8Consumer::Create(cfg);
    if (!consumer) {
        fprintf(stderr, "Failed to create consumer\n");
        return 1;
    }

    consumer->Subscribe("myapp.20260228T120000Z-AABBCC");

    while (kv8util::AppSignal::IsRunning())
    {
        consumer->Poll(200,
            [](std::string_view topic,
               const void *pPayload, size_t cbPayload,
               int64_t tsKafkaMs)
            {
                if (cbPayload < sizeof(kv8::Kv8TelValue)) return;

                kv8::Kv8TelValue val;
                memcpy(&val, pPayload, sizeof(val));

                printf("[%s] ts=%s  counter=%u  seq=%u  value=%.6f\n",
                       std::string(topic).c_str(),
                       kv8::Kv8FormatKafkaTimestamp(tsKafkaMs).c_str(),
                       val.wID, val.wSeqN, val.dbValue);
            });

        if (kv8util::CheckEscKey())
            kv8util::AppSignal::RequestStop();
    }

    consumer->Stop();
    return 0;
}
```

**Key points:**
- `Poll(200)` blocks at most 200 ms -- responsive to shutdown signals.
- Do not call `Subscribe()` inside the `Poll` callback.  Collect topic names
  and subscribe after `Poll()` returns.
- `AppSignal` + `CheckEscKey` provide graceful shutdown on Ctrl+C or Esc.

---

### 4.3 Session discovery and replay

Discover all sessions in a channel, pick one, subscribe to its data topics.

```cpp
#include <kv8/IKv8Consumer.h>
#include <kv8util/Kv8AppUtils.h>
#include <cstdio>

int main()
{
    kv8util::AppSignal::Install();

    auto cfg = kv8util::BuildKv8Config(
        "localhost:19092", "sasl_plaintext", "PLAIN",
        "kv8producer", "kv8secret");

    auto consumer = kv8::IKv8Consumer::Create(cfg);

    // Step 1: List available channels
    auto channels = consumer->ListChannels(10000);
    printf("Found %zu channels:\n", channels.size());
    for (auto &ch : channels)
        printf("  %s\n", ch.c_str());

    if (channels.empty()) return 0;

    // Step 2: Discover sessions in the first channel
    std::string channel = channels[0];
    auto sessions = consumer->DiscoverSessions(channel);

    printf("Found %zu sessions in '%s':\n", sessions.size(), channel.c_str());
    for (auto &[prefix, sm] : sessions)
    {
        printf("  %-40s  counters=%zu  topics=%zu\n",
               prefix.c_str(),
               sm.topicToCounter.size(),
               sm.dataTopics.size());
    }

    if (sessions.empty()) return 0;

    // Step 3: Subscribe to the first session's data + control topics
    auto &[prefix, session] = *sessions.begin();
    for (auto &t : session.dataTopics)
        consumer->Subscribe(t);
    consumer->Subscribe(session.sControlTopic);

    printf("Subscribed to %zu topics. Streaming...\n",
           session.dataTopics.size() + 1);

    // Step 4: Replay loop
    while (kv8util::AppSignal::IsRunning())
    {
        consumer->Poll(200,
            [&](std::string_view topic,
                const void *pPayload, size_t cbPayload,
                int64_t tsKafkaMs)
            {
                // Look up counter metadata from SessionMeta
                std::string sTopic(topic);
                auto it = session.topicToCounter.find(sTopic);
                const char *name = (it != session.topicToCounter.end())
                                    ? it->second.sName.c_str() : "?";

                if (cbPayload >= sizeof(kv8::Kv8TelValue))
                {
                    kv8::Kv8TelValue val;
                    memcpy(&val, pPayload, sizeof(val));
                    printf("[%s] %s  seq=%u  val=%.4f\n",
                           name,
                           kv8::Kv8FormatKafkaTimestamp(tsKafkaMs).c_str(),
                           val.wSeqN, val.dbValue);
                }
            });

        if (kv8util::CheckEscKey())
            kv8util::AppSignal::RequestStop();
    }

    consumer->Stop();
    return 0;
}
```

**Key points:**
- `DiscoverSessions()` uses a temporary internal consumer -- it does not
  disturb the active subscription.
- `SessionMeta.topicToCounter` maps each data topic to its `CounterMeta`,
  giving you the counter name, ID, and thresholds without extra lookups.

---

### 4.4 High-throughput benchmark pattern

For maximum ingest rate, use `PollBatch()` in a dedicated consumer thread.

```cpp
#include <kv8/IKv8Producer.h>
#include <kv8/IKv8Consumer.h>
#include <kv8util/Kv8Timer.h>
#include <kv8util/Kv8BenchMsg.h>
#include <atomic>
#include <thread>
#include <cstdio>

void ConsumerThread(kv8::Kv8Config cfg,
                    const std::string &topic,
                    std::atomic<bool> &stop,
                    std::atomic<int64_t> &nRecv)
{
    auto consumer = kv8::IKv8Consumer::Create(cfg);
    consumer->Subscribe(topic);

    while (!stop.load(std::memory_order_relaxed))
    {
        // Drain up to 50,000 messages per call.
        // Timeout 5 ms on the first message; 0 for the rest.
        consumer->PollBatch(50000, 5,
            [&](std::string_view, const void *pPayload,
                size_t cbPayload, int64_t tsKafkaMs)
            {
                nRecv.fetch_add(1, std::memory_order_relaxed);
                // Process message here ...
            });
    }

    // Final drain: keep draining for up to 2 seconds.
    for (int i = 0; i < 20; ++i)
    {
        int n = consumer->PollBatch(50000, 100,
            [&](std::string_view, const void*, size_t, int64_t)
            { nRecv.fetch_add(1, std::memory_order_relaxed); });
        if (n == 0) break;
    }

    consumer->Stop();
}

int main()
{
    kv8util::TimerInit();

    auto cfg = kv8util::BuildKv8Config(
        "localhost:19092", "sasl_plaintext", "PLAIN",
        "kv8producer", "kv8secret");

    auto producer = kv8::IKv8Producer::Create(cfg);
    std::string topic = kv8util::GenerateTopicName("bench");

    std::atomic<bool> stop{false};
    std::atomic<int64_t> nRecv{0};

    std::thread cThread(ConsumerThread, cfg, topic, std::ref(stop),
                        std::ref(nRecv));

    // Allow consumer partition assignment to settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const int N = 100000;
    for (int i = 0; i < N; ++i)
    {
        kv8util::BenchMsg msg;
        msg.qSendTick   = kv8util::TimerNow();
        msg.tSendWallMs = kv8util::QpcToWallMs(msg.qSendTick);
        msg.nSeq        = (uint64_t)i;

        while (!producer->Produce(topic, &msg, sizeof(msg),
                                  &msg.nSeq, sizeof(msg.nSeq)))
            producer->Flush(100);

        if ((i & 0xFFF) == 0xFFF)
            producer->Flush(0);
    }

    producer->Flush(30000);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    stop.store(true);
    cThread.join();

    printf("Produced: %d  Received: %" PRId64 "\n", N, nRecv.load());
    return 0;
}
```

**Key points:**
- `PollBatch(50000, 5, ...)` amortizes per-call overhead across thousands of
  messages.  This is critical for keeping up with a fast producer.
- Dedicated consumer thread avoids blocking the producer.
- Short first-message timeout (5 ms) keeps latency low; subsequent messages
  drain at timeout=0 (no waiting).
- Final drain loop after producer completes catches in-flight messages.

---

### 4.5 Point-in-time verification

Read all messages from a topic and verify sequence integrity.

```cpp
#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    auto cfg = kv8util::BuildKv8Config(
        "localhost:19092", "sasl_plaintext", "PLAIN",
        "kv8producer", "kv8secret");

    auto consumer = kv8::IKv8Consumer::Create(cfg);

    int64_t nTotal = 0;
    int64_t nGaps  = 0;
    uint16_t lastSeq = 0;
    bool first = true;

    // Read the entire topic from offset 0, with a 30-second hard timeout.
    consumer->ConsumeTopicFromBeginning("myapp.data.topic", 30000,
        [&](const void *pPayload, size_t cbPayload, int64_t /*tsMs*/)
        {
            if (cbPayload < sizeof(kv8::Kv8TelValue)) return;

            kv8::Kv8TelValue val;
            memcpy(&val, pPayload, sizeof(val));

            if (!first)
            {
                uint16_t expected = (uint16_t)(lastSeq + 1);
                if (val.wSeqN != expected)
                    ++nGaps;
            }
            first = false;
            lastSeq = val.wSeqN;
            ++nTotal;
        });

    printf("Read %" PRId64 " messages, %" PRId64 " sequence gaps\n",
           nTotal, nGaps);
    return 0;
}
```

**Key points:**
- `ConsumeTopicFromBeginning()` opens a temporary consumer internally, reads
  until `PARTITION_EOF`, and returns.  No subscription state is modified.
- Pair with `GetTopicMessageCounts()` to know the expected count in advance.

---

### 4.6 Maintenance operations

List channels, inspect sessions, delete stale data.

```cpp
#include <kv8/IKv8Consumer.h>
#include <cstdio>

int main()
{
    auto cfg = kv8util::BuildKv8Config(
        "localhost:19092", "sasl_plaintext", "PLAIN",
        "kv8producer", "kv8secret");

    auto consumer = kv8::IKv8Consumer::Create(cfg);

    // List all Kv8 channels on the broker
    auto channels = consumer->ListChannels(10000);
    for (auto &ch : channels)
        printf("Channel: %s\n", ch.c_str());

    // Inspect a specific channel
    auto sessions = consumer->DiscoverSessions("kv8.myapp");
    for (auto &[prefix, sm] : sessions)
    {
        printf("\nSession: %s\n", prefix.c_str());
        printf("  Name     : %s\n", sm.sName.c_str());
        printf("  Log topic: %s\n", sm.sLogTopic.c_str());
        printf("  Data topics: %zu\n", sm.dataTopics.size());

        // Get message counts for all data topics
        std::vector<std::string> topics(sm.dataTopics.begin(),
                                        sm.dataTopics.end());
        auto counts = consumer->GetTopicMessageCounts(topics, 5000);
        for (auto &[t, n] : counts)
            printf("    %-50s  %" PRId64 " msgs\n", t.c_str(), n);
    }

    // Delete a session (topics + tombstone)
    if (!sessions.empty())
    {
        auto &[prefix, sm] = *sessions.begin();
        consumer->DeleteSessionTopics(sm);
        consumer->MarkSessionDeleted("kv8.myapp", sm);
        printf("\nDeleted session %s\n", prefix.c_str());
    }

    // Delete an entire channel (all sessions, all topics, registry)
    // consumer->DeleteChannel("kv8.myapp");

    return 0;
}
```

---

## 5. Consumer best practices -- zero-gap reception

High-frequency telemetry (sub-millisecond sampling) demands careful consumer
tuning.  These practices have been validated on the kv8bench and kv8util_test
pipelines at 100K+ messages per run with zero gaps.

### Use PollBatch, not Poll

`Poll()` processes one message per call.  At 100K+ msg/s the per-call overhead
(function dispatch, rdkafka queue lock) becomes the bottleneck.

```cpp
// BAD -- one message per call, high overhead at scale
consumer->Poll(200, callback);

// GOOD -- drain up to 50K messages per call
consumer->PollBatch(50000, 5, callback);
```

`PollBatch()` blocks on the first message for `timeoutMs` then drains all
queued messages with timeout=0.  This matches the rdkafka fetch cycle and
minimizes wasted CPU on empty polls.

### Dedicate a thread to the consumer

Producing and consuming on the same thread serializes two inherently concurrent
operations.  Run the consumer in its own thread:

```
Main thread:  Produce() + periodic Flush(0)
Consumer thread:  PollBatch() loop
Shared state:  atomic counters / bitmap only
```

### Warm up before measuring

Kafka consumer group rebalancing and initial partition assignment introduce a
start-up delay of 1-3 seconds.  Before starting the real data flow:

1. Send a **warmup sentinel** message (e.g. `nSeq = UINT64_MAX`).
2. Wait until the consumer receives it.
3. Reset counters and begin the production run.

This ensures the consumer has its partition assignment before any real data
arrives.

### Flush the producer periodically

librdkafka batches messages internally.  Without periodic `Flush(0)` calls
the internal queue can grow unbounded and eventually return `false` from
`Produce()`:

```cpp
if ((i & 0xFFF) == 0xFFF)
    producer->Flush(0);   // pump delivery reports, non-blocking
```

### Drain after production completes

After the producer's final `Flush(30000)` some messages may still be in-flight
between the broker and the consumer.  Allow the consumer to drain:

```cpp
producer->Flush(30000);  // all messages confirmed on broker
std::this_thread::sleep_for(std::chrono::seconds(3)); // let consumer catch up
bConsumerStop.store(true);
consumerThread.join();
```

Alternatively, keep draining in a loop until `PollBatch()` returns 0 for
several consecutive calls.

### Verify with a second pass

For critical workloads, run a post-production verification using
`ConsumeTopicFromBeginning()`.  This reads the topic from offset 0 after all
messages have landed on the broker, catching any gaps the real-time consumer
may have missed during rebalance or start-up:

```cpp
auto verifier = kv8::IKv8Consumer::Create(cfg);
verifier->ConsumeTopicFromBeginning(topic, 30000,
    [&](const void *p, size_t len, int64_t ts) { checkSequence(p, len); });
```

### Use per-message sequence numbers

The `Kv8TelValue.wSeqN` field wraps at 65535.  For longer runs, embed a
full 64-bit sequence in the payload (as `BenchMsg.nSeq` does) and track
received sequences in a bitmap:

```cpp
auto pSeen = std::unique_ptr<uint8_t[]>(new uint8_t[nCount]());
// In callback:
if (msg.nSeq < nCount) pSeen[msg.nSeq] = 1;
// After drain:
for (uint64_t i = 0; i < nCount; ++i)
    if (!pSeen[i]) ++nMissing;
```

### Pre-create topics with multiple partitions

More partitions = higher consumer throughput (parallel fetch streams).  Use
`CreateTopic()` before producing:

```cpp
consumer->CreateTopic(topic, /*partitions=*/4, /*replicas=*/1, 8000);
```

### Keep allocations off the hot path

Pre-allocate all tracking structures (bitmaps, latency vectors, buffers)
before the produce/consume loop begins.  `BenchMsg` at 24 bytes fits in a
single cache line.  Avoid `std::string` or heap allocation inside the poll
callback.

### Summary checklist

| Practice | Why |
|----------|-----|
| `PollBatch(50000, 5)` | Amortize per-call overhead; match rdkafka fetch cycle |
| Dedicated consumer thread | Decouple producer and consumer throughput |
| Warmup sentinel | Ensure partition assignment before real data |
| Periodic `Flush(0)` | Prevent producer queue overflow |
| Post-production drain | Catch in-flight messages after final flush |
| Second-pass verification | Guarantee zero gaps with `ConsumeTopicFromBeginning` |
| 64-bit sequence + bitmap | Detect gaps, duplicates, reorderings |
| Pre-create topics | Control partition count for throughput |
| Zero-allocation hot path | Keep latency predictable and low |

---

## 6. kv8log -- Application telemetry producer

`kv8log` provides a zero-overhead C++ wrapper around the `kv8log_runtime.dll` /
`.so` shared library.  Applications link against `kv8log_facade` (a thin stub)
and load the real implementation at runtime via `dlopen` / `LoadLibrary`.

### 6.1 Macros (`<kv8log/KV8_Log.h>`)

All user-facing entry points are macros.  When `KV8_LOG_ENABLE` is **not**
defined, every macro expands to `((void)0)` -- zero binary footprint.

| Macro | Signature | Description |
|-------|-----------|-------------|
| `KV8_SESSION_OPEN(var, chan, sesId, cfg)` | `kv8::Kv8Config cfg` | Open a session.  Returns `kv8log_h` handle stored in `var`. |
| `KV8_SESSION_CLOSE(var)` | -- | Flush and close; sets `var = nullptr`. |
| `KV8_TEL_DEFINE(var, name, min, max)` | -- | Declare a `Counter` object with display range. |
| `KV8_TEL_REGISTER(var, session)` | -- | Register counter with Kafka; must be called before `ADD`. |
| `KV8_TEL_ADD(var, value)` | `double value` | Record a sample using the internal monotonic clock. |
| `KV8_TEL_ADD_TS(var, value, ts_ns)` | `double value, uint64_t ts_ns` | Record a sample with a caller-supplied Unix-epoch nanosecond timestamp. |
| `KV8_TEL_ENABLE(var)` | -- | Re-enable a previously disabled counter (no-op if already enabled). |
| `KV8_TEL_DISABLE(var)` | -- | Suppress all subsequent `KV8_TEL_ADD` calls for this counter until re-enabled. |
| `KV8_SESSION_FLUSH(var, ms)` | `int ms` | Block until all queued messages are delivered or `ms` elapses. |

### 6.2 Counter enable/disable

Counters can be toggled at runtime from two directions:

**From the application:**
```cpp
KV8_TEL_DISABLE(myCpuCounter);   // stop producing samples
// ... later ...
KV8_TEL_ENABLE(myCpuCounter);    // resume
```

**From kv8scope (remote):**
The kv8scope UI "E" checkbox writes a JSON message to the `._ctl` topic.
The producer's background control consumer thread picks it up and updates the
per-counter `aEnabled` flag with `memory_order_relaxed`.  The gate in
`kv8log_add()` checks this flag before producing each sample:

```cpp
if (!s->aEnabled[id].load(std::memory_order_relaxed)) return;
```

Both directions write the new state back to the `._ctl` topic so kv8scope
always stays in sync.

### 6.3 Control topic wire format

Topic name: `<sessionPrefix>._ctl`

Each record is a JSON text message (UTF-8, no NUL terminator):

```json
{"v":1,"cmd":"ctr_state","wid":42,"enabled":true,"ts":1715000000000}
```

| Field | Type | Description |
|-------|------|-------------|
| `v` | int | Schema version, currently `1`. |
| `cmd` | string | Always `"ctr_state"`. |
| `wid` | uint16 | Counter wire ID (0..1023). |
| `enabled` | bool | `true` = counter is active; `false` = suppressed. |
| `ts` | int64 | Unix-epoch milliseconds when the change was made. |

kv8scope replays the entire `._ctl` topic from offset 0 at session open time to
restore the enabled state from prior sessions.

### 6.4 kv8log_runtime plain-C ABI

The shared library exports the following symbols:

| Symbol | Signature | Description |
|--------|-----------|-------------|
| `kv8log_open` | `(char* chan, char* sesId, Kv8Config cfg) -> void*` | Open session; allocates `Kv8LogSession`. |
| `kv8log_close` | `(void* h)` | Stop ctl thread, flush, free session. |
| `kv8log_register_counter` | `(void* h, uint16_t* out_id, char* name, double min, double max) -> int` | Register counter and obtain wire ID. |
| `kv8log_add` | `(void* h, uint16_t id, double value)` | Record sample (internal clock). Skipped if counter disabled. |
| `kv8log_add_ts` | `(void* h, uint16_t id, double value, uint64_t ts_ns)` | Record sample (caller timestamp). Skipped if counter disabled. |
| `kv8log_flush` | `(void* h, int timeout_ms)` | Flush the producer. |
| `kv8log_set_counter_enabled` | `(void* h, uint16_t id, int bEnabled)` | Enable (1) or disable (0) a counter; publishes to `._ctl`. |
| `kv8log_monotonic_to_ns` | `(void* h, uint64_t ticks) -> uint64_t` | Convert internal ticks to nanoseconds. |
