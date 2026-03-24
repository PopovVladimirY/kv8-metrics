cmake_minimum_required(VERSION 3.20)
project(kv8feeder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -- kv8feeder (telemetry enabled) --------------------------------------------
add_executable(kv8feeder main.cpp UdtFeeds.cpp)

target_compile_definitions(kv8feeder PRIVATE
    KV8_LOG_ENABLE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8feeder PRIVATE kv8log_facade)

set_target_properties(kv8feeder PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8feeder RUNTIME DESTINATION bin)

# -- kv8feeder_silent (compile-out smoke test; no KV8_LOG_ENABLE) -------------
# Verifies that the binary links and runs with telemetry fully silenced.
add_executable(kv8feeder_silent main.cpp UdtFeeds.cpp)

# KV8_LOG_ENABLE intentionally NOT defined.
target_compile_definitions(kv8feeder_silent PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8feeder_silent PRIVATE kv8log_facade)

set_target_properties(kv8feeder_silent PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)
