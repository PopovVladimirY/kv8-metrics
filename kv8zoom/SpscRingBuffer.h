////////////////////////////////////////////////////////////////////////////////
// kv8zoom/SpscRingBuffer.h -- lock-free single-producer / single-consumer ring.
//
// Constraints:
//   - N must be a power of two.
//   - push() called only from the producer thread.
//   - pop()  called only from the consumer thread.
//   - No mutexes.  Ordering is enforced with release/acquire atomics.
//   - Cache-line aligned head and tail to prevent false sharing.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <cstddef>

namespace kv8zoom {

template <typename T, size_t N>
class SpscRingBuffer
{
    static_assert((N & (N - 1)) == 0, "SpscRingBuffer: N must be a power of two");
    static_assert(N >= 2,             "SpscRingBuffer: N must be at least 2");

public:
    SpscRingBuffer() = default;

    // Not copyable or movable -- the buffer owns live atomic state.
    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Push one item.  Returns false when the buffer is full (item dropped).
    // Called from the producer thread only.
    bool push(const T& item) noexcept
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & kMask;
        // Acquire: pairs with release in pop() -- ensures we see the latest head
        // before deciding the buffer is full.
        if (next == m_head.load(std::memory_order_acquire))
            return false;  // full
        m_buf[tail] = item;
        // Release: makes the new item visible to the consumer.
        m_tail.store(next, std::memory_order_release);
        return true;
    }

    // Pop one item into 'out'.  Returns false when the buffer is empty.
    // Called from the consumer thread only.
    bool pop(T& out) noexcept
    {
        const size_t head = m_head.load(std::memory_order_relaxed);
        // Acquire: pairs with release in push() -- ensures we see the stored item.
        if (head == m_tail.load(std::memory_order_acquire))
            return false;  // empty
        out = m_buf[head];
        // Release: makes the slot available to the producer.
        m_head.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Approximate occupancy (relaxed, for monitoring).
    size_t size_approx() const noexcept
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_relaxed);
        return (tail - head) & kMask;
    }

    bool empty() const noexcept
    {
        return m_head.load(std::memory_order_relaxed) ==
               m_tail.load(std::memory_order_relaxed);
    }

    /// Reset head and tail to zero. ONLY safe when both producer and consumer
    /// threads are known to be stopped (e.g. during a session switch).
    void clear() noexcept
    {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    static constexpr size_t capacity() noexcept { return N - 1; }

private:
    static constexpr size_t kMask = N - 1;

    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};

    T m_buf[N] = {};
};

} // namespace kv8zoom
