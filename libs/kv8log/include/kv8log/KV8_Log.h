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

// Trace log macros -- compiled out: zero overhead, no symbol references.
#define KV8_LOG(level_, msg_)            ((void)0)
#define KV8_LOGF(level_, fmt_, ...)      ((void)0)
#define KV8_LOG_DEBUG(msg_)              ((void)0)
#define KV8_LOG_INFO(msg_)               ((void)0)
#define KV8_LOG_WARN(msg_)               ((void)0)
#define KV8_LOG_ERROR(msg_)              ((void)0)
#define KV8_LOG_FATAL(msg_)              ((void)0)
#define KV8_LOGF_DEBUG(fmt_, ...)        ((void)0)
#define KV8_LOGF_INFO(fmt_, ...)         ((void)0)
#define KV8_LOGF_WARN(fmt_, ...)         ((void)0)
#define KV8_LOGF_ERROR(fmt_, ...)        ((void)0)
#define KV8_LOGF_FATAL(fmt_, ...)        ((void)0)

#else // ── Enabled ─────────────────────────────────────────────────────────

#include "kv8log/Channel.h"
#include "kv8log/Counter.h"
#include "kv8log/Runtime.h"

#include <atomic>
#include <cstdint>
#include <cstdio>     // snprintf
#include <kv8/Kv8Types.h>   // Kv8LogLevel, KV8_LOG_FLAG_TEXT, KV8_LOG_MAX_PAYLOAD

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

// ── Trace log macros (Phase L2) ────────────────────────────────────────────
//
// Per-call-site cache: one std::atomic<uint32_t> per macro expansion holds
// the registered site hash.  First call: register (slow path, once); store
// the returned hash; emit.  Subsequent calls: relaxed atomic load + emit.
// Concurrent first calls are safe because registration is idempotent --
// every thread that races writes the same hash into the static.
//
// Source location is stripped to the basename inside the registry record on
// the slow path; the macro passes the full __FILE__ literal so sizeof can be
// taken at compile time.  Thread ID and CPU are captured inside the runtime
// library on the hot path -- they need no platform headers in the user TU.

#define KV8_LOG_IMPL_(level_, fmt_literal_, payload_, plen_) \
    do { \
        static std::atomic<uint32_t> s_kv8_site_(0); \
        uint32_t _kv8_h = s_kv8_site_.load(std::memory_order_relaxed); \
        if (_kv8_h == 0) { \
            _kv8_h = kv8log::Runtime::RegisterLogSite( \
                __FILE__,     static_cast<uint16_t>(sizeof(__FILE__)     - 1), \
                __FUNCTION__, static_cast<uint16_t>(sizeof(__FUNCTION__) - 1), \
                static_cast<uint32_t>(__LINE__), \
                (fmt_literal_), static_cast<uint16_t>(sizeof(fmt_literal_) - 1)); \
            s_kv8_site_.store(_kv8_h, std::memory_order_relaxed); \
        } \
        kv8log::Runtime::Log(_kv8_h, \
            static_cast<uint8_t>(level_), \
            (payload_), (plen_), \
            static_cast<uint8_t>(kv8::KV8_LOG_FLAG_TEXT)); \
    } while (0)

// KV8_LOG -- compile-time string literal, no formatting.
// sizeof(msg_) yields the literal byte count at compile time, so msg_ MUST be
// a string literal (e.g. "stalled").  Passing a runtime const char* will not
// compile.
#define KV8_LOG(level_, msg_) \
    KV8_LOG_IMPL_((level_), (msg_), (msg_), \
                  static_cast<uint16_t>(sizeof(msg_) - 1))

// KV8_LOGF -- printf-style formatting into a 4096-byte stack buffer.
// Text longer than KV8_LOG_MAX_PAYLOAD is silently truncated.
#define KV8_LOGF(level_, fmt_, ...) \
    do { \
        char _kv8_buf[4096]; \
        int _kv8_n = std::snprintf(_kv8_buf, sizeof(_kv8_buf), \
                                   (fmt_), ##__VA_ARGS__); \
        if (_kv8_n > 0) { \
            uint16_t _kv8_len = (_kv8_n < static_cast<int>(kv8::KV8_LOG_MAX_PAYLOAD)) \
                ? static_cast<uint16_t>(_kv8_n) \
                : static_cast<uint16_t>(kv8::KV8_LOG_MAX_PAYLOAD); \
            KV8_LOG_IMPL_((level_), (fmt_), _kv8_buf, _kv8_len); \
        } \
    } while (0)

// Severity-named convenience wrappers.
#define KV8_LOG_DEBUG(msg_) KV8_LOG(static_cast<uint8_t>(kv8::Kv8LogLevel::Debug),   (msg_))
#define KV8_LOG_INFO(msg_)  KV8_LOG(static_cast<uint8_t>(kv8::Kv8LogLevel::Info),    (msg_))
#define KV8_LOG_WARN(msg_)  KV8_LOG(static_cast<uint8_t>(kv8::Kv8LogLevel::Warning), (msg_))
#define KV8_LOG_ERROR(msg_) KV8_LOG(static_cast<uint8_t>(kv8::Kv8LogLevel::Error),   (msg_))
#define KV8_LOG_FATAL(msg_) KV8_LOG(static_cast<uint8_t>(kv8::Kv8LogLevel::Fatal),   (msg_))

#define KV8_LOGF_DEBUG(fmt_, ...) KV8_LOGF(static_cast<uint8_t>(kv8::Kv8LogLevel::Debug),   (fmt_), ##__VA_ARGS__)
#define KV8_LOGF_INFO(fmt_, ...)  KV8_LOGF(static_cast<uint8_t>(kv8::Kv8LogLevel::Info),    (fmt_), ##__VA_ARGS__)
#define KV8_LOGF_WARN(fmt_, ...)  KV8_LOGF(static_cast<uint8_t>(kv8::Kv8LogLevel::Warning), (fmt_), ##__VA_ARGS__)
#define KV8_LOGF_ERROR(fmt_, ...) KV8_LOGF(static_cast<uint8_t>(kv8::Kv8LogLevel::Error),   (fmt_), ##__VA_ARGS__)
#define KV8_LOGF_FATAL(fmt_, ...) KV8_LOGF(static_cast<uint8_t>(kv8::Kv8LogLevel::Fatal),   (fmt_), ##__VA_ARGS__)

#endif // KV8_LOG_ENABLE

// UDT telemetry macros (always included; disabled path expands to no-ops).
#include "kv8log/KV8_UDT.h"
