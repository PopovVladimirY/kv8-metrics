////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_log_wire.cpp
//
// Phase L1 deliverable check: standalone round-trip test for the trace log
// wire format (Kv8LogRecord and KV8_CID_LOG_SITE tail).  No Kafka needed.
////////////////////////////////////////////////////////////////////////////////

#include <kv8/Kv8Types.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace kv8;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> EncodeLogRecord(const Kv8LogRecord& hdr,
                                            const void* pPayload, size_t cbPayload)
{
    std::vector<uint8_t> buf(sizeof(Kv8LogRecord) + cbPayload);
    memcpy(buf.data(), &hdr, sizeof(Kv8LogRecord));
    if (cbPayload) memcpy(buf.data() + sizeof(Kv8LogRecord), pPayload, cbPayload);
    return buf;
}

// ── Tests ───────────────────────────────────────────────────────────────────

static void test_size_and_layout()
{
    // sizeof must be exactly 28 bytes, no padding.
    EXPECT(sizeof(Kv8LogRecord) == 28);

    // Verify field offsets to lock the wire layout.
    Kv8LogRecord r{};
    auto base = reinterpret_cast<uintptr_t>(&r);
    EXPECT(reinterpret_cast<uintptr_t>(&r.dwMagic)    - base == 0);
    EXPECT(reinterpret_cast<uintptr_t>(&r.dwSiteHash) - base == 4);
    EXPECT(reinterpret_cast<uintptr_t>(&r.tsNs)       - base == 8);
    EXPECT(reinterpret_cast<uintptr_t>(&r.dwThreadID) - base == 16);
    EXPECT(reinterpret_cast<uintptr_t>(&r.wCpuID)     - base == 20);
    EXPECT(reinterpret_cast<uintptr_t>(&r.bLevel)     - base == 22);
    EXPECT(reinterpret_cast<uintptr_t>(&r.bFlags)     - base == 23);
    EXPECT(reinterpret_cast<uintptr_t>(&r.wArgLen)    - base == 24);
    EXPECT(reinterpret_cast<uintptr_t>(&r.wReserved)  - base == 26);
}

static void test_round_trip_basic()
{
    Kv8LogRecord hdr{};
    hdr.dwMagic     = KV8_LOG_MAGIC;
    hdr.dwSiteHash  = 0xABCD1234u;
    hdr.tsNs        = 0xDEADBEEFCAFEULL;
    hdr.dwThreadID  = 0x00001234u;
    hdr.wCpuID      = 3;
    hdr.bLevel      = static_cast<uint8_t>(Kv8LogLevel::Warning);
    hdr.bFlags      = KV8_LOG_FLAG_TEXT;
    const char text[] = "hello world";
    hdr.wArgLen     = static_cast<uint16_t>(sizeof(text) - 1);
    hdr.wReserved   = 0;

    auto buf = EncodeLogRecord(hdr, text, hdr.wArgLen);

    Kv8LogRecord     dec{};
    std::string_view payload;
    EXPECT(Kv8DecodeLogRecord(buf.data(), buf.size(), dec, payload));

    EXPECT(dec.dwMagic    == hdr.dwMagic);
    EXPECT(dec.dwSiteHash == hdr.dwSiteHash);
    EXPECT(dec.tsNs       == hdr.tsNs);
    EXPECT(dec.dwThreadID == hdr.dwThreadID);
    EXPECT(dec.wCpuID     == hdr.wCpuID);
    EXPECT(dec.bLevel     == hdr.bLevel);
    EXPECT(dec.bFlags     == hdr.bFlags);
    EXPECT(dec.wArgLen    == hdr.wArgLen);
    EXPECT(dec.wReserved  == 0);
    EXPECT(payload.size() == hdr.wArgLen);
    EXPECT(memcmp(payload.data(), text, hdr.wArgLen) == 0);
}

