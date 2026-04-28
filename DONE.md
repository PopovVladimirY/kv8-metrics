# Kv8 Project -- Status Report

Date: 2026-04-28

This document summarises what has been built and where the project stands.
It is updated after every major phase.

---

## 1. Executive Summary

The Kv8 telemetry-over-Kafka stack is feature-complete for its planned
scope. The core library, all CLI tools, the oscilloscope application, and
the 3D path viewer are built, tested, and operational.

**Three telemetry pillars are now in place:**

1. **Scalar telemetry** -- per-counter numeric feeds (`KV8_TEL_ADD`).
2. **Structured telemetry (UDT)** -- per-feed compound records with a
   user-defined schema (`KV8_UDT_ADD`).
3. **Trace logging** -- text log records with severity levels, source
   location, thread/CPU stamps and per-call-site registry deduplication
   (`KV8_LOG_INFO`, `KV8_LOGF_WARN`, ...). Visualised in kv8scope as a
   dockable LogPanel synchronised with the waveform timeline.

The LOG feature was delivered in six phases (L1..L6, see
[Kv8Planning/LOG_PHASES.md](Kv8Planning/LOG_PHASES.md)) and is fully
validated by unit, integration and benchmark tests.

---

## 2. Completed Work

### 2.1 Infrastructure

| Item | Status |
|------|--------|
| Docker Compose Kafka broker (KRaft, single node, SASL/PLAIN) | DONE |
| Volume mapping for Kafka data and config | DONE |
| Local network accessibility (multiple hosts) | DONE |
| docker/ and docker-compose.yml at root | DONE |

---

### 2.2 Core Libraries

#### libkv8 -- Kafka abstraction layer

| Component | File(s) | Status |
|-----------|---------|--------|
| `Kv8Config` -- broker, SASL, heartbeat config | `Kv8Types.h` | DONE |
| `IKv8Producer` -- abstract producer with factory | `IKv8Producer.h`, `Kv8Producer.cpp` | DONE |
| `IKv8Consumer` -- abstract consumer with factory | `IKv8Consumer.h`, `Kv8Consumer.cpp` | DONE |
| On-wire binary structs: `Kv8TelValue`, `Kv8PacketHeader`, `Kv8UdtSample` | `Kv8Types.h` | DONE |
| Session metadata: `SessionMeta`, `CounterMeta`, `GroupMeta` | `Kv8Types.h` | DONE |
| `ListChannels()`, `DiscoverSessions()`, `ConsumeTopicFromBeginning()` | consumer impl | DONE |
| Per-counter topic layout: `<ch>._registry`, `<ch>.<sid>.d.<HASH>.<CCCC>` | impl | DONE |
| Heartbeat topic: `<ch>.<sid>.hb` with liveness records | `Kv8Types.h`, producer impl | DONE |
| Control topic: `<ch>.<sid>._ctl` (enable/disable JSON commands) | impl | DONE |
| UDT schema records: `KV8_CID_SCHEMA`, `KV8_CID_UDT_FEED` | `Kv8Types.h` | DONE |
| `UdtSchemaParser` -- schema JSON to field layout | `UdtSchemaParser.h/.cpp` | DONE |

#### kv8util -- Application utilities

| Component | Status |
|-----------|--------|
| `Kv8Timer` -- high-resolution wall clock + monotonic timer | DONE |
| `Kv8Stats` -- running min/max/avg/throughput accumulators | DONE |
| `Kv8TopicUtils` -- topic naming helpers, FNV-32 hash | DONE |
| `Kv8AppUtils` -- signal handling, Ctrl-C, clean exit | DONE |
| `Kv8BenchMsg` -- benchmark message struct | DONE |

#### kv8log -- Telemetry producer facade + runtime

This is the instrumentation library that user applications link.

