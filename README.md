# Kv8 Quick Start Guide

Build the project, start a local Kafka broker, produce telemetry,
consume and verify it with the Kv8 tools.

---

## 1. Tools Overview

The project is split into three top-level source directories:

- `libs/` -- shared libraries (`libkv8`, `kv8util`, `kv8log`)
- `tools/` -- maintenance and diagnostic CLI tools (`kv8maint`, `kv8probe`, `kv8verify`)
- `examples/` -- standalone example programs (`kv8bench`, `kv8bench_log`, `kv8cli`, `kv8feeder`)

All binaries are installed to `build/_output_/bin/` by `cmake --install`.

| Binary | Role | One-line description |
|--------|------|----------------------|
| `kv8cli` | Consumer | Stream telemetry and logs from Kafka to stdout |
| `kv8feeder` | Producer | Continuous live telemetry producer; scalar and UDT feeds; emits a 5-level trace-log stream so the kv8scope Log Panel always has live data |
| `kv8maint` | Admin | Inspect, audit, and delete channels and sessions |
| `kv8probe` | Producer | Write deterministic synthetic samples for verification |
| `kv8verify` | Consumer | Verify timestamp integrity and sequence continuity |
| `kv8bench` | Producer + Consumer | Measure end-to-end latency and throughput |
| `kv8bench_log` | Producer | Stress the `KV8_LOG*` hot path and report throughput / latency percentiles |

---

## 2. Tool Reference

All tools share these common Kafka connection flags (defaults match the
Docker Compose broker shipped in `docker/`):

```
--brokers <host:port>     Bootstrap brokers           [localhost:19092]
--security-proto <p>      plaintext|sasl_plaintext|sasl_ssl [sasl_plaintext]
--sasl-mechanism <m>      PLAIN|SCRAM-SHA-256|SCRAM-SHA-512  [PLAIN]
--user <username>         SASL username               [kv8producer]
--pass <password>         SASL password               [kv8secret]
```

### 2.1 kv8cli -- Kafka telemetry consumer

Connects to a Kafka broker, discovers all sessions under a channel prefix,
reads registry metadata, and streams every telemetry value and log message to
stdout.

```
kv8cli --channel <prefix> [options]

Required:
  --channel <prefix>       Channel prefix, e.g. kv8/test or kv8.test
                           (slashes are auto-converted to dots)

Options:
  --brokers <list>         Bootstrap broker list [localhost:19092]
  --security-proto <p>     plaintext|sasl_plaintext|sasl_ssl [sasl_plaintext]
  --sasl-mechanism <m>     PLAIN|SCRAM-SHA-256|SCRAM-SHA-512 [PLAIN]
  --user <username>        SASL username [kv8producer]
  --pass <password>        SASL password [kv8secret]
  --group <group-id>       Consumer group ID [auto-generated per run]
  --from-beginning         Start from offset earliest (default: offset latest)
  --help                   Show this help
```

**Implemented features:**
- Dynamic topic discovery: polls Kafka metadata periodically and subscribes to
  new sessions without restart.
- Rebalance-safe offset management: newly assigned topic-partitions always
  start from `OFFSET_BEGINNING`; partitions already in progress retain their
  current offset across rebalances.
- Registry-based name resolution: counter names and group names are read from
  the `_registry` topic and used to annotate each `[DATA]` line.
- Absolute wall-clock timestamps: `qwTimer` ticks are converted to UTC using
  the `qwTimerFrequency` and `qwTimerValue` anchor stored in the registry.
- Log forwarding: `_log` topics are decoded and printed as `[LOG]` lines.
- Clean exit on Ctrl+C or Esc with a registry summary.

**Known limitations:**
- SSL certificate verification is not configured; `sasl_ssl` requires
  additional `ssl.ca.location` setup not yet exposed as a CLI flag.
- Only the `Kv8TelValue` (double-value) wire format is supported; structured
  binary blob telemetry (if added in future) will be skipped.
- The consumer reads the `_registry` topic for every session found, even
  sessions from previous runs. Use `--from-beginning` only when replaying
  historical data; omit it for live monitoring to avoid replaying old sessions.
- No filtering by session ID or counter name; all sessions under the channel
  prefix are merged into a single output stream.

---

### 2.2 kv8maint -- Kafka channel/session maintenance

Interactive and scriptable tool for inspecting, auditing, and deleting Kv8
sessions and channels stored in Kafka. Supports both the current per-counter
topic layout and legacy shared-topic sessions.

```
kv8maint [--channel <prefix>] [options]

Connection:
  --brokers <list>         Bootstrap broker list [localhost:19092]
  --security-proto <p>     plaintext|sasl_plaintext|sasl_ssl [sasl_plaintext]
  --sasl-mechanism <m>     PLAIN|SCRAM-SHA-256|SCRAM-SHA-512 [PLAIN]
  --user <username>        SASL username [kv8producer]
  --pass <password>        SASL password [kv8secret]

Selection:
  --channel <prefix>       Target channel prefix (auto-discovered if omitted)
  --session <sessionID>    Jump directly to the detail view for this session

Actions:
  --delete <sessionID>     Delete a named session (requires --channel)
  --delete-channel         Delete the entire channel and all its topics
  --yes                    Skip all confirmation prompts (non-interactive)
  --help                   Show this help
```

**Interactive navigation flow:**

1. **Channel list** -- lists all `kv8.*` channels found on the
   broker. Select a channel by entering its number. If `--channel` is given,
   this step is skipped.
2. **Session list** -- lists all sessions under the selected channel with
   their IDs and names. Available keys:
   - Number: select a session and open its detail view.
   - `C`: delete the entire channel after confirmation.
   - `Q`: quit.
3. **Session detail view** -- shows full metadata and per-counter statistics
   for the selected session. Available keys:
   - `D`: delete the session (tombstones all counter topics and the group
     virtual topics; marks the session deleted in the registry).
   - `B`: back to the session list.
   - `Q`: quit.

**Session detail output (per-counter layout):**

For sessions recorded with the current per-counter topic layout the detail
view shows:
- Session header: ID, name, channel prefix, log topic, control topic.
- Summary table: one row per counter topic with columns `Counter topic`,
  `Messages`, and `Counter name`.
- Per-group blocks: group name, timing anchor (`qwTimerFrequency`,
  `qwTimerValue`), and per-counter message counts.

Registry metadata is read directly from `_registry`; no data-topic scan is
required for per-counter sessions.

**Session detail output (legacy layout):**

For older sessions using shared data topics (one topic per counter group)
the detail view shows:
- Summary table: `Data topic`, `Messages`, `Cntrs`, `Group name`.
- Per-group detail blocks showing counter names and ranges.

**Deletion behaviour:**

`--delete` (or pressing `D` interactively) publishes a Kafka tombstone
(null-value message) for every counter topic and group virtual topic in the
session, and writes a deleted marker to `_registry`. The topics themselves
are removed from the broker once Kafka's log-compaction and cleanup
retention policies apply.

`--delete-channel` removes all topics belonging to the channel, including
the shared `_registry` topic. This is a destructive, irreversible operation.

**Known limitations:**
- Channel auto-discovery scans all topics on the broker; on brokers with
  thousands of topics this can be slow. Use `--channel` to scope the scan.
- `--delete-channel` does not wait for broker-side topic deletion to
  complete; re-listing immediately after may still show the channel briefly.

---

### 2.3 kv8probe -- Timestamp integrity probe (producer side)

Writes N samples of `Kv8TelValue` directly to Kafka using a fully
deterministic synthetic clock. No external library is required. A
manifest message is written first so `kv8verify` can independently reconstruct
the expected value for every field of every sample.

```
kv8probe --topic <name> [options]

  --brokers <b>          Bootstrap brokers [localhost:19092]
  --topic <name>         Target topic (auto-generated session ID if omitted)
  --count <N>            Number of samples [1000]
  --start-tick <t>       Initial qwTimer value [1000000]
  --wid <id>             Counter wID [0]
  --security-proto <p>   plaintext|sasl_plaintext|sasl_ssl [sasl_plaintext]
  --sasl-mechanism <m>   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512 [PLAIN]
  --user / --pass        SASL credentials
```

**Synthetic clock parameters (compile-time constants):**

| Constant | Value | Meaning |
|----------|-------|---------|
| `PROBE_FREQUENCY` | 1,000,000 Hz | 1 tick = 1 us |
| `PROBE_START_TICK` | 1,000,000 | `qwTimer` of sample 0 (t = 1 s) |
| `PROBE_TICK_INTERVAL` | 230 ticks | 230 us between consecutive samples |

After writing all samples, `kv8probe` prints the exact `kv8verify` command
line and a `launch.json` arg snippet so the verifier can be run immediately.

**Known limitations:**
- The synthetic clock constants are compile-time only; changing them requires
  a rebuild. The manifest carries the runtime values so `kv8verify` always
  reads the correct parameters regardless of when it was built.
- Batch settings (`linger.ms=5`, `batch.num.messages=10000`) are hardcoded
  for test throughput; they are not appropriate for production use.

---

