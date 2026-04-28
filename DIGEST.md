# Kv8 Project Digest

## What It Is

Kv8 is a high-performance Kafka telemetry library and toolchain for C++ applications.
The goal: stream structured, high-frequency telemetry from production C++ code to a
Kafka bus with near-zero overhead on the hot path, and visualize or analyze it in
real time.

Primary use case demonstrated in MODEL.md: a 1000 Hz aerial platform producing
~158 KB/s across three topics (Navigation, Attitude, Motors), with offline ML
anomaly detection trained on recorded flights and re-deployed as a live scoring
consumer.

---

## Repository Layout

```
libs/kv8log/     -- core telemetry producer library (the "instrument" side)
kv8scope/        -- ImGui software oscilloscope for live waveform visualization
kv8zoom/         -- attitude filter and frame assembler (signal analysis tool)
tools/           -- CLI maintenance and verification tools
  kv8maint/      -- channel/session admin (inspect, audit, delete)
  kv8probe/      -- deterministic synthetic sample producer
  kv8verify/     -- timestamp integrity and sequence continuity checker
examples/        -- standalone example programs
  kv8bench/      -- end-to-end latency and throughput benchmark
  kv8cli/        -- stream telemetry/logs from Kafka to stdout
  kv8feeder/     -- continuous live telemetry producer (scalar + UDT feeds)
docker/          -- docker-compose Kafka broker for local development
```

---

## Core Library: kv8log

The instrumentation API lives entirely behind a single header (`KV8_Log.h`).
All macros compile out to nothing unless `KV8_LOG_ENABLE` is defined -- zero
cost in production builds that omit the flag.

kv8log exposes **three pillars** of telemetry, all sharing the same Kafka
session and the same `_registry` discovery topic:

1. **Scalar counters** -- continuous numeric streams (altitude, RPM, queue
   depth) via `KV8_TEL_ADD*`.
2. **UDT feeds** -- structured user-defined-type records for compound
   payloads via `KV8_UDT_*`.
3. **Trace logs** -- discrete severity-tagged events via `KV8_LOG_INFO`,
   `KV8_LOGF_WARN`, etc. Records reach the kv8scope Log Panel and `kv8cli`
   `[LOG]` output.

```cpp
KV8_CHANNEL(nav, "Aerial/Navigation");           // named producer channel
KV8_TEL(alt_m, "altitude", 0.0, 10000.0);        // declare counter (once per TU)
KV8_TEL_ADD_CH(nav, alt_m, current_altitude);    // record sample -- hot path

KV8_LOG_INFO("navigation initialised");          // discrete trace event
KV8_LOGF_WARN("GPS dropout %.1fs", dropout_s);   // printf-style trace event
```

The shared runtime (`kv8log.so` / `kv8log.dll`) is loaded lazily on first use,
so the user application has no link-time dependency on it.

### Key Types

| Type | Role |
|------|------|
| `Channel` | One Kafka producer session; lazy-init, thread-safe |
| `Counter` | Named metric with min/max range, bound to a Channel |
| `UdtFeed` | Structured (user-defined type) feed for compound records |
| `Kv8LogRecord` | 28-byte fixed header + payload for one trace-log emission (in `<kv8/Kv8Types.h>`) |
| `Runtime`  | Opaque handle to the loaded shared library |

---

## kv8scope -- Software Oscilloscope

ImGui/OpenGL desktop application. Subscribes to Kafka, discovers sessions
dynamically, and renders live waveforms per counter.

Architecture highlights:
- `ConsumerThread` pulls decoded samples off Kafka.
- `SpscRingBuffer<TelemetrySample>` (lock-free, cache-line-aligned head/tail)
  bridges the consumer thread to the render thread without mutexes.
- `WaveformRenderer` draws scrolling waveforms via ImGui draw lists.
- `LogStore` + `LogPanel` host a dockable trace-log viewer (Ctrl+L). Rows
  are tinted by severity; clicking a row seeks the waveform timeline.
- `SessionManager` tracks open sessions; `SessionListPanel` lists available ones.
- `StatsEngine` / `StatsPanel` compute and display per-counter statistics.
- `TimeConverter` maps hardware timer ticks to wall-clock UTC using the
  `qwTimerFrequency` + `qwTimerValue` anchor stored in the Kafka registry topic.

---

## kv8zoom -- Frame Assembler / Attitude Filter

Reads raw Kafka telemetry, assembles multi-topic frames time-aligned by the
hardware timestamp, applies an attitude filter (`AttitudeFilter`), and feeds
a decimation-controlled output stream. Useful for post-processing and zoomed
playback.

---

## Architecture Principles

**Zero-allocation hot path** -- buffers are pre-allocated at startup and
recycled via a free list. No heap allocation on the `KV8_TEL_ADD` path.

**Cache-line alignment** -- all shared data structures use `alignas(64)` on
struct definitions and 64-byte-aligned dynamic allocations to prevent false
sharing between producer and consumer threads.

**Lock-free SPSC** -- the render pipeline uses a single-producer /
single-consumer ring buffer (power-of-two capacity, acquire/release atomics)
between the Kafka consumer thread and the ImGui render thread.

**Compile-out by default** -- `#define KV8_LOG_ENABLE` is the explicit opt-in.
Uninstrumented builds carry exactly zero overhead.

**Lazy runtime loading** -- `kv8log.dll` / `kv8log.so` is `dlopen`-ed on first
use; applications do not need to be relinked to disable telemetry.

---

## Build

```sh
# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build          # output -> build/_output_/bin/

# Windows (vcpkg required for librdkafka)
# Set VCPKG_ROOT, then:
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKV8_ARCH=AVX2
cmake --build build
```

**Dependencies**: librdkafka (via vcpkg on Windows, system package on Linux),
Dear ImGui, GLFW, OpenGL; C++17; CMake >= 3.20.

A `docker-compose.yml` in `docker/` starts a local Kafka broker with SASL/PLAIN
auth for development (default credentials: `kv8producer` / `kv8secret`,
broker at `localhost:19092`).

---

## ML Extension (MODEL.md)

Describes but does not yet ship a full `kv8infer` consumer: offline training
(Isolation Forest + LSTM Autoencoder via scikit-learn / PyTorch), ONNX export,
and a lightweight C++ inference consumer that publishes anomaly scores back to
Kafka at 20 Hz. kv8scope would overlay the score as a live waveform channel.
