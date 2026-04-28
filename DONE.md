# Kv8 Project -- Status Report

Date: 2026-04-28

This document summarises what has been built, what remains open, and where
the project stands for the next phase: adding trace LOG support to kv8log.

---

## 1. Executive Summary

The Kv8 telemetry-over-Kafka stack is largely complete through its planned
phases. The core library, all CLI tools, the oscilloscope application, and
the 3D path viewer are all built and functional. UDT (structured telemetry)
support has been implemented end-to-end. The enable/disable counter
mechanism has been designed. The documentation set is in place.

What is NOT yet present: text trace logging. kv8log currently handles only
numeric (scalar) telemetry and structured UDT feeds. The "LOG" pillar --
named trace messages with severity levels stored in Kafka and rendered in
kv8scope -- is the explicit goal of the next development phase.

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

**NOT present in kv8log:**

- Trace / text log macros (`KV8_LOG`, `KV8_TRACE`, severity levels, format strings).
- Log topic wire format.
- Log consumer support in libkv8 / kv8scope.

---

### 2.3 Tools

| Binary | Purpose | Status |
|--------|---------|--------|
| `kv8cli` | Stream telemetry/log records from Kafka to stdout (human-readable) | DONE |
| `kv8bench` | End-to-end latency and throughput benchmark | DONE |
| `kv8feeder` | Continuous live telemetry producer: three scalar feeds + UDT feeds | DONE |
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

## 4. Where We Stand for the LOG Phase

### 4.1 What "LOG" means

kv8log was designed from the start to carry both **Telemetry** (numeric feeds,
structured UDT feeds) and **Trace** (text log messages with severity levels).
Telemetry is complete. Trace is not started.

A trace message is fundamentally different from a telemetry sample:
- It is a string (format + args), not a numeric value.
- The format string shall be transmitted once and recoded into the regestry. 
  Only args are sent after that with reference to regestry.
- It carries a severity level (Debug / Info / Warning / Error / Fatal).
- It shall carry a source location (file, line, function).
- It is stored in a dedicated Kafka topic: `<channel>.<sessionID>._log`.
- It does not have a counter ID or a Y-axis range.
- kv8scope should display trace messages as event markers on the time axis,
  not as waveforms.
- a separate window shall display log in a table format. 
- navigation on telemetry timeline and log list shall be synchronized

### 4.2 Current state -- what exists

| Component | Exists? | Notes |
|-----------|---------|-------|
| `._log` topic name defined in kv8log_impl.cpp | YES | Topic is allocated at session open; no records are written |
| Vtable entry for trace | NO | No `log` or `trace` fn pointer in `struct Vtable` |
| `KV8_LOG` / `KV8_TRACE` macros | NO | Not in `KV8_Log.h` |
| Wire type for trace messages | NO | No struct equivalent of `Kv8TelValue` for text |
| kv8log_runtime trace implementation | NO | kv8log_impl.cpp has no log-producing function |
| libkv8 consumer support for trace records | NO | `IKv8Consumer` has no log-specific decode |
| kv8scope trace display | NO | No log panel, no event markers on time axis |
| kv8cli trace output | PARTIAL | kv8cli prints raw bytes; no structured trace decode |

### 4.3 What needs to be built

#### Layer 1: Wire format (Kv8Types.h)

Define a new on-wire struct for trace records, stored in `<ch>.<sid>._log`:

```cpp
// Severity levels -- stored as 1 byte in the wire record.
enum class Kv8LogLevel : uint8_t
{
    Debug   = 0,
    Info    = 1,
    Warning = 2,
    Error   = 3,
    Fatal   = 4,
};

// On-wire trace record (variable length; max payload 4096 bytes).
// Kafka message key = empty.
// Kafka message value = Kv8LogRecord header + message bytes (no null terminator).
#pragma pack(push, 1)
struct Kv8LogRecord
{
    uint32_t dwMagic;     // 0x4B563854 ("KV8T") -- identifies record type
    uint8_t  bLevel;      // Kv8LogLevel
    uint8_t  bReserved;   // alignment / future use
    uint16_t wMsgLen;     // byte length of the message string that follows
    uint64_t tsNs;        // nanoseconds since Unix epoch
    // Followed immediately by wMsgLen bytes of UTF-8 text (no null terminator).
};
#pragma pack(pop)
```

A separate optional wire field (or a second Kafka header) can carry source
location (file:line) for debug-level messages.

#### Layer 2: Vtable extension (Runtime.h)

Add two entries to `struct Vtable`:

```cpp
void (*log)(kv8log_h h, uint8_t level, const char* msg, uint16_t len);
```

Both `kv8log_facade` (static, user side) and `kv8log_runtime` (shared, producer
side) need to implement the resolution and dispatch path respectively.

#### Layer 3: User macros (KV8_Log.h)

