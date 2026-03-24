cmake_minimum_required(VERSION 3.20)
project(kv8log LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── kv8log_facade (static; compiled into user application) ──────────────────
# No dependency on librdkafka or libkv8 -- purely C++ stdlib.
add_library(kv8log_facade STATIC
    src/Runtime.cpp
    src/Channel.cpp
    src/Counter.cpp
    src/DefaultChannel.cpp
)

target_include_directories(kv8log_facade
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../libkv8/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(kv8log_facade PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

if(WIN32)
    target_link_libraries(kv8log_facade PUBLIC shell32)   # CommandLineToArgvW
endif()

set_target_properties(kv8log_facade PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

# ── kv8log_runtime (shared; side-drop alongside the user executable) ────────
# Requires libkv8 (which itself requires librdkafka).
add_library(kv8log_runtime SHARED
    lib/kv8log_impl.cpp
)

target_include_directories(kv8log_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(kv8log_runtime PRIVATE
    KV8LOG_EXPORTS
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

# kv8log_runtime links against libkv8 (static) and kv8util (static helpers).
target_link_libraries(kv8log_runtime PRIVATE kv8 kv8util)

set_target_properties(kv8log_runtime PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8log_facade  ARCHIVE DESTINATION lib)
install(TARGETS kv8log_runtime RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

# ── Unit tests ────────────────────────────────────────────────────────────────
enable_testing()

foreach(test_name
        test_compile_out
        test_null_lib
        test_default_channel
        test_monotonic)

    add_executable(${test_name} test/${test_name}.cpp)

    # test_compile_out must NOT define KV8_LOG_ENABLE.
    if(NOT test_name STREQUAL "test_compile_out")
        target_compile_definitions(${test_name} PRIVATE KV8_LOG_ENABLE)
    endif()

    target_compile_definitions(${test_name} PRIVATE
        $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
    )

    target_link_libraries(${test_name} PRIVATE kv8log_facade)

    set_target_properties(${test_name} PROPERTIES
        VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
    )

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
