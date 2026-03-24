////////////////////////////////////////////////////////////////////////////////
// kv8zoom/tests/TestSpscRingBuffer.cpp
////////////////////////////////////////////////////////////////////////////////

#include "SpscRingBuffer.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace kv8zoom;

static void TestEmptyAndCapacity()
{
    SpscRingBuffer<int, 8> ring;
    assert(ring.empty());
    assert(ring.size_approx() == 0);
    assert(ring.capacity() == 7); // capacity = N-1 (one slot reserved for full detection)
}

static void TestPushPop()
{
    SpscRingBuffer<int, 8> ring;
    for (int i = 0; i < 4; ++i) {
        bool ok = ring.push(i);
        assert(ok);
    }
    assert(!ring.empty());
    for (int i = 0; i < 4; ++i) {
        int v = -1;
        bool ok = ring.pop(v);
        assert(ok);
        assert(v == i);
    }
    assert(ring.empty());
}

static void TestFull()
{
    SpscRingBuffer<int, 4> ring; // capacity = 3
    assert(ring.push(10));
    assert(ring.push(20));
    assert(ring.push(30));
    // Ring is full; the next push should fail.
    assert(!ring.push(99));

    int v;
    assert(ring.pop(v)); assert(v == 10);
    assert(ring.pop(v)); assert(v == 20);
    assert(ring.pop(v)); assert(v == 30);
    assert(!ring.pop(v));
}

static void TestSizeApprox()
{
    SpscRingBuffer<int, 16> ring;
    for (int i = 0; i < 5; ++i) ring.push(i);
    assert(ring.size_approx() == 5);
    int v;
    ring.pop(v);
    assert(ring.size_approx() == 4);
}

static void TestWraparound()
{
    SpscRingBuffer<int, 8> ring; // capacity = 7
    for (int i = 0; i < 7; ++i) ring.push(i);
    int v;
    for (int i = 0; i < 7; ++i) ring.pop(v);
    // After drain, push past the physical end of the backing array.
    for (int i = 100; i < 107; ++i) { bool ok = ring.push(i); assert(ok); }
    for (int i = 100; i < 107; ++i) { bool ok = ring.pop(v); assert(ok); assert(v == i); }
    assert(ring.empty());
}

static void TestConcurrentProducerConsumer()
{
    SpscRingBuffer<int, 1024> ring;
    const int N = 10000;

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            while (!ring.push(i)) {} // spin until space
        }
    });

    std::vector<int> received;
    received.reserve(N);
    while ((int)received.size() < N) {
        int v;
        if (ring.pop(v)) received.push_back(v);
    }
    producer.join();

    assert((int)received.size() == N);
    for (int i = 0; i < N; ++i)
        assert(received[i] == i);
}

int main()
{
    TestEmptyAndCapacity();
    TestPushPop();
    TestFull();
    TestSizeApprox();
    TestWraparound();
    TestConcurrentProducerConsumer();
    fprintf(stderr, "TestSpscRingBuffer: all pass\n");
    return 0;
}
