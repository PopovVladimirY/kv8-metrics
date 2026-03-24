cmake_minimum_required(VERSION 3.20)
project(kv8maint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kv8maint kv8maint.cpp)

target_compile_definitions(kv8maint PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8maint PRIVATE kv8util)

set_target_properties(kv8maint PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8maint RUNTIME DESTINATION bin)