```cpp
// Enabled path:
#define KV8_LOG(level, msg) \
    kv8log::Runtime::Log(level, (msg), (uint16_t)strlen(msg))

#define KV8_LOGF(level, fmt, ...) \
    kv8log::Runtime::LogFmt(level, fmt, ##__VA_ARGS__)

// Convenience wrappers:
#define KV8_LOG_DEBUG(msg)   KV8_LOG(kv8log::LogLevel::Debug,   (msg))
#define KV8_LOG_INFO(msg)    KV8_LOG(kv8log::LogLevel::Info,    (msg))
#define KV8_LOG_WARN(msg)    KV8_LOG(kv8log::LogLevel::Warning, (msg))
#define KV8_LOG_ERROR(msg)   KV8_LOG(kv8log::LogLevel::Error,   (msg))
#define KV8_LOG_FATAL(msg)   KV8_LOG(kv8log::LogLevel::Fatal,   (msg))

// Disabled path -- all expand to ((void)0).
```

`LogFmt` should format into a stack-allocated buffer (max 4096 bytes) to
preserve the zero-allocation constraint.

#### Layer 4: kv8log_runtime implementation

Add a `kv8log_log()` exported function to `kv8log_impl.cpp`:
- Formats the `Kv8LogRecord` header + text into a stack buffer.
- Calls `IKv8Producer::Produce()` on the `._log` topic.
- Thread-safe (producer is already thread-safe via internal rdkafka locking).
- No heap allocation: format into a fixed 4096-byte stack buffer.

#### Layer 5: libkv8 consumer (IKv8Consumer.h)

Add a decode helper for `._log` topics. kv8cli and kv8scope ConsumerThread
need to detect records on `._log` topics and parse `Kv8LogRecord`.

Option A: add a `DecodeLogRecord()` static helper to `Kv8Types.h`.
Option B: detect in the consumer loop by topic suffix `._log`.

#### Layer 6: kv8scope -- log display

Add a `LogPanel` (similar to StatsPanel) that:
- Lists log records for the open session filtered by visible time window.
- Columns: timestamp, level (colored badge), message.
- Severity filter checkboxes (Debug / Info / Warning / Error / Fatal).
- Click on a row seeks the waveform to that timestamp.
- On the waveform time axis, draw small colored triangular event markers for
  Warning, Error, and Fatal records (Debug and Info hidden by default).

The ConsumerThread already subscribes to all session topics; adding `._log`
subscription is straightforward.

#### Layer 7: kv8cli -- log output

kv8cli should decode and print log records in a human-readable line format:

```
[2026-04-28T10:35:12.345678Z] [INFO ] kv8feeder: producer started, 3 counters
[2026-04-28T10:35:12.350000Z] [WARN ] cpu_temp: exceeded 95C threshold
[2026-04-28T10:35:13.000000Z] [ERROR] motors: encoder fault on axis 2
```

### 4.4 Effort estimate

| Layer | Scope | Effort |
|-------|-------|--------|
| Wire format + Kv8Types.h additions | Small | 0.5 d |
| Vtable + Runtime.h + KV8_Log.h macros | Small | 0.5 d |
| kv8log_runtime log producer function | Small | 0.5 d |
| libkv8 consumer decode helper | Small | 0.25 d |
| kv8scope LogPanel + waveform event markers | Medium | 2 d |
| kv8cli log decoding and pretty-print | Small | 0.5 d |
| Tests + kv8feeder example log calls | Small | 0.5 d |
| **Total** | | **~5 d** |

### 4.5 Design constraints

- No heap allocation on the hot path: `LogFmt` uses a 4096-byte stack buffer.
  Strings longer than 4095 bytes are truncated with a trailing "..." marker.
- The `._log` topic is separate from `._registry` and data topics.
  kv8scope does not need to rename existing topics.
- Log records are NOT routed through `SpscRingBuffer<TelemetrySample>`.
  A separate `SpscRingBuffer<Kv8LogRecord>` (or a simple mutex-guarded deque,
  given much lower volume) feeds the LogPanel.
- Severity filter in kv8scope must NOT be a Kafka consumer filter -- all records
  are consumed and filtered client-side so that switching filters does not
  require a Kafka seek.
- Source location (file:line) is optional and off by default. Carrying it on
  every record is wasteful; it should be a compile-time opt-in
  (`KV8_LOG_WITH_LOCATION`).
- The LOG feature must compile out completely when `KV8_LOG_ENABLE` is not
  defined, consistent with the existing telemetry compile-out behavior.

---

## 5. Recommended Next Steps

Priority order:

1. **Fix kv8scope annotation bug** (issue #8 above). Annotations are used in
   production sessions. The delete-marker logic needs a correctness review.

2. **Fix kv8scope zoom stuck bug** (issue #9). This blocks detailed waveform
   analysis.

3. **Implement LOG wire format + macros + runtime** (Layers 1-3 in section 4.3).
   Self-contained; no UI changes required. kv8feeder can emit log records as a
   first smoke test.

4. **kv8cli log decode** (Layer 7). Quick validation that the log producer is
   working before building the UI.

5. **kv8scope LogPanel** (Layer 6). The most visible user-facing feature of the
   LOG phase.

6. **test_monotonic and test_null_lib fixes** (issues #4 and #5). Both are
   correctness issues in the test suite.
