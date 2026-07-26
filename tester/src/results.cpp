#include "results.h"

#include <ranges>
#include <utility>

namespace tester {

std::expected<void, std::string> endpoint_result_collector::submit(endpoint_result result) {
    if (result.run_id.empty()) {
        uncorrelated_results_.push_back(std::move(result));
        return {};
    }

    const auto run_id = result.run_id;

    if (auto [iterator, inserted] = results_.try_emplace(run_id, std::move(result)); not inserted) {
        return std::unexpected{"duplicate endpoint result for run_id: " + iterator->first};
    }

    return {};
}

std::vector<endpoint_result> endpoint_result_collector::take_all() {
    std::vector<endpoint_result> result;
    result.reserve(results_.size() + uncorrelated_results_.size());

    for (auto& session_result : results_ | std::views::values) {
        result.push_back(std::move(session_result));
    }
    results_.clear();

    for (auto& session_result : uncorrelated_results_) {
        result.push_back(std::move(session_result));
    }
    uncorrelated_results_.clear();

    return result;
}

std::string_view to_string(const termination_reason reason) {
    switch (reason) {
        case termination_reason::completed:
            return "completed";
        case termination_reason::tls_shutdown:
            return "tls_shutdown";
        case termination_reason::partial_http_message:
            return "partial_http_message";
        case termination_reason::stream_truncated:
            return "stream_truncated";
        case termination_reason::connection_reset:
            return "connection_reset";
        case termination_reason::timeout:
            return "timeout";
        case termination_reason::resource_exhausted:
            return "resource_exhausted";
        case termination_reason::protocol_error:
            return "protocol_error";
        case termination_reason::io_error:
            return "io_error";
    }
    return "unknown";
}

} // namespace tester
