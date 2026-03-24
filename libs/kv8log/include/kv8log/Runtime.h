////////////////////////////////////////////////////////////////////////////////
// kv8log/Runtime.h -- library-wide singleton: shared library load, config,
//                     vtable resolution, and utility accessors.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>

namespace kv8log {

// Opaque handle returned by kv8log_open in the runtime shared library.
using kv8log_h = void*;

// ── Vtable ────────────────────────────────────────────────────────────────────
// Plain-C function pointers resolved from the runtime shared library.
// All fields are nullptr when the library is absent (silent no-op path).
struct Vtable
{
    kv8log_h (*open )(const char* brokers, const char* channel,
                      const char* user, const char* pass);
    void     (*close)(kv8log_h h);
    int      (*register_counter)(kv8log_h h, const char* name,
                                 double mn, double mx, uint16_t* out_id);
    void     (*add   )(kv8log_h h, uint16_t id, double value);
    void     (*add_ts)(kv8log_h h, uint16_t id, double value, uint64_t ts_ns);
    void     (*flush )(kv8log_h h, int timeout_ms);
    uint64_t (*monotonic_to_ns)(kv8log_h h, uint64_t mono_ns);
    // Optional (nullptr when the runtime library does not export it).
    void     (*set_counter_enabled)(kv8log_h h, uint16_t id, int bEnabled);

    // UDT entries (nullptr when the runtime library does not support UDT).
    // register_udt_schema: write a bare KV8_CID_SCHEMA record (no feed ID assigned).
    //                      Used to pre-register embedded sub-schemas for nested UDTs.
    // register_udt_feed:   write schema + feed registry records; assign feed ID.
    // add_udt:             enqueue a sample with an auto-captured HPC timestamp.
    // add_udt_ts:          enqueue a sample with a caller-supplied Unix-epoch timestamp.
    int      (*register_udt_schema)(kv8log_h h, const char* schema_json);
    int      (*register_udt_feed)(kv8log_h h, const char* display_name,
                                  const char* schema_json, uint16_t* out_id);
    void     (*add_udt   )(kv8log_h h, uint16_t feed_id,
                           const void* packed_data, uint16_t data_size);
    void     (*add_udt_ts)(kv8log_h h, uint16_t feed_id,
                           const void* packed_data, uint16_t data_size,
                           uint64_t ts_ns);
};

// ── Runtime ───────────────────────────────────────────────────────────────────
class Runtime
{
public:
    // Optional explicit override (priority 1 in init resolution).
    // Must be called before the first KV8_TEL_ADD invocation.
    static void Configure(const char* brokers,
                          const char* channel,
                          const char* user = nullptr,
                          const char* pass = nullptr);

    // Access the fully-resolved vtable.  All pointers are nullptr if the
    // runtime shared library could not be loaded.
    static const Vtable& Fn();

    // Block until all enqueued messages are delivered (or timed out).
    static void Flush(int timeout_ms = 10000);

    // Convert a monotonic clock reading (nanoseconds) to nanoseconds since
    // Unix epoch, using the per-session wallclock/HPC anchor.
    // Returns 0 if the runtime library is absent.
    static uint64_t MonotonicToNs(uint64_t mono_ns);
};

} // namespace kv8log
