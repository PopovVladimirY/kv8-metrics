////////////////////////////////////////////////////////////////////////////////
// kv8log/src/Counter.cpp
////////////////////////////////////////////////////////////////////////////////

#include "kv8log/Counter.h"
#include "kv8log/Channel.h"
#include "kv8log/Runtime.h"

#include <cstdio>

namespace kv8log {

Counter::Counter(Channel& ch, const char* name, double mn, double mx)
    : m_ch(ch), m_name(name), m_min(mn), m_max(mx)
{}

void Counter::EnsureRegistered() noexcept
{
    std::call_once(m_reg_once, [this]() {
        void* h = m_ch.Handle();
        if (!h) return;
        const Vtable& fn = Runtime::Fn();
        if (!fn.register_counter) return;
        uint16_t id = 0xFFFF;
        if (fn.register_counter(h, m_name, m_min, m_max, &id) == 0) {
            m_id            = id;
            m_cached_handle = h;
            m_fn_add        = fn.add;
            m_fn_add_ts     = fn.add_ts;
        } else {
            fprintf(stderr, "[kv8log] register_counter failed: %s\n", m_name);
        }
    });
}

void Counter::Add(double value) noexcept
{
    EnsureRegistered();
    if (!m_fn_add) return;
    m_fn_add(m_cached_handle, m_id, value);
}

void Counter::AddTs(double value, uint64_t ts_ns) noexcept
{
    EnsureRegistered();
    if (!m_fn_add_ts) return;
    m_fn_add_ts(m_cached_handle, m_id, value, ts_ns);
}

void Counter::Enable() noexcept
{
    EnsureRegistered();
    auto fn = Runtime::Fn().set_counter_enabled;
    if (fn) fn(m_cached_handle, m_id, 1);
}

void Counter::Disable() noexcept
{
    EnsureRegistered();
    auto fn = Runtime::Fn().set_counter_enabled;
    if (fn) fn(m_cached_handle, m_id, 0);
}

} // namespace kv8log
