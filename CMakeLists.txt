cmake_minimum_required(VERSION 3.20)

# ── Auto-detect vcpkg via VCPKG_ROOT env variable (Windows only) ──────────────
# Must be set before project() so the toolchain is active for find_package().
# On Linux, system-installed librdkafka is found via find_package / pkg-config.
# Note: Visual Studio 2022 ships its own vcpkg and pre-sets CMAKE_TOOLCHAIN_FILE
# pointing there.  When the user explicitly sets VCPKG_ROOT we override it so
# that the user's vcpkg (which has librdkafka installed) is used instead.
if(CMAKE_HOST_WIN32 AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file" FORCE)
    message(STATUS "Kv8: Using vcpkg toolchain from VCPKG_ROOT=$ENV{VCPKG_ROOT}")
endif()

project(Kv8)

# -- Architecture and tuning -------------------------------------------------
# KV8_ARCH  controls the target architecture:
#   GCC/Clang : -march=<value>   (default: native)
#   MSVC      : /arch:<value>    (default: native is ignored -- MSVC has no
#                                 -march=native equivalent; supply a concrete
#                                 value such as AVX2 to override)
# KV8_TUNE  controls micro-arch tuning:
#   GCC/Clang : -mtune=<value>   (default: native)
#   MSVC      : ignored          (MSVC has no -mtune equivalent)
#
# Override from the command line:
#   cmake -B build -DKV8_ARCH=x86-64-v3 -DKV8_TUNE=generic
#   cmake -B build -DKV8_ARCH=AVX2          (MSVC)
#   cmake -B build -DKV8_ARCH= -DKV8_TUNE=  (disable both)
set(KV8_ARCH "native" CACHE STRING
    "Architecture flag: -march=<KV8_ARCH> (GCC/Clang) or /arch:<KV8_ARCH> (MSVC). Empty = omit flag.")
set(KV8_TUNE "native" CACHE STRING
    "Tuning flag: -mtune=<KV8_TUNE> (GCC/Clang only). Empty = omit flag.")

if(MSVC)
    # Keep frame pointers so profilers (VTune, perf, WPA) can walk the stack.
    add_compile_options(/Oy-)
    if(NOT KV8_ARCH STREQUAL "" AND NOT KV8_ARCH STREQUAL "native")
        add_compile_options(/arch:${KV8_ARCH})
        message(STATUS "Kv8: MSVC /arch:${KV8_ARCH}")
    else()
        message(STATUS "Kv8: MSVC arch flag omitted (no -march=native on MSVC; set KV8_ARCH=AVX2 etc. to override)")
    endif()
    # KV8_TUNE is not applicable to MSVC.
else()
    # GCC / Clang
    add_compile_options(-fno-omit-frame-pointer)
    if(NOT KV8_ARCH STREQUAL "")
        add_compile_options(-march=${KV8_ARCH})
        message(STATUS "Kv8: -march=${KV8_ARCH}")
    endif()
    if(NOT KV8_TUNE STREQUAL "")
        add_compile_options(-mtune=${KV8_TUNE})
        message(STATUS "Kv8: -mtune=${KV8_TUNE}")
    endif()
endif()

# Install all artifacts into build/_output_/bin
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/_output_" CACHE PATH "Install path" FORCE)
endif()

enable_testing()
# -- Libraries ----------------------------------------------------------------
add_subdirectory(libs/libkv8)
add_subdirectory(libs/kv8util)
add_subdirectory(libs/kv8log)
# -- Tools --------------------------------------------------------------------
add_subdirectory(tools/kv8maint)
add_subdirectory(tools/kv8probe)
add_subdirectory(tools/kv8verify)
# -- Examples -----------------------------------------------------------------
add_subdirectory(examples/kv8bench)
add_subdirectory(examples/kv8bench_log)
add_subdirectory(examples/kv8cli)
add_subdirectory(examples/kv8feeder)
# -- UI application -----------------------------------------------------------
add_subdirectory(kv8scope)
# -- kv8zoom WebSocket bridge -------------------------------------------------
add_subdirectory(kv8zoom)

# ── Deploy vcpkg runtime DLLs alongside the installed executables ─────────────
# applocal.ps1 (the vcpkg DLL copy hook) requires dumpbin in PATH, which is
# only available inside a VS Developer Prompt.  In plain PowerShell/CI the hook
# silently skips DLL copying.  We therefore install them explicitly here.
if(WIN32 AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    file(GLOB VCPKG_RUNTIME_DLLS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll"
    )
    if(VCPKG_RUNTIME_DLLS)
        install(FILES ${VCPKG_RUNTIME_DLLS} DESTINATION bin)
        message(STATUS "Kv8: will install ${CMAKE_INSTALL_PREFIX}/bin "
                       "with vcpkg DLLs from "
                       "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/")
    else()
        message(WARNING "Kv8: VCPKG_INSTALLED_DIR set but no DLLs found at "
                        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/")
    endif()
endif()