| Component | File(s) | Status |
|-----------|---------|--------|
| `KV8_Log.h` -- single user-facing header; all macros compile out by default | `include/kv8log/KV8_Log.h` | DONE |
| `KV8_CHANNEL`, `KV8_TEL`, `KV8_TEL_ADD`, `KV8_TEL_ADD_TS` macros | `KV8_Log.h` | DONE |
| `KV8_TEL_FLUSH`, `KV8_TEL_ENABLE`, `KV8_TEL_DISABLE` macros | `KV8_Log.h` | DONE |
| `KV8_LOG_CONFIGURE` -- explicit broker/channel override | `KV8_Log.h` | DONE |
| `KV8_MONO_TO_NS` -- monotonic clock to Unix epoch conversion | `KV8_Log.h` | DONE |
| `Channel` -- lazy-init Kafka producer session per channel | `Channel.h/.cpp` | DONE |
| `Counter` -- lazy registration, cached vtable pointers, lock-free Add() | `Counter.h/.cpp` | DONE |
| `DefaultChannel` -- per-exe default channel derived from exe name | `DefaultChannel.cpp` | DONE |
| `Runtime` -- shared library loader (dlopen/LoadLibrary), vtable resolution | `Runtime.h/.cpp` | DONE |
| Config resolution: explicit > argv > env > compiled defaults | `Runtime.cpp` | DONE |
| `kv8log_facade` -- static lib; user app links this, no rdkafka dep | `CMakeLists.txt` | DONE |
| `kv8log_runtime` -- shared lib; loaded lazily at first use | `lib/kv8log_impl.cpp` | DONE |
| Timer anchor: mono+wall captured at session open for timestamp conversion | `kv8log_impl.cpp` | DONE |
| Control topic consumer thread inside runtime (enable/disable reception) | `kv8log_impl.cpp` | DONE |
| `KV8_UDT.h` -- User Defined Type telemetry macros | `include/kv8log/KV8_UDT.h` | DONE |
| `UdtFeed<T>` -- lazy registration, hot-path Add() for structured types | `UdtFeed.h` | DONE |
| UDT macro system: `KV8_UDT_DEFINE`, `KV8_UDT_DEFINE_NS`, `KV8_UDT_FEED`, `KV8_UDT_ADD` | `KV8_UDT.h` | DONE |
| UDT P1-P6 implementation plan executed to completion | `KV8_UDT_IMPL_PLAN.md` | DONE |
| `KV8_LOG`, `KV8_LOGF` and severity wrappers (DEBUG / INFO / WARN / ERROR / FATAL) | `KV8_Log.h` | DONE |
| Per-call-site `std::atomic<uint32_t>` cache; first-call registration only | `KV8_Log.h`, `Runtime.cpp` | DONE |
| Log call-site registry: `KafkaRegistryRecord` with `wCounterID = KV8_CID_LOG_SITE` | `Kv8Types.h`, `kv8log_impl.cpp` | DONE |
| Log data records: `Kv8LogRecord` (28-byte header + UTF-8 payload) on `<ch>.<sid>._log` | `Kv8Types.h`, `kv8log_impl.cpp` | DONE |
| `Kv8DecodeLogRecord` / `Kv8DecodeLogSiteTail` -- consumer helpers | `Kv8Types.h` | DONE |
| LOG phases L1-L6 implemented and tested end-to-end | `Kv8Planning/LOG_PHASES.md` | DONE |

**Trace log details:**
- Hot path: `KV8_LOGF_INFO("...", args)` formats into a 4096-byte stack
  buffer and dispatches via the lazy runtime.  No heap allocation, no
  syscalls in the typical path.
- Source location (`__FILE__`, `__LINE__`, `__FUNCTION__`) and the format
  string travel only **once**, in a registry record keyed by call-site
  hash. Data records carry only `dwSiteHash` (4 bytes) plus payload.
- Wire constants: `KV8_LOG_MAGIC = 0x4B563854 ("KV8T")`,
  `KV8_LOG_MAX_PAYLOAD = 4095`, severity codes `Debug=0..Fatal=4`.
- Compile-out: when `KV8_LOG_ENABLE` is undefined every `KV8_LOG*` macro
  expands to `((void)0)` -- zero overhead, zero symbol references.

**NOT present in kv8log:**

