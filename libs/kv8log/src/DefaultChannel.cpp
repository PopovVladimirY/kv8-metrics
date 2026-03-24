////////////////////////////////////////////////////////////////////////////////
// kv8log/src/DefaultChannel.cpp
//
// Process-wide default Channel singleton and the helper stubs used by
// Runtime::Flush() and Runtime::MonotonicToNs().
////////////////////////////////////////////////////////////////////////////////

#include "kv8log/Channel.h"
#include "kv8log/Runtime.h"

#include <string>
#include <cstdint>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace kv8log {

// ── Exe-name resolver (duplicated lightly from Runtime.cpp for independence).
static std::string ResolveDefaultChannelName()
{
#ifdef _WIN32
    wchar_t buf[512]{};
    GetModuleFileNameW(nullptr, buf, 511);
    char narrow[512]{};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, 511, nullptr, nullptr);
    std::string path(narrow);
#elif defined(__linux__)
    char buf[512]{};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string path = (n > 0) ? std::string(buf, (size_t)n) : "";
#else
    std::string path;
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path = path.substr(slash + 1);
    auto dot = path.rfind('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    std::string exe = path.empty() ? "kv8app" : path;
    return "kv8log/" + exe;
}

// ── Default channel name storage (static, persists for process lifetime).
static const std::string& DefaultChannelName()
{
    static std::string s_name = ResolveDefaultChannelName();
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
