// kv8scope -- Kv8 Software Oscilloscope
// LogStore.h -- Per-session decoded trace-log entries (Phase L4).
//
// Mirrors the AnnotationStore pattern: the consumer thread pushes raw
// Kafka payloads through PushLogRecordFromKafka(), and the render thread
// drains them into a stable, time-sorted vector once per frame via
// DrainPending().  All non-trivial work (decoding, sorting) happens on
// the render thread to keep the consumer hot path lock-free of UI state.
//
// Trace-log call-site descriptors (KV8_CID_LOG_SITE) are seeded by
// SessionMeta::logSites at construction; live additions arriving on
// ._registry are forwarded via PushLogSiteFromKafka().

#pragma once

#include <kv8/Kv8Types.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class LogStore
{
public:
    /// One fully decoded log entry, ready for rendering.
    struct Entry
    {
        uint64_t          tsNs        = 0;   // Wall-clock ns since Unix epoch
        kv8::Kv8LogLevel  eLevel      = kv8::Kv8LogLevel::Info;
        uint32_t          dwThreadID  = 0;
        uint16_t          wCpuID      = 0;
        uint32_t          dwLine      = 0;
        std::string       sFile;             // Basename only
        std::string       sFunc;
        std::string       sMessage;          // Pre-formatted UTF-8 text (L2 path)
        uint32_t          dwSiteHash  = 0;   // FNV-32 site hash
        bool              bSiteResolved = false; // false = file/func/line are placeholders
    };

    LogStore() = default;

    /// Seed with known call sites loaded by Kv8Consumer::DiscoverSessions.
    /// Idempotent: safe to call multiple times before the consumer thread starts.
    void SeedLogSites(const std::map<uint32_t, kv8::SessionMeta::LogSiteRecord>& sites);

    /// Consumer-thread entry point.  Decodes the raw Kafka payload of a
    /// ._log topic message and queues the result for the render thread.
    /// Malformed records are silently dropped.
    void PushLogRecordFromKafka(const void* pData, size_t cbData);

    /// Consumer-thread entry point.  Decodes a KV8_CID_LOG_SITE registry
    /// record (full KafkaRegistryRecord + tail) and queues the descriptor
    /// for the render thread.  Used for live registrations that occur
    /// after DiscoverSessions has already returned.
    void PushLogSiteFromKafka(const void* pData, size_t cbData);

    /// Render-thread entry point.  Merges all pending records and sites
    /// into the live vectors and re-sorts the entry vector by tsNs.
    /// Call once per frame.
    void DrainPending();

    /// Read-only access to the time-sorted entry vector (render thread only).
    const std::vector<Entry>& GetAll() const { return m_entries; }

    /// Read-only access to the call-site map (render thread only).
    const std::map<uint32_t, kv8::SessionMeta::LogSiteRecord>& GetSites() const
    { return m_sites; }

    /// Bitmask of enabled severity levels (bit N = level N).  Default = all on.
    void SetLevelMask(uint8_t mask) { m_uLevelMask = mask; }
    uint8_t GetLevelMask() const    { return m_uLevelMask; }

    /// Visible-window selector for the panel and renderer.  An entry is
    /// considered visible when (eLevel bit set in mask) AND tsNs >= tsMinNs
    /// AND tsNs <= tsMaxNs AND sMessage contains m_sFilter substring (case
    /// sensitive for now -- ASCII strings only).
    void SetTextFilter(std::string s) { m_sFilter = std::move(s); }
    const std::string& GetTextFilter() const { return m_sFilter; }

    /// Returns the number of currently visible entries that pass all filters.
    /// Cheap O(N) scan; render thread only.
    size_t CountVisible() const;

private:
    void ResolveSiteForEntry(Entry& e) const;

    // ---- Live data (render thread only) ---------------------------------
    std::vector<Entry>                                          m_entries;
    std::map<uint32_t, kv8::SessionMeta::LogSiteRecord>         m_sites;
    uint8_t                                                     m_uLevelMask = 0x1F; // bits 0..4 set
    std::string                                                 m_sFilter;

    // ---- Pending queues (consumer -> render) ----------------------------
    std::mutex                                                  m_mtxPending;
    std::vector<Entry>                                          m_pendingEntries;
    std::vector<std::pair<uint32_t, kv8::SessionMeta::LogSiteRecord>>
                                                                m_pendingSites;
};