- Packed typed-args payload format (`Kv8LogArgType` enum is reserved in
  the wire schema for L2+; current path emits pre-formatted UTF-8 text).

---

### 2.3 Tools

| Binary | Purpose | Status |
|--------|---------|--------|
| `kv8cli` | Stream telemetry/log records from Kafka to stdout (human-readable) | DONE |
| `kv8bench` | End-to-end latency and throughput benchmark | DONE |
| `kv8bench_log` | LOG hot-path dispatch latency and throughput benchmark | DONE |
| `kv8feeder` | Continuous live telemetry producer: three scalar feeds + UDT feeds + 5-level log emitter | DONE |
| `kv8maint` | Channel/session admin: list, inspect, delete sessions and channels | DONE |
| `kv8probe` | Deterministic synthetic sample producer for verification | DONE |
| `kv8verify` | Timestamp integrity and sequence continuity checker | DONE |

kv8feeder produces:
- `Numbers/Counter` -- 0..1023 ramp, configurable sample rate
- `Physics/Waldhausen` -- damped oscillation
- `Scope/Phases` -- random-interval cyclogram feed
- UDT feeds via `UdtFeeds.cpp` (kv8feeder example)

All tools use `kv8` and `kv8util` exclusively; no direct librdkafka exposure.

---

### 2.4 kv8scope -- Software Oscilloscope

All KV8_WORK_PLAN.md phases (P1 through P4) are implemented.

#### P1 -- Skeleton + session list

| Item | Status |
|------|--------|
| CMake integration, FetchContent (ImGui, ImPlot, GLFW, nlohmann/json) | DONE |
| GLFW + ImGui/OpenGL application shell with DockSpace | DONE |
| `ConfigStore` -- JSON config load/save, broker/auth/theme/font settings | DONE |
| `ThemeManager` -- 11 themes: Night Sky, Sahara Desert, Ocean Deep, Inferno, Classic Dark, Morning Light, Blizzard, Neon Bubble Gum, Psychedelic Delirium, Classic Light, Paper White | DONE |
| `SessionManager` -- background discovery thread, liveness detection (Live/GoingOffline/Offline/Historical) | DONE |
| Heartbeat-based liveness (T_live, T_dead thresholds) | DONE |
| `SessionListPanel` -- ImGui tree: Channel > Session with Online/Offline dots | DONE |
| Multi-column table: status, session ID, name, counter count, started time | DONE |
| Context menu: Open, Inspect, Delete session (yes/no), Delete channel (yes/no) | DONE |
| Multiple session selection with Shift/Ctrl click | DONE |
| `ConnectionDialog` -- modal popup, Test Connection button | DONE |

#### P2 -- Waveform rendering

| Item | Status |
|------|--------|
| `ScopeWindow` -- per-session ImGui window, toolbar ([S]/[R]/[C] modes, play/pause/live) | DONE |
| `ConsumerThread` -- background IKv8Consumer thread, per-counter ring buffer dispatch | DONE |
| `SpscRingBuffer<TelemetrySample>` -- cache-line-aligned head/tail, lock-free SPSC | DONE |
| `TimeConverter` -- QPC/monotonic to wall-clock UTC via session anchor | DONE |
| `WaveformRenderer` -- ImPlot-based strip charts, per-counter master buffers | DONE |
| Simple / Range / Cyclogram visualization modes | DONE |
| Colorblind-safe palette (Wong 8-color + per-theme variants) | DONE |
| Auto-scroll for live sessions; pause/resume | DONE |
| Offline session playback from offset 0 | DONE |
| Mouse interaction: scroll zoom, left-drag pan, right-drag box zoom, double-click reset | DONE |
| Status bar: ingest rate, lag, memory, FPS, Online/Offline badge | DONE |
| Crossbar: cursor-following vertical/horizontal hair with value tooltip | DONE |

#### P3 -- Counter tree + feed control

