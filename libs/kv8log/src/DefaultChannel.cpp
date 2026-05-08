////////////////////////////////////////////////////////////////////////////////
// kv8log/src/DefaultChannel.cpp
//
// Process-wide default Channel singleton and the helper stubs used by
// Runtime::Flush() and Runtime::MonotonicToNs().
////////////////////////////////////////////////////////////////////////////////

#include "kv8log/Channel.h"
#include "kv8log/Runtime.h"

#include <kv8util/Kv8Platform.h>

#include <string>
#include <cstdint>

namespace kv8log {

// Default channel name = "kv8log/<exe-basename>".  Resolved once on first use.
static const std::string& DefaultChannelName()
{
    static std::string s_name = std::string("kv8log/") + kv8util::GetProcessExeName();
    return s_name;
}

Channel& DefaultChannel()
{
    static Channel s_channel(DefaultChannelName().c_str());
    return s_channel;
}

// ── Helpers called by Runtime::Flush() and Runtime::MonotonicToNs() ─────────

// These are defined here (not in Runtime.cpp) so that Channel is fully
// defined without a circular include.  Runtime.cpp declares them extern.

void FlushDefaultChannel(int timeout_ms)
{
    void* h = DefaultChannel().Handle();
    if (!h) return;
    const Vtable& fn = Runtime::Fn();
    if (fn.flush) fn.flush(h, timeout_ms);
}

void* GetDefaultChannelHandle()
{
    return DefaultChannel().Handle();
}

} // namespace kv8log
