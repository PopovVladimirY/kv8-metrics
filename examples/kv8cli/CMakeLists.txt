cmake_minimum_required(VERSION 3.20)
project(kv8cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kv8cli kv8cli.cpp)

target_compile_definitions(kv8cli PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8cli PRIVATE kv8util)

set_target_properties(kv8cli PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8cli RUNTIME DESTINATION bin)