| Item | Status |
|------|--------|
| `CounterTree` -- two-level tree (group > counter), multi-column ImGui table | DONE |
| V (Visible) checkbox -- hides/shows trace, group-level propagation | DONE |
| E (Enabled) checkbox -- live sessions only; Kafka ._ctl command producer | DONE |
| Color swatch (12x12 px) + dim on disabled | DONE |
| Bidirectional highlight: click tree row highlights graph and vice versa | DONE |
| Y-axis custom range via right-click context menu | DONE |
| Trace color customization via ImGui color picker; reset to palette | DONE |
| Per-counter visualization mode selection | DONE |
| Show All / Hide All toolbar buttons | DONE |

#### P4 -- Statistics

| Item | Status |
|------|--------|
| `StatsEngine` -- full-duration running accumulators (min/max/mean/count) | DONE |
| `P2Median` -- Jain & Chlamtac P-square online incremental median estimator | DONE |
| `StatsPanel` -- dockable statistics window (Ctrl+I), visible-window + full-duration columns | DONE |
| Visible-window stats per counter (min/max/avg/med/count, recomputed on zoom/pan) | DONE |
| Stats columns in CounterTree (N, Nv, Vmin, Vmax, Vavg, Vmed, Min, Max, Avg) | DONE |

#### Additional features beyond P4

| Item | Status |
|------|--------|
| `AnnotationStore` -- timeline annotations with Kafka persistence (._annotations topic) | DONE |
| Soft-delete markers: "deleted" annotations stored as new Kafka records | DONE |
| Deleted annotations shown with transparency when toggled | DONE |
| `FontManager` -- 6 face options x 5 sizes, atlas bake + runtime reload | DONE |
| Multi-panel layout: one graph per counter, stacked vertically (max 8 visible) | DONE |
| Counters sharing a graph when count > 8 (greedy Y-range overlap binning) | DONE |
| Single X-axis at the bottom of all stacked panels (ImPlotSubplotFlags_LinkAllX) | DONE |
| Counter label at top-left of each graph, colored to match trace | DONE |
| Relative time scale axis at top of graphs (s/ms/us/ns/m/h/d ticks) | DONE |
| Hierarchical counter list (collapsible groups per '/' in counter name) | DONE |
| Sample dots on waveform: hollow circles (single sample), solid (multiple per pixel) | DONE |
| kv8scope.json config persisted alongside session in Kafka topic | DONE |
| Live session: smooth >10 Hz refresh rate | DONE |
| Live window: max 15s, allows zoom in/out; reverts to offline when panning back | DONE |
| Session deletion without typing name confirmation (yes/no only) | DONE |

#### LOG support (Phase L4)

| Item | Status |
|------|--------|
| `LogStore` -- per-session in-memory ring of decoded `Kv8LogRecord`s, channel-wide site registry | DONE |
| `LogPanel` -- dockable trace viewer: timestamp, severity badge, thread, site, message | DONE |
| Severity row tinting (WARN amber, ERROR red, FATAL magenta) | DONE |
| Severity filter checkboxes (Debug / Info / Warning / Error / Fatal) | DONE |
| Substring text filter | DONE |
| Auto-scroll (Follow) toggle; click-to-seek timeline integration | DONE |
| Visible-window highlight overlay -- entries inside the waveform X-range stand out | DONE |
| `ConsumerThread` subscribes to `<sid>._log` and channel `._registry`, dispatches to LogStore | DONE |
| Toolbar `[Log]` / `[Log ON N]` button + `Ctrl+L` toggle | DONE |

---

### 2.5 kv8zoom -- 3D Path Viewer

| Component | Status |
|-----------|--------|
| `KafkaReader` -- reads live or historical telemetry topics | DONE |
| `FrameAssembler` -- time-aligns multi-topic samples into frames | DONE |
| `AttitudeFilter` -- orientation computation from telemetry | DONE |
| `DecimationController` -- adaptive output rate for visualization needs | DONE |
| `PathSimplifier` -- Ramer-Douglas-Peucker for path data reduction | DONE |
| `WsServer` -- uWebSockets WebSocket server for browser frontend | DONE |
| `ZoomBridge` -- connects Kafka reader to WebSocket output | DONE |
| `Serializer` -- JSON frame serializer for WebSocket clients | DONE |
| Frontend: isometric coordinate plane, zoom/rotate/pan, timeline navigation | DONE |
| Live session discovery and data flow | DONE |
| Timeline playback with pause/play and jump-to-live button | DONE |

