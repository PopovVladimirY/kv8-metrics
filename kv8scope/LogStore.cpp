// kv8scope -- Kv8 Software Oscilloscope
// LogStore.cpp -- Per-session decoded trace-log entries (Phase L4).

#include "LogStore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

void LogStore::SeedLogSites(
    const std::map<uint32_t, kv8::SessionMeta::LogSiteRecord>& sites)
{
    for (const auto& kv : sites)
        m_sites.insert(kv);

    // Re-resolve any already-loaded entries whose site was previously unknown.
    for (auto& e : m_entries)
        if (!e.bSiteResolved)
            ResolveSiteForEntry(e);
}

void LogStore::PushLogRecordFromKafka(const void* pData, size_t cbData)
{
    if (!pData || cbData == 0) return;

    kv8::Kv8LogRecord    hdr;
    std::string_view     sPayload;
    if (!kv8::Kv8DecodeLogRecord(pData, cbData, hdr, sPayload))
        return;

    Entry e;
    e.tsNs       = hdr.tsNs;
    e.eLevel     = static_cast<kv8::Kv8LogLevel>(hdr.bLevel);
    e.dwThreadID = hdr.dwThreadID;
    e.wCpuID     = hdr.wCpuID;
    e.dwSiteHash = hdr.dwSiteHash;

    if (hdr.bFlags & kv8::KV8_LOG_FLAG_TEXT)
    {
        e.sMessage.assign(sPayload.data(), sPayload.size());
    }
    else
    {
        // Typed-args path is reserved for L2+; for now stash a placeholder so
        // operators see that the record was received but cannot be rendered.
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "<typed args, len=%zu>", sPayload.size());
        e.sMessage = buf;
    }

    {
        std::lock_guard<std::mutex> lk(m_mtxPending);
        m_pendingEntries.push_back(std::move(e));
    }
}

void LogStore::PushLogSiteFromKafka(const void* pData, size_t cbData)
{
    if (!pData || cbData < sizeof(kv8::KafkaRegistryRecord)) return;

    const auto* r = reinterpret_cast<const kv8::KafkaRegistryRecord*>(pData);
    if (r->wVersion   != kv8::KV8_REGISTRY_VERSION)        return;
    if (r->wCounterID != kv8::KV8_CID_LOG_SITE)            return;
    if (cbData < sizeof(kv8::KafkaRegistryRecord) + r->wNameLen) return;

    const char* pTail = reinterpret_cast<const char*>(r + 1);
    kv8::Kv8LogSiteInfo info;
    if (!kv8::Kv8DecodeLogSiteTail(pTail, r->wNameLen, info))
        return;

    kv8::SessionMeta::LogSiteRecord rec;
    rec.sFile  = std::string(info.sFile);
    rec.sFunc  = std::string(info.sFunc);
    rec.sFmt   = std::string(info.sFmt);
    rec.dwLine = info.dwLine;

    {
        std::lock_guard<std::mutex> lk(m_mtxPending);
        m_pendingSites.emplace_back(r->dwHash, std::move(rec));
    }
}

void LogStore::DrainPending()
{
    std::vector<Entry>                                          newEntries;
    std::vector<std::pair<uint32_t, kv8::SessionMeta::LogSiteRecord>> newSites;
    {
        std::lock_guard<std::mutex> lk(m_mtxPending);
        newEntries.swap(m_pendingEntries);
        newSites.swap(m_pendingSites);
    }

    if (!newSites.empty())
    {
        for (auto& kv : newSites)
            m_sites[kv.first] = std::move(kv.second);

        // Re-resolve any unresolved earlier entries (cheap when sites are rare).
        for (auto& e : m_entries)
            if (!e.bSiteResolved)
                ResolveSiteForEntry(e);
    }

    if (!newEntries.empty())
    {
        const size_t firstNew = m_entries.size();
        for (auto& e : newEntries)
        {
            ResolveSiteForEntry(e);
            m_entries.push_back(std::move(e));
        }

        // Sort only when arrivals are out of order.  Network reordering across
        // partitions is rare for ._log (single producer), but a single
        // out-of-order pair forces a full sort.  std::sort + lambda; cheap
        // for the small frame-batch sizes we expect (< 1k entries per drain).
        bool bSorted = true;
        for (size_t i = firstNew; i < m_entries.size(); ++i)
        {
            if (i > 0 && m_entries[i].tsNs < m_entries[i - 1].tsNs)
            { bSorted = false; break; }
        }
        if (!bSorted)
        {
            std::sort(m_entries.begin(), m_entries.end(),
                [](const Entry& a, const Entry& b) { return a.tsNs < b.tsNs; });
        }
    }
}

