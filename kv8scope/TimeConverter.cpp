// kv8scope -- Kv8 Software Oscilloscope
// TimeConverter.cpp

#include "TimeConverter.h"

// FILETIME epoch (1601-01-01) to Unix epoch (1970-01-01) in seconds.
static constexpr double kFileTimeToUnixSec = 11644473600.0;

// FILETIME units: 100-nanosecond intervals per second.
static constexpr double kFileTimeUnitsPerSec = 10000000.0;

// ---------------------------------------------------------------------------
void TimeConverter::Init(uint64_t qwFrequency,
                         uint64_t qwTimerAnchor,
                         uint32_t dwTimeHi,
                         uint32_t dwTimeLo)
{
    if (qwFrequency == 0)
        return;

    m_dFrequencyInv = 1.0 / static_cast<double>(qwFrequency);
    m_qwTimerAnchor = qwTimerAnchor;

    // FILETIME -> Unix epoch seconds.
    uint64_t ft = (static_cast<uint64_t>(dwTimeHi) << 32) | dwTimeLo;
    m_dEpochBase = static_cast<double>(ft) / kFileTimeUnitsPerSec
                 - kFileTimeToUnixSec;

    m_bValid = true;
}

// ---------------------------------------------------------------------------
double TimeConverter::Convert(uint64_t qwTimer) const
{
    // qwTimer is already a session-relative offset: (MonoNs - mono_anchor_ns).
    // m_dEpochBase is the wall-clock Unix epoch second at session open.
    // No anchor subtraction needed -- adding the offset directly gives the
    // correct absolute timestamp.  Precision of m_dEpochBase is one second,
    // which is sufficient for display purposes.
    return m_dEpochBase + static_cast<double>(qwTimer) * m_dFrequencyInv;
}