---

### 2.6 Documentation

| Document | Location | Status |
|----------|----------|--------|
| `KV8LIB_API_REFERENCE.md` -- libkv8 + kv8util API reference with examples | `docs/` | DONE |
| `KV8SCOPE_USER_GUIDE.md` -- comprehensive kv8scope user guide | `docs/` | DONE |
| `DIGEST.md` -- project overview and architecture summary | root | DONE |
| `MODEL.md` -- aerial platform example model | root | DONE |
| Design proposals | `Kv8Planning/` | DONE |

---

## 3. Known Open Issues -- Audit Results

This section has been audited against the actual source code (post-session
review). Each issue now carries a confirmed status.

| # | Area | Issue | Severity | Status |
|---|------|-------|----------|--------|
| 1 | kv8feeder | Sample interval for Counter and Waldhausen not uniform; randomness not within specified 0.1x--10x bounds | Medium | **RESOLVED** -- `tight_wait()` spin-loop with 200 us margin gives sub-microsecond precision; `run_counter_feed()` and `run_wald_feed()` use `next += interval; tight_wait(next)` uniform pattern. |
| 2 | kv8feeder | Phases counter arrives in batches instead of uniformly at 100K/s | Medium | **RESOLVED** -- `run_phases_feed()` uses truncated exponential distribution with mean = nominal_ns, clamped to [0.1x, 10x], same uniform emission loop. No batching. |
| 3 | kv8log | P7 ClKafka telemetry (CPP example) not visible in kv8scope -- registry format mismatch | Medium | **NOT RELEVANT** -- P7 ClKafka integration is out of scope. No further action. |
| 4 | kv8log tests | `test_monotonic` produces no samples visible in kv8scope | High | **RESOLVED** -- `test_monotonic.cpp` now declares counter range `[0.0, kSamples-1]` and uses `KV8_TEL_ADD_TS` with `KV8_MONO_TO_NS`-converted timestamps; falls back to `KV8_TEL_ADD` when runtime absent. |
| 5 | kv8log tests | `test_null_lib` counter min/max range does not match produced data | Medium | **RESOLVED** -- `test_null_lib.cpp` now declares `KV8_TEL(null_ctr, "null/counter", 0.0, 9999.0)` exactly matching the 0..9999 ramp values. |
| 6 | kv8scope | Enable/disable counter: kv8scope produces `._ctl` JSON but kv8log runtime does not consume it | Medium | **RESOLVED** -- `kv8log_impl.cpp` runs a `ctl_thread` (`RunCtlConsumerThread`) that subscribes to the `._ctl` topic and parses `{"v":1,"cmd":"ctr_state",...}` JSON via `ParseCtlMsg()`. `kv8log_set_counter_enabled` is exported and wired into the vtable. |
| 7 | kv8scope | Vtable has no enable/disable entry; `set_counter_enabled` is declared but gap between scope command and producer response is unverified | Medium | **RESOLVED** -- `struct Vtable` in `Runtime.h` includes `set_counter_enabled` as an optional fn pointer; `Runtime.cpp` resolves it via `LoadSym`; `Counter.cpp` calls it with null-check; `kv8log_impl.cpp` exports `kv8log_set_counter_enabled`. Full path is closed. |
| 8 | kv8scope | Annotations: older annotations can disappear; delete affects other annotations | High | **RESOLVED** -- IDs are now `<wall_clock_hex>-<seq>` (unique across restarts); `Delete()` uses exact-ID soft-delete via `bDeleted` flag; `DrainPending()` upserts by exact ID match; no cross-annotation contamination possible. |
| 9 | kv8scope | Zoom stuck at maximum resolution when trying to zoom out | High | **RESOLVED** -- Explicit bug-fix comment in `WaveformRenderer.cpp`: clamps `dXRange` to `kMinXSpan = 1e-9` before computing zoom center; zoom-in is also clamped to `kMinXSpan` minimum span. Escape is always possible. |
| 10 | kv8scope | Nanosecond-scale zoom may not render well at highest resolution | Low | **RESOLUTION TO BE CONFIRMED** -- `kMinXSpan = 1e-9` (1 ns) prevents the locked state, but rendering quality at that zoom (double-precision loss at Unix-epoch timestamps ~1.7e9 s) still needs visual verification in a live session. |
| 11 | kv8scope | Two incompatible control topic formats (`._control` ASCII vs `.ctrl` JSON) | Medium | **RESOLVED** -- `Kv8Consumer.cpp` now assigns `sControlTopic = sPrefix + "._ctl"` (JSON format); `DeleteSessionTopics()` deletes the correct topic; all documentation (README, API reference, Kv8Types.h, IKv8Consumer.h) updated to reflect `._ctl`. |

