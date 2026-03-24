cmake_minimum_required(VERSION 3.20)
project(kv8zoom_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Helper macro: build a test executable and register it with CTest.
macro(kv8zoom_test NAME SRC)
    add_executable(${NAME} ${SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/../AttitudeFilter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../Config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../DecimationController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../FrameAssembler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../PathSimplifier.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../Serializer.cpp
    )
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    target_link_libraries(${NAME} PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME ${NAME} COMMAND ${NAME})
endmacro()

kv8zoom_test(TestSpscRingBuffer    TestSpscRingBuffer.cpp)
kv8zoom_test(TestDecimation        TestDecimationController.cpp)
kv8zoom_test(TestPathSimplifier    TestPathSimplifier.cpp)
kv8zoom_test(TestAttitudeFilter    TestAttitudeFilter.cpp)
kv8zoom_test(TestSerializer        TestSerializer.cpp)
