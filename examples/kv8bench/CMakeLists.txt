cmake_minimum_required(VERSION 3.20)
project(kv8bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kv8bench kv8bench.cpp)

target_compile_definitions(kv8bench PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8bench PRIVATE kv8 kv8util)

target_include_directories(kv8bench PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libkv8/src)

set_target_properties(kv8bench PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8bench RUNTIME DESTINATION bin)
