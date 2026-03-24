// kv8scope -- Kv8 Software Oscilloscope
// TimeConverter.h -- Converts Kv8 QPC timer ticks to wall-clock seconds.
//
// Each kv8 telemetry group carries its own timer anchor: the QPC frequency,
// a QPC tick captured at session start, and the corresponding wall-clock
// FILETIME.  TimeConverter uses these three values to map any QPC tick to
// a double-precision Unix epoch timestamp.

#pragma once

#include <cstdint>

class TimeConverter
{
public:
    TimeConverter() = default;

    /// Initialise from the group-record fields stored in SessionMeta.
    ///
    /// @param qwFrequency    QPC frequency in Hz (ticks per second).
    /// @param qwTimerAnchor  QPC tick at the anchor wall-clock time.
    /// @param dwTimeHi       High 32 bits of the anchor FILETIME.
    /// @param dwTimeLo       Low  32 bits of the anchor FILETIME.
    void Init(uint64_t qwFrequency,
              uint64_t qwTimerAnchor,
              uint32_t dwTimeHi,
              uint32_t dwTimeLo);

    /// Convert a QPC tick to Unix epoch seconds (double precision).
    double Convert(uint64_t qwTimer) const;

    /// True after a successful Init() call.
    bool IsValid() const { return m_bValid; }

private:
    double   m_dFrequencyInv  = 0.0;   ///< 1.0 / qwFrequency
    uint64_t m_qwTimerAnchor  = 0;     ///< QPC anchor tick
    double   m_dEpochBase     = 0.0;   ///< Unix epoch seconds for the anchor
    bool     m_bValid         = false;
};
