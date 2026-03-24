cmake_minimum_required(VERSION 3.20)
project(kv8verify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kv8verify kv8verify.cpp)

target_compile_definitions(kv8verify PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8verify PRIVATE kv8util)

set_target_properties(kv8verify PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8verify RUNTIME DESTINATION bin)