### 2.4 kv8verify -- Telemetry integrity verifier (consumer side)

Two operating modes:

1. **Manifest mode** (`--topic`): reads the manifest from `<topic>._manifest`,
   then consumes all messages from `<topic>` and performs exact per-sample
   verification against kv8probe's deterministic synthetic clock.
2. **Sequence gap mode** (`--channel`): discovers sessions from the channel
   registry, reads all data topics from offset 0, and verifies that `wSeqN`
   values are contiguous for each counter (`wID`). Works with real kv8
   telemetry -- no manifest required.

```
Usage:
  kv8verify --topic <name> [options]            Manifest mode (kv8probe data)
  kv8verify --channel <name> [--session <id>]   Sequence gap mode (kv8 telemetry)

Common options:
  --brokers <b>          Bootstrap brokers [localhost:19092]
  --security-proto <p>   plaintext|sasl_plaintext|sasl_ssl [sasl_plaintext]
  --sasl-mechanism <m>   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512 [PLAIN]
  --user / --pass        SASL credentials

Manifest mode only:
  --topic <name>         Topic written by kv8probe (required for this mode)
  --count <N>            Expected sample count (cross-validated vs manifest)
  --start-tick <t>       Expected initial qwTimer (cross-validated vs manifest)
  --wid <id>             Expected counter wID (cross-validated vs manifest)

Sequence gap mode only:
  --channel <prefix>     Channel prefix (e.g. kv8/test or kv8.test)
  --session <id>         Session ID to verify (default: newest)
```

**Manifest mode checks (per sample):**

| Check | Description |
|-------|-------------|
| `qwTimer` exact match | Must equal `startTick + globalIndex * tickInterval` |
| `dbValue` rolling counter | Must equal `globalIndex % 1024` |
| `wID` match | Must equal the wID recorded in the manifest |
| No duplicates | Each global index must appear exactly once |
| No missing samples | All indices 0..N-1 must be present |
| In-order delivery | Messages must arrive in ascending index order |

**Sequence gap mode checks (per counter):**

| Check | Description |
|-------|-------------|
| `wSeqN` continuity | Each successive message must have `wSeqN == prev + 1` (mod 65536) |
| No gaps | Forward jumps in `wSeqN` are reported with exact miss count |
| No duplicates | Same `wSeqN` repeated consecutively is flagged |
| No out-of-order | Backward jumps beyond half the wrap range are flagged |
| Per-counter summary | Received count, gap count, missed samples, loss rate |

Exit code: `0` = PASS, `1` = FAIL.

**Known limitations:**
- The idle-timeout logic waits 30 seconds for the first data message and 5
  seconds of silence after the last. On slow networks or heavily loaded brokers
  this may cause a premature FAIL; increase `idleLimit` compile-time constants
  if needed.
- On Windows, `rd_kafka_destroy()` can hang; the verifier has a 3-second
  guarded shutdown that detaches the destroy thread and exits anyway.
- Only single-partition topics are supported; `kv8verify` always assigns
  partition 0.
- Sequence gap mode reads each data topic sequentially; for sessions with many
  counter topics this may take longer than the manifest mode single-topic read.

### 2.5 kv8bench -- Kafka producer/consumer latency benchmark

Produces and consumes `BenchMsg` records (24 bytes each) on a dedicated Kafka
topic and measures dispatch, producer-to-broker, broker-to-consumer, and
end-to-end latency.  After the benchmark run completes, a **verification
consumer** re-reads the entire topic from offset 0 and independently checks
that every sequence number (0 through N-1) was delivered exactly once.

```text
Usage:
  kv8bench [options]

Options:
  --brokers <host:port>   Kafka bootstrap servers (default: localhost:19092)
  --security <proto>      Security protocol (default: SASL_PLAINTEXT)
  --sasl-mech <mech>      SASL mechanism (default: PLAIN)
  --user <user>           SASL username (default: kv8producer)
  --pass <pass>           SASL password (default: kv8secret)
  --topic <name>          Topic name (default: auto-generated kv8bench.*)
  --count <N>             Produce exactly N messages (count mode)
  --duration <sec>        Produce for the given duration (duration mode, default: 10)
  --tempo <msg/s>         Rate-limit producer to N msg/s (default: unlimited)
  --sample-rate <N>       Latency sample every Nth message (default: 100)
  --report <file>         Report file name (default: auto-generated)
  --partitions <N>        Number of topic partitions (default: 1)
  --linger <ms>           librdkafka linger.ms (default: 5)
  --batch <N>             librdkafka batch.num.messages (default: 10000)
  --no-cleanup            Keep the topic after the run (default: delete)
```

**Report sections:**

| # | Section | Description |
|---|---------|-------------|
| 1 | Dispatch Latency | `Produce()` call duration (QPC/CLOCK_MONOTONIC, ns) |
| 2 | Producer -> Broker | Send wall clock vs broker timestamp (ms) |
| 3 | Broker -> Consumer | Broker timestamp vs consumer wall clock (ms) |
| 4 | End-to-End | Sum of [2]+[3] (ms) |
| 5 | Throughput Summary | Overall msg/s, payload MB/s, queue-full events |
| 6 | Verification Consumer | Independent re-read from offset 0: expected, received, missing, duplicates, out-of-range, PASSED/FAILED |

**Verification pass details:**

After the concurrent consumer finishes, a second consumer reads the topic from
the beginning using `ConsumeTopicFromBeginning()`.  It builds an independent
bitmap of seen sequence numbers and reports:

- **Missing** -- sequence numbers in 0..N-1 that were not received.
- **Duplicates** -- sequence numbers seen more than once.
- **Out-of-range** -- sequence values outside the expected range.
- **Result** -- PASSED if all counters are zero, FAILED otherwise.

The warmup sentinel message (`nSeq == UINT64_MAX`) is automatically skipped.

---

### 2.6 kv8bench_log -- KV8_LOG hot-path benchmark

Micro-benchmark for the `kv8log` producer hot path. It spins up N worker
threads (default: 2) that hammer `KV8_LOGF_INFO` in a tight loop, samples
latency 1-in-100 calls, and reports per-second progress plus a final
summary with throughput and latency percentiles.

```text
Usage:
  kv8bench_log [options]

Options:
  --threads <N>          Number of worker threads (default: 2; max: 32)
  --duration <sec>       Run duration in seconds (default: 10)
  --brokers <host:port>  Kafka bootstrap servers (default: localhost:19092)
  --user <user> / --pass <pass>
                         SASL credentials (default: kv8producer / kv8secret)
```

**Acceptance thresholds (exit 1 on FAIL):**

| Metric | Threshold |
|--------|-----------|
| Throughput | > 1,000,000 calls / second (aggregate across threads) |
| Per-call latency p50 | < 200 ns |
| Per-call latency p99 | < 1000 ns |

A timestamped report is written to `kv8bench_log_<unix-ts>.txt` in the
current working directory. Recent representative numbers on a desktop
x86_64 host: 1.6 M calls/s on 1 thread, 2.2 M calls/s on 2 threads. The
latency thresholds are aspirational on shared developer hardware -- a FAIL
on latency alone with PASS on throughput is reported but not necessarily
actionable.

**Implementation notes:**

- Each worker thread owns a `static std::atomic<uint64_t>` progress
  counter (templated `HotPathLoop<Idx>` so the static is unique per
  thread) and batches updates every 1024 iterations to keep the contended
  cache line traffic minimal.
- Latency is measured around the macro call only -- not around the
  Kafka send -- so the number reflects what the calling thread actually
  pays. Network/broker effects are absorbed asynchronously by the
  background `Runtime` thread.

---

## 3. The kv8util Shared Library

The `libs/kv8util/` directory provides cross-platform utility code shared by
all Kv8 tools. Two libraries are involved:

| Library | Namespace | Location | Purpose |
|---------|-----------|----------|---------|
| **libkv8** | `kv8` | `libs/libkv8/` | Core Kafka abstraction: `Kv8Config`, `Kv8Producer`, `Kv8Consumer`, topic discovery, registry parsing |
| **kv8util** | `kv8util` | `libs/kv8util/` | Application utilities: timing, statistics, signal handling, CLI config builder |
| **kv8log** | (macros) | `libs/kv8log/` | Trace logging: `KV8_LOG_INFO`, `KV8_LOGF_WARN`, etc. Two artefacts -- `kv8log_facade` (static, link-time) and `kv8log_runtime` (shared, lazy-loaded) -- so application TUs that compile out the macros carry zero kv8log symbols |

**libkv8** encapsulates all interaction with librdkafka behind a clean C++
API. Application code never calls `rd_kafka_*` functions directly. See
[kv8util_API_REFERENCE.md](kv8util_API_REFERENCE.md) for the full API
reference.

