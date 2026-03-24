cmake_minimum_required(VERSION 3.20)
project(kv8probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kv8probe kv8probe.cpp)

target_compile_definitions(kv8probe PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8probe PRIVATE kv8util)

set_target_properties(kv8probe PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8probe RUNTIME DESTINATION bin)