size_t LogStore::CountVisible() const
{
    size_t n = 0;
    for (const auto& e : m_entries)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << static_cast<unsigned>(e.eLevel));
        if (!(m_uLevelMask & bit)) continue;
        if (!m_sFilter.empty() &&
            e.sMessage.find(m_sFilter) == std::string::npos)
            continue;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// ExportCSV -- write entries in [tsMinNs, tsMaxNs] to a CSV file.
// Mirrors WaveformRenderer::ExportCSV in spirit (long format, RFC 4180
// quoting) but with the columns the LogPanel exposes.  No locks taken;
// caller must invoke from the render thread (same constraint as GetAll()).
// ---------------------------------------------------------------------------
bool LogStore::ExportCSV(uint64_t tsMinNs, uint64_t tsMaxNs,
                          uint8_t uLevelMask, const std::string& sFilter,
                          const std::string& sPath,
                          size_t* pnWritten) const
{
    if (sPath.empty())
        return false;

#ifdef _WIN32
    FILE* fp = nullptr;
    fopen_s(&fp, sPath.c_str(), "wb");
#else
    FILE* fp = std::fopen(sPath.c_str(), "wb");
#endif
    if (!fp)
        return false;

    // Header row.
    std::fprintf(fp,
        "timestamp_iso,timestamp_ns,level,cpu,thread,file,line,function,message\n");

    static const char* const kLevelNames[5] =
        { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

    auto write_quoted = [&](const std::string& s)
    {
        std::fputc('"', fp);
        for (char c : s)
        {
            if (c == '"')
            {
                std::fputc('"', fp);
                std::fputc('"', fp);
            }
            else
            {
                std::fputc(static_cast<unsigned char>(c), fp);
            }
        }
        std::fputc('"', fp);
    };

    size_t nWritten = 0;
    for (const auto& e : m_entries)
    {
        if (e.tsNs < tsMinNs || e.tsNs > tsMaxNs)
            continue;

        const unsigned uLvl = static_cast<unsigned>(e.eLevel);
        const uint8_t  bit  = static_cast<uint8_t>(1u << uLvl);
        if (!(uLevelMask & bit))
            continue;

        if (!sFilter.empty() &&
            e.sMessage.find(sFilter) == std::string::npos)
            continue;

        // ISO-8601 UTC with microsecond precision (matches LogPanel display).
        const uint64_t qwSec = e.tsNs / 1000000000ULL;
        const uint64_t qwUs  = (e.tsNs % 1000000000ULL) / 1000ULL;
        std::time_t t = static_cast<std::time_t>(qwSec);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char szIso[40];
        std::snprintf(szIso, sizeof(szIso),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%06lluZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<unsigned long long>(qwUs));

        const char* sLvl = (uLvl < 5) ? kLevelNames[uLvl] : "?????";

        std::fprintf(fp, "%s,%llu,%s,%u,0x%08X,",
                     szIso,
                     static_cast<unsigned long long>(e.tsNs),
                     sLvl,
                     static_cast<unsigned>(e.wCpuID),
                     static_cast<unsigned>(e.dwThreadID));
        write_quoted(e.sFile);
        std::fprintf(fp, ",%u,", static_cast<unsigned>(e.dwLine));
        write_quoted(e.sFunc);
        std::fputc(',', fp);
        write_quoted(e.sMessage);
        std::fputc('\n', fp);
        ++nWritten;
    }

    std::fclose(fp);
    if (pnWritten) *pnWritten = nWritten;
    return true;
}

void LogStore::ResolveSiteForEntry(Entry& e) const
{
    auto it = m_sites.find(e.dwSiteHash);
    if (it != m_sites.end())
    {
        e.sFile         = it->second.sFile;
        e.sFunc         = it->second.sFunc;
        e.dwLine        = it->second.dwLine;
        e.bSiteResolved = true;
    }
    else
    {
        // Render placeholder until the site descriptor arrives.
        char buf[24];
        std::snprintf(buf, sizeof(buf), "<site:%08X>", e.dwSiteHash);
        e.sFile = buf;
        e.sFunc.clear();
        e.dwLine = 0;
        e.bSiteResolved = false;
    }
}
