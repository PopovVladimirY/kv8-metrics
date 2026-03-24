////////////////////////////////////////////////////////////////////////////////
// kv8util/Kv8BenchMsg.h -- benchmark message payload embedded in each Kafka
//                          message produced by the benchmark framework.
//
// Two clocks are embedded so the consumer can compute all four latency splits:
//
//   qSendTick    -- QPC/CLOCK_MONOTONIC tick at Produce() entry.
//                   Used only for dispatch latency (bracketing Produce()).
//
//   tSendWallMs  -- system_clock milliseconds since Unix epoch at Produce()
//                   entry.  Same timescale as the Kafka broker timestamp so
//                   the P->B and B->C splits are computable without any
//                   cross-clock conversion.
//
//   nSeq         -- 0-based sequence number for loss/reorder detection.
//
// Total: 24 bytes -- fits comfortably within a single cache line.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>

namespace kv8util {

#pragma pack(push, 8)
struct BenchMsg
{
    uint64_t qSendTick;    // QPC tick at Produce() entry (dispatch measurement)
    int64_t  tSendWallMs;  // wall-clock ms at Produce() entry (broker-split measurement)
    uint64_t nSeq;         // 0-based sequence number
    // Total: 24 bytes
};
#pragma pack(pop)

} // namespace kv8util
