////////////////////////////////////////////////////////////////////////////////
// kv8log/Channel.h -- producer channel (one Kafka producer session per channel).
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "kv8log/kv8log_types.h"

#include <mutex>
#include <cstdint>

namespace kv8log {

// kv8log_h is defined in kv8log_types.h (using kv8log_h = void*).

class Channel
{
public:
    // 'name' is borrowed -- must be a string literal or have static lifetime.
    // Use std::string::c_str() only if the string outlives all Channel uses.
    explicit Channel(const char* name);

    // Returns the opaque producer handle for this channel.
    // Triggers lazy initialization on the first call (thread-safe).
    // Returns nullptr when the runtime library is absent or init failed.
    kv8log_h Handle();

    const char* Name() const { return m_name; }

private:
    const char*    m_name;
    std::once_flag m_once;
    kv8log_h       m_handle = nullptr;
};

// Returns a reference to the process-wide default Channel.
// The channel name is derived from the executable base name: "kv8log/<exe>".
Channel& DefaultChannel();

} // namespace kv8log
