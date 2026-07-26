add_executable(tester
        main.cpp
        src/client.cpp
        src/http_io.cpp
        src/results.cpp
        src/runner.cpp
        src/scenario.cpp
        src/target_endpoint.cpp
        src/program_options.cpp
        src/program_options.h
)

target_include_directories(tester PRIVATE src)

target_link_libraries(
        tester
        PRIVATE
        vpn_core
        Boost::beast
        Boost::program_options
        fmt::fmt-header-only
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)
