cmake_minimum_required(VERSION 3.20)
project(kv8bench_log LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kv8bench_log kv8bench_log.cpp)

target_compile_definitions(kv8bench_log PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
    KV8_LOG_ENABLE=1
)

target_link_libraries(kv8bench_log PRIVATE kv8log_facade kv8 kv8util)

set_target_properties(kv8bench_log PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8bench_log RUNTIME DESTINATION bin)