**kv8log** adds a third pillar to the telemetry surface: alongside scalar
counters and UDT (user-defined type) blob telemetry, applications emit
discrete trace events with `KV8_LOG_INFO("...")` or `KV8_LOGF_WARN("%d
items", n)`. Records flow over Kafka into the kv8scope Log Panel and can
be replayed historically. See
[KV8LIB_API_REFERENCE.md, section 5](docs/KV8LIB_API_REFERENCE.md#5-kv8log----trace-logging-api)
for the macro reference.

**kv8util** provides the following modules:

| Header | Key symbols | Purpose |
|--------|-------------|---------|
| `Kv8Timer.h` | `TimerInit()`, `TimerNow()`, `TicksToNs()`, `QpcToWallMs()`, `WallMs()` | Cross-platform high-resolution timer (QPC on Windows, `CLOCK_MONOTONIC` on Linux) |
| `Kv8BenchMsg.h` | `BenchMsg` | 24-byte benchmark payload: send tick, wall-clock ms, sequence number |
| `Kv8Stats.h` | `ComputeStats()`, `PrintStatsBlock()`, `PrintHistogram()` | Percentile computation and formatted report output |
| `Kv8TopicUtils.h` | `GenerateTopicName()`, `NowUTC()`, `ProgressRow` | Topic name generation and progress tracking |
| `Kv8AppUtils.h` | `AppSignal`, `CheckEscKey()`, `BuildKv8Config()` | Signal-based shutdown, non-blocking Esc detection, CLI-to-config builder |

**Typical usage pattern** (every Kv8 tool follows this):

```cpp
#include <kv8util/Kv8AppUtils.h>
#include <kv8/Kv8Consumer.h>

int main(int argc, char* argv[])
{
    kv8util::AppSignal::Install();               // Ctrl+C handler
    kv8::Kv8Config cfg = kv8util::BuildKv8Config(argc, argv);
    kv8::Kv8Consumer consumer(cfg);

    while (kv8util::AppSignal::IsRunning()) {
        auto msgs = consumer.Poll(200);
        // process msgs...
        if (kv8util::CheckEscKey())
            kv8util::AppSignal::RequestStop();
    }
}
```

The kv8util unit tests (`kv8util_test`) run automatically during the build via
CTest. See section 6 for details.

---

## 4. Build

### Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Docker Desktop | 4.x+ | With WSL2 backend on Windows |
| CMake | 3.20+ | |
| C++17 compiler | MSVC 2022+ / GCC 13+ | |
| vcpkg | latest | Windows only; `VCPKG_ROOT` must be set |
| librdkafka | 2.x | `vcpkg install librdkafka` (Windows) or `apt install librdkafka-dev` (Linux) |

### Windows

```powershell
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `

cmake --build . --config Release
cmake --install . --config Release
```

### Linux

```bash
sudo apt install -y librdkafka-dev librdkafka1   # Ubuntu/Debian
# sudo dnf install -y librdkafka-devel            # Fedora/RHEL

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build
```

No vcpkg is needed on Linux; CMake finds librdkafka via `pkg-config`. If
librdkafka is in a non-standard prefix, pass `-DCMAKE_PREFIX_PATH=/your/prefix`.

### Output directory

```
build/_output_/bin/
    kv8feeder[.exe]          -- continuous live telemetry producer
    kv8cli[.exe]           -- Kafka telemetry consumer
    kv8maint[.exe]         -- Kafka channel/session maintenance
    kv8probe[.exe]         -- timestamp integrity probe (producer)
    kv8verify[.exe]        -- telemetry integrity verifier
    kv8bench[.exe]         -- latency benchmark with verification
    kv8bench_log[.exe]     -- KV8_LOG hot-path benchmark
```

On Linux the `.exe` suffix is absent. All command examples below use the
Windows form; on Linux use `./kv8cli` instead of `.\kv8cli.exe`.

---

## 5. Quick Demo

### 5.1 Start Kafka

```powershell
cd docker
docker compose up -d
```

Wait for the SASL endpoint (the automated test scripts probe for up to 60 s):

```powershell
docker compose logs kafka | Select-String "started"
```

**Connection details** (from `docker/.env`):

| Setting | Value |
|---------|-------|
| Bootstrap | `localhost:19092` |
| Security | `SASL_PLAINTEXT` |
| SASL mechanism | `PLAIN` |
| Username | `kv8producer` |
| Password | `kv8secret` |

### 5.2 Produce telemetry

**Scalar feeds (default):**

```powershell
cd build\_output_\bin

.\kv8feeder.exe `
    /KV8.brokers=localhost:19092 `
    /KV8.channel=kv8/test `
    /KV8.user=kv8producer `
    /KV8.pass=kv8secret `
    --duration=30
```

This starts three scalar threads:

| Feed | Rate | Signal |
|------|------|--------|
| `Numbers/Counter` | 100 Hz | Wrapping integer 0..1023 |
| `Numbers/Wald` | 10,000 Hz | Gaussian random walk -4000..4000 |
| `Scope/Phases` | 100,000 Hz | Piecewise-constant cyclogram -5..5 |

**UDT feeds (add `--udt`):**

```powershell
.\kv8feeder.exe `
    /KV8.brokers=localhost:19092 `
    /KV8.channel=kv8/test `
    /KV8.user=kv8producer `
    /KV8.pass=kv8secret `
    --udt --duration=30
```

Adds two UDT (User-Defined Type) feeds whose fields each appear as individual
counters in kv8scope:

| UDT Feed | Rate | Fields |
|----------|------|--------|
| `Environment/WeatherStation` | 1 Hz | temperature_c, humidity_pct, pressure_hpa, wind_speed_ms, wind_dir_deg, rain_mm_h, uv_index |
| `Aerial/Navigation` | 1,000 Hz | position (lat/lon/alt), velocity (x/y/z), nav_status (gps_fix, sats, hdop, baro_alt) |
| `Aerial/Attitude` | 1,000 Hz | orientation quaternion (w/x/y/z), angular_rate_rads (x/y/z), accel_ms2 (x/y/z) |
| `Aerial/Motors` | 1,000 Hz | battery_v (6..25.2 V), motor1_rpm..motor4_rpm (0..12,000) |

The three Aerial feeds share a common Unix-epoch timestamp so kv8scope
correlates them on a single time axis.

On Linux replace `.\kv8feeder.exe` with `./kv8feeder` and use
backslash-continuation (`\`) instead of backtick.

Each run creates a unique session under the channel prefix. The sink writes:

| Topic suffix | Content |
|-------------|---------|
| `_registry` | Counter and group metadata (shared across sessions) |
| `_log` | log trace messages |
| `d.<channelID>.<counterID>` | Binary telemetry samples (one topic per counter) |

### 5.3 Consume with kv8cli

In a **separate terminal**:

```powershell
.\kv8cli.exe --channel kv8/test --brokers localhost:19092 --user kv8producer --pass kv8secret
```

Press **Ctrl+C** or **Esc** to stop. Expected output:

```
[REGISTRY] cid=0  ch=0  ON  range=[0.00 .. 1023.00]  name="Group/counter 1"
[DATA] 2026-02-22T... | cid=0 | seq=0 | value=0.000000 | name="Group/counter 1"
...
```

### 5.4 Stop everything

```powershell
# Stop Kafka (keep data):
cd docker ; docker compose down

# Stop Kafka and delete all data:
docker compose down -v
```

---

## 6. Tests

All test scripts are in `scripts/`. They expect the project to be built and
installed before running.

### 6.1 kv8util_test -- Unit tests (CTest)

The kv8util unit test binary runs automatically during the build. It can also
be run manually:

```powershell
cd build
ctest --output-on-failure -C Release
```

On Linux:

```bash
cd build
ctest --output-on-failure
```

The test validates `Timer`, `Stats`, `TopicUtils`, and `BenchMsg`
functionality.

---

### 6.2 test_kafka_e2e.ps1 -- Full end-to-end smoke test

Runs the complete producer-consumer pipeline: starts Kafka, produces 100,000
telemetry samples via `kv8feeder`, consumes them with `kv8cli`, and
reports line counts.

**Use cases:**
- Verify the Kafka sink produces valid messages that `kv8cli` can decode.
- Smoke-test the build after changes to the kv8 Kafka sink or `kv8cli`.
- CI gate for the main producer-consumer path.

**Usage:**

```powershell
# Standard run (leaves Kafka running for inspection)
.\scripts\test_kafka_e2e.ps1

# Run and remove all Kafka data on exit
.\scripts\test_kafka_e2e.ps1 -TearDown

# Skip the CMake build step (use existing binaries)
.\scripts\test_kafka_e2e.ps1 -SkipBuild

# Combine flags
.\scripts\test_kafka_e2e.ps1 -SkipBuild -TearDown
```

**Parameters:**

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `-TearDown` | off | Run `docker compose down -v` after the test |
| `-SkipBuild` | off | Skip the CMake configure+build+install steps |

**What the script does:**

1. Optionally builds the project (`cmake` configure, build, install).
2. Starts Kafka via `docker compose up -d` and waits for the SASL endpoint
   to be ready (up to 60 seconds).
3. Starts `kv8cli` in the background, redirecting stdout/stderr to log files
   in `build/`.
4. Runs `kv8feeder` with `/KV8.channel=kv8/e2e_test`.
5. Waits 5 seconds for message delivery.
6. Stops `kv8cli` and prints the last 40 lines of its output plus a summary
   of `[DATA]`, `[REGISTRY]`, and `[LOG]` line counts.
7. Optionally tears down Docker.

**Pass criteria (manual):** The consumer log must contain `[REGISTRY]` lines
and a non-zero count of `[DATA]` lines.

---

### 6.3 test_ts_e2e.ps1 -- Kafka timestamp end-to-end integrity test

Runs `kv8probe` (producer) then `kv8verify` (consumer) against a dedicated
Kafka topic. Verifies exact timestamp, value, ordering, and delivery
completeness independently of any external library.

**Use cases:**
- Validate that Kafka preserves `qwTimer` values without modification.
- Regression test after changes to the rdkafka producer configuration
  (batching, linger, EOS settings).
- Re-verify an existing topic from a previous run without re-producing data.

**Usage:**

```powershell
# Standard run (auto-generates a topic name)
.\scripts\test_ts_e2e.ps1

# Custom sample count and start tick
.\scripts\test_ts_e2e.ps1 -Count 200000 -StartTick 5000000

# Re-verify an existing topic (skip probe)
.\scripts\test_ts_e2e.ps1 -VerifyOnly -SessionId kv8ts.20260222T143000Z-AB12CD

# Point to a custom binary directory
.\scripts\test_ts_e2e.ps1 -BinDir "C:\custom\bin"
```

**Parameters:**

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `-SessionId` | auto | Topic base name; auto-generated if omitted |
| `-VerifyOnly` | off | Skip `kv8probe`, run `kv8verify` only; requires `-SessionId` |
| `-BinDir` | `build\tools` | Directory to search for `kv8probe.exe` / `kv8verify.exe` |
| `-Brokers` | `localhost:19092` | Kafka bootstrap brokers |
| `-Count` | `1000` | Number of samples |
| `-StartTick` | `1000000` | Initial synthetic `qwTimer` value |
| `-Wid` | `0` | Counter `wID` |
| `-SecurityProto` | `sasl_plaintext` | Kafka security protocol |
| `-SaslMechanism` | `PLAIN` | SASL mechanism |
| `-User` / `-Pass` | `kv8producer` / `kv8secret` | SASL credentials |

**Pass criteria:** `kv8verify` exits with code `0` and prints
`OVERALL: PASS`.

---

### 6.4 Linux test scripts

Both PowerShell scripts have a Bash counterpart for Linux:

```bash
# End-to-end smoke test
./scripts/test_kafka_e2e.sh
./scripts/test_kafka_e2e.sh --teardown
./scripts/test_kafka_e2e.sh --skip-build
```

The Bash script accepts `--teardown` and `--skip-build` flags that mirror
the PowerShell `-TearDown` and `-SkipBuild` switches. The timestamp
integrity test (`test_ts_e2e.ps1`) currently has a Windows-only script;
run `kv8probe` and `kv8verify` directly on Linux using the CLI arguments
documented in section 6.

---

## 7. Kafka Topics and Registry

This chapter describes the exact naming scheme, binary wire formats, and
discovery protocol used by the Kv8 Kafka sink and the consumer tools.

---

### 7.1 Channel prefix and topic-name sanitization

The channel name defines the top-level namespace for all topics produced
in a run. Kafka topic names may only contain `[a-zA-Z0-9._-]`; any `/`
character in the user-supplied prefix is automatically replaced with `.`
before the first topic is created.

Examples:

| User-supplied prefix | Sanitized form stored internally |
|----------------------|----------------------------------|
| `kv8` | `kv8` |
| `kv8/test` | `kv8.test` |
| `myapp/prod/node-01` | `myapp.prod.node-01` |

Consumer tools (`kv8cli`) apply the same substitution to the `--channel`
argument, so `--channel kv8/test` and `--channel kv8.test` are equivalent.

---

### 7.2 Session identity

Every time a kv8 producer connects to the Kafka sink it generates a unique
**session ID** with the format:

```
YYYYMMDDTHHMMSSZ-PPPP-RRRR
```

| Part | Width | Source |
|------|-------|--------|
| `YYYYMMDDTHHMMSSZ` | 16 chars | UTC wall-clock second (ISO-8601 compact) |
| `PPPP` | 4 hex digits | Lower 16 bits of the process ID |
| `RRRR` | 4 hex digits | 16-bit value derived from the high-resolution timer |

Example: `20260222T143015Z-1A3F-7C02`

The session ID and the sanitized prefix are joined with `.` to build the
**session path**, which acts as the common prefix for all session-scoped
topics:

```
<sanitized-prefix>.<session-id>
```

Example: `kv8.test.20260222T143015Z-1A3F-7C02`

The session ID can be overridden at runtime via the `/KV8.session=<id>`
argument if a fixed or externally assigned identifier is required.

---

### 7.3 Topic hierarchy

A complete channel produces the following topics:

```
<prefix>._registry                                          -- channel-level (shared by all sessions)
<prefix>.<session>._log                                     -- session trace log
<prefix>.<session>._ctl                                     -- bidirectional feed control (JSON)
<prefix>.<session>.d.<channelID_08X>                       -- group virtual topic (GROUP record only; no data)
<prefix>.<session>.d.<channelID_08X>.<counterID_04X>       -- telemetry data (one per counter)
```

Concrete example with prefix `kv8.test`, session `20260222T143015Z-1A3F-7C02`,
and two counter groups (channelIDs 0 and 1) each containing two counters:

```
kv8.test._registry
kv8.test.20260222T143015Z-1A3F-7C02._log
kv8.test.20260222T143015Z-1A3F-7C02._ctl
kv8.test.20260222T143015Z-1A3F-7C02.d.00000000
kv8.test.20260222T143015Z-1A3F-7C02.d.00000000.0000
kv8.test.20260222T143015Z-1A3F-7C02.d.00000000.0001
kv8.test.20260222T143015Z-1A3F-7C02.d.00000001
kv8.test.20260222T143015Z-1A3F-7C02.d.00000001.0000
kv8.test.20260222T143015Z-1A3F-7C02.d.00000001.0001
```

Key properties of each topic type:

| Topic | Scope | Kafka cleanup policy | Purpose |
|-------|-------|---------------------|--------|
| `_registry` | Channel (shared) | `compact` | Counter and group metadata; one message per counter or group |
| `_log` | Session | `delete` (retention: 168 h default) | session trace log in binary kv8 packet format |
| `_ctl` | Session | `delete` | ENABLE/DISABLE feed-control commands (JSON format) |
| `d.<channelID_08X>` | Session | `compact` | Group virtual topic (GROUP registry record only; no data messages) |
| `d.<channelID_08X>.<counterID_04X>` | Session | `delete` (retention: 168 h default) | Binary `Kv8TelValue` telemetry samples for one counter |

The `_registry` topic is **not** session-scoped. It lives at
`<prefix>._registry` so that a consumer only needs to subscribe to that
one well-known address to discover every session and every data topic
ever produced under that channel, across all time.

---

### 7.4 Data topic naming: per-counter layout

Each telemetry counter gets its own dedicated Kafka topic. The topic name
suffix encodes both the kv8 channel ID (32-bit, assigned sequentially from 0
at process startup) and the counter ID (16-bit `wID` within that channel),
formatted as zero-padded uppercase hex:

```
d.<channelID_08X>.<counterID_04X>   e.g.  d.00000000.0003
```

In addition, each counter group has a **group virtual topic** whose name is
the channel ID alone with no counter suffix:

```
d.<channelID_08X>   e.g.  d.00000000
```

The group virtual topic carries exactly one Kafka message: the GROUP
registry record (timing anchor fields `qwTimerFrequency`, `qwTimerValue`,
`dwTimeHi`, `dwTimeLo`). No `Kv8TelValue` data messages are ever written
to it.

This design makes every counter independently consumable: a downstream
consumer can subscribe to a single `d.<ch>.<ctr>` topic to receive exactly
one counter's samples without any `wID`-based filtering. The `channelID` and
`counterID` are also encoded in the registry record key and in the `pTopic`
tail of each counter record, so consumers can recover the full topic name
from the registry alone without scanning broker metadata.

---

### 7.5 Registry record binary format

The `_registry` topic uses Kafka log-compaction. Each message has a
composite key (hash + counterID, 6 bytes) and a variable-length binary
payload.

```
Bytes  Field             Type        Notes
-----  ----------------  ----------  ------------------------------------------
 0-3   dwChannelID       uint32      kv8 channel ID (sequential index from 0).
                                     Replaces the former FNV-1a hash field.
                                     Combined with wCounterID this uniquely
                                     identifies both the counter and its topic.
 4-5   wCounterID        uint16      Counter ID within the group.
                                     0xFFFF = group-level record.
                                     0xFFFE = log-topic announcement record.
 6-7   wFlags            uint16      Bit 0: counter enabled (1) / disabled (0).
                                     Bits 1-15: reserved, must be 0.
 8-15  dbMin             double      Lower bound of the counter value range.
16-23  dbAlarmMin        double      Low alarm threshold.
24-31  dbMax             double      Upper bound of the counter value range.
32-39  dbAlarmMax        double      High alarm threshold.
40-41  wNameLen          uint16      Byte length of the UTF-8 name that follows.
42-43  wTopicLen         uint16      Byte length of the full data topic name.
44-45  wVersion          uint16      Record format version; must equal 2.
46-47  wPad              uint16      Reserved, must be 0.
48-55  qwTimerFrequency  uint64      QPC ticks per second (group records only;
                                     0 in counter records).
56-63  qwTimerValue      uint64      QPC tick captured at channel open (group
                                     records only; 0 in counter records).
64-67  dwTimeHi          uint32      FILETIME.dwHighDateTime at channel open
                                     (group records only; 0 in counter records).
68-71  dwTimeLo          uint32      FILETIME.dwLowDateTime  at channel open
                                     (group records only; 0 in counter records).
                         ---         Variable-length tail:
 72+   pName[wNameLen]   uint8[]     UTF-8 counter or group name.
 72+wNameLen pTopic[wTopicLen] uint8[] Full data topic name.
                                     For counter records: the counter's own
                                     dedicated topic, e.g.
                                     kv8.test.20260222T...d.00000000.0000
                                     For group records: the group virtual
                                     topic, e.g.
                                     kv8.test.20260222T...d.00000000
```

All fields are **little-endian**. The struct is packed with 2-byte
alignment (`#pragma pack(2)`).

**Record types distinguished by `wCounterID`:**

| `wCounterID` value | Record type | Significant extra fields |
|--------------------|-------------|-------------------------|
| `0x0000`..`0xFFFD` | Counter record | `dbMin/Max`, `dbAlarmMin/Max`, `wFlags`; `qwTimerFrequency` = 0 |
| `0xFFFF` | Group record | `qwTimerFrequency`, `qwTimerValue`, `dwTimeHi/Lo`; ranges = 0 |
| `0xFFFE` | Log-topic announcement | `wNameLen` = session ID length, `wTopicLen` = log topic name length; all numeric fields = 0 |

**How consumers use registry records:**

1. Read all messages from `<prefix>._registry` from offset 0 (log-compacted;
   last value wins per key).
2. Group records (`wCounterID == 0xFFFF`) provide the clock anchor:
   `qwTimerFrequency` (ticks/second), `qwTimerValue` (tick at channel open),
   and the FILETIME at channel open. These allow conversion of any `qwTimer`
   value in a `Kv8TelValue` message to an absolute UTC timestamp:
   ```
   elapsed_ticks = qwTimer - qwTimerValue
   elapsed_seconds = elapsed_ticks / qwTimerFrequency
   wall_clock = FILETIME_to_UTC(dwTimeHi, dwTimeLo) + elapsed_seconds
   ```
3. Counter records map `(channelID, wCounterID)` to a human-readable name
   and give consumers the exact per-counter data topic (`pTopic`) that
   carries this counter's samples exclusively. No `wID` filtering is needed:
   every message on `d.<ch>.<ctr>` belongs to that counter alone.
4. Log-topic announcement records (`wCounterID == 0xFFFE`) give consumers
   the full name of the `_log` topic without a broker metadata scan.

---

### 7.6 Data message format

Every message on a `d.<channelID_08X>.<counterID_04X>` topic belongs
exclusively to that one counter. Each message has:

- **Key**: `wID` of the counter encoded as 2 bytes, big-endian.
- **Payload**: one `Kv8TelValue` struct (22 bytes, `#pragma pack(2)`).

```
Bytes  Field          Type      Notes
-----  -------------  --------  --------------------------------------------
 0-3   dwBits         uint32    Extension header.
                                Bits  4:0  = 2 (KV8_PACKET_TYPE_TELEMETRY)
                                Bits  9:5  = 2 (KV8_TEL_TYPE_VALUE)
                                Bits 31:10 = sizeof(Kv8TelValue) in bytes
 4-5   wID            uint16    Counter ID within the group (matches registry).
 6-7   wSeqN          uint16    Per-counter sequence number, wraps at 65535.
 8-15  qwTimer        uint64    Source high-resolution timer tick.
                                Convert to UTC using the group registry record.
16-23  dbValue        double    Counter value.
```

The `wSeqN` field increments per counter and wraps at 65536. For sessions
with more than 65536 samples per counter, consumers must unwrap the
sequence number using the message arrival order (see `kv8verify` for a
reference implementation).

Both the struct and all Kafka message keys use **little-endian** byte
order except the 2-byte key (which is big-endian per the kv8 wire
convention).

---

### 7.7 Topic and session discovery algorithm

Consumers (`kv8cli`) use the following sequence to discover
all active sessions and data topics without any broker-side configuration:

```
1. Create a Kafka consumer configured with:
      auto.offset.reset = earliest
      enable.partition.eof = true

2. Fetch broker metadata to list all topics.

3. Filter: keep topics whose name starts with "<sanitized-prefix>."

4. Subscribe to "<sanitized-prefix>._registry" and consume from offset 0.
   For each registry message:
     a. wCounterID == 0xFFFF  -> store group record (name, clock anchor,
                                 group virtual topic from pTopic tail).
     b. wCounterID == 0xFFFE  -> store log topic name for this session.
     c. 0x0000..0xFFFD        -> store counter name, range, flags, and the
                                 per-counter data topic from pTopic tail.

5. Build the set of data topics from the pTopic fields of all counter
   and group records.

6. Subscribe to all discovered data + log topics.

7. Periodically (every few seconds) repeat steps 2-6 to detect new
   sessions added after the consumer started.
```

This design means consumers need no prior knowledge of session IDs;
the shared `_registry` topic is the single discovery point.

---

### 7.8 Example: topic layout for two counter groups

Assume the producer is launched with:

```
/KV8.channel=myapp/prod
/KV8.session=20260222T090000Z-0001-ABCD
```

And the application opens two telemetry channels in order:
- Channel 0: `"SensorGroup"` (counters: temperature wID=0, pressure wID=1, humidity wID=2)
- Channel 1: `"MotorGroup"` (counters: rpm wID=0, torque wID=1)

Resulting topics on the broker:

```
myapp.prod._registry
myapp.prod.20260222T090000Z-0001-ABCD._log
myapp.prod.20260222T090000Z-0001-ABCD._ctl
myapp.prod.20260222T090000Z-0001-ABCD.d.00000000
myapp.prod.20260222T090000Z-0001-ABCD.d.00000000.0000
myapp.prod.20260222T090000Z-0001-ABCD.d.00000000.0001
myapp.prod.20260222T090000Z-0001-ABCD.d.00000000.0002
myapp.prod.20260222T090000Z-0001-ABCD.d.00000001
myapp.prod.20260222T090000Z-0001-ABCD.d.00000001.0000
myapp.prod.20260222T090000Z-0001-ABCD.d.00000001.0001
```

The `_registry` topic would contain (after compaction):

```
Key: 00000000-FFFF  Value: group record for "SensorGroup"   (qwTimerFrequency, anchor, topic=...d.00000000)
Key: 00000000-0000  Value: counter record for "temperature" (range, topic=...d.00000000.0000)
Key: 00000000-0001  Value: counter record for "pressure"    (range, topic=...d.00000000.0001)
Key: 00000000-0002  Value: counter record for "humidity"    (range, topic=...d.00000000.0002)
Key: 00000001-FFFF  Value: group record for "MotorGroup"    (qwTimerFrequency, anchor, topic=...d.00000001)
Key: 00000001-0000  Value: counter record for "rpm"         (range, topic=...d.00000001.0000)
Key: 00000001-0001  Value: counter record for "torque"      (range, topic=...d.00000001.0001)
Key: 00000000-FFFE  Value: log-topic announcement           (topic=...._log)
```

A consumer starting from scratch reads the `_registry` topic, recovers
all group clock anchors and per-counter topic names, then subscribes to
all five counter topics and the log topic -- without ever scanning broker
metadata for session IDs or counter IDs.

---

### 7.9 Default producer configuration

The sink uses these defaults:

| Parameter | Default | CLI override |
|-----------|---------|-------------|
| `bootstrap.servers` | `localhost:19092` | `/KV8.brokers=` |
| `acks` | `all` | _(compile-time)_ |
| `linger.ms` | `5` | _(compile-time)_ |
| `batch.size` | `65536` | _(compile-time)_ |
| `compression.type` | `lz4` | _(compile-time)_ |
| `security.protocol` | `plaintext` | `/KV8.security-protocol=` |
| Partitions (auto-create) | `1` | _(compile-time)_ |
| Replication factor | `1` | _(compile-time)_ |
| Data topic retention | `168 hours` | _(compile-time)_ |
| EOS / idempotent | `0` (idempotent) | _(compile-time)_ |
| Producer queue depth | `500000 msgs` | _(compile-time)_ |
| Exit drain timeout | `5000 ms` | _(compile-time)_ |

---

## 8. UDT Telemetry

User Defined Types (UDTs) let you transmit structured, multi-field telemetry
samples as a single atomic unit. Each field becomes an individual counter on
the consumer side; all fields in one sample share the same timestamp so
kv8scope can correlate them on a single time axis.

This chapter covers the full producer-side and consumer-side workflow for UDTs.

---

### 8.1 Concepts

| Term | Meaning |
|------|---------|
| **Schema** | A named description of all fields in a UDT: field names, types, and value ranges. Stored as a compact JSON string. |
| **UdtFeed** | A C++ object (`kv8log::UdtFeed<T>`) that holds the runtime handle and sequence state for one named telemetry feed. |
| **Sample** | One populated `Kv8UDT_<Name>` struct submitted to a `UdtFeed`. Each field is flattened into a `Kv8TelValue` message on its own Kafka topic. |
| **Flat schema** | A schema whose fields are all primitive scalar types. No nesting. |
| **Composite schema** | A schema that embeds other named schemas. The consumer resolves the type hierarchy from the registry. |
| **Payload limit** | `KV8_UDT_MAX_PAYLOAD` = 240 bytes. The packed struct must fit within this limit; a `static_assert` enforces it at compile time. |

---

### 8.2 Include and compile-definition

UDT support lives in `kv8log`:

```cpp
// In your source file or a shared header:
#include "kv8log/KV8_UDT.h"
```

The macros that register schemas and emit samples are **active only when
`KV8_LOG_ENABLE` is defined**. The struct definitions and `static_assert`
size checks are always emitted regardless, so code that constructs UDT
structs compiles cleanly in both enabled and disabled builds.

In CMake:

```cmake
# Enable UDT telemetry for a target:
target_compile_definitions(my_app PRIVATE KV8_LOG_ENABLE)
target_link_libraries(my_app PRIVATE kv8log_runtime kv8 kv8util)
```

---

### 8.3 Field type tokens

The type token used in schema macros maps directly to a C++ type:

| Token | C++ type | Size |
|-------|----------|------|
| `i8` | `int8_t` | 1 B |
| `u8` | `uint8_t` | 1 B |
| `i16` | `int16_t` | 2 B |
| `u16` | `uint16_t` | 2 B |
| `i32` | `int32_t` | 4 B |
| `u32` | `uint32_t` | 4 B |
| `i64` | `int64_t` | 8 B |
| `u64` | `uint64_t` | 8 B |
| `f32` | `float` | 4 B |
| `f64` | `double` | 8 B |

All struct members are tightly packed (`pack(1)`) so the byte offsets match
the sequential layout parsed by the consumer. Do not add explicit padding
members; the macro framework manages alignment automatically.

---

### 8.4 Defining a flat schema

A flat schema contains only primitive scalar fields. Use the `X-macro`
pattern: a helper macro that accepts four callback tokens `(F, FN, E, EN)`,
then pass it to `KV8_UDT_DEFINE`.

```cpp
// MyUdts.h
#include "kv8log/KV8_UDT.h"

// Field-list helper:
//   F (type, cname, min, max)             -- primitive field; name == cname
//   FN(type, cname, display_name, mn, mx) -- primitive field; explicit display label
//   E (TypeToken, cname)                  -- embedded flat UDT (not used here)
//   EN(prefix, Name, cname)               -- embedded scoped UDT (not used here)

#define KV8_ENV_FIELDS(F, FN, E, EN)                              \
    FN(f32, temperature_c,  "Temperature (degC)", -40.0f, 85.0f)  \
    FN(f32, humidity_pct,   "Humidity (%)",          0.0f, 100.0f) \
    FN(f32, pressure_hpa,   "Pressure (hPa)",      870.0f, 1084.0f)

KV8_UDT_DEFINE(EnvSensor, KV8_ENV_FIELDS)
```

`KV8_UDT_DEFINE(EnvSensor, KV8_ENV_FIELDS)` generates:

- `struct Kv8UDT_EnvSensor { float temperature_c; float humidity_pct; float pressure_hpa; };`
- `static_assert(sizeof(Kv8UDT_EnvSensor) <= 240);`
- (enabled) `kv8_udt_schema::GetSchema_EnvSensor()` returning the schema JSON string.
- (enabled) `kv8_udt_schema::RegisterDeps_EnvSensor(...)` (no-op for flat schemas).

The `FN` variant attaches a human-readable display label (shown in kv8scope)
that is separate from the C identifier. The `F` variant uses the C identifier
as both the path key and the display label.

---

### 8.5 Defining a composite (nested) schema

Composite schemas embed other named schemas. Use `KV8_UDT_DEFINE_NS` for
schemas that belong to a namespace prefix and `EN(prefix, Name, cname)` to
embed them. The schema name seen by the consumer is `"prefix.Name"`.

This pattern is taken directly from `kv8feeder/UdtFeeds.h` (the aerial
platform telemetry):

```cpp
// -- Leaf schemas (no embedded types) ----------------------------------------

#define KV8_AP_VEC3_FIELDS(F, FN, E, EN)           \
    FN(f64, x, "X", -1e4, 1e4)                      \
    FN(f64, y, "Y", -1e4, 1e4)                      \
    FN(f64, z, "Z", -1e4, 1e4)

KV8_UDT_DEFINE_NS(ap, Vec3, KV8_AP_VEC3_FIELDS)    // struct: Kv8UDT_ap_Vec3
                                                     // schema name: "ap.Vec3"

#define KV8_AP_NAVSTATUS_FIELDS(F, FN, E, EN)                    \
    FN(u8,  gps_fix,    "GPS Fix",        0,     5)               \
    FN(u8,  sats,       "Satellites",     0,    32)               \
    FN(f32, hdop,       "HDOP",           0.5f, 99.0f)            \
    FN(f32, baro_alt_m, "Baro Alt (m)", -500.0f, 9000.0f)

KV8_UDT_DEFINE_NS(ap, NavStatus, KV8_AP_NAVSTATUS_FIELDS)        // schema name: "ap.NavStatus"

// -- Composite: embeds Vec3 and NavStatus ------------------------------------

#define KV8_AP_NAVIGATION_FIELDS(F, FN, E, EN)  \
    EN(ap, Vec3,      velocity_ms)               \
    EN(ap, NavStatus, nav_status)

KV8_UDT_DEFINE_NS(ap, Navigation, KV8_AP_NAVIGATION_FIELDS)      // schema name: "ap.Navigation"

// -- Top-level UDT: embeds Navigation ----------------------------------------

#define KV8_AERINAV_FIELDS(F, FN, E, EN) \
    EN(ap, Navigation, navigation)

KV8_UDT_DEFINE(AerialNav, KV8_AERINAV_FIELDS)                    // struct: Kv8UDT_AerialNav
```

**Rules for composite schemas:**
- Always define leaf schemas before composites that embed them.
- Use `EN(prefix, Name, cname)` (not `E`) when embedding a scoped schema;
  `EN` emits the type reference as `"prefix.Name"`, which is exactly what
  the consumer expects.
- The registry pre-registers all embedded sub-schemas and the top-level schema
  in dependency order. `RegisterDeps_<Name>` handles this automatically.
- Each namespace prefix must be a valid C identifier token (no dots or slashes).

---

### 8.6 Payload size limit

Every `Kv8UDT_*` struct must fit within `KV8_UDT_MAX_PAYLOAD` (240 bytes).
A `static_assert` fires at compile time if the limit is exceeded:

```
error: static_assert failed: "Kv8UDT_MyType exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)"
```

When a single logical object exceeds this limit, split it into multiple
time-synchronised UDTs (as `kv8feeder` does for the aerial platform):

```
AerialNav    (58 B)  -- position, velocity, nav status
AerialAtt    (80 B)  -- orientation quaternion, angular rate, acceleration
AerialMotors (20 B)  -- battery voltage, 4 motor RPMs
```

All three feeds are emitted with the same captured Unix-epoch timestamp so
kv8scope correlates them on a shared time axis (see section 8.9).

---

### 8.7 Declaring a feed

Declare a `UdtFeed` inside the function or thread that will produce samples.
The declaration must be `static` so the runtime registers the feed exactly once:

```cpp
// Default channel (uses the /KV8.channel configured at startup)
KV8_UDT_FEED(my_feed, EnvSensor, "Environment/Sensor");

// Explicit channel object
KV8_UDT_FEED_CH(my_channel, my_feed, EnvSensor, "Environment/Sensor");

// Scoped (NS) schema on default channel
KV8_UDT_FEED_NS(my_feed, ap, Navigation, "Aerial/Navigation");

// Scoped (NS) schema on explicit channel
KV8_UDT_FEED_NS_CH(my_channel, my_feed, ap, Navigation, "Aerial/Navigation");
```

| Macro | When to use |
|-------|-------------|
| `KV8_UDT_FEED` | Flat schema, default channel. Most common. |
| `KV8_UDT_FEED_CH` | Flat schema, explicit `Channel` object. |
| `KV8_UDT_FEED_NS` | Scoped schema (defined with `KV8_UDT_DEFINE_NS`), default channel. |
| `KV8_UDT_FEED_NS_CH` | Scoped schema, explicit `Channel` object. |

The `display_name` string sets the hierarchical path shown in kv8scope.
Slashes produce a tree: `"Aerial/Navigation"` appears under the `Aerial`
group.

**Thread safety:** Each `UdtFeed` object must be used from a single thread.
If multiple threads need to emit the same UDT type, declare a separate feed
per thread (each with its own display name).

---

### 8.8 Emitting samples (auto-timestamp)

Populate the struct and call `KV8_UDT_ADD`:

```cpp
#include "kv8log/KV8_UDT.h"
#include "MyUdts.h"          // contains KV8_UDT_DEFINE(EnvSensor, ...)
#include <cmath>

void SensorThread(const std::atomic<bool>& stop)
{
    // Static feed declaration -- registered once on first Add().
    KV8_UDT_FEED(env_feed, EnvSensor, "Environment/Sensor");

    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::seconds(1);
    auto next = Clock::now();

    while (!stop.load(std::memory_order_relaxed))
    {
        next += interval;
        std::this_thread::sleep_until(next);

        Kv8UDT_EnvSensor s;
        s.temperature_c = ReadTemperature();
        s.humidity_pct  = ReadHumidity();
        s.pressure_hpa  = ReadPressure();

        KV8_UDT_ADD(env_feed, s);    // timestamp captured inside Add()
    }
}
```

`KV8_UDT_ADD` is a macro that expands to `env_feed.Add(s)`. The runtime
captures a high-resolution timestamp at the moment of the call. This is the
correct choice for feeds where the sample is valid at the moment of emission.

---

### 8.9 Emitting samples with a caller-supplied timestamp

Use `KV8_UDT_ADD_TS` when you need to correlate multiple feeds to a single
wall-clock instant, or when the sample was captured before the emit call:

```cpp
// Capture a shared Unix-epoch nanosecond timestamp once,
// then emit all three feeds with the same value.
const uint64_t ts_ns = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

KV8_UDT_ADD_TS(aerial_nav,    nav,    ts_ns);
KV8_UDT_ADD_TS(aerial_att,    att,    ts_ns);
KV8_UDT_ADD_TS(aerial_motors, motors, ts_ns);
```

kv8scope uses this shared timestamp to plot all three feeds on the same
time axis, making cross-feed correlation exact. The timestamp unit is
**nanoseconds since Unix epoch** (same as `std::chrono::system_clock`).

---

### 8.10 Enabling and disabling a feed at runtime

A feed can be paused and resumed without restarting the producer:

```cpp
KV8_UDT_DISABLE(env_feed);   // stop emitting; Add/AddTs become no-ops
// ... reconfigure, recalibrate ...
KV8_UDT_ENABLE(env_feed);    // resume
```

`Enable` and `Disable` write to the session `._ctl` topic so all
consumers observing the channel see the state change.

---

### 8.11 Complete producer example (flat schema)

The following self-contained example mirrors the WeatherStation feed in
`kv8feeder/UdtFeeds.cpp`.

**MyUdts.h:**

```cpp
#pragma once
#include "kv8log/KV8_UDT.h"

#define KV8_WS_FIELDS(F, FN, E, EN)                                        \
    FN(f32, temperature_c, "Temperature (degC)", -40.0f,  85.0f)           \
    FN(f32, humidity_pct,  "Humidity (%)",          0.0f, 100.0f)          \
    FN(f32, pressure_hpa,  "Pressure (hPa)",      870.0f, 1084.0f)         \
    FN(f32, wind_speed_ms, "Wind Speed (m/s)",      0.0f,  60.0f)          \
    FN(f32, wind_dir_deg,  "Wind Dir (deg)",         0.0f, 360.0f)         \
    FN(f32, rain_mm_h,     "Rainfall (mm/h)",        0.0f, 200.0f)         \
    FN(f32, uv_index,      "UV Index",               0.0f,  16.0f)

KV8_UDT_DEFINE(WeatherStation, KV8_WS_FIELDS)
```

**main.cpp:**

```cpp
#include "MyUdts.h"
#include <kv8util/Kv8AppUtils.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

static void WeatherThread(const std::atomic<bool>& stop)
{
    KV8_UDT_FEED(wx, WeatherStation, "Environment/WeatherStation");

    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::seconds(1);
    auto next = Clock::now();
    uint64_t n = 0;

    while (!stop.load(std::memory_order_relaxed))
    {
        next += interval;
        std::this_thread::sleep_until(next);
        ++n;

        const float t = static_cast<float>(n) * 0.01f;

        Kv8UDT_WeatherStation s;
        s.temperature_c = 20.0f + 10.0f * sinf(t);
        s.humidity_pct  = 62.0f + 18.0f * sinf(t * 0.7f + 1.0f);
        s.pressure_hpa  = 1013.0f + 5.0f * sinf(t * 0.3f);
        s.wind_speed_ms =  8.0f +  6.0f * sinf(t * 1.3f + 2.0f);
        s.wind_dir_deg  = fmodf(s.wind_speed_ms * 20.0f + t * 15.0f, 360.0f);
        s.rain_mm_h     = (s.wind_speed_ms > 12.0f)
                          ? (s.wind_speed_ms - 12.0f) * 3.0f : 0.0f;
        s.uv_index      = 5.0f + 4.0f * sinf(t * 0.5f);

        KV8_UDT_ADD(wx, s);
    }
}

int main()
{
    kv8util::AppSignal::Install();
    // kv8log runtime is initialised by /KV8.* command-line args or
    // by calling kv8log_open() directly before starting threads.

    std::atomic<bool> stop{false};
    std::thread t(WeatherThread, std::ref(stop));

    while (kv8util::AppSignal::IsRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    stop.store(true);
    t.join();
    return 0;
}
```

CMakeLists.txt snippet:

```cmake
add_executable(my_weather_app main.cpp)
target_compile_definitions(my_weather_app PRIVATE KV8_LOG_ENABLE)
target_link_libraries(my_weather_app PRIVATE kv8log_runtime kv8 kv8util)
```

---

### 8.12 Complete producer example (composite schema)

This example reproduces the aerial platform split from `kv8feeder/UdtFeeds.h`
and `kv8feeder/UdtFeeds.cpp` at a level of detail that shows how to build
and emit nested UDT schemas.

**AerialUdts.h** (schema definitions):

```cpp
#pragma once
#include "kv8log/KV8_UDT.h"

// Leaf schemas
#define KV8_AP_VEC3_FIELDS(F, FN, E, EN) \
    FN(f64, x, "X", -1e4, 1e4) FN(f64, y, "Y", -1e4, 1e4) FN(f64, z, "Z", -1e4, 1e4)
KV8_UDT_DEFINE_NS(ap, Vec3, KV8_AP_VEC3_FIELDS)

#define KV8_AP_QUAT_FIELDS(F, FN, E, EN) \
    FN(f64, w, "W", -1.0, 1.0) FN(f64, x, "X", -1.0, 1.0) \
    FN(f64, y, "Y", -1.0, 1.0) FN(f64, z, "Z", -1.0, 1.0)
KV8_UDT_DEFINE_NS(ap, Quat, KV8_AP_QUAT_FIELDS)

#define KV8_AP_NAVSTATUS_FIELDS(F, FN, E, EN)                    \
    FN(u8,  gps_fix,    "GPS Fix",         0,     5)             \
    FN(u8,  sats,       "Satellites",      0,    32)             \
    FN(f32, hdop,       "HDOP",            0.5f, 99.0f)          \
    FN(f32, baro_alt_m, "Baro Alt (m)", -500.0f, 9000.0f)
KV8_UDT_DEFINE_NS(ap, NavStatus, KV8_AP_NAVSTATUS_FIELDS)

// First-level composites
#define KV8_AP_NAV_FIELDS(F, FN, E, EN) \
    EN(ap, Vec3,      velocity_ms) EN(ap, NavStatus, nav_status)
KV8_UDT_DEFINE_NS(ap, Navigation, KV8_AP_NAV_FIELDS)

#define KV8_AP_ATT_FIELDS(F, FN, E, EN) \
    EN(ap, Quat, orientation) EN(ap, Vec3, angular_rate_rads) EN(ap, Vec3, accel_ms2)
KV8_UDT_DEFINE_NS(ap, Attitude, KV8_AP_ATT_FIELDS)

// Top-level UDTs (one feed each)
#define KV8_AERIAL_NAV_FIELDS(F, FN, E, EN)  EN(ap, Navigation, navigation)
KV8_UDT_DEFINE(AerialNav, KV8_AERIAL_NAV_FIELDS)

#define KV8_AERIAL_ATT_FIELDS(F, FN, E, EN)  EN(ap, Attitude, attitude)
KV8_UDT_DEFINE(AerialAtt, KV8_AERIAL_ATT_FIELDS)

#define KV8_AERIAL_MOTORS_FIELDS(F, FN, E, EN)                          \
    FN(f32, battery_v,  "Battery (V)",    6.0f,  25.2f)                 \
    FN(f32, motor1_rpm, "Motor 1 (RPM)",  0.0f, 12000.0f)               \
    FN(f32, motor2_rpm, "Motor 2 (RPM)",  0.0f, 12000.0f)               \
    FN(f32, motor3_rpm, "Motor 3 (RPM)",  0.0f, 12000.0f)               \
    FN(f32, motor4_rpm, "Motor 4 (RPM)",  0.0f, 12000.0f)
KV8_UDT_DEFINE(AerialMotors, KV8_AERIAL_MOTORS_FIELDS)
```

**AerialThread.cpp** (emission loop at 1 kHz):

```cpp
#include "AerialUdts.h"
#include <chrono>
#include <cmath>

void RunAerialPlatform(const std::atomic<bool>& stop)
{
    // Three feeds -- each is a separate Kafka counter group.
    KV8_UDT_FEED(aerial_nav,    AerialNav,    "Aerial/Navigation");
    KV8_UDT_FEED(aerial_att,    AerialAtt,    "Aerial/Attitude");
    KV8_UDT_FEED(aerial_motors, AerialMotors, "Aerial/Motors");

    using Clock    = std::chrono::steady_clock;
    using SysClock = std::chrono::system_clock;
    const auto interval = std::chrono::microseconds(1000); // 1 kHz
    auto next = Clock::now();
    double theta = 0.0;
    float  battery = 24.0f;

    while (!stop.load(std::memory_order_relaxed))
    {
        next += interval;
        std::this_thread::sleep_until(next);
        theta += 0.1 * 0.001;  // omega * dt

        // Single shared timestamp -- all three feeds correlated in kv8scope.
        const uint64_t ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                SysClock::now().time_since_epoch()).count());

        // Navigation
        Kv8UDT_AerialNav nav{};
        nav.navigation.velocity_ms.x         = -100.0 * sin(theta);
        nav.navigation.velocity_ms.y         =  100.0 * cos(theta);
        nav.navigation.velocity_ms.z         = 0.0;
        nav.navigation.nav_status.gps_fix    = 3;
        nav.navigation.nav_status.sats       = 12;
        nav.navigation.nav_status.hdop       = 1.2f;
        nav.navigation.nav_status.baro_alt_m = 50.0f;
        KV8_UDT_ADD_TS(aerial_nav, nav, ts_ns);

        // Attitude
        Kv8UDT_AerialAtt att{};
        att.attitude.orientation.w         = cos(theta * 0.5);
        att.attitude.orientation.z         = sin(theta * 0.5);
        att.attitude.angular_rate_rads.z   = 0.1;
        att.attitude.accel_ms2.x           = -100.0 * 0.1 * 0.1 * cos(theta);
        att.attitude.accel_ms2.z           = 9.81;
        KV8_UDT_ADD_TS(aerial_att, att, ts_ns);

        // Motors
        battery -= 0.00001f;
        if (battery < 12.0f) battery = 24.0f;
        Kv8UDT_AerialMotors motors{};
        motors.battery_v  = battery;
        motors.motor1_rpm = 2800.0f;
        motors.motor2_rpm = 2800.0f;
        motors.motor3_rpm = 2800.0f;
        motors.motor4_rpm = 2800.0f;
        KV8_UDT_ADD_TS(aerial_motors, motors, ts_ns);
    }
}
```

---

### 8.13 Consumer side: reading UDT samples

From the consumer perspective, UDT fields are individual `Kv8TelValue`
counters. Each field has its own Kafka topic. The schema JSON stored in
the registry is used by kv8scope to reconstruct the original type hierarchy
in the UI, but the wire format is identical to any other telemetry counter.

The session registry carries schema records alongside counter records.
`DiscoverSessions()` returns a `SessionMeta` whose `topicToCounter` map
includes every UDT field flattened to a path like `"Aerial/Navigation.navigation.velocity_ms.x"`.

**Subscribing to UDT fields:**

```cpp
auto consumer = kv8::IKv8Consumer::Create(cfg);
auto sessions = consumer->DiscoverSessions("kv8.test");
auto& [prefix, sm] = *sessions.begin();

// Subscribe to all topics -- includes both scalar and UDT-field topics.
for (auto& topic : sm.dataTopics)
    consumer->Subscribe(topic);

while (kv8util::AppSignal::IsRunning())
{
    consumer->Poll(200,
        [&](std::string_view topic,
            const void* pPayload, size_t cbPayload,
            int64_t tsKafkaMs)
        {
            if (cbPayload < sizeof(kv8::Kv8TelValue)) return;

            kv8::Kv8TelValue val;
            memcpy(&val, pPayload, sizeof(val));

            auto it = sm.topicToCounter.find(std::string(topic));
            const char* name = (it != sm.topicToCounter.end())
                               ? it->second.sName.c_str() : "?";

            printf("%-50s  seq=%5u  val=%.4f\n",
                   name, val.wSeqN, val.dbValue);
        });

    if (kv8util::CheckEscKey())
        kv8util::AppSignal::RequestStop();
}
consumer->Stop();
```

There is no special decoding step: each `Kv8TelValue.dbValue` is the
converted scalar value of that one field. Integer types (`i8`, `u16`, etc.)
are widened to `double` when the sample is emitted; the schema `min`/`max`
values provide range context to the consumer UI.

**Identifying UDT fields:**

The counter name stored in the registry for a UDT field uses dot-separated
path notation derived from the embedding chain:

```
feed display name / field path
```

Example for the AerialNav feed declared as `"Aerial/Navigation"`:

| Field | Counter name in registry |
|-------|--------------------------|
| `velocity_ms.x` | `Aerial/Navigation.navigation.velocity_ms.x` |
| `velocity_ms.y` | `Aerial/Navigation.navigation.velocity_ms.y` |
| `nav_status.gps_fix` | `Aerial/Navigation.navigation.nav_status.gps_fix` |
| `nav_status.baro_alt_m` | `Aerial/Navigation.navigation.nav_status.baro_alt_m` |

The display label (from `FN`) overrides the path segment label in the
kv8scope tree view but the dot-path key in the registry always uses the
C identifier name supplied to `FN`.

---

### 8.14 Macro reference summary

**Schema definition:**

| Macro | Purpose |
|-------|---------|
| `KV8_UDT_DEFINE(Name, FIELDS)` | Define a flat or composite UDT struct + schema. Schema name = `"Name"`. |
| `KV8_UDT_DEFINE_NS(prefix, Name, FIELDS)` | Same, with namespace prefix. Schema name = `"prefix.Name"`. Struct = `Kv8UDT_prefix_Name`. |

**Field-list callbacks** (used inside `FIELDS` helpers):

| Token | Signature | Purpose |
|-------|-----------|---------|
| `F` | `(type, cname, min, max)` | Primitive field; display name = C identifier. |
| `FN` | `(type, cname, display_name, min, max)` | Primitive field; explicit human-readable display name. |
| `E` | `(TypeToken, cname)` | Embed a flat UDT by token. Schema reference = `"TypeToken"`. |
| `EN` | `(prefix, Name, cname)` | Embed a scoped UDT. Schema reference = `"prefix.Name"`. |

**Feed declaration:**

| Macro | Purpose |
|-------|---------|
| `KV8_UDT_FEED(var, TypeToken, display_name)` | Declare feed for flat UDT on default channel. |
| `KV8_UDT_FEED_CH(ch, var, TypeToken, display_name)` | Flat UDT on explicit channel. |
| `KV8_UDT_FEED_NS(var, prefix, Name, display_name)` | Scoped UDT on default channel. |
| `KV8_UDT_FEED_NS_CH(ch, var, prefix, Name, display_name)` | Scoped UDT on explicit channel. |

**Sample emission:**

| Macro | Purpose |
|-------|---------|
| `KV8_UDT_ADD(var, val)` | Emit sample; timestamp captured at call time. |
| `KV8_UDT_ADD_TS(var, val, ts_ns)` | Emit sample with caller-supplied Unix-epoch nanosecond timestamp. |

**Feed control:**

| Macro | Purpose |
|-------|---------|
| `KV8_UDT_ENABLE(var)` | Resume emission; writes to `._ctl` topic. |
| `KV8_UDT_DISABLE(var)` | Pause emission; `Add`/`AddTs` become no-ops. |

**Struct-only (no schema) -- when `KV8_LOG_ENABLE` is not defined:**

| Macro | Purpose |
|-------|---------|
| `KV8_UDT_BEGIN(Name)` / `KV8_UDT_END(Name)` | Inline struct definition without schema JSON. |
| `KV8_UDT_FIELD(type, cname, mn, mx)` | Struct member inside `BEGIN`/`END`. |
| `KV8_UDT_FIELD_NAMED(type, cname, disp, mn, mx)` | Struct member with display name. |
| `KV8_UDT_EMBED(TypeToken, cname)` | Embed flat UDT member inside `BEGIN`/`END`. |
| `KV8_UDT_EMBED_NS(prefix, Name, cname)` | Embed scoped UDT member inside `BEGIN`/`END`. |

All `KV8_UDT_ADD`, `KV8_UDT_FEED`, and related macros expand to `((void)0)`
or nothing when `KV8_LOG_ENABLE` is not defined, so no additional `#ifdef`
guards are needed in application code.
