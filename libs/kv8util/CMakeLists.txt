cmake_minimum_required(VERSION 3.20)
project(kv8util LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -- Static library -----------------------------------------------------------
add_library(kv8util STATIC
    src/Kv8Stats.cpp
    src/Kv8TopicUtils.cpp
)

# PUBLIC include path so that consumers find <kv8util/...> headers automatically.
target_include_directories(kv8util
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
)

target_compile_definitions(kv8util PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

# kv8util builds on top of libkv8, so consumers transitively get kv8 headers
# and link the kv8 static library.
target_link_libraries(kv8util PUBLIC kv8)

if(WIN32)
    target_link_libraries(kv8util PUBLIC ws2_32)
endif()

set_target_properties(kv8util PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

# -- Test executable ----------------------------------------------------------
add_executable(kv8util_test test/kv8lib_test.cpp)

target_link_libraries(kv8util_test PRIVATE kv8util)

target_compile_definitions(kv8util_test PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

set_target_properties(kv8util_test PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8util_test RUNTIME DESTINATION bin)

# Run the test automatically as part of every build via CTest.
# Exit codes: 0 = PASS or SKIP (broker unreachable), 1 = FAIL.
add_test(NAME kv8util_integration_test
    COMMAND $<TARGET_FILE:kv8util_test>
    WORKING_DIRECTORY $<TARGET_FILE_DIR:kv8util_test>
)
set_tests_properties(kv8util_integration_test PROPERTIES TIMEOUT 180)

# Make the test runnable via cmake --build . --target run_kv8lib_test
# (not in ALL -- ctest runs tests separately, running ctest mid-build in
# a parallel rebuild causes "Unable to find executable" for other test targets
# that are not yet compiled when this fires).
add_custom_target(run_kv8lib_test
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure --test-dir "${CMAKE_BINARY_DIR}"
    COMMENT "Running kv8lib integration test..."
    DEPENDS kv8lib_test
)
