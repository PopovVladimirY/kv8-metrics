////////////////////////////////////////////////////////////////////////////////
// kv8util/Kv8TopicUtils.h -- topic name generation and misc utilities
//
// Provides:
//   GenerateTopicName()  -- generate a timestamped topic name
//   NowUTC()             -- current UTC time as "YYYY-MM-DDTHH:MM:SSZ"
//   ProgressRow          -- per-second throughput + lag snapshot
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <string>

namespace kv8util {

/// Generate a unique topic name: "<prefix>.YYYYMMDDTHHMMSSz-XXXXXX"
/// where XXXXXX is a sub-second hex suffix for uniqueness.
std::string GenerateTopicName(const std::string &prefix = "kv8bench");

/// Return current UTC time as "YYYY-MM-DDTHH:MM:SSZ".
std::string NowUTC();

/// Per-second progress snapshot captured during the benchmark.
struct ProgressRow
{
    int     tSec        = 0;     // elapsed seconds (end of interval)
    int64_t nSent       = 0;     // cumulative messages produced
    int64_t nRecv       = 0;     // cumulative messages consumed
    int64_t nQueueFull  = 0;     // cumulative Produce() failures (queue full)
    double  sendRateMps = 0.0;   // messages/s in this 1-s window (producer side)
    double  recvRateMps = 0.0;   // messages/s in this 1-s window (consumer side)
};

} // namespace kv8util
