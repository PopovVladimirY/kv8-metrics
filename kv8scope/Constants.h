// kv8scope -- Kv8 Software Oscilloscope
// Constants.h -- UI / pipeline tuning constants used across kv8scope.
//
// Library-wide constants (Kafka defaults, liveness thresholds, etc.) live in
// <kv8/Kv8Constants.h>.  This header only contains constants that are
// scope-app specific.

#pragma once

#include <cstdint>
#include <cstddef>

namespace kv8scope {

// ─────────────────────────────────────────────────────────────────────────────
// Frame pacing
// kv8scope is a data-visualisation tool, not an interactive renderer.
// 30 fps is sufficient and reduces CPU/GPU pressure, leaving more
// headroom for the consumer / decoder threads.
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr std::uint64_t FRAME_BUDGET_US = 33333;  // ~30 fps

// ─────────────────────────────────────────────────────────────────────────────
// Default visible time window (seconds)
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr double DEFAULT_TIME_WINDOW_S  = 15.0;
inline constexpr double REALTIME_TIME_WINDOW_S = 15.0;

// ─────────────────────────────────────────────────────────────────────────────
// Per-trace storage caps
// MAX_POINTS_PER_TRACE bounds memory use under long offline replay.
// TRACE_BUFFER_RESERVE is the up-front allocation to avoid early reallocs.
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr int         MAX_POINTS_PER_TRACE = 16'000'000;
inline constexpr std::size_t TRACE_BUFFER_RESERVE = 65'536;

// ─────────────────────────────────────────────────────────────────────────────
// Pump batch sizes
// CONSUMER_POLL_BATCH    -- max messages drained per librdkafka poll call
// CONSUMER_POLL_TIMEOUT_MS -- per-poll wait when no messages are ready
// RENDER_DRAIN_BATCH     -- pre-allocated render-side scratch buffer size
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr int         CONSUMER_POLL_BATCH      = 4096;
inline constexpr int         CONSUMER_POLL_TIMEOUT_MS = 10;
inline constexpr std::size_t RENDER_DRAIN_BATCH       = 4096;

} // namespace kv8scope
