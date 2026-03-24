////////////////////////////////////////////////////////////////////////////////
// Kv8TopicUtils.cpp -- topic name generation and misc utilities
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <kv8util/Kv8TopicUtils.h>

#include <cstdio>
#include <ctime>
#include <random>

namespace kv8util {

std::string GenerateTopicName(const std::string &prefix)
{
    // CR-05: Use a random 32-bit suffix instead of a millisecond wall-clock
    // value.  The millisecond approach generated identical names for two
    // sessions started within the same second, causing topic collisions.
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist;
    unsigned suffix = dist(rd);

    char buf[80];
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(buf, sizeof(buf), "%s.%04u%02u%02uT%02u%02u%02uZ-%08X",
             prefix.c_str(),
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, suffix);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    gmtime_r(&ts.tv_sec, &t);
    snprintf(buf, sizeof(buf), "%s.%04d%02d%02dT%02d%02d%02dZ-%08X",
             prefix.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, suffix);
#endif
    return std::string(buf);
}

std::string NowUTC()
{
    char buf[32];
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    // CR-13: gmtime() uses a global buffer and is not thread-safe.
    // Use gmtime_r() instead.
    time_t t = time(nullptr);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
#endif
    return buf;
}

} // namespace kv8util
