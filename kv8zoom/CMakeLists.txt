cmake_minimum_required(VERSION 3.20)
project(kv8zoom LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
# The kv8scope sub-project declares this first; FetchContent deduplicates.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── uSockets ──────────────────────────────────────────────────────────────────
FetchContent_Declare(
    uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG        v0.8.8
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  TRUE
)
FetchContent_MakeAvailable(uSockets)

# uSockets uses libuv as its cross-platform eventing backend (Windows + Linux).
find_package(libuv CONFIG REQUIRED)

# Compile uSockets as a static C library.
set(USOCKETS_SOURCES
    ${usockets_SOURCE_DIR}/src/bsd.c
    ${usockets_SOURCE_DIR}/src/context.c
    ${usockets_SOURCE_DIR}/src/loop.c
    ${usockets_SOURCE_DIR}/src/socket.c
    ${usockets_SOURCE_DIR}/src/udp.c
    ${usockets_SOURCE_DIR}/src/eventing/libuv.c
)

add_library(uSockets_static STATIC ${USOCKETS_SOURCES})
target_include_directories(uSockets_static PUBLIC
    ${usockets_SOURCE_DIR}/src
)
target_compile_definitions(uSockets_static PUBLIC
    LIBUS_NO_SSL
    LIBUS_USE_LIBUV
)
target_link_libraries(uSockets_static PUBLIC
    $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
)
if(WIN32)
    target_link_libraries(uSockets_static PUBLIC ws2_32 userenv psapi)
endif()

# ── uWebSockets ───────────────────────────────────────────────────────────────
FetchContent_Declare(
    uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG        v20.62.0
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  TRUE
)
FetchContent_MakeAvailable(uWebSockets)

add_library(uWebSockets_iface INTERFACE)
# Mark as SYSTEM so warnings from uWS/uSockets headers are suppressed on
# all compilers (MSVC uses /external:I, GCC/Clang use -isystem).
target_include_directories(uWebSockets_iface SYSTEM INTERFACE
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
)
target_compile_definitions(uWebSockets_iface INTERFACE LIBUS_NO_SSL LIBUS_USE_LIBUV UWS_NO_ZLIB)
target_link_libraries(uWebSockets_iface INTERFACE uSockets_static)

# ── kv8zoom executable ────────────────────────────────────────────────────────
add_executable(kv8zoom
    main.cpp
    AttitudeFilter.cpp
    Config.cpp
    DecimationController.cpp
    FrameAssembler.cpp
    KafkaReader.cpp
    PathSimplifier.cpp
    Serializer.cpp
    WsServer.cpp
    ZoomBridge.cpp
)

target_include_directories(kv8zoom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(kv8zoom PRIVATE
    kv8
    nlohmann_json::nlohmann_json
    uWebSockets_iface
)

if(MSVC)
    # /wd4324 - padded struct (expected from alignas(64) in SpscRingBuffer)
    # /wd4701 - potentially uninitialized (persists from uWS WebSocketProtocol.h
    #           even with SYSTEM includes due to MSVC analysis depth)
    target_compile_options(kv8zoom PRIVATE /W4 /WX- /wd4324 /wd4701)
    target_compile_definitions(kv8zoom PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(kv8zoom PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS kv8zoom DESTINATION bin)

# Install the default config file next to the executable.
install(FILES kv8zoom.json DESTINATION bin)

# ── Tests ─────────────────────────────────────────────────────────────────────
add_subdirectory(tests)
