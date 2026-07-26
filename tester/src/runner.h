#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/cobalt/config.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/cobalt/task.hpp>

#include "client.h"
#include "results.h"
#include "scenario.h"

namespace tester {

struct workload {
    std::string scenario_name;
    std::size_t connections{};
    std::size_t concurrency{1};
};

struct workload_state {
    std::size_t next_connection{};
};

struct run_config {
    proxy_address proxy;
    std::vector<boost::asio::ip::address> source_addresses;
    std::vector<workload> workloads;
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::size_t> connection_rate;
    std::chrono::milliseconds warmup{};
    std::chrono::milliseconds report_interval{1'000};
    std::chrono::milliseconds failure_backoff{100};
    std::size_t threads{1};
};

struct latency_summary {
    std::chrono::microseconds p50{};
    std::chrono::microseconds p90{};
    std::chrono::microseconds p95{};
    std::chrono::microseconds p99{};
    std::chrono::microseconds maximum{};
};

struct slo_summary {
    bool evaluated{};
    bool passed{};
    double error_rate_percent{};
    scenario_slo limits;
    std::vector<std::string> violations;
};

struct scenario_summary {
    std::size_t succeeded{};
    std::size_t failed{};
    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::chrono::microseconds successful_latency{};
    latency_summary success_latency;
    latency_summary failure_latency;
    std::map<termination_reason, std::size_t> errors;
    slo_summary slo;
};

struct run_summary {
    bool success{};
    bool infrastructure_valid{true};
    int stop_signal{};
    std::map<std::string, scenario_summary, std::less<>> scenarios;
    std::vector<client_result> client_results;
    std::vector<endpoint_result> endpoint_results;
    std::vector<std::string> infrastructure_errors;
};

struct shard_result {
    std::vector<client_result> client_results;
    std::vector<endpoint_result> endpoint_results;
    std::vector<std::string> infrastructure_errors;
};

struct runner_callbacks {
    std::function<void()> connection_started;
    std::function<void(const client_result&)> connection_finished;
    std::function<bool()> stopping;
    std::function<void()> infrastructure_failed;
};

class runner {
public:
    runner(boost::cobalt::executor executor, const scenario_registry& scenarios, std::size_t shard_index,
        runner_callbacks callbacks);

    boost::cobalt::task<shard_result> run(run_config config, std::chrono::steady_clock::time_point started_at);

private:
    using clock = std::chrono::steady_clock;

    boost::cobalt::promise<void> run_worker(workload workload, proxy_address proxy, endpoint_address endpoint,
        std::size_t worker_index, std::shared_ptr<workload_state> next_connection, clock::time_point measurement_start,
        std::optional<clock::time_point> finish_at);
    boost::cobalt::promise<bool> acquire_start_slot(std::optional<clock::time_point> finish_at);
    bool stopping() const;
    shard_result build_result(std::vector<endpoint_result> endpoint_results);

    boost::cobalt::executor executor_;
    const scenario_registry& scenarios_;
    std::size_t shard_index_;
    runner_callbacks callbacks_;
    boost::asio::ssl::context client_tls_context_;

    std::uint64_t run_sequence_{};
    std::vector<client_result> client_results_;
    clock::time_point next_start_at_{};
    std::optional<clock::duration> rate_interval_;
    std::chrono::milliseconds failure_backoff_{};
    std::optional<std::string> infrastructure_failure_;
    std::vector<boost::asio::ip::address> source_addresses_;
};

run_summary run_tester(const scenario_registry& scenarios, run_config config);

} // namespace tester
