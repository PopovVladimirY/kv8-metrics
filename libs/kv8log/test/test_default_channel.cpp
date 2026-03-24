////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_default_channel.cpp
//
// Verifies that DefaultChannel() returns a channel whose name starts with
// "kv8log/" and does not contain an empty exe segment.
// No Kafka connection is required -- the runtime library is absent, so this
// test exercises only the facade layer.
////////////////////////////////////////////////////////////////////////////////

#ifndef KV8_LOG_ENABLE
#  define KV8_LOG_ENABLE
#endif
#include "kv8log/KV8_Log.h"
#include "kv8log/Channel.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    kv8log::Channel& ch = kv8log::DefaultChannel();
    const char* name = ch.Name();

    assert(name != nullptr);
    assert(strncmp(name, "kv8log/", 7) == 0);
    // Exe segment must not be empty (length > 7).
    assert(strlen(name) > 7);

    fprintf(stdout, "[test_default_channel] PASS -- default channel: '%s'\n", name);
    return 0;
}
