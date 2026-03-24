////////////////////////////////////////////////////////////////////////////////
// kv8log/KV8_Log.h -- single user-facing header for the kv8log telemetry API.
//
// Usage
// -----
//   // Enable telemetry (typically via -DKV8_LOG_ENABLE in CMake):
//   #define KV8_LOG_ENABLE
//   #include "kv8log/KV8_Log.h"
//
//   void my_loop() {
//       KV8_TEL(cpu_temp, "system/cpu_temp", -40.0, 120.0);  // declare counter (once per TU)
//       KV8_TEL_ADD(cpu_temp, 36.6);                        // record sample
//   }
//
// Without KV8_LOG_ENABLE all macros expand to nothing / ((void)0).
// The kv8log shared runtime (kv8log.so / kv8log.dll) is loaded lazily at
// first use and is NOT a link-time dependency of the user application.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef KV8_LOG_ENABLE

// ── Compile-out path (default) ──────────────────────────────────────────────
#include <cstdint>  // uint64_t used by KV8_MONO_TO_NS no-op expansion
#define KV8_CHANNEL(ch, name)
#define KV8_TEL(var, name, mn, mx)
#define KV8_TEL_CH(ch, var, name, mn, mx)
#define KV8_TEL_ADD(var, val)            ((void)0)
#define KV8_TEL_ADD_CH(ch, var, val)     ((void)0)
#define KV8_TEL_ADD_TS(var, val, ts_ns)  ((void)0)
#define KV8_TEL_FLUSH()                  ((void)0)
#define KV8_TEL_ENABLE(var)              ((void)0)
#define KV8_TEL_DISABLE(var)             ((void)0)
#define KV8_MONO_TO_NS(mono_ns)          (static_cast<uint64_t>(0))
#define KV8_LOG_CONFIGURE(b, c, u, p)    ((void)0)

#else // ── Enabled ─────────────────────────────────────────────────────────

#include "kv8log/Channel.h"
#include "kv8log/Counter.h"
#include "kv8log/Runtime.h"

// Declare an explicitly named channel (static singleton within the TU).
// 'ch' becomes a kv8log::Channel variable usable with KV8_TEL_CH / KV8_TEL_ADD_CH.
#define KV8_CHANNEL(ch, name) \
    static kv8log::Channel ch(name)

// Declare a counter on the default (per-exe) channel.
// 'var' is a function/TU-scoped static kv8log::Counter.
// 'name' is the human-readable display name shown in kv8scope (e.g. "Numbers/Counter").
#define KV8_TEL(var, name, mn, mx) \
    static kv8log::Counter var(kv8log::DefaultChannel(), (name), (mn), (mx))

// Declare a counter on an explicit Channel object 'ch'.
// 'name' is the human-readable display name shown in kv8scope.
#define KV8_TEL_CH(ch, var, name, mn, mx) \
    static kv8log::Counter var((ch), (name), (mn), (mx))

// Record a sample (auto-timestamp, default channel implied by 'var').
#define KV8_TEL_ADD(var, val) \
    (var).Add(static_cast<double>(val))

// Record a sample (auto-timestamp, explicit channel -- 'ch' is ignored at
// build time because the Counter already holds a reference to its channel;
// the macro exists for source-level symmetry with KV8_TEL_CH).
#define KV8_TEL_ADD_CH(ch, var, val) \
    (var).Add(static_cast<double>(val))

// Record a sample with a caller-supplied timestamp (nanoseconds, Unix epoch).
// Use KV8_MONO_TO_NS() to convert a CLOCK_MONOTONIC / QPC reading first.
#define KV8_TEL_ADD_TS(var, val, ts_ns) \
    (var).AddTs(static_cast<double>(val), static_cast<uint64_t>(ts_ns))

// Convert a CLOCK_MONOTONIC (Linux) or QueryPerformanceCounter (Windows)
// reading expressed in nanoseconds to nanoseconds since Unix epoch, using
// the session anchor established at library init time.
#define KV8_MONO_TO_NS(mono_ns) \
    kv8log::Runtime::MonotonicToNs(static_cast<uint64_t>(mono_ns))

// Block until all enqueued messages have been delivered (or timed out).
#define KV8_TEL_FLUSH() \
    kv8log::Runtime::Flush()

// Enable or disable data collection for a counter at runtime.
// Also writes a ._ctl Kafka message so kv8scope reflects the change.
#define KV8_TEL_ENABLE(var)  (var).Enable()
#define KV8_TEL_DISABLE(var) (var).Disable()

// Optional explicit configuration override (overrides argv / env defaults).
// Call BEFORE the first KV8_TEL_ADD; otherwise init has already occurred.
#define KV8_LOG_CONFIGURE(b, c, u, p) \
    kv8log::Runtime::Configure((b), (c), (u), (p))

#endif // KV8_LOG_ENABLE

// UDT telemetry macros (always included; disabled path expands to no-ops).
#include "kv8log/KV8_UDT.h"
