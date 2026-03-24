cmake_minimum_required(VERSION 3.20)
project(kv8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Find librdkafka (same detection logic used by all tools) ──────────────────
find_package(RdKafka QUIET)
if(RdKafka_FOUND)
    message(STATUS "libkv8: librdkafka found via CMake config")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RDKAFKA IMPORTED_TARGET rdkafka)
        if(RDKAFKA_FOUND)
            message(STATUS "libkv8: librdkafka found via pkg-config")
        endif()
    endif()
    if(NOT RDKAFKA_FOUND)
        message(FATAL_ERROR "libkv8: librdkafka not found. "
                "Install via vcpkg (Windows) or system package manager (Linux).")
    endif()
endif()

# ── Static library ────────────────────────────────────────────────────────────
add_library(kv8 STATIC
    src/Kv8Consumer.cpp
    src/Kv8Producer.cpp
    src/UdtSchemaParser.cpp
)

# PUBLIC include path: tools that link kv8 automatically find <kv8/...> headers.
target_include_directories(kv8
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
)

target_compile_definitions(kv8 PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

# librdkafka is PUBLIC: consumers of libkv8 get the rdkafka include path but
# never have to link it manually -- this target does it for them.
if(RdKafka_FOUND)
    target_link_libraries(kv8 PUBLIC RdKafka::rdkafka)
else()
    target_link_libraries(kv8 PUBLIC PkgConfig::RDKAFKA)
endif()

if(WIN32)
    target_link_libraries(kv8 PUBLIC ws2_32)
endif()

set_target_properties(kv8 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)
