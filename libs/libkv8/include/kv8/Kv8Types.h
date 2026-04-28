////////////////////////////////////////////////////////////////////////////////
// kv8/Kv8Types.h -- shared binary types, constants, and data-model structs
//                   for the kv8 telemetry-over-Kafka protocol.
//
// This header is the single source of truth for:
//   - The on-wire binary packet structures (kv8 telemetry wire format).
//   - The session and counter metadata models populated during registry scans.
//   - Connection configuration shared between consumers and producers.
//   - Protocol-level utility helpers (channel sanitization, hash extraction,
//     timestamp formatting).
//
// All headers in this directory include this file.
// No librdkafka types appear here -- callers are fully decoupled from rdkafka.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>

namespace kv8 {

////////////////////////////////////////////////////////////////////////////////
// Connection configuration
////////////////////////////////////////////////////////////////////////////////

/// All authentication and transport settings needed to reach a Kafka broker.
/// Passed to IKv8Consumer::Create() and IKv8Producer::Create().
struct Kv8Config
{
    std::string sBrokers       = "localhost:19092"; ///< Bootstrap broker list (host:port,...)
    std::string sSecurityProto = "sasl_plaintext"; ///< plaintext|sasl_plaintext|sasl_ssl
    std::string sSaslMechanism = "PLAIN";          ///< PLAIN|SCRAM-SHA-256|SCRAM-SHA-512
    std::string sUser          = "kv8producer";    ///< SASL username
    std::string sPass          = "kv8secret";      ///< SASL password
    std::string sGroupID;                          ///< Consumer group; empty = auto-generate

