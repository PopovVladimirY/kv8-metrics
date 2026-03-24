////////////////////////////////////////////////////////////////////////////////
// kv8log/UdtFeed.h -- per-UDT-feed state: lazy registration and sample dispatch.
//
// UdtFeed<T> is the UDT analogue of Counter.  It follows the same lazy-
// registration pattern: registration happens once on first use via std::call_once,
// after which the hot Add() path is lock-free with zero heap allocation.
//
// The template parameter T must be a struct defined with KV8_UDT_DEFINE (or
// equivalent). sizeof(T) must not exceed KV8_UDT_MAX_PAYLOAD.
//
// Every UdtFeed instance holds:
//   - A reference to its channel.
//   - The feed display name and a pointer to the GetSchema function.
//   - Post-registration: a cached handle + vtable pointers for Add/AddTs.
//   - A feed ID (uint16_t) assigned by the runtime.
//   - A rolling sequence counter (atomic).
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "kv8log/kv8log_types.h"
#include "kv8log/Channel.h"
#include "kv8log/Runtime.h"

#include <mutex>
#include <cstdint>
#include <cstring>

namespace kv8log {

template <typename T>
class UdtFeed
{
public:
    // SchemaFn must return a null-terminated JSON schema string.
    // The pointer must remain valid for the lifetime of the process
    // (i.e., point to a function-local static string).
    using SchemaFnPtr = const char* (*)();

    // RegDepsFn registers all transitive embedded sub-schemas before the feed
    // itself is registered.  reg_fn is kv8log_register_udt_schema; h is the
    // session handle.  For flat schemas this function is a no-op.
    using RegDepsFnPtr = void (*)(int (*reg_fn)(void*, const char*), void* h);

    UdtFeed(Channel& ch, const char* display_name, SchemaFnPtr schema_fn,
            RegDepsFnPtr reg_deps_fn = nullptr)
        : m_ch(ch)
        , m_display_name(display_name)
        , m_schema_fn(schema_fn)
        , m_reg_deps_fn(reg_deps_fn)
    {}

    // Record a UDT sample (auto-captured HPC timestamp).
    // Lock-free after first registration.  No-op when the runtime is absent.
    void Add(const T& val) noexcept
    {
        EnsureRegistered();
        if (!m_fn_add_udt || !m_cached_handle) return;
        m_fn_add_udt(m_cached_handle, m_id,
                     &val, static_cast<uint16_t>(sizeof(T)));
    }

    // Record a UDT sample with a caller-supplied timestamp (ns, Unix epoch).
    void AddTs(const T& val, uint64_t ts_ns) noexcept
    {
        EnsureRegistered();
        if (!m_fn_add_udt_ts || !m_cached_handle) return;
        m_fn_add_udt_ts(m_cached_handle, m_id,
                        &val, static_cast<uint16_t>(sizeof(T)), ts_ns);
    }

    // Enable / disable the feed.  Writes to the ._ctl topic.
    void Enable()  noexcept { SetEnabled(1); }
    void Disable() noexcept { SetEnabled(0); }

private:
    void EnsureRegistered() noexcept
    {
        std::call_once(m_reg_once, [this]() { DoRegister(); });
    }

    void DoRegister() noexcept
    {
        kv8log_h h = m_ch.Handle();
        if (!h) return;

        const Vtable& fn = Runtime::Fn();
        if (!fn.register_udt_feed) return;

        // Pre-register all embedded sub-schemas (no-op for flat schemas).
        // This ensures the consumer can resolve nested type references.
        if (m_reg_deps_fn && fn.register_udt_schema)
            m_reg_deps_fn(fn.register_udt_schema, h);

        uint16_t id = 0xFFFF;
        const char* schema_json = m_schema_fn ? m_schema_fn() : nullptr;
        if (!schema_json) return;

        if (fn.register_udt_feed(h, m_display_name, schema_json, &id) != 0)
            return;

        m_id              = id;
        m_cached_handle   = h;
        m_fn_add_udt      = fn.add_udt;
        m_fn_add_udt_ts   = fn.add_udt_ts;
    }

    void SetEnabled(int bEnabled) noexcept
    {
        EnsureRegistered();
        const Vtable& fn = Runtime::Fn();
        if (fn.set_counter_enabled && m_cached_handle)
            fn.set_counter_enabled(m_cached_handle, m_id, bEnabled);
    }

    Channel&      m_ch;
    const char*   m_display_name;
    SchemaFnPtr   m_schema_fn;
    RegDepsFnPtr  m_reg_deps_fn;

    std::once_flag m_reg_once;
    uint16_t       m_id             = 0xFFFF;

    // Cached after first-use registration (hot-path avoids vtable lookup).
    kv8log_h    m_cached_handle = nullptr;
    void      (*m_fn_add_udt   )(kv8log_h, uint16_t,
                                  const void*, uint16_t)            = nullptr;
    void      (*m_fn_add_udt_ts)(kv8log_h, uint16_t,
                                  const void*, uint16_t, uint64_t)  = nullptr;
};

} // namespace kv8log
