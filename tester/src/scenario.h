#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace tester {

namespace literals {

consteval std::uint64_t operator""_KiB(unsigned long long value) {
    return value * 1024ULL;
} // namespace literals

consteval std::uint64_t operator""_MiB(unsigned long long value) {
    return value * 1024ULL * 1024ULL;
}

consteval std::uint64_t operator""_GiB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

}

struct socks5_negotiate {
    std::vector<std::uint8_t> methods;
    std::uint8_t expected_method;
};

struct socks5_connect {
    std::uint8_t expected_reply;
};

struct tls_handshake {};

struct send_http_request {
    std::string method;
    std::string target;
    std::string body;
};

struct expect_http_response {
    unsigned status;
    std::string body;
};

struct expect_incomplete_response {
    std::uint64_t declared_size;
    std::uint64_t minimum_received;
    std::uint64_t maximum_received;
};

struct expect_complete_download {
    unsigned status;
    std::uint64_t declared_size;
};

using client_step = std::variant<
    socks5_negotiate,
    socks5_connect,
    tls_handshake,
    send_http_request,
    expect_http_response,
    expect_incomplete_response,
    expect_complete_download
>;

struct pong_behavior {
    unsigned status{200};
    std::string body{"pong"};
};

enum class connection_close_mode {
    tls_shutdown,
    tcp_close,
    tcp_reset
};

struct interrupted_download_behavior {
    std::uint64_t declared_size;
    std::uint64_t close_after_bytes;
    std::size_t chunk_size;
    std::chrono::milliseconds chunk_delay;
    connection_close_mode close_mode;
};

struct complete_download_behavior {
    std::uint64_t size;
    std::size_t chunk_size;
    std::chrono::milliseconds chunk_delay;
};

using endpoint_behavior = std::variant<
    pong_behavior,
    interrupted_download_behavior,
    complete_download_behavior
>;

struct scenario_slo {
    double maximum_error_rate_percent{};
    std::chrono::milliseconds maximum_p95{};
    std::chrono::milliseconds maximum_p99{};
};

struct scenario {
    std::string name;

    std::vector<client_step> client_steps;
    endpoint_behavior endpoint;
    scenario_slo slo;

    std::chrono::milliseconds step_timeout{5'000};
    std::chrono::milliseconds session_timeout{30'000};
};

using scenario_registry = std::map<
    std::string,
    scenario,
    std::less<>
>;

const scenario_registry& scenarios();

} // namespace tester
