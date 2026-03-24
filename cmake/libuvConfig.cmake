# Shim: provide uv_a and libuv::uv targets from system pkg-config (Ubuntu).
# Windows builds use vcpkg which ships real cmake config files, so this
# shim is only needed on Linux.
if(NOT TARGET uv_a)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(_LIBUV REQUIRED IMPORTED_TARGET libuv)
    add_library(uv_a ALIAS PkgConfig::_LIBUV)
endif()
if(NOT TARGET libuv::uv)
    add_library(libuv::uv ALIAS PkgConfig::_LIBUV)
endif()
