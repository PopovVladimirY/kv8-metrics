// kv8scope -- Kv8 Software Oscilloscope
// AnnotationStore.cpp -- Annotation store with Kafka persistence.
//
// Kafka format (Section 14.2):
//   Topic  : <channel>.<sessionID>._annotations  (log-compacted)
//   Key    : annotation ID string
//   Value  : JSON record (below), OR a JSON record with "deleted":true for removes.
//
// JSON record:
//   { "id":"1", "version":1, "timestamp_us":1740764415000000,
//     "title":"...", "description":"...",
//     "created_at":"2026-02-28T15:30:15Z", "created_by":"kv8scope", "type":0 }
//
// Note: log-compaction tombstones (null value) are not used because the
// IKv8Consumer callback does not expose the Kafka message key separately.
// Instead, deletes are published as JSON records with "deleted":true.

#include "AnnotationStore.h"

#include <kv8/IKv8Producer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// ===========================================================================
// Internal JSON helpers (minimal, no external dependencies)
// ===========================================================================

// ---------------------------------------------------------------------------
// JsonEscape -- escape a string suitable for embedding in a JSON value.
// Escapes \, ", and control characters (0x00-0x1F).
// ---------------------------------------------------------------------------
static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        if (c == '"')       { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (c < 0x20)
        {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            out += esc;
        }
        else { out += (char)c; }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JsonUnescape -- reverse of JsonEscape; handles \", \\, \n, \r, \t.
// Does NOT handle full Unicode escape (\uXXXX) -- sufficient for our data.
// ---------------------------------------------------------------------------
static std::string JsonUnescape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            ++i;
            switch (s[i])
            {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            default:   out += s[i]; break;
            }
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JsonExtractString -- extract the value for "key" from a flat JSON object.
// Returns empty string if key not found.
// ---------------------------------------------------------------------------
static std::string JsonExtractString(const char* json, const char* key)
{
    // Build search pattern: "key":
    char pattern[128];
    std::snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = std::strstr(json, pattern);
    if (!p) return {};
    p += std::strlen(pattern);
    // Skip whitespace
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return {};
    ++p; // skip opening quote
    std::string val;
    while (*p && *p != '"')
    {
        if (*p == '\\' && *(p + 1))
        {
            val += '\\';
            val += *(p + 1);
            p += 2;
        }
        else
        {
            val += *p++;
        }
    }
    return JsonUnescape(val);
}

// ---------------------------------------------------------------------------
// JsonExtractInt64 -- extract the integer value for "key" from a flat JSON object.
// ---------------------------------------------------------------------------
static int64_t JsonExtractInt64(const char* json, const char* key)
{
    char pattern[128];
    std::snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = std::strstr(json, pattern);
    if (!p) return 0;
    p += std::strlen(pattern);
    while (*p == ' ' || *p == '\t') ++p;
    // Handles optional minus sign
    int64_t val = 0;
    int sign = 1;
    if (*p == '-') { sign = -1; ++p; }
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p++ - '0'); }
    return sign * val;
}

// ---------------------------------------------------------------------------
// JsonExtractBool -- extract a boolean value for "key" ("true"/"false").
// ---------------------------------------------------------------------------
static bool JsonExtractBool(const char* json, const char* key)
{
    char pattern[128];
    std::snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = std::strstr(json, pattern);
    if (!p) return false;
    p += std::strlen(pattern);
    while (*p == ' ' || *p == '\t') ++p;
    return (std::strncmp(p, "true", 4) == 0);
}

// ===========================================================================
// Static type metadata
// ===========================================================================

const char* AnnotationStore::TypeName(AnnotationType t)
{
    switch (t)
    {
    case AnnotationType::Info:      return "Info";
    case AnnotationType::Warning:   return "Warning";
    case AnnotationType::Error:     return "Error";
    case AnnotationType::Milestone: return "Milestone";
    case AnnotationType::Note:      return "Note";
    default:                        return "Unknown";
    }
}

ImVec4 AnnotationStore::TypeColor(AnnotationType t)
{
    switch (t)
    {
    // cyan         #00C8FF -- info-level, high-visibility on dark backgrounds
    case AnnotationType::Info:      return ImVec4(0.000f, 0.784f, 1.000f, 1.0f);
    // amber        #FFB800 -- warning, easy to distinguish from error
    case AnnotationType::Warning:   return ImVec4(1.000f, 0.722f, 0.000f, 1.0f);
    // orange-red   #FF4422 -- fault / error events
    case AnnotationType::Error:     return ImVec4(1.000f, 0.267f, 0.133f, 1.0f);
    // bright green #00CC66 -- milestones / key events
    case AnnotationType::Milestone: return ImVec4(0.000f, 0.800f, 0.400f, 1.0f);
    // lavender     #C488FF -- free-form personal notes
    case AnnotationType::Note:      return ImVec4(0.769f, 0.533f, 1.000f, 1.0f);
    default:                        return ImVec4(1.000f, 1.000f, 1.000f, 1.0f);
    }
}

