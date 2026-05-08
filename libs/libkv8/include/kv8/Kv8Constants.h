////////////////////////////////////////////////////////////////////////////////
// kv8/Kv8Constants.h -- single source of truth for shared library-level
//                       defaults and tuning knobs.
//
// Why a dedicated header (vs in-place literals or per-module #defines):
//   - Kafka connection defaults (broker URL, SASL credentials) were previously
//     duplicated in 4+ files.  Drift between them caused real bugs.
//   - Liveness thresholds and the heartbeat interval are shared between
//     producer (kv8log runtime) and consumer (kv8scope) -- they MUST agree.
//   - Centralized so that downstream tooling and tests can reference the same
//     values without re-stating them.
//
// This header is intentionally header-only (inline constexpr) and has no
// dependencies beyond <cstdint> / <cstddef>, so it is safe to include from
// any other header in the project (including the public libkv8 API).
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstddef>

namespace kv8 {

// ─────────────────────────────────────────────────────────────────────────────
// Kafka connection defaults
// Used by kv8::Kv8Config (struct member initializers), the kv8log runtime
// (Runtime.cpp), and the kv8scope ScopeConfig defaults (ConfigStore.h).
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr const char* KV8_DEFAULT_BROKERS         = "localhost:19092";
inline constexpr const char* KV8_DEFAULT_SECURITY_PROTO  = "sasl_plaintext";
inline constexpr const char* KV8_DEFAULT_SASL_MECHANISM  = "PLAIN";
inline constexpr const char* KV8_DEFAULT_USER            = "kv8producer";
inline constexpr const char* KV8_DEFAULT_PASSWORD        = "kv8secret";

// ─────────────────────────────────────────────────────────────────────────────
// Heartbeat / liveness timing
// The producer publishes a heartbeat record every KV8_HEARTBEAT_INTERVAL_MS.
// The consumer classifies a session as Offline / Dead based on the elapsed
// time since the last heartbeat, using the seconds thresholds below.
// Producer and consumer MUST agree on KV8_HEARTBEAT_INTERVAL_MS.
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr int KV8_HEARTBEAT_INTERVAL_MS         = 3000;
inline constexpr int KV8_LIVENESS_OFFLINE_THRESHOLD_S  = 7;   // GoingOffline
inline constexpr int KV8_LIVENESS_DEAD_THRESHOLD_S     = 30;  // Offline / Dead

// ─────────────────────────────────────────────────────────────────────────────
// Session discovery (consumer-side)
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr int KV8_SESSION_POLL_MS   = 2000;
// Debounce for MetaUpdated events.  Must be strictly greater than
// KV8_SESSION_POLL_MS so that at least one full discovery cycle completes
// before the consumer reacts to a metadata change.
inline constexpr int KV8_META_STABILIZE_MS = 4000;

// ─────────────────────────────────────────────────────────────────────────────
// Hot-path layout
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr std::size_t KV8_CACHELINE_BYTES = 64;

} // namespace kv8