    // When non-empty the producer auto-starts a heartbeat thread on the given
    // topic immediately after connecting.  The thread stops automatically when
    // the producer is destroyed (sending a clean-shutdown marker).
    std::string sHeartbeatTopic;                   ///< e.g. "<channel>.<sessionId>.hb"; empty = no heartbeat
    int         nHeartbeatIntervalMs = 3000;       ///< Interval between successive heartbeat records
};

////////////////////////////////////////////////////////////////////////////////
// On-wire binary structs  (pack(2) mirrors Kv8 extension packet layout)
////////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 2)

/// Raw 4-byte extension packet header encoding type, sub-type,
/// and payload size in the first 32 bits of every telemetry packet.
struct Kv8PacketHeader
{
    uint32_t dwBits;
};

/// Kv8 telemetry value packet, version 2 (22 bytes wire size).
/// The kv8 producer stores this struct verbatim as the Kafka message payload.
///
/// Memory layout (pack(2), little-endian):
///   offset  0 : Kv8PacketHeader sCommonRaw (4 bytes) -- type=2, subtype=2
///   offset  4 : uint16_t wID              (2 bytes) -- counter ID
///   offset  6 : uint16_t wSeqN            (2 bytes) -- sequence number (wraps at 65535)
///   offset  8 : uint64_t qwTimer          (8 bytes) -- producer QPC tick
///   offset 16 : double   dbValue          (8 bytes) -- sampled value
struct Kv8TelValue
{
    Kv8PacketHeader sCommonRaw; ///< Extension header
    uint16_t wID;        ///< Counter ID -- unique within the telemetry channel
    uint16_t wSeqN;      ///< Rolling sequence number
    uint64_t qwTimer;    ///< Producer-side high-resolution timer value
    double   dbValue;    ///< Sampled counter value
};

/// Heartbeat record written to <sessionPrefix>.hb by the producer every N seconds.
/// Allows consumers to distinguish a live session from a historical one without
/// scanning all data topics for new messages.
///
/// Written inside a #pragma pack(push, 2) block; the struct is 16 bytes on all
/// supported platforms (x86/ARM, 32/64-bit).
struct HbRecord
{
    uint8_t  bVersion;   ///< Format version; must equal KV8_HB_VERSION (1).
    uint8_t  bState;     ///< KV8_HB_STATE_ALIVE = 1; KV8_HB_STATE_SHUTDOWN = 0.
    uint16_t wReserved;  ///< Reserved; zero.
    uint32_t dwSeqNo;    ///< Monotonically increasing sequence number (per session).
    int64_t  tsUnixMs;   ///< Wall-clock POSIX time in milliseconds when this record was written.
};

/// HbRecord format version stored in bVersion.
static const uint8_t KV8_HB_VERSION  = 1u;

/// bState values for HbRecord.
static const uint8_t KV8_HB_STATE_ALIVE    = 1u;
static const uint8_t KV8_HB_STATE_SHUTDOWN = 0u;

#pragma pack(pop)

#pragma pack(push, 2)

/// Exact rational value: numerator / denominator.
/// A denominator of 0 is undefined (do not divide).
struct Kv8Rational
{
    int32_t  num; ///< Signed numerator
    uint32_t den; ///< Unsigned denominator; 0 = undefined
};
static_assert(sizeof(Kv8Rational) == 8, "Kv8Rational must be 8 bytes");

/// On-wire header for a single UDT sample (16 bytes).
/// Immediately followed by wDataSize bytes of packed field values,
/// where wDataSize is encoded in sCommonRaw via Kv8GetExtSize().
struct Kv8UDTSample
{
    Kv8PacketHeader sCommonRaw; ///< type=2 (TELEMETRY), subtype=5 (UDT)
    uint16_t        wFeedID;    ///< Feed ID assigned by the runtime
    uint16_t        wSeqN;      ///< Rolling sequence number (wraps at 65535)
    uint64_t        qwTimer;    ///< Producer-side session-relative HPC tick (ns)
};
static_assert(sizeof(Kv8UDTSample) == 16, "Kv8UDTSample must be 16 bytes");

#pragma pack(pop)

#pragma pack(push, 2)

/// Registry record written by ClKafka into the channel-level _registry topic.
/// Describes one counter, one Kv8 channel group, or one session announcement.
///
/// A variable-length tail immediately follows the fixed header:
///   [wNameLen bytes of UTF-8 name][wTopicLen bytes of UTF-8 Kafka topic name]
///
/// Several record types are distinguished by wCounterID:
///   KV8_CID_LOG_SITE(0xFFFB) -- log call-site descriptor (see Kv8LogRecord below).
///                               dwHash = FNV-32 of (basename, line, function),
///                               wNameLen = total tail length, wTopicLen = 0,
///                               variable tail packs file/line/func/fmt strings.
///                               Kafka message key = sprintf("%08X", dwHash) so log
///                               compaction keeps exactly one record per call site.
///   KV8_CID_SCHEMA  (0xFFFC) -- UDT schema: sName = schema display name,
///                               sTopic = full JSON schema text.
///   KV8_CID_DELETED (0xFFFD) -- tombstone: session was deleted.  sName = session
///                               prefix, wTopicLen = 0.  Written by kv8maint after
///                               deleting session topics so DiscoverSessions() can
///                               omit the deleted session in future scans.
///   KV8_CID_LOG     (0xFFFE) -- session announcement: sName = session display name,
///                               sTopic = log topic name.
///   KV8_CID_GROUP   (0xFFFF) -- group record: sName = Kv8 channel name,
///                               sTopic = data topic name.  qwTimerFrequency,
///                               qwTimerValue, dwTimeHi/Lo carry the time anchor.
///   other                    -- counter record: sName = counter display name,
///                               sTopic = data topic name.
struct KafkaRegistryRecord
{
    uint32_t dwHash;           ///< FNV-32 hash of the channel name
    uint16_t wCounterID;       ///< Counter ID; 0xFFFF = group; 0xFFFE = log sentinel
    uint16_t wFlags;           ///< Bit 0: counter was enabled at creation time
    double   dbMin;            ///< Counter minimum value
    double   dbAlarmMin;       ///< Counter alarm-low threshold
    double   dbMax;            ///< Counter maximum value
    double   dbAlarmMax;       ///< Counter alarm-high threshold
    uint16_t wNameLen;         ///< Byte length of UTF-8 name in the variable tail
    uint16_t wTopicLen;        ///< Byte length of UTF-8 topic in the variable tail
    uint16_t wVersion;         ///< Format version; must equal KV8_REGISTRY_VERSION
    uint16_t wPad;             ///< Reserved; zero
    uint64_t qwTimerFrequency; ///< Producer QPC frequency Hz (group records; else 0)
    uint64_t qwTimerValue;     ///< Producer QPC anchor tick  (group records; else 0)
    uint32_t dwTimeHi;         ///< FILETIME.dwHighDateTime at channel start (group; else 0)
    uint32_t dwTimeLo;         ///< FILETIME.dwLowDateTime  at channel start (group; else 0)
    // followed by: [wNameLen bytes UTF-8][wTopicLen bytes UTF-8]
};

#pragma pack(pop)

////////////////////////////////////////////////////////////////////////////////
// Protocol constants
////////////////////////////////////////////////////////////////////////////////

/// Current registry record format version stored in wVersion.
static const uint16_t KV8_REGISTRY_VERSION = 2u;

/// wCounterID value that marks a group-level record (not an individual counter).
static const uint16_t KV8_CID_GROUP = 0xFFFFu;

/// wCounterID value that marks a tombstone record (session was deleted).
static const uint16_t KV8_CID_DELETED = 0xFFFDu;

/// wCounterID value that marks a UDT schema registry record.
static const uint16_t KV8_CID_SCHEMA  = 0xFFFCu;

/// wCounterID value that marks a session log-topic announcement.
static const uint16_t KV8_CID_LOG     = 0xFFFEu;

/// wCounterID value that marks a log call-site registry record (see Kv8LogRecord).
static const uint16_t KV8_CID_LOG_SITE = 0xFFFBu;

// Kv8 extension header field widths.
static const int KV8_EXT_TYPE_BITS    = 5;
static const int KV8_EXT_SUBTYPE_BITS = 5;

/// Kv8 packet type value identifying a telemetry packet.
static const uint32_t KV8_PACKET_TYPE_TELEMETRY = 2u;

/// Kv8 telemetry sub-type values.
static const uint32_t KV8_TEL_TYPE_INFO    = 0u;
static const uint32_t KV8_TEL_TYPE_COUNTER = 1u;
static const uint32_t KV8_TEL_TYPE_VALUE   = 2u;
static const uint32_t KV8_TEL_TYPE_ENABLE  = 3u;
static const uint32_t KV8_TEL_TYPE_CLOSE   = 4u;
/// Kv8 telemetry sub-type: User Defined Type sample.
static const uint32_t KV8_TEL_TYPE_UDT     = 5u;

/// wFlags bit indicating a UDT feed (as opposed to a scalar counter).
static const uint16_t KV8_FLAG_UDT         = 0x0002u;

/// Maximum packed-payload size for a UDT sample, in bytes.
/// Combined with the 16-byte Kv8UDTSample header: 256 bytes total (2 cache lines).
static const uint16_t KV8_UDT_MAX_PAYLOAD  = 240u;

/// Extract the packet type from a Kv8PacketHeader header word.
inline uint32_t Kv8GetExtType(Kv8PacketHeader h)
{
    return h.dwBits & ((1u << KV8_EXT_TYPE_BITS) - 1u);
}

/// Extract the packet sub-type from a Kv8PacketHeader header word.
inline uint32_t Kv8GetExtSubtype(Kv8PacketHeader h)
{
    return (h.dwBits >> KV8_EXT_TYPE_BITS) & ((1u << KV8_EXT_SUBTYPE_BITS) - 1u);
}

/// Extract the payload size (bytes) from a Kv8PacketHeader header word.
inline uint32_t Kv8GetExtSize(Kv8PacketHeader h)
{
    return h.dwBits >> (KV8_EXT_TYPE_BITS + KV8_EXT_SUBTYPE_BITS);
}

////////////////////////////////////////////////////////////////////////////////
// UDT field type helpers (used by CounterMeta and ConsumerThread decoding)
////////////////////////////////////////////////////////////////////////////////

/// Field type codes used in UDT schema JSON and CounterMeta.nUdtFieldType.
/// Values are identical to UdtFieldType in UdtSchemaParser.h (cast to uint8_t).
enum class Kv8UdtFieldType : uint8_t
{
    I8  = 0, U8  = 1, I16 = 2, U16 = 3,
    I32 = 4, U32 = 5, I64 = 6, U64 = 7,
    F32 = 8, F64 = 9, RATIONAL = 10
};

/// Decode one UDT field from packed payload bytes into a double.
/// nType must be a Kv8UdtFieldType cast to uint8_t.
/// 'src' must point to at least Kv8UdtFieldWireSize(nType) valid bytes.
inline double Kv8DecodeUdtField(const uint8_t* src, uint8_t nType)
{
    switch (static_cast<Kv8UdtFieldType>(nType))
    {
    case Kv8UdtFieldType::I8:  { int8_t   v; memcpy(&v, src, 1); return (double)v; }
    case Kv8UdtFieldType::U8:  { uint8_t  v; memcpy(&v, src, 1); return (double)v; }
    case Kv8UdtFieldType::I16: { int16_t  v; memcpy(&v, src, 2); return (double)v; }
    case Kv8UdtFieldType::U16: { uint16_t v; memcpy(&v, src, 2); return (double)v; }
    case Kv8UdtFieldType::I32: { int32_t  v; memcpy(&v, src, 4); return (double)v; }
    case Kv8UdtFieldType::U32: { uint32_t v; memcpy(&v, src, 4); return (double)v; }
    case Kv8UdtFieldType::I64: { int64_t  v; memcpy(&v, src, 8); return (double)v; }
    case Kv8UdtFieldType::U64: { uint64_t v; memcpy(&v, src, 8); return (double)v; }
    case Kv8UdtFieldType::F32: { float    v; memcpy(&v, src, 4); return (double)v; }
    case Kv8UdtFieldType::F64: { double   v; memcpy(&v, src, 8); return v;         }
    case Kv8UdtFieldType::RATIONAL:
    {
        int32_t num; uint32_t den;
        memcpy(&num, src,     4);
        memcpy(&den, src + 4, 4);
        return (den != 0u) ? ((double)num / (double)den) : 0.0;
    }
    }
    return 0.0;
}

/// Wire size in bytes for a Kv8UdtFieldType.
inline uint16_t Kv8UdtFieldWireSize(uint8_t nType)
{
    switch (static_cast<Kv8UdtFieldType>(nType))
    {
    case Kv8UdtFieldType::I8:  case Kv8UdtFieldType::U8:  return 1;
    case Kv8UdtFieldType::I16: case Kv8UdtFieldType::U16: return 2;
    case Kv8UdtFieldType::I32: case Kv8UdtFieldType::U32:
    case Kv8UdtFieldType::F32:                             return 4;
    case Kv8UdtFieldType::I64: case Kv8UdtFieldType::U64:
    case Kv8UdtFieldType::F64: case Kv8UdtFieldType::RATIONAL: return 8;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Trace log wire format (Phase L1)
////////////////////////////////////////////////////////////////////////////////
//
// Two record types:
//
//   1. Site descriptor record  -- written to <ch>._registry exactly once per
//      call site per process run.  KafkaRegistryRecord with wCounterID =
//      KV8_CID_LOG_SITE; wNameLen = total tail length, wTopicLen = 0.
//      Tail layout (little-endian, packed):
//          uint16_t wFileLen
//          char     file[wFileLen]   (basename only, no terminator)
//          uint32_t dwLine
//          uint16_t wFuncLen
//          char     func[wFuncLen]   (no terminator)
//          uint16_t wFmtLen
//          char     fmt [wFmtLen]    (UTF-8, display only, no terminator)
//      Kafka message key = sprintf("%08X", dwHash) so log compaction keeps
//      the latest value for each call site.
//
//   2. Data record (Kv8LogRecord, 28-byte fixed header + variable payload)
//      -- written to <ch>.<sid>._log on every log emission.  Payload is
//      pre-formatted UTF-8 text in the L2 path (bFlags bit 0 set) or packed
//      typed args (Kv8LogArgType) in the future L2+ path.
//
// Source location (file/line/function) lives only in the registry record.
// Data records carry only dwSiteHash (4 bytes), eliminating per-record
// overhead for source-location strings.
////////////////////////////////////////////////////////////////////////////////

/// Severity level carried in Kv8LogRecord::bLevel.
enum class Kv8LogLevel : uint8_t
{
    Debug   = 0,
    Info    = 1,
    Warning = 2,
    Error   = 3,
    Fatal   = 4,
};
static const uint8_t KV8_LOG_LEVEL_COUNT = 5u;

/// Type tags used by the future packed-arg payload format (L2+).
/// Not used in the L2 raw-text path; reserved here so the wire enum is stable.
enum class Kv8LogArgType : uint8_t
{
    I32  = 0,   ///< int32_t  (4 bytes) -- covers int/short/char via promotion
    U32  = 1,   ///< uint32_t (4 bytes)
    I64  = 2,   ///< int64_t  (8 bytes)
    U64  = 3,   ///< uint64_t (8 bytes)
    F64  = 4,   ///< double   (8 bytes) -- covers float via promotion
    STR  = 5,   ///< uint16_t length prefix + UTF-8 bytes (no null terminator)
};

/// Magic constant in Kv8LogRecord::dwMagic ("KV8T" little-endian).
static const uint32_t KV8_LOG_MAGIC = 0x4B563854u;

/// Maximum payload length (text or packed args) carried in one log record.
/// Mirrors the producer-side stack buffer used by KV8_LOGF.
static const uint16_t KV8_LOG_MAX_PAYLOAD = 4095u;

/// bFlags bit 0: payload is pre-formatted UTF-8 text (L2 raw-text path).
static const uint8_t KV8_LOG_FLAG_TEXT = 0x01u;

#pragma pack(push, 1)

/// Fixed 28-byte header of a single trace log record.  Followed by wArgLen
/// payload bytes (text in the L2 path, packed typed args in L2+).
struct Kv8LogRecord
{
    uint32_t dwMagic;       ///< KV8_LOG_MAGIC
    uint32_t dwSiteHash;    ///< FNV-32 of (basename, line, function) -- registry key
    uint64_t tsNs;          ///< Wall-clock nanoseconds since Unix epoch
    uint32_t dwThreadID;    ///< OS thread ID at moment of call
    uint16_t wCpuID;        ///< CPU core index at moment of call
    uint8_t  bLevel;        ///< Kv8LogLevel cast to uint8_t
    uint8_t  bFlags;        ///< Bit 0 = KV8_LOG_FLAG_TEXT (raw text payload)
    uint16_t wArgLen;       ///< Byte length of payload that follows this header
    uint16_t wReserved;     ///< Reserved; must be zero
    // Followed by wArgLen bytes of payload.
};
static_assert(sizeof(Kv8LogRecord) == 28, "Kv8LogRecord must be 28 bytes");

#pragma pack(pop)

/// FNV-32 over a single contiguous byte run.  Returns the canonical FNV-1a
/// value (initial offset 2166136261, prime 16777619).  The result is never
/// remapped here; callers that use 0 as a sentinel must remap themselves.
inline uint32_t Kv8FNV32(const void* pData, size_t cbData)
{
    uint32_t h = 2166136261u;
    const uint8_t* p = static_cast<const uint8_t*>(pData);
    for (size_t i = 0; i < cbData; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/// Compute the call-site hash from (basename, line, function).
///
/// The hash is FNV-32 over the concatenation
///     basename + '\0' + decimal(line) + '\0' + function
/// which is unique per call site -- (file, line) alone fix one source
/// location, function is appended for human readability and to disambiguate
/// the theoretical case of two files sharing a basename.
///
/// The result is never zero: zero is reserved as the "not yet registered"
/// sentinel used by the producer-side static cache.  A canonical-FNV result
/// of zero is remapped to 1.
inline uint32_t Kv8LogSiteHash(const char* pFile, uint16_t wFileLen,
                               uint32_t    dwLine,
                               const char* pFunc, uint16_t wFuncLen)
{
    uint32_t h = 2166136261u;
    if (pFile)
        for (uint16_t i = 0; i < wFileLen; ++i) { h ^= (uint8_t)pFile[i]; h *= 16777619u; }
    h ^= 0u;             h *= 16777619u;  // explicit '\0' separator
    char szLine[16];
    int  cbLine = snprintf(szLine, sizeof(szLine), "%u", static_cast<unsigned>(dwLine));
    if (cbLine < 0) cbLine = 0;
    for (int i = 0; i < cbLine; ++i)         { h ^= (uint8_t)szLine[i]; h *= 16777619u; }
    h ^= 0u;             h *= 16777619u;  // explicit '\0' separator
    if (pFunc)
        for (uint16_t i = 0; i < wFuncLen; ++i) { h ^= (uint8_t)pFunc[i]; h *= 16777619u; }
    return h ? h : 1u;
}

/// Encode a log-site descriptor tail (the variable-length bytes that follow
/// a KafkaRegistryRecord with wCounterID == KV8_CID_LOG_SITE).
///
/// Output layout matches the Trace log wire format documentation above.
/// Returns the number of bytes written to outBuf, or 0 if buffers are
/// invalid or cbOut is too small.
inline size_t Kv8EncodeLogSiteTail(const char* pFile, uint16_t wFileLen,
                                   uint32_t    dwLine,
                                   const char* pFunc, uint16_t wFuncLen,
                                   const char* pFmt,  uint16_t wFmtLen,
                                   void* pOut, size_t cbOut)
{
    const size_t cbNeed = static_cast<size_t>(2u + wFileLen)
                        + static_cast<size_t>(4u)
                        + static_cast<size_t>(2u + wFuncLen)
                        + static_cast<size_t>(2u + wFmtLen);
    if (!pOut || cbOut < cbNeed) return 0;
    uint8_t* p = static_cast<uint8_t*>(pOut);
    memcpy(p, &wFileLen, 2); p += 2;
    if (wFileLen) memcpy(p, pFile, wFileLen); p += wFileLen;
    memcpy(p, &dwLine, 4);   p += 4;
    memcpy(p, &wFuncLen, 2); p += 2;
    if (wFuncLen) memcpy(p, pFunc, wFuncLen); p += wFuncLen;
    memcpy(p, &wFmtLen, 2);  p += 2;
    if (wFmtLen)  memcpy(p, pFmt,  wFmtLen);  p += wFmtLen;
    return cbNeed;
}

/// Decoded view of a log-site descriptor tail.  String views point into the
/// caller's input buffer; lifetime is bounded by that buffer.
struct Kv8LogSiteInfo
{
    std::string_view sFile;
    std::string_view sFunc;
    std::string_view sFmt;
    uint32_t         dwLine = 0;
};

/// Decode a log-site descriptor tail produced by Kv8EncodeLogSiteTail.
/// Returns true on success; false if the tail is truncated or malformed.
inline bool Kv8DecodeLogSiteTail(const void* pData, size_t cbData,
                                 Kv8LogSiteInfo& out)
{
    if (!pData) return false;
    const uint8_t* p   = static_cast<const uint8_t*>(pData);
    const uint8_t* end = p + cbData;

    uint16_t wFileLen = 0;
    if (p + 2 > end) return false;
    memcpy(&wFileLen, p, 2); p += 2;
    if (p + wFileLen > end) return false;
    out.sFile = std::string_view(reinterpret_cast<const char*>(p), wFileLen);
    p += wFileLen;

    if (p + 4 > end) return false;
    memcpy(&out.dwLine, p, 4); p += 4;

    uint16_t wFuncLen = 0;
    if (p + 2 > end) return false;
    memcpy(&wFuncLen, p, 2); p += 2;
    if (p + wFuncLen > end) return false;
    out.sFunc = std::string_view(reinterpret_cast<const char*>(p), wFuncLen);
    p += wFuncLen;

    uint16_t wFmtLen = 0;
    if (p + 2 > end) return false;
    memcpy(&wFmtLen, p, 2); p += 2;
    if (p + wFmtLen > end) return false;
    out.sFmt = std::string_view(reinterpret_cast<const char*>(p), wFmtLen);
    p += wFmtLen;

    return true;
}

/// Decode one Kv8LogRecord from raw Kafka payload bytes.
///
/// Returns false when the record is malformed (wrong magic, truncated,
/// invalid level, or wReserved != 0).  Trailing bytes after the declared
/// payload are NOT an error -- the function ignores them and returns true,
/// preserving forward-compatibility for future header extensions.
///
/// On success outPayload is populated as a raw view into pData (lifetime
/// bounded by the caller's buffer).
inline bool Kv8DecodeLogRecord(const void*       pData,
                               size_t            cbData,
                               Kv8LogRecord&     outHeader,
                               std::string_view& outPayload)
{
    if (!pData || cbData < sizeof(Kv8LogRecord)) return false;
    memcpy(&outHeader, pData, sizeof(Kv8LogRecord));
    if (outHeader.dwMagic   != KV8_LOG_MAGIC)            return false;
    if (outHeader.bLevel    >= KV8_LOG_LEVEL_COUNT)      return false;
    if (outHeader.wReserved != 0u)                       return false;
    if (outHeader.wArgLen   >  KV8_LOG_MAX_PAYLOAD)      return false;
    if (cbData < sizeof(Kv8LogRecord) + outHeader.wArgLen) return false;
    const char* p = static_cast<const char*>(pData) + sizeof(Kv8LogRecord);
    outPayload = std::string_view(p, outHeader.wArgLen);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Session and counter data models
////////////////////////////////////////////////////////////////////////////////

/// Metadata for one counter, decoded from a KafkaRegistryRecord.
struct CounterMeta
{
    std::string sName;       ///< Human-readable counter name (UTF-8)
    uint16_t    wCounterID;  ///< Original counter ID from the producer
    uint16_t    wFlags;      ///< Bit 0 = enabled at creation time; bit 1 = UDT feed
    double      dbMin;
    double      dbAlarmMin;
    double      dbMax;
    double      dbAlarmMax;
    std::string sDataTopic;  ///< Kafka topic that carries this counter's data

    // UDT feed descriptor (populated only when bIsUdtFeed == true).
    bool        bIsUdtFeed   = false; ///< true when KV8_FLAG_UDT is set in wFlags
    uint32_t    dwSchemaHash = 0;     ///< FNV-32 of the schema JSON (links to schema record)
    std::string sSchemaJson;          ///< Full JSON schema text (populated during registry scan)

    // UDT virtual scalar field -- synthesized one per schema field in DiscoverSessions.
    // These entries (bIsUdtVirtualField==true) are added alongside the feed descriptor in
    // hashToCounters[schema_hash] and carry all decoding info needed by ConsumerThread.
    bool     bIsUdtVirtualField = false; ///< true = decoded scalar from a UDT binary payload
    uint16_t wUdtFieldOffset    = 0;     ///< Byte offset within the packed UDT payload
    uint8_t  nUdtFieldType      = 0;     ///< Kv8UdtFieldType cast to uint8_t
    std::string sDisplayName;            ///< Human-readable label (from schema "d" key); empty = use sName leaf segment
};

/// Aggregated metadata for one producer session, built from the registry scan.
/// A session corresponds to a single Kv8 Client lifetime on the producer side.
struct SessionMeta
{
    std::string sSessionID;     ///< Session ID segment (e.g. 20240101T120000Z-AABB-CCDD)
    std::string sSessionPrefix; ///< "<channel>.<sessionID>"
    std::string sName;          ///< Human-readable session name (from 0xFFFE record)
    std::string sLogTopic;      ///< "<channel>.<sessionID>._log"
    std::string sControlTopic;  ///< "<channel>.<sessionID>._ctl"

    /// Registry hash -> group display name  (from 0xFFFF records).
    std::map<uint32_t, std::string> hashToGroup;

    /// Registry hash -> counter list  (from regular counter records).
    std::map<uint32_t, std::vector<CounterMeta>> hashToCounters;

    /// All unique data topics referenced by any counter in this session.
    std::set<std::string> dataTopics;

    // Per-data-topic metadata sourced from the 0xFFFF group record:
    std::map<std::string, std::string> topicToGroupName;  ///< topic -> group display name
    std::map<std::string, uint32_t>    topicToGroupHash;  ///< topic -> dwHash
    std::map<std::string, uint64_t>    topicToFrequency;  ///< topic -> QPC freq (Hz)
    std::map<std::string, uint64_t>    topicToTimerValue; ///< topic -> QPC anchor tick
    std::map<std::string, uint32_t>    topicToTimeHi;     ///< topic -> FILETIME hi
    std::map<std::string, uint32_t>    topicToTimeLo;     ///< topic -> FILETIME lo
    /// Direct lookup from a per-counter topic name to the counter it carries.
    /// Populated only for the new per-counter topic layout (one counter per topic).
    /// Empty when a session was recorded with the legacy shared-topic layout.
    std::map<std::string, CounterMeta> topicToCounter;

    /// Owned snapshot of one trace-log call site (KV8_CID_LOG_SITE record).
    /// Strings are owned to keep the SessionMeta self-contained.
    struct LogSiteRecord
    {
        std::string sFile;
        std::string sFunc;
        std::string sFmt;
        uint32_t    dwLine = 0;
    };
    /// Trace-log call sites known at session-discovery time, keyed by FNV-32
    /// site hash.  Live registrations during a session are merged into this
    /// map by the consumer thread.
    std::map<uint32_t, LogSiteRecord> logSites;
};

////////////////////////////////////////////////////////////////////////////////
// Utility helpers
////////////////////////////////////////////////////////////////////////////////

/// Replace every '/' in a channel prefix with '.' to match Kafka topic naming.
/// Mirrors the sanitisation performed by ClKafka on the producer side.
inline std::string Kv8SanitizeChannel(std::string s)
{
    for (char &c : s) { if (c == '/') c = '.'; }
    return s;
}

/// Extract the channel ID (encoded as 8-hex-digit field) from a data topic name.
///
/// Handles both topic layouts:
///   Legacy shared-topic : <prefix>.d.XXXXXXXX         (8 hex digits at end)
///   Per-counter topic   : <prefix>.d.XXXXXXXX.YYYY    (8 hex digits, dot, counter ID)
///
/// Searches for the ".d." separator and parses the 8 hex digits that follow it.
/// Returns true on success; o_dwHash is set to the parsed channel ID.
/// Returns false when the topic name does not contain a ".d." segment.
///
/// Accepts std::string_view (zero-copy); std::string converts implicitly.
inline bool Kv8ExtractHashFromTopic(std::string_view sTopic, uint32_t &o_dwHash)
{
    size_t pos = sTopic.find(".d.");
    if (pos == std::string_view::npos) return false;
    // pHex points into the original buffer (null-terminated from rdkafka)
    const char *pHex = sTopic.data() + pos + 3; // skip ".d."
    if (sTopic.size() - pos - 3 < 8) return false;
    char buf[9];
    memcpy(buf, pHex, 8);
    buf[8] = '\0';
    char *pEnd = nullptr;
    unsigned long val = strtoul(buf, &pEnd, 16);
    if (!pEnd || pEnd != buf + 8) return false;
    // Verify the 8 chars are all hex digits
    for (int i = 0; i < 8; ++i)
        if (!isxdigit((unsigned char)buf[i])) return false;
    o_dwHash = static_cast<uint32_t>(val);
    return true;
}

/// Format a Kafka broker timestamp (milliseconds since Unix epoch) as ISO 8601.
/// Example: Kv8FormatKafkaTimestamp(1704067200123) -> "2024-01-01T00:00:00.123Z"
inline std::string Kv8FormatKafkaTimestamp(int64_t tsMs)
{
    if (tsMs <= 0) return "1970-01-01T00:00:00.000Z";
    time_t tSec = static_cast<time_t>(tsMs / 1000);
    int    iMs  = static_cast<int>(tsMs % 1000);
    struct tm tmBuf;
#ifdef _WIN32
    gmtime_s(&tmBuf, &tSec);
#else
    gmtime_r(&tSec, &tmBuf);
#endif
    char sBuf[32], sOut[40];
    strftime(sBuf, sizeof(sBuf), "%Y-%m-%dT%H:%M:%S", &tmBuf);
    snprintf(sOut, sizeof(sOut), "%s.%03dZ", sBuf, iMs);
    return std::string(sOut);
}

} // namespace kv8