---

## 4. Trace LOG Feature -- Completed (Phases L1..L6)

The LOG feature was planned in [Kv8Planning/LOG_PHASES.md](Kv8Planning/LOG_PHASES.md)
and delivered in six phases. All phases are complete and verified.

### 4.1 What "LOG" means

A trace message is fundamentally different from a telemetry sample:
- It is a string (format + args), not a numeric value.
- The format string is transmitted **once**, recorded in the channel
  registry, and referenced from every data record by `dwSiteHash`.
- It carries a severity level (Debug / Info / Warning / Error / Fatal),
  a wall-clock nanosecond timestamp, the OS thread ID, and the CPU core
  index at the moment of the call.
- It lives in a dedicated Kafka topic: `<channel>.<sessionID>._log`.
- kv8scope renders log entries in a dockable LogPanel with a click-to-seek
  link to the waveform timeline; severity-coloured row tints make WARN /
  ERROR / FATAL entries stand out without per-record markers on the graph.

### 4.2 Phase delivery summary

| Phase | Title | Key files | Status |
|-------|-------|-----------|--------|
| L1 | Wire format | `libs/libkv8/include/kv8/Kv8Types.h` (`Kv8LogRecord`, `Kv8LogLevel`, `Kv8DecodeLogRecord`, site-tail codec) | DONE |
| L2 | Producer (kv8log) | `libs/kv8log/include/kv8log/KV8_Log.h` (`KV8_LOG`, `KV8_LOGF`, severity wrappers), `libs/kv8log/src/Runtime.{h,cpp}`, `libs/kv8log/lib/kv8log_impl.cpp` (registry write + Produce on `._log`) | DONE |
| L3 | Consumer + kv8cli | `examples/kv8cli/main.cpp` -- `[LOG]` lines with severity, site, message; site-registry resolution | DONE |
| L4 | kv8scope LogPanel | `kv8scope/LogStore.{h,cpp}`, `kv8scope/LogPanel.{h,cpp}`, `kv8scope/ConsumerThread.cpp`, toolbar `[Log]` button + `Ctrl+L` | DONE |
| L5 | Unit + integration tests | `libs/kv8log/test/test_log_levels.cpp`, `test_log_static_cache.cpp`, `test_log_e2e.cpp` (broker-conditional skip) | DONE -- 7/7 PASS |
| L6 | Throughput benchmark | `examples/kv8bench_log/` + `kv8feeder` 5-level emitter | DONE |

### 4.3 Wire format -- as built