static void test_max_payload_accepted()
{
    Kv8LogRecord hdr{};
    hdr.dwMagic = KV8_LOG_MAGIC;
    hdr.bLevel  = static_cast<uint8_t>(Kv8LogLevel::Info);
    hdr.bFlags  = KV8_LOG_FLAG_TEXT;
    hdr.wArgLen = KV8_LOG_MAX_PAYLOAD;   // 4095

    std::vector<uint8_t> payload(KV8_LOG_MAX_PAYLOAD, 'A');
    auto buf = EncodeLogRecord(hdr, payload.data(), payload.size());

    Kv8LogRecord     dec{};
    std::string_view view;
    EXPECT(Kv8DecodeLogRecord(buf.data(), buf.size(), dec, view));
    EXPECT(view.size() == KV8_LOG_MAX_PAYLOAD);
}

static void test_oversize_payload_rejected()
{
    // wArgLen > KV8_LOG_MAX_PAYLOAD must be rejected (malformed).
    Kv8LogRecord hdr{};
    hdr.dwMagic = KV8_LOG_MAGIC;
    hdr.bLevel  = static_cast<uint8_t>(Kv8LogLevel::Info);
    hdr.bFlags  = KV8_LOG_FLAG_TEXT;
    hdr.wArgLen = KV8_LOG_MAX_PAYLOAD + 1;  // 4096 -- one byte over the cap

    std::vector<uint8_t> payload(KV8_LOG_MAX_PAYLOAD + 1, 'B');
    auto buf = EncodeLogRecord(hdr, payload.data(), payload.size());

    Kv8LogRecord     dec{};
    std::string_view view;
    EXPECT(!Kv8DecodeLogRecord(buf.data(), buf.size(), dec, view));
}

static void test_malformed_cases()
{
    // 1. Empty buffer.
    {
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(nullptr, 0, dec, v));
        uint8_t z = 0;
        EXPECT(!Kv8DecodeLogRecord(&z, 0, dec, v));
    }
    // 2. Buffer shorter than the fixed header.
    {
        uint8_t buf[16] = {};
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(buf, sizeof(buf), dec, v));
    }
    // 3. Wrong magic.
    {
        Kv8LogRecord hdr{};
        hdr.dwMagic = 0u;
        hdr.bLevel  = 0;
        auto buf = EncodeLogRecord(hdr, nullptr, 0);
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(buf.data(), buf.size(), dec, v));
    }
    // 4. Magic off by one bit.
    {
        Kv8LogRecord hdr{};
        hdr.dwMagic = KV8_LOG_MAGIC ^ 1u;
        auto buf = EncodeLogRecord(hdr, nullptr, 0);
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(buf.data(), buf.size(), dec, v));
    }
    // 5. wArgLen claims more bytes than provided.
    {
        Kv8LogRecord hdr{};
        hdr.dwMagic = KV8_LOG_MAGIC;
        hdr.bLevel  = 0;
        hdr.wArgLen = 500;
        // Provide only 499 payload bytes (one short).
        std::vector<uint8_t> payload(499, 'C');
        auto buf = EncodeLogRecord(hdr, payload.data(), payload.size());
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(buf.data(), buf.size(), dec, v));
    }
    // 6. bLevel out of range.
    {
        Kv8LogRecord hdr{};
        hdr.dwMagic = KV8_LOG_MAGIC;
        hdr.bLevel  = 255;
        auto buf = EncodeLogRecord(hdr, nullptr, 0);
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(buf.data(), buf.size(), dec, v));
    }
    // 7. wReserved nonzero -- treated as malformed (forward-compat lock).
    {
        Kv8LogRecord hdr{};
        hdr.dwMagic   = KV8_LOG_MAGIC;
        hdr.bLevel    = 0;
        hdr.wReserved = 1;
        auto buf = EncodeLogRecord(hdr, nullptr, 0);
        Kv8LogRecord     dec{};
        std::string_view v;
        EXPECT(!Kv8DecodeLogRecord(buf.data(), buf.size(), dec, v));
    }
}

