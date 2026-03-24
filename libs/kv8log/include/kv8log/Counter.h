////////////////////////////////////////////////////////////////////////////////
// kv8log/Counter.h -- per-counter state: lazy registration and sample dispatch.
////////////////////////////////////////////////////////////////////////////////

#pragma once

// CR-19: kv8log_h is now in kv8log_types.h; included here directly so code
// that only needs the type can include just kv8log_types.h without Channel.h.
#include "kv8log/kv8log_types.h"
#include "kv8log/Channel.h"

#include <mutex>
#include <cstdint>

namespace kv8log {

class Counter
{
public:
    Counter(Channel& ch, const char* name, double mn, double mx);

    // Record a sample with an auto-captured HPC timestamp.
    // No-op (single branch) when the runtime library is absent.
    void Add(double value) noexcept;

    // Record a sample with a caller-supplied timestamp in nanoseconds since
    // Unix epoch. Use KV8_MONO_TO_NS() to convert a monotonic reading first.
    void AddTs(double value, uint64_t ts_ns) noexcept;

    // Enable or disable data collection for this counter.  Publishes to the
    // session ._ctl topic so kv8scope reflects the change immediately.
    // No-op when the runtime library is absent or does not support this feature.
    void Enable()  noexcept;
    void Disable() noexcept;

private:
    // Ensure the counter is registered with the channel (called once).
    void EnsureRegistered() noexcept;

    Channel&       m_ch;
    const char*    m_name;
    double         m_min;
    double         m_max;
    std::once_flag m_reg_once;
    uint16_t       m_id = 0xFFFF;   // 0xFFFF = not yet registered

    // Cached after first-use registration.  The hot path (Add/AddTs) reads
    // these directly -- no Channel::Handle() call_once or Runtime::Fn()
    // call_once needed after the Counter is registered.
    kv8log_h m_cached_handle                                   = nullptr;
    void   (*m_fn_add   )(kv8log_h, uint16_t, double)          = nullptr;
    void   (*m_fn_add_ts)(kv8log_h, uint16_t, double, uint64_t) = nullptr;
};

} // namespace kv8log
