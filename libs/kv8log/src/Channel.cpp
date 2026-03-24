////////////////////////////////////////////////////////////////////////////////
// kv8log/src/Channel.cpp
////////////////////////////////////////////////////////////////////////////////

#include "kv8log/Channel.h"
#include "kv8log/Runtime.h"

#include <cstdio>

// Config accessors defined in Runtime.cpp.
namespace kv8log {
extern const char* RuntimeBrokers();
extern const char* RuntimeUser();
extern const char* RuntimePass();
} // namespace kv8log

namespace kv8log {

Channel::Channel(const char* name) : m_name(name) {}

kv8log_h Channel::Handle()
{
    std::call_once(m_once, [this]() {
        const Vtable& fn = Runtime::Fn();
        if (!fn.open) return;                  // runtime absent -- stay nullptr

        m_handle = fn.open(RuntimeBrokers(),
                           m_name,
                           RuntimeUser(),
                           RuntimePass());
        if (!m_handle)
            fprintf(stderr, "[kv8log] kv8log_open failed for channel '%s'\n", m_name);
    });
    return m_handle;
}

} // namespace kv8log
