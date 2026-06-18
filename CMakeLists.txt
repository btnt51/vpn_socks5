cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build native Apple Silicon binaries")
endif()

project(vpn LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

find_package(Threads REQUIRED)

set(BOOST_INCLUDE_LIBRARIES cobalt asio beast program_options CACHE STRING "" FORCE)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
set(BOOST_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

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

FetchContent_MakeAvailable(boost fmt)
FetchContent_MakeAvailable(spdlog)

target_link_libraries(spdlog PUBLIC fmt::fmt)

add_executable(vpn main.cpp
        server/server.cpp
        server/server.h
        server/session.cpp
        server/session.h
        server/logger.cpp
        server/logger.h
        server/socks5.cpp
        server/socks5.h
        server/utils.h)
add_custom_target(cobalt_vpn DEPENDS vpn)
target_link_libraries(
        vpn
        PRIVATE
        Boost::asio
        Boost::beast
        Boost::cobalt::io
        Boost::program_options
        spdlog::spdlog
        Threads::Threads
)