```cpp
// libs/libkv8/include/kv8/Kv8Types.h
static const uint32_t KV8_LOG_MAGIC        = 0x4B563854u;  // "KV8T"
static const uint16_t KV8_LOG_MAX_PAYLOAD  = 4095u;
static const uint8_t  KV8_LOG_LEVEL_COUNT  = 5u;
static const uint8_t  KV8_LOG_FLAG_TEXT    = 0x01u;
static const uint16_t KV8_CID_LOG_SITE     = 0xFFFBu;

enum class Kv8LogLevel : uint8_t {
    Debug = 0, Info = 1, Warning = 2, Error = 3, Fatal = 4,
};

#pragma pack(push, 1)
struct Kv8LogRecord {                 // 28 bytes, fixed
    uint32_t dwMagic;     // KV8_LOG_MAGIC
    uint32_t dwSiteHash;  // FNV-32 of (basename, line, function); registry key
    uint64_t tsNs;        // wall-clock nanoseconds since Unix epoch
    uint32_t dwThreadID;  // OS thread ID at time of call
    uint16_t wCpuID;      // CPU core index at time of call
    uint8_t  bLevel;      // Kv8LogLevel
    uint8_t  bFlags;      // bit 0 = KV8_LOG_FLAG_TEXT (raw UTF-8 payload)
    uint16_t wArgLen;     // payload bytes that follow
    uint16_t wReserved;   // must be zero
    // followed by wArgLen bytes of payload
};
static_assert(sizeof(Kv8LogRecord) == 28, "Kv8LogRecord must be 28 bytes");
#pragma pack(pop)
```

The site descriptor lives in the channel `._registry` topic as a
`KafkaRegistryRecord` with `wCounterID = KV8_CID_LOG_SITE`. Tail layout:

```
uint16_t wFileLen ; char file[wFileLen]
uint32_t dwLine
uint16_t wFuncLen ; char func[wFuncLen]
uint16_t wFmtLen  ; char fmt [wFmtLen]
```

This means a 100-byte format string is transmitted **once per process run**,
no matter how many million log records reference it.

### 4.4 Hot path -- as built

```cpp
// libs/kv8log/include/kv8log/KV8_Log.h (KV8_LOG_ENABLE on)
KV8_LOG_INFO("kv8feeder started");
KV8_LOGF_WARN("Waldhausen amplitude exceeded %.2f at t=%.6f s", amp, t);
```

Each macro expansion owns a TU-scoped `static std::atomic<uint32_t>` site
hash. First call: registers the site (one Kafka write to `._registry`);
stores the hash in the atomic. Subsequent calls: relaxed atomic load, then
format into a 4096-byte stack buffer and dispatch via `Runtime::Log`.

Measured on Windows / MSVC Release / single thread (kv8bench_log):
- Throughput: 1.6 M calls/s on 1 thread, 2.2 M calls/s on 2 threads.
- Dispatch latency p50 = 500 ns, p99 = 1.5 us. (Note: `TimerNow()`
  measurement overhead alone is ~200 ns on Windows QPC, so the true
  hot-path cost is closer to 300 ns.)

### 4.5 Compile-out path

When `KV8_LOG_ENABLE` is not defined, every macro in `KV8_Log.h` expands
to `((void)0)`. The user TU keeps zero references to kv8log symbols and
the resulting binary has zero overhead from instrumentation.

--- the resulting binary has zero overhead from instrumentation.

---

## 5. Recommended Next Steps

The planned scope (telemetry + UDT + trace LOG) is complete. Open work
items below are improvements rather than blockers.

1. **Packed typed-args log payload (LOG L7).** Replace `KV8_LOGF`'s
   pre-formatted text with a packed `Kv8LogArgType` payload. The wire enum
   is already reserved. Benefits: smaller records, deferred / locale-correct
   formatting in the consumer, lower hot-path cost.

2. **Confirm nanosecond-zoom rendering quality (issue #10).** `kMinXSpan`
   prevents the locked state, but visual verification at 1 ns spans on a
   live session is still pending.

3. **Optional source-location compile-time gate.** A `KV8_LOG_WITH_LOCATION=0`
   build switch could strip `__FILE__` / `__FUNCTION__` from the binary for
   size-sensitive embedded targets. Currently file/line/function always
   appear in the registry tail (paid once per call site).

4. **kv8scope event markers on the waveform timeline.** LogPanel covers the
   read path; thin coloured triangles on the time axis for WARN/ERROR/FATAL
   would make severity events visible without opening the panel.

