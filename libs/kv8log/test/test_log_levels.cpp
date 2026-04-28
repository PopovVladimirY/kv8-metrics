////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_log_levels.cpp
//
// Phase L5 unit test: every Kv8LogLevel produces the correct bLevel field on
// the wire when round-tripped through Kv8DecodeLogRecord.  Pure wire test --
// no Kafka, no runtime library required.
////////////////////////////////////////////////////////////////////////////////

#include <kv8/Kv8Types.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace kv8;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

static std::vector<uint8_t> EncodeOne(Kv8LogLevel eLevel, std::string_view sText)
{
    Kv8LogRecord hdr{};
    hdr.dwMagic    = KV8_LOG_MAGIC;
    hdr.dwSiteHash = 0xDEADBEEFu;
    hdr.tsNs       = 0x1122334455667788ULL;
    hdr.dwThreadID = 0x42u;
    hdr.wCpuID     = 0;
    hdr.bLevel     = static_cast<uint8_t>(eLevel);
    hdr.bFlags     = KV8_LOG_FLAG_TEXT;
    hdr.wArgLen    = static_cast<uint16_t>(sText.size());
    hdr.wReserved  = 0;

    std::vector<uint8_t> buf(sizeof(Kv8LogRecord) + sText.size());
    std::memcpy(buf.data(), &hdr, sizeof(Kv8LogRecord));
    std::memcpy(buf.data() + sizeof(Kv8LogRecord), sText.data(), sText.size());
    return buf;
}

int main()
{
    // Cross-check the level enum count constant matches what we test below.
    EXPECT(KV8_LOG_LEVEL_COUNT == 5);

    struct Case { Kv8LogLevel eLevel; const char* psz; };
    const Case cases[] = {
        { Kv8LogLevel::Debug,   "debug-message"   },
        { Kv8LogLevel::Info,    "info-message"    },
        { Kv8LogLevel::Warning, "warning-message" },
        { Kv8LogLevel::Error,   "error-message"   },
        { Kv8LogLevel::Fatal,   "fatal-message"   },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        const auto& c = cases[i];
        auto buf = EncodeOne(c.eLevel, c.psz);

        Kv8LogRecord     dec{};
        std::string_view payload;
        EXPECT(Kv8DecodeLogRecord(buf.data(), buf.size(), dec, payload));

        // The level field must round-trip exactly and match the numeric value
        // assigned by the enum (Debug=0 .. Fatal=4).
        EXPECT(dec.bLevel == static_cast<uint8_t>(c.eLevel));
        EXPECT(dec.bLevel == static_cast<uint8_t>(i));

        // Payload survives intact.
        EXPECT(payload.size() == std::strlen(c.psz));
        EXPECT(std::memcmp(payload.data(), c.psz, payload.size()) == 0);

        // Other fixed-header fields are preserved.
        EXPECT(dec.dwMagic    == KV8_LOG_MAGIC);
        EXPECT(dec.dwSiteHash == 0xDEADBEEFu);
        EXPECT(dec.bFlags     == KV8_LOG_FLAG_TEXT);
    }

    fprintf(stdout, "[PASS] test_log_levels: 5 severity levels round-trip OK\n");
    return 0;
}
