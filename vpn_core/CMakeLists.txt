add_library(vpn_core
        src/server.cpp
        include/vpn/server.h
        src/session.cpp
        src/session.h
        src/logger.cpp
        include/vpn/logger.h
        src/socks5.cpp
        include/vpn/socks5.h
        src/utils.h
        src/config.cpp
        include/vpn/config.h
)

target_include_directories(
        vpn_core
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
        vpn_core
        PUBLIC
        Boost::asio
        Boost::cobalt::io
        Boost::json
        spdlog::spdlog
        Threads::Threads
)

if(VPN_BUILD_TESTS)
    add_executable(vpn_tests
            tests/socks5_tests.cpp
            tests/config_tests.cpp
    )

    target_link_libraries(
            vpn_tests
            PRIVATE
            vpn_core
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(vpn_tests)
endif()
