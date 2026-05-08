////////////////////////////////////////////////////////////////////////////////
// kv8util/Kv8Platform.h -- header-only cross-platform helpers
//
// Centralises the small platform shims (gmtime, exe-path lookup, UTC
// formatting) that were previously open-coded in multiple translation units.
// Header-only: consumers just need the include path, no link dependency on
// the kv8util static library.
//
// Provided helpers (all in namespace kv8util):
//   GetProcessExeName()   -- basename of the current executable, no extension.
//                            Returns "kv8app" if the OS lookup fails.
//   GmTimeUtc(t)          -- portable wrapper around gmtime_s / gmtime_r.
//   FormatUtcCompact(t)   -- "YYYY-MM-DDTHH:MM:SSZ" (ISO-8601 to second).
//
// Thread safety
// -------------
//   All helpers are stateless and safe to call from any thread.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace kv8util {

////////////////////////////////////////////////////////////////////////////////
// GetProcessExeName -- basename of the running executable
//
// Strips directory components and the file extension.  Returns "kv8app" when
// the OS lookup fails or yields an empty path.  ASCII-only path assumption
// matches the project convention.
////////////////////////////////////////////////////////////////////////////////
inline std::string GetProcessExeName()
{
#ifdef _WIN32
    wchar_t wbuf[512]{};
    GetModuleFileNameW(nullptr, wbuf, 511);
    char buf[512]{};
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 511, nullptr, nullptr);
    std::string path(buf);
#elif defined(__linux__)
    char buf[512]{};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string path = (n > 0) ? std::string(buf, static_cast<size_t>(n)) : "";
#else
    std::string path;
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path = path.substr(slash + 1);
    auto dot = path.rfind('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    return path.empty() ? std::string("kv8app") : path;
}

////////////////////////////////////////////////////////////////////////////////
// GmTimeUtc -- portable gmtime wrapper
//
// Returns the broken-down UTC time for a Unix-epoch second count.  Wraps the
// MSVC gmtime_s / POSIX gmtime_r divergence.
////////////////////////////////////////////////////////////////////////////////
inline std::tm GmTimeUtc(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

////////////////////////////////////////////////////////////////////////////////
// FormatUtcCompact -- ISO-8601 second-resolution UTC stamp
//
// Returns "YYYY-MM-DDTHH:MM:SSZ" (20 characters).  Used by annotation export
// and maintenance tools where sub-second precision is not required.
////////////////////////////////////////////////////////////////////////////////
inline std::string FormatUtcCompact(std::time_t t)
{
    std::tm tm = GmTimeUtc(t);
    // Buffer sized generously: GCC -Wformat-truncation considers %04d as
    // potentially writing more than 4 chars when inlined into hot paths.
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

} // namespace kv8util