static void test_trailing_garbage_accepted()
{
    // A buffer larger than sizeof(header) + wArgLen must decode successfully
    // and ignore trailing bytes (forward-compatibility requirement).
    Kv8LogRecord hdr{};
    hdr.dwMagic = KV8_LOG_MAGIC;
    hdr.bLevel  = static_cast<uint8_t>(Kv8LogLevel::Info);
    hdr.bFlags  = KV8_LOG_FLAG_TEXT;
    const char text[] = "hi";
    hdr.wArgLen = sizeof(text) - 1;

    auto buf = EncodeLogRecord(hdr, text, hdr.wArgLen);
    // Append 32 bytes of garbage.
    buf.insert(buf.end(), 32u, 0xCDu);

    Kv8LogRecord     dec{};
    std::string_view view;
    EXPECT(Kv8DecodeLogRecord(buf.data(), buf.size(), dec, view));
    EXPECT(view.size() == hdr.wArgLen);
    EXPECT(memcmp(view.data(), text, hdr.wArgLen) == 0);
}

static void test_site_hash_stable_and_nonzero()
{
    // Same inputs always hash to the same value.
    const char file[] = "motors.cpp";
    const char func[] = "RunMotor";
    uint32_t a = Kv8LogSiteHash(file, sizeof(file) - 1, 142, func, sizeof(func) - 1);
    uint32_t b = Kv8LogSiteHash(file, sizeof(file) - 1, 142, func, sizeof(func) - 1);
    EXPECT(a == b);
    EXPECT(a != 0u);

    // Different line -> different hash.
    uint32_t c = Kv8LogSiteHash(file, sizeof(file) - 1, 143, func, sizeof(func) - 1);
    EXPECT(a != c);

    // Different file -> different hash.
    const char file2[] = "motors2.cpp";
    uint32_t d = Kv8LogSiteHash(file2, sizeof(file2) - 1, 142, func, sizeof(func) - 1);
    EXPECT(a != d);

    // Different function -> different hash.
    const char func2[] = "StopMotor";
    uint32_t e = Kv8LogSiteHash(file, sizeof(file) - 1, 142, func2, sizeof(func2) - 1);
    EXPECT(a != e);
}

static void test_site_tail_round_trip()
{
    const char file[] = "motors.cpp";
    const char func[] = "RunMotor";
    const char fmt [] = "stall at %d rpm";

    uint8_t buf[256] = {};
    size_t n = Kv8EncodeLogSiteTail(file, sizeof(file) - 1,
                                    142,
                                    func, sizeof(func) - 1,
                                    fmt,  sizeof(fmt)  - 1,
                                    buf, sizeof(buf));
    EXPECT(n > 0);

    Kv8LogSiteInfo info{};
    EXPECT(Kv8DecodeLogSiteTail(buf, n, info));
    EXPECT(info.dwLine == 142);
    EXPECT(info.sFile == "motors.cpp");
    EXPECT(info.sFunc == "RunMotor");
    EXPECT(info.sFmt  == "stall at %d rpm");

    // Truncated input: any byte short of n must be rejected.
    for (size_t cut = 0; cut < n; ++cut)
    {
        Kv8LogSiteInfo bad{};
        EXPECT(!Kv8DecodeLogSiteTail(buf, cut, bad));
    }

    // Output buffer too small: encoder returns 0 (no partial writes).
    EXPECT(Kv8EncodeLogSiteTail(file, sizeof(file) - 1,
                                142,
                                func, sizeof(func) - 1,
                                fmt,  sizeof(fmt)  - 1,
                                buf, n - 1) == 0);
}

int main()
{
    test_size_and_layout();
    test_round_trip_basic();
    test_max_payload_accepted();
    test_oversize_payload_rejected();
    test_malformed_cases();
    test_trailing_garbage_accepted();
    test_site_hash_stable_and_nonzero();
    test_site_tail_round_trip();

    fprintf(stdout, "[test_log_wire] PASS\n");
    return 0;
}