// ===========================================================================
// Kafka sink
// ===========================================================================

void AnnotationStore::SetKafkaSink(kv8::IKv8Producer* pProducer,
                                   std::string         sTopic)
{
    m_pProducer   = pProducer;
    m_sKafkaTopic = std::move(sTopic);
}

// ---------------------------------------------------------------------------
// SerializeAnnotation -- produce the JSON string for an annotation record.
// ---------------------------------------------------------------------------
static std::string SerializeAnnotation(const Annotation& ann)
{
    // timestamp_us: convert epoch seconds (double) -> microseconds (int64)
    int64_t ts_us = static_cast<int64_t>(ann.dTimestamp * 1.0e6);

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "{\"id\":\"%s\",\"version\":1,\"timestamp_us\":%lld,"
        "\"title\":\"%s\",\"description\":\"%s\","
        "\"created_at\":\"%s\",\"created_by\":\"%s\",\"type\":%d}",
        ann.sID.c_str(),
        static_cast<long long>(ts_us),
        JsonEscape(ann.sTitle).c_str(),
        JsonEscape(ann.sDescription).c_str(),
        ann.sCreatedAt.c_str(),
        ann.sCreatedBy.c_str(),
        static_cast<int>(ann.eType));
    return buf;
}

// ---------------------------------------------------------------------------
// SerializeDeleteMarker -- produce the JSON tombstone for a deleted annotation.
// ---------------------------------------------------------------------------
static std::string SerializeDeleteMarker(const std::string& sID)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"deleted\":true}", sID.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// ProduceAnnotation -- publish a JSON record to the _annotations topic.
// The Kafka message key is the annotation ID string.
// ---------------------------------------------------------------------------
void AnnotationStore::ProduceAnnotation(const Annotation& ann)
{
    if (!m_pProducer || m_sKafkaTopic.empty())
        return;

    std::string json = SerializeAnnotation(ann);
    m_pProducer->Produce(m_sKafkaTopic,
                         json.c_str(), json.size(),
                         ann.sID.c_str(), ann.sID.size());
    m_pProducer->Flush(0); // non-blocking -- fire and forget
}

// ---------------------------------------------------------------------------
// ProduceTombstone -- publish a delete-marker JSON to the _annotations topic.
// ---------------------------------------------------------------------------
void AnnotationStore::ProduceTombstone(const std::string& sID)
{
    if (!m_pProducer || m_sKafkaTopic.empty())
        return;

    std::string json = SerializeDeleteMarker(sID);
    m_pProducer->Produce(m_sKafkaTopic,
                         json.c_str(), json.size(),
                         sID.c_str(), sID.size());
    m_pProducer->Flush(0);
}

// ===========================================================================
// Mutating operations
// ===========================================================================

void AnnotationStore::Add(double dTimestamp,
                          const std::string& sTitle,
                          const std::string& sDesc,
                          AnnotationType eType)
{
    Annotation ann;

    // Assign a unique ID.  Format: <wallClock_hex>-<seq> combining the
    // wall-clock creation time with a per-instance sequence number.
    // Wall-clock time (not data timestamp) prevents ID collisions when
    // m_nNextID resets to 1 after a restart and the same data-time point
    // is annotated again.
    {
        char buf[48];
        std::time_t now    = std::time(nullptr);  // wall-clock at creation
        uint64_t    seq    = m_nNextID++;
        std::snprintf(buf, sizeof(buf), "%08x-%llu",
                      static_cast<unsigned>(now),
                      static_cast<unsigned long long>(seq));
        ann.sID = buf;
    }

    ann.dTimestamp   = dTimestamp;
    ann.sTitle       = sTitle;
    ann.sDescription = sDesc;
    ann.eType        = eType;
    ann.sCreatedBy   = "kv8scope";

    // ISO 8601 wall-clock timestamp for display
    {
        std::time_t tSec = static_cast<std::time_t>(dTimestamp);
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &tSec);
#else
        gmtime_r(&tSec, &tmv);
#endif
        char tbuf[32];
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        ann.sCreatedAt = tbuf;
    }

    m_annotations.push_back(ann);
    SortByTimestamp();

    // Persist to Kafka.
    ProduceAnnotation(ann);
}

