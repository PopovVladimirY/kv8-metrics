// kv8scope -- Kv8 Software Oscilloscope
// SpscRingBuffer.h -- Lock-free Single-Producer / Single-Consumer ring buffer.
//
// Template class with power-of-two capacity and cache-line-aligned
// head / tail indices to prevent false sharing.  The writer (consumer
// thread) calls Push(); the reader (render thread) calls Pop() or
// PopBatch().  No mutexes -- correctness relies on acquire/release
// atomics.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <malloc.h>   // _aligned_malloc / _aligned_free
#endif

// ---------------------------------------------------------------------------
// TelemetrySample -- one decoded data point for the renderer.
// ---------------------------------------------------------------------------

/// A single decoded telemetry sample ready for the waveform renderer.
/// Kept at 16 bytes (two doubles) so the ring buffer stays compact.
struct TelemetrySample
{
    double dTimestamp;  ///< Wall-clock seconds since Unix epoch
    double dValue;      ///< Sampled counter value
};

// ---------------------------------------------------------------------------
// SpscRingBuffer<T>
// ---------------------------------------------------------------------------

/// Lock-free SPSC ring buffer with power-of-two capacity.
///
/// Memory layout:
///   - m_head (writer index) and m_tail (reader index) are on separate
///     64-byte cache lines to avoid false sharing.
///   - The data array is 64-byte aligned for cache-friendly access.
template<typename T>
class SpscRingBuffer
{
public:
    /// @param requestedCapacity  Minimum number of slots.  Rounded up to
    ///                           the next power of two.
    explicit SpscRingBuffer(uint64_t requestedCapacity)
    {
        m_capacity = NextPow2(requestedCapacity);
        m_mask     = m_capacity - 1;
        size_t cb  = static_cast<size_t>(m_capacity) * sizeof(T);

#ifdef _WIN32
        m_pData = static_cast<T*>(_aligned_malloc(cb, 64));
#else
        void* p = nullptr;
        int rc  = posix_memalign(&p, 64, cb);
        m_pData = (rc == 0) ? static_cast<T*>(p) : nullptr;
#endif
        assert(m_pData && "SpscRingBuffer: aligned allocation failed");
        std::memset(m_pData, 0, cb);
    }

    ~SpscRingBuffer()
    {
        if (m_pData)
        {
#ifdef _WIN32
            _aligned_free(m_pData);
#else
            std::free(m_pData);
#endif
        }
    }

    // Non-copyable, non-movable.
    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&)                 = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&)      = delete;

    // ---- Writer (producer / consumer thread) ----------------------------

    /// Push one item.  Returns false if the ring is full (the caller
    /// should count the drop and continue).
    bool Push(const T& item)
    {
        uint64_t head = m_head.load(std::memory_order_relaxed);
        uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= m_capacity)
            return false;
        m_pData[head & m_mask] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // ---- Reader (renderer / main thread) --------------------------------

    /// Pop one item.  Returns false if the ring is empty.
    bool Pop(T& out)
    {
        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail >= head)
            return false;
        out = m_pData[tail & m_mask];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Pop up to @p maxCount items into @p out.
    /// @return Number of items actually popped (0 if empty).
    size_t PopBatch(T* out, size_t maxCount)
    {
        uint64_t tail  = m_tail.load(std::memory_order_relaxed);
        uint64_t head  = m_head.load(std::memory_order_acquire);
        uint64_t avail = head - tail;
        if (avail == 0)
            return 0;
        size_t n = (avail < maxCount) ? static_cast<size_t>(avail) : maxCount;
        for (size_t i = 0; i < n; ++i)
            out[i] = m_pData[(tail + i) & m_mask];
        m_tail.store(tail + static_cast<uint64_t>(n), std::memory_order_release);
        return n;
    }

    // ---- Queries (safe from the reader thread) --------------------------

    /// Approximate number of items in the ring.
    uint64_t Size() const
    {
        uint64_t head = m_head.load(std::memory_order_acquire);
        uint64_t tail = m_tail.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : 0;
    }

    uint64_t Capacity() const { return m_capacity; }

private:
    static uint64_t NextPow2(uint64_t v)
    {
        if (v == 0) return 1;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    // ---- Cache-line-isolated writer / reader indices --------------------
    alignas(64) std::atomic<uint64_t> m_head{0};
    alignas(64) std::atomic<uint64_t> m_tail{0};

    // ---- Read-only after construction (no false-sharing concern) --------
    T*       m_pData    = nullptr;
    uint64_t m_capacity = 0;
    uint64_t m_mask     = 0;
};
