#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tester {

enum class termination_reason {
    completed,
    tls_shutdown,
    partial_http_message,
    stream_truncated,
    connection_reset,
    timeout,
    resource_exhausted,
    protocol_error,
    io_error,
};

struct client_result {
    std::string run_id;
    std::string scenario_name;

    bool success{};
    std::size_t failed_step{};
    std::string failed_step_name;

    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::chrono::microseconds latency{};
    bool measured{true};

    termination_reason termination{termination_reason::completed};
    std::string error;
};

struct endpoint_result {
    std::string run_id;
    std::string scenario_name;
    bool measured{true};

    std::uint64_t bytes_sent{};
    termination_reason termination{termination_reason::completed};
    std::string error;
};

class endpoint_result_collector {
public:
    std::expected<void, std::string> submit(endpoint_result result);

    std::vector<endpoint_result> take_all();

private:
    std::unordered_map<std::string, endpoint_result> results_;
    std::vector<endpoint_result> uncorrelated_results_;
};

std::string_view to_string(termination_reason reason);

} // namespace tester
