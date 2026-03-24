?# GitHub Copilot Instructions -- SolarFlair ONLOAD Benchmark

## Foundation
- **The ground truth**: always respect the existing codebase. Consistency is more important than cleverness.
- **Read before editing**: before editing any file, read the actual current content -- never act on a summary or cached view. Summaries can be stale; the file is always authoritative.
- **Self-correction loop**: after any correction from the user, update `tasks/lessons.md` with the pattern that caused the mistake and a rule that prevents it in future. Review lessons at the start of each session.
- **Autonomous bug fixing**: when given a bug report, fix it -- do not ask for hand-holding. Follow logs, errors, and failing tests to the root cause and resolve it directly.
- **No laziness**: find root causes. No temporary fixes. Senior developer standard.
- **Minimal impact**: changes must only touch what is necessary. Avoid introducing unrelated side-effects.
- **No new warnings**: code must compile cleanly without warnings. If a change introduces a warning, fix the warning before proceeding.
- **Test-driven**: if a bug is reported, add a test that reproduces the bug before fixing it. This ensures the bug is fully understood and prevents regressions.
- **Documentation**: if a change affects public APIs or usage, update documentation accordingly. Documentation must be clear, accurate, and reflect the current state of the codebase.
- **Performance-conscious**: for performance-critical code, ensure that changes do not degrade performance. Use profiling tools to verify performance characteristics when necessary. 
- **GIT**: never commit to git. Let the user review and commit changes. Always provide a clear commit message describing the change and its purpose.
- **TODO.md**: never touch the TODO.md file. This file is for the user to track tasks and should not be modified by the assistant.

## Project Purpose
This project is a high-performance Kafka telemetry library and toolchain (Kv8Logger). The goal is to achieve high-throughput, low-latency telemetry delivery from C++ applications to Kafka, suitable for real-time monitoring and analytics in production environments.

## Language & Build
- **C++** throughout. Build system: **CMake >= 3.20**.
- Compiler flags: `-O3 -march=native -mtune=native -fno-omit-frame-pointer`.
- Standard build:
  ```sh
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
  ```

## Text characters

- do not use any non-ASCII characters in the code or documentation. Stick to plain English and standard programming symbols. Avoid emojis, special punctuation, or any characters that might not render correctly in all environments.

## Repository Structure
```
 
```

## Critical Architecture Rules

### Memory Alignment -- never compromise
- All shared data structures (e.g. `BenchmarkHeader`, `ProbeHeader`, `SpscQueue`) must be aligned to **cache line boundaries** (typically 64 bytes) to prevent false sharing and ensure optimal performance.
- Use `alignas(64)` on struct definitions and ensure that any dynamically allocated buffers are also aligned (e.g. via `posix_memalign` or C++17's `std::aligned_alloc`).

### Zero-allocation Hot Path
  Buffer are pre-allocated at startup and recycled via a free list.