void AnnotationStore::Edit(const std::string& sID,
                           const std::string& sTitle,
                           const std::string& sDesc,
                           AnnotationType eType)
{
    for (auto& ann : m_annotations)
    {
        if (ann.sID == sID)
        {
            ann.sTitle       = sTitle;
            ann.sDescription = sDesc;
            ann.eType        = eType;

            // Re-publish updated record to Kafka (same key overwrites).
            ProduceAnnotation(ann);
            return;
        }
    }
}

void AnnotationStore::Delete(const std::string& sID)
{
    // Soft-delete: mark the annotation as deleted in the local store so
    // it can still be displayed with transparency when "show deleted" is on.
    // A JSON delete marker is published to Kafka so other instances and
    // future loads know the annotation is deleted.
    for (auto& ann : m_annotations)
    {
        if (ann.sID == sID)
        {
            ann.bDeleted = true;
            break;
        }
    }

    // Publish delete marker to Kafka.
    ProduceTombstone(sID);
}

// ===========================================================================
// Kafka pending queue (consumer thread -> render thread)
// ===========================================================================

void AnnotationStore::PushFromKafka(std::string sKey,
                                    const void* pData,
                                    size_t      cbData)
{
    PendingItem item;
    item.sKey  = std::move(sKey);
    if (pData && cbData > 0)
        item.sJson = std::string(static_cast<const char*>(pData), cbData);
    // empty sJson signals a tombstone / delete marker handled inside DrainPending

    std::lock_guard<std::mutex> lk(m_mtxPending);
    m_pendingItems.push_back(std::move(item));
}

void AnnotationStore::DrainPending()
{
    std::vector<PendingItem> batch;
    {
        std::lock_guard<std::mutex> lk(m_mtxPending);
        if (m_pendingItems.empty())
            return;
        batch.swap(m_pendingItems);
    }

    for (const auto& item : batch)
    {
        const char* json = item.sJson.c_str();

        // Empty payload = raw null tombstone (unusual; our deletes use JSON
        // markers, but handle defensively without re-publishing to Kafka).
        if (item.sJson.empty())
        {
            if (!item.sKey.empty())
            {
                for (auto& ann : m_annotations)
                {
                    if (ann.sID == item.sKey)
                    {
                        ann.bDeleted = true;
                        break;
                    }
                }
            }
            continue;
        }

        // JSON delete marker: {"id":"...","deleted":true}
        if (JsonExtractBool(json, "deleted"))
        {
            std::string id = JsonExtractString(json, "id");
            if (!id.empty())
            {
                for (auto& ann : m_annotations)
                {
                    if (ann.sID == id)
                    {
                        ann.bDeleted = true;
                        break;
                    }
                }
            }
            continue;
        }

        // Normal annotation record -- upsert (add if new, update if same ID).
        std::string sID     = JsonExtractString(json, "id");
        if (sID.empty())
            continue;

        int64_t ts_us = JsonExtractInt64(json, "timestamp_us");
        double  dTs   = static_cast<double>(ts_us) / 1.0e6;

        int     iType = static_cast<int>(JsonExtractInt64(json, "type"));
        if (iType < 0 || iType >= kTypeCount) iType = 0;
        AnnotationType eType = static_cast<AnnotationType>(iType);

        std::string sTitle = JsonExtractString(json, "title");
        std::string sDesc  = JsonExtractString(json, "description");
        std::string sCrAt  = JsonExtractString(json, "created_at");
        std::string sCrBy  = JsonExtractString(json, "created_by");

        // Try to find existing annotation with this ID.
        bool bFound = false;
        for (auto& ann : m_annotations)
        {
            if (ann.sID == sID)
            {
                ann.sTitle       = sTitle;
                ann.sDescription = sDesc;
                ann.eType        = eType;
                ann.dTimestamp   = dTs;
                ann.sCreatedAt   = sCrAt;
                ann.sCreatedBy   = sCrBy;
                bFound = true;
                break;
            }
        }

        if (!bFound)
        {
            Annotation ann;
            ann.sID          = sID;
            ann.dTimestamp   = dTs;
            ann.sTitle       = sTitle;
            ann.sDescription = sDesc;
            ann.eType        = eType;
            ann.sCreatedAt   = sCrAt;
            ann.sCreatedBy   = sCrBy;
            m_annotations.push_back(std::move(ann));
        }
    }

    SortByTimestamp();
}

// ===========================================================================
// Sort helper
// ===========================================================================

void AnnotationStore::SortByTimestamp()
{
    std::sort(m_annotations.begin(), m_annotations.end(),
              [](const Annotation& a, const Annotation& b)
              { return a.dTimestamp < b.dTimestamp; });
}

