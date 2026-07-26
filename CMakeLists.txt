cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build native Apple Silicon binaries")
endif()

project(vpn LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VPN_BUILD_TESTS "Build vpn tests" ON)

include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(BOOST_INCLUDE_LIBRARIES cobalt asio beast program_options json CACHE STRING "" FORCE)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
set(BOOST_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

if(UNIX AND NOT APPLE)
    add_compile_definitions(BOOST_ASIO_DISABLE_IO_URING)
endif()

FetchContent_Declare(
        boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
)

FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.2.0
        GIT_SHALLOW TRUE
)

FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.3
        GIT_SHALLOW TRUE
)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
)

FetchContent_Declare(
        sqlite3
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip
        URL_HASH SHA3_256=d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9
)

FetchContent_MakeAvailable(boost fmt)
FetchContent_MakeAvailable(spdlog)
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
add_library(SQLite::SQLite3 ALIAS sqlite3)

target_include_directories(sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

target_link_libraries(spdlog PUBLIC fmt::fmt)

if(VPN_BUILD_TESTS)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

add_subdirectory(vpn_core)
add_subdirectory(tester)

add_executable(vpn main.cpp)
target_link_libraries(vpn PRIVATE vpn_core)
