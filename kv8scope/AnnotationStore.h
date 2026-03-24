// kv8scope -- Kv8 Software Oscilloscope
// AnnotationStore.h -- Timeline annotation store with Kafka persistence.
//
// Each annotation marks a point on the time axis with a title,
// an optional description, and a type (Info/Warning/Error/Milestone/Note).
// Type determines the display color on the waveform.
//
// Kafka topic: <channel>.<sessionID>._annotations  (log-compacted)
// Key   = annotation ID string.
// Value = JSON record (see Section 14.2 of the design proposal).
//         A JSON record with "deleted":true acts as a delete marker.

#pragma once

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Forward declaration -- avoids pulling in kv8 headers in every translation
// unit that only needs the store's display interface.
namespace kv8 { class IKv8Producer; }

// ---------------------------------------------------------------------------
// AnnotationType -- determines the display color on the waveform.
// ---------------------------------------------------------------------------

enum class AnnotationType : int
{
    Info      = 0,  // cyan        -- general informational note
    Warning   = 1,  // amber       -- something worth watching
    Error     = 2,  // orange-red  -- abnormal / fault event
    Milestone = 3,  // green       -- key event / milestone
    Note      = 4,  // lavender    -- free-form personal note
    Count     = 5,  // sentinel -- number of types (not a valid type)
};

// ---------------------------------------------------------------------------
// Annotation -- single annotation record.
// ---------------------------------------------------------------------------

struct Annotation
{
    std::string    sID;           // Unique identifier (simple counter string)
    double         dTimestamp;    // Unix epoch seconds (plot X coordinate)
    std::string    sTitle;        // Short label (displayed on waveform flag)
    std::string    sDescription;  // Optional longer text (shown in tooltip)
    AnnotationType eType;         // Type (determines color)
    std::string    sCreatedAt;    // ISO 8601 wall-clock time of creation
    std::string    sCreatedBy;    // "kv8scope" identifier
    bool           bDeleted = false;  // soft-delete flag; set by delete marker

    Annotation()
        : dTimestamp(0.0)
        , eType(AnnotationType::Info)
        , bDeleted(false)
    {}
};

// ---------------------------------------------------------------------------
// AnnotationStore -- owns the annotation list for one session.
// ---------------------------------------------------------------------------

class AnnotationStore
{
public:
    AnnotationStore() = default;

    static constexpr int kTypeCount = static_cast<int>(AnnotationType::Count);

    // ---- Mutating operations -------------------------------------------

    /// Add a new annotation.  A unique ID is assigned automatically.
    /// The store is kept sorted by timestamp after insertion.
    void Add(double dTimestamp,
             const std::string& sTitle,
             const std::string& sDesc,
             AnnotationType eType);

    /// Edit the title, description, and type of an existing annotation by ID.
    void Edit(const std::string& sID,
              const std::string& sTitle,
              const std::string& sDesc,
              AnnotationType eType);

    /// Delete an annotation by ID.
    void Delete(const std::string& sID);

    // ---- Read access ----------------------------------------------------

    /// Read-only access to all annotations (sorted by timestamp ascending).
    /// Includes soft-deleted annotations (check bDeleted to filter).
    const std::vector<Annotation>& GetAll() const { return m_annotations; }

    /// Toggle visibility of soft-deleted annotations.
    void SetShowDeleted(bool b) { m_bShowDeleted = b; }
    bool GetShowDeleted()  const { return m_bShowDeleted; }

    // ---- Type metadata --------------------------------------------------

    /// Human-readable name for an annotation type.
    static const char* TypeName(AnnotationType t);

    /// Display color for an annotation type.
    static ImVec4 TypeColor(AnnotationType t);

    // ---- Kafka persistence -----------------------------------------------

    /// Attach a Kafka producer and the session annotation topic.
    /// After this call, Add/Edit/Delete automatically publish to Kafka.
    /// Safe to call only from the render/UI thread, before any mutations.
    void SetKafkaSink(kv8::IKv8Producer* pProducer, std::string sTopic);

    /// Called from the consumer thread when an annotation JSON record (or
    /// delete-marker) arrives from Kafka.  Thread-safe (uses m_mtxPending).
    /// pData==nullptr or cbData==0 acts as a delete marker for the key sID.
    void PushFromKafka(std::string sKey,
                       const void* pData,
                       size_t      cbData);

    /// Drain pending Kafka-loaded annotations into the live list.
    /// Must be called from the render/UI thread each frame (or at session open).
    void DrainPending();

private:
    std::vector<Annotation> m_annotations;  // sorted by dTimestamp, render-thread only
    uint64_t                m_nNextID = 1;  // monotonically increasing local ID
    bool                    m_bShowDeleted = false;

    void SortByTimestamp();

    // ---- Kafka write path -----------------------------------------------
    // Owned by ScopeWindow; raw pointer, no lifetime responsibility here.
    kv8::IKv8Producer* m_pProducer   = nullptr;
    std::string        m_sKafkaTopic;

    void ProduceAnnotation(const Annotation& ann);      // publishes JSON record
    void ProduceTombstone(const std::string& sID);      // publishes delete marker

    // ---- Pending queue (consumer thread -> render thread) ---------------
    struct PendingItem
    {
        std::string sKey;
        std::string sJson;   // empty string = delete marker
    };
    std::mutex               m_mtxPending;
    std::vector<PendingItem> m_pendingItems;
};
