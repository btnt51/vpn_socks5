#include "runner.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/cobalt/io/sleep.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/spawn.hpp>
#include <boost/cobalt/this_thread.hpp>
#include <boost/cobalt/wait_group.hpp>
#include <fmt/base.h>
#include <fmt/format.h>

#include "target_endpoint.h"

namespace tester {

namespace {

using clock = std::chrono::steady_clock;

std::size_t request_step_index(const scenario& scenario) {
    for (std::size_t index = 0; index < scenario.client_steps.size();++index) {
        if (std::holds_alternative<send_http_request>(scenario.client_steps[index])) {
            return index;
        }
    }
    return scenario.client_steps.size();
}

std::chrono::microseconds percentile(const std::vector<std::chrono::microseconds>& sorted, const std::size_t percentage) {
    if (sorted.empty()) {
        return {};
    }
    const auto index = ((sorted.size() * percentage) + 99) / 100 - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

latency_summary summarize_latency(std::vector<std::chrono::microseconds> values) {
    std::ranges::sort(values);
    return latency_summary{
        .p50 = percentile(values, 50),
        .p90 = percentile(values, 90),
        .p95 = percentile(values, 95),
        .p99 = percentile(values, 99),
        .maximum = values.empty() ? std::chrono::microseconds{} : values.back(),
    };
}

std::string format_bytes_per_second(const double bytes) {
    constexpr double kibibyte = 1024.0;
    constexpr double mebibyte = kibibyte * 1024.0;
    constexpr double gibibyte = mebibyte * 1024.0;
    if (bytes >= gibibyte) {
        return fmt::format("{:.1f}GiB/s", bytes / gibibyte);
    }
    if (bytes >= mebibyte) {
        return fmt::format("{:.1f}MiB/s", bytes / mebibyte);
    }
    if (bytes >= kibibyte) {
        return fmt::format("{:.1f}KiB/s", bytes / kibibyte);
    }
    return fmt::format("{:.0f}B/s", bytes);
}

std::string format_errors(const std::map<termination_reason, std::size_t>& errors) {
    std::string result;
    for (const auto& [reason, count] : errors) {
        if (not result.empty()) {
            result += ',';
        }
        result += fmt::format("{}={}", to_string(reason), count);
    }
    return result.empty() ? "none" : result;
}

class runtime_statistics {
public:
    runtime_statistics(const clock::time_point started_at, const clock::time_point measurement_start)
        : started_at_{started_at}, measurement_start_{measurement_start}, previous_report_at_{measurement_start} { }

    void connection_started() {
        std::lock_guard lock{mutex_};
        ++active_connections_;
    }

    void connection_finished(const client_result& result) {
        std::lock_guard lock{mutex_};
        --active_connections_;
        if (not result.measured) {
            return;
        }

        ++completed_connections_;
        ++interval_completed_;
        interval_bytes_sent_ += result.bytes_sent;
        interval_bytes_received_ += result.bytes_received;
        if (result.success) {
            ++succeeded_connections_;
            interval_success_latencies_.push_back(result.latency);
        } else {
            ++failed_connections_;
            interval_failure_latencies_.push_back(result.latency);
            ++interval_errors_[result.termination];
        }
    }

    void finish() {
        {
            std::lock_guard lock{mutex_};
            finished_ = true;
        }
        wakeup_.notify_one();
    }

    void report(const std::chrono::milliseconds interval) {
        std::unique_lock lock{mutex_};
        auto next_report_at = started_at_ + interval;
        while (not finished_) {
            if (wakeup_.wait_until(lock, next_report_at, [this] { return finished_; })) {
                break;
            }
            print(clock::now());
            next_report_at = clock::now() + interval;
        }
        if (interval_completed_ != 0 or completed_connections_ == 0) {
            print(clock::now());
        }
    }

private:
    void print(const clock::time_point now) {
        if (now < measurement_start_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started_at_).count();
            fmt::println("[warmup {:02}:{:02}] active={}", elapsed / 60, elapsed % 60, active_connections_);
            return;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - measurement_start_).count();
        const auto interval_seconds = std::max(std::chrono::duration<double>(now - previous_report_at_).count(), 0.001);
        const auto rate = static_cast<double>(interval_completed_) / interval_seconds;
        const auto error_rate = completed_connections_ == 0 ? 0.0 :
            static_cast<double>(failed_connections_) * 100.0 / static_cast<double>(completed_connections_);
        const auto success_latency = summarize_latency(interval_success_latencies_);
        const auto failure_latency = summarize_latency(interval_failure_latencies_);
        fmt::println("[{:02}:{:02}] active={} completed={} succeeded={} failed={} error-rate={:.1f}% rate={:.0f}/s "
            "success-p95={:.1f}ms failure-p95={:.1f}ms tx={} rx={} errors=[{}]",
            elapsed / 60, elapsed % 60, active_connections_, completed_connections_, succeeded_connections_,
            failed_connections_, error_rate, rate, static_cast<double>(success_latency.p95.count()) / 1'000.0,
            static_cast<double>(failure_latency.p95.count()) / 1'000.0,
            format_bytes_per_second(static_cast<double>(interval_bytes_sent_) / interval_seconds),
            format_bytes_per_second(static_cast<double>(interval_bytes_received_) / interval_seconds),
            format_errors(interval_errors_));

        interval_completed_ = 0;
        interval_bytes_sent_ = 0;
        interval_bytes_received_ = 0;
        interval_success_latencies_.clear();
        interval_failure_latencies_.clear();
        interval_errors_.clear();
        previous_report_at_ = now;
    }

    std::mutex mutex_;
    std::condition_variable wakeup_;
    clock::time_point started_at_;
    clock::time_point measurement_start_;
    clock::time_point previous_report_at_;
    std::size_t active_connections_{};
    std::size_t completed_connections_{};
    std::size_t succeeded_connections_{};
    std::size_t failed_connections_{};
    std::size_t interval_completed_{};
    std::uint64_t interval_bytes_sent_{};
    std::uint64_t interval_bytes_received_{};
    std::vector<std::chrono::microseconds> interval_success_latencies_;
    std::vector<std::chrono::microseconds> interval_failure_latencies_;
    std::map<termination_reason, std::size_t> interval_errors_;
    bool finished_{};
};

struct shared_control {
    std::atomic_bool stopping{};
    std::atomic_int stop_signal{};
};

std::vector<run_config> split_config(const run_config& config) {
    auto maximum_concurrency = std::size_t{};
    for (const auto& selected_workload : config.workloads) {
        const auto concurrency = selected_workload.connections == 0 ? selected_workload.concurrency :
            std::min(selected_workload.connections, selected_workload.concurrency);
        maximum_concurrency = std::max(maximum_concurrency, concurrency);
    }

    auto shard_count = std::min(config.threads, maximum_concurrency);
    if (config.connection_rate) {
        shard_count = std::min(shard_count, *config.connection_rate);
    }
    shard_count = std::max<std::size_t>(shard_count, 1);

    std::vector<run_config> shards(shard_count, config);
    for (auto& shard : shards) {
        shard.workloads.clear();
        shard.connection_rate.reset();
        shard.threads = 1;
    }

    if (config.connection_rate) {
        const auto rate_per_shard = *config.connection_rate / shard_count;
        const auto rate_remainder = *config.connection_rate % shard_count;
        for (std::size_t index = 0; index < shard_count;++index) {
            shards[index].connection_rate = rate_per_shard + (index < rate_remainder ? 1 : 0);
        }
    }

    for (const auto& selected_workload : config.workloads) {
        const auto effective_concurrency = selected_workload.connections == 0 ? selected_workload.concurrency :
            std::min(selected_workload.connections, selected_workload.concurrency);
        const auto workload_shards = std::min(shard_count, effective_concurrency);
        const auto concurrency_per_shard = effective_concurrency / workload_shards;
        const auto concurrency_remainder = effective_concurrency % workload_shards;
        const auto remaining_connections = selected_workload.connections == 0 ? 0 :
            selected_workload.connections - effective_concurrency;
        const auto connections_per_shard = selected_workload.connections == 0 ? 0 :
            remaining_connections / workload_shards;
        const auto connections_remainder = selected_workload.connections == 0 ? 0 :
            remaining_connections % workload_shards;

        for (std::size_t index = 0; index < workload_shards;++index) {
            const auto concurrency = concurrency_per_shard + (index < concurrency_remainder ? 1 : 0);
            const auto connections = selected_workload.connections == 0 ? 0 :
                concurrency + connections_per_shard + (index < connections_remainder ? 1 : 0);
            shards[index].workloads.push_back(workload{
                .scenario_name = selected_workload.scenario_name,
                .connections = connections,
                .concurrency = concurrency,
            });
        }
    }

    return shards;
}

run_summary build_summary(const scenario_registry& scenarios, const std::vector<std::string>& selected_scenarios,
    const bool evaluate_slo, std::vector<client_result> client_results, std::vector<endpoint_result> endpoint_results,
    std::vector<std::string> infrastructure_errors, const int stop_signal) {

    run_summary summary{
        .stop_signal = stop_signal,
        .client_results = std::move(client_results),
        .endpoint_results = std::move(endpoint_results),
        .infrastructure_errors = std::move(infrastructure_errors),
    };

    std::unordered_map<std::string, const endpoint_result*> endpoint_by_run_id;
    endpoint_by_run_id.reserve(summary.endpoint_results.size());
    for (const auto& endpoint_result : summary.endpoint_results) {
        if (endpoint_result.run_id.empty()) {
            summary.infrastructure_errors.push_back(endpoint_result.error.empty() ? "uncorrelated endpoint result" : endpoint_result.error);
            continue;
        }
        endpoint_by_run_id.emplace(endpoint_result.run_id, &endpoint_result);
    }

    summary.success = true;
    std::map<std::string, std::vector<std::chrono::microseconds>, std::less<>> success_latencies;
    std::map<std::string, std::vector<std::chrono::microseconds>, std::less<>> failure_latencies;
    for (const auto& client_result : summary.client_results) {
        if (client_result.termination == termination_reason::resource_exhausted) {
            continue;
        }
        auto& scenario_summary = summary.scenarios[client_result.scenario_name];
        scenario_summary.bytes_sent += client_result.bytes_sent;
        scenario_summary.bytes_received += client_result.bytes_received;

        const auto endpoint_iterator = endpoint_by_run_id.find(client_result.run_id);
        const auto has_endpoint_result = endpoint_iterator != endpoint_by_run_id.end();
        const auto endpoint_is_valid = has_endpoint_result and endpoint_iterator->second->error.empty();
        const auto scenario_definition = scenarios.find(client_result.scenario_name);
        const auto endpoint_result_expected = client_result.success or (scenario_definition != scenarios.end() and
            client_result.failed_step > request_step_index(scenario_definition->second));

        if (client_result.success and endpoint_is_valid) {
            ++scenario_summary.succeeded;
            scenario_summary.successful_latency += client_result.latency;
            success_latencies[client_result.scenario_name].push_back(client_result.latency);
        } else {
            ++scenario_summary.failed;
            if (not evaluate_slo) {
                summary.success = false;
            }
            failure_latencies[client_result.scenario_name].push_back(client_result.latency);
            const auto reason = client_result.success and has_endpoint_result ?
                endpoint_iterator->second->termination : client_result.termination;
            ++scenario_summary.errors[reason];

            if (endpoint_result_expected and not has_endpoint_result and not client_result.run_id.empty()) {
                summary.infrastructure_errors.push_back("missing endpoint result for " + client_result.run_id);
                summary.success = false;
            }
        }
    }

    for (auto& [name, scenario_summary] : summary.scenarios) {
        scenario_summary.success_latency = summarize_latency(std::move(success_latencies[name]));
        scenario_summary.failure_latency = summarize_latency(std::move(failure_latencies[name]));
        if (evaluate_slo and scenarios.contains(name)) {
            const auto& limits = scenarios.at(name).slo;
            const auto completed = scenario_summary.succeeded + scenario_summary.failed;
            scenario_summary.slo.evaluated = true;
            scenario_summary.slo.error_rate_percent = completed == 0 ? 100.0 :
                static_cast<double>(scenario_summary.failed) * 100.0 / static_cast<double>(completed);
            scenario_summary.slo.limits = limits;

            if (scenario_summary.slo.error_rate_percent > limits.maximum_error_rate_percent) {
                scenario_summary.slo.violations.push_back(fmt::format("error-rate {:.2f}% exceeds {:.2f}%",
                    scenario_summary.slo.error_rate_percent, limits.maximum_error_rate_percent));
            }
            if (scenario_summary.success_latency.p95 > limits.maximum_p95) {
                scenario_summary.slo.violations.push_back(fmt::format("success-p95 {:.1f}ms exceeds {}ms",
                    static_cast<double>(scenario_summary.success_latency.p95.count()) / 1'000.0, limits.maximum_p95.count()));
            }
            if (scenario_summary.success_latency.p99 > limits.maximum_p99) {
                scenario_summary.slo.violations.push_back(fmt::format("success-p99 {:.1f}ms exceeds {}ms",
                    static_cast<double>(scenario_summary.success_latency.p99.count()) / 1'000.0, limits.maximum_p99.count()));
            }

            scenario_summary.slo.passed = scenario_summary.slo.violations.empty();
            if (not scenario_summary.slo.passed) {
                summary.success = false;
            }
        }
    }

    for (const auto& scenario_name : selected_scenarios) {
        if (not summary.scenarios.contains(scenario_name)) {
            summary.scenarios.emplace(scenario_name, scenario_summary{});
            summary.infrastructure_errors.push_back("no measured results for " + scenario_name);
            summary.success = false;
        }
    }

    summary.infrastructure_valid = summary.infrastructure_errors.empty();
    if (not summary.infrastructure_valid) {
        summary.success = false;
    }
    return summary;
}

shard_result run_shard(const scenario_registry& scenarios, run_config config, const std::size_t shard_index,
    const clock::time_point started_at, runner_callbacks callbacks) {

    boost::asio::io_context io_context;
    auto executor = io_context.get_executor();
    boost::cobalt::this_thread::set_executor(executor);
    runner shard_runner{executor, scenarios, shard_index, std::move(callbacks)};
    std::optional<shard_result> result;
    std::exception_ptr failure;

    boost::cobalt::spawn(executor, shard_runner.run(std::move(config), started_at),
        [&](std::exception_ptr error, shard_result value) {
            failure = error;
            result = std::move(value);
        });
    io_context.run();

    if (failure) {
        std::rethrow_exception(failure);
    }
    if (not result) {
        throw std::runtime_error{"tester shard did not produce a result"};
    }
    return std::move(*result);
}

} // namespace

runner::runner(boost::cobalt::executor executor, const scenario_registry& scenarios, const std::size_t shard_index,
    runner_callbacks callbacks)
    : executor_{std::move(executor)}, scenarios_{scenarios}, shard_index_{shard_index}, callbacks_{std::move(callbacks)},
      client_tls_context_{boost::asio::ssl::context::tls_client} {

    client_tls_context_.set_verify_mode(boost::asio::ssl::verify_none);
}

boost::cobalt::task<shard_result> runner::run(run_config config, const clock::time_point started_at) {
    endpoint_result_collector endpoint_results;
    target_endpoint endpoint{executor_,scenarios_,endpoint_results};
    failure_backoff_ = config.failure_backoff;
    source_addresses_ = std::move(config.source_addresses);
    next_start_at_ = started_at;
    if (config.connection_rate) {
        rate_interval_ = std::max(std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>{1.0 / static_cast<double>(*config.connection_rate)}), clock::duration{1});
    }
    const auto measurement_start = started_at + config.warmup;
    const auto finish_at = config.duration ? std::optional{measurement_start + *config.duration} : std::nullopt;
    endpoint.set_measurement_start(measurement_start);
    auto endpoint_run = endpoint.run();
    if (clock::now() < started_at) {
        co_await boost::cobalt::io::sleep(started_at);
    }
    boost::cobalt::wait_group workers;

    for (const auto& selected_workload : config.workloads) {
        const auto worker_count = selected_workload.connections == 0 ? selected_workload.concurrency :
            std::min(selected_workload.connections, selected_workload.concurrency);
        auto state = std::make_shared<workload_state>();
        for (std::size_t worker_index = 0; worker_index < worker_count;++worker_index) {
            workers.push_back(run_worker(selected_workload, config.proxy, endpoint.address(), worker_index, state,
                measurement_start, finish_at));
        }
    }

    co_await workers;
    endpoint.stop();
    co_await endpoint_run;
    co_return build_result(endpoint_results.take_all());
}

boost::cobalt::promise<void> runner::run_worker(workload selected_workload, proxy_address proxy, endpoint_address endpoint,
    const std::size_t worker_index, std::shared_ptr<workload_state> next_connection, const clock::time_point measurement_start,
    const std::optional<clock::time_point> finish_at) {

    const auto& selected_scenario = scenarios_.at(selected_workload.scenario_name);
    while (not stopping()) {
        if (selected_workload.connections != 0 and next_connection->next_connection >= selected_workload.connections) {
            break;
        }
        if (not co_await acquire_start_slot(finish_at)) {
            break;
        }
        const auto connection_started_at = clock::now();
        if (selected_workload.connections != 0) {
            if (next_connection->next_connection >= selected_workload.connections) {
                break;
            }
            ++next_connection->next_connection;
        }

        const auto current_run_sequence = run_sequence_++;
        const auto source_address = source_addresses_.empty() ? std::optional<boost::asio::ip::address>{} :
            source_addresses_[(current_run_sequence + shard_index_) % source_addresses_.size()];
        client test_client{
            client_context{
                .run_id = fmt::format("{}-{:08}-s{}-w{}",selected_workload.scenario_name,current_run_sequence,
                    shard_index_, worker_index),
                .selected_scenario = selected_scenario,
                .proxy = proxy,
                .endpoint = endpoint,
                .source_address = source_address,
                .executor = executor_,
                .tls_context = client_tls_context_,
            }
        };

        callbacks_.connection_started();
        auto result = co_await test_client.run();
        const auto succeeded = result.success;
        const auto termination = result.termination;
        const auto run_id = result.run_id;
        const auto error = result.error;
        result.measured = connection_started_at >= measurement_start;
        callbacks_.connection_finished(result);
        client_results_.push_back(std::move(result));

        if (termination == termination_reason::resource_exhausted) {
            if (not infrastructure_failure_) {
                infrastructure_failure_ = fmt::format("{}: {}", run_id, error);
            }
            callbacks_.infrastructure_failed();
            break;
        }
        if (not succeeded and failure_backoff_.count() > 0 and not stopping()) {
            co_await boost::cobalt::io::sleep(failure_backoff_);
        }
    }
}

boost::cobalt::promise<bool> runner::acquire_start_slot(const std::optional<clock::time_point> finish_at) {
    while (not stopping()) {
        const auto now = clock::now();
        if (finish_at and now >= *finish_at) {
            co_return false;
        }
        if (not rate_interval_) {
            co_return true;
        }
        if (now >= next_start_at_) {
            next_start_at_ = now + *rate_interval_;
            co_return true;
        }

        auto wake_at = std::min(next_start_at_, now + std::chrono::milliseconds{100});
        if (finish_at) {
            wake_at = std::min(wake_at, *finish_at);
        }
        co_await boost::cobalt::io::sleep(wake_at);
    }
    co_return false;
}

bool runner::stopping() const {
    return callbacks_.stopping();
}

shard_result runner::build_result(std::vector<endpoint_result> endpoint_results) {
    std::erase_if(client_results_, [](const client_result& result) {
        return not result.measured;
    });
    std::erase_if(endpoint_results, [](const endpoint_result& result) {
        return not result.measured;
    });

    shard_result result{
        .client_results = std::move(client_results_),
        .endpoint_results = std::move(endpoint_results),
    };
    if (infrastructure_failure_) {
        result.infrastructure_errors.push_back("load generator resource exhausted: " + *infrastructure_failure_);
    }
    return result;
}

run_summary run_tester(const scenario_registry& scenarios, run_config config) {
    auto configured_scenarios = scenarios;
    if (config.payload_size) {
        set_payload_size(configured_scenarios,*config.payload_size);
    }
    const auto shard_configs = split_config(config);
    const auto started_at = clock::now() + std::chrono::milliseconds{100};
    const auto measurement_start = started_at + config.warmup;
    runtime_statistics statistics{started_at, measurement_start};
    shared_control control;

    boost::asio::io_context signal_context;
    boost::asio::signal_set signals{signal_context, SIGINT, SIGTERM};
    signals.async_wait([&](const boost::system::error_code& error, const int signal) {
        if (not error) {
            control.stop_signal = signal;
            control.stopping = true;
        }
    });
    std::thread signal_thread{[&] {
        signal_context.run();
    }};
    std::thread reporter{[&] {
        statistics.report(config.report_interval);
    }};

    struct shard_outcome {
        std::optional<shard_result> result;
        std::exception_ptr failure;
    };
    std::vector<shard_outcome> outcomes(shard_configs.size());
    std::vector<std::thread> threads;
    threads.reserve(shard_configs.size());

    for (std::size_t index = 0; index < shard_configs.size();++index) {
        threads.emplace_back([&,index] {
            try {
                outcomes[index].result = run_shard(configured_scenarios, shard_configs[index], index, started_at, runner_callbacks{
                    .connection_started = [&] {
                        statistics.connection_started();
                    },
                    .connection_finished = [&](const client_result& result) {
                        statistics.connection_finished(result);
                    },
                    .stopping = [&] {
                        return control.stopping.load();
                    },
                    .infrastructure_failed = [&] {
                        control.stopping = true;
                    },
                });
            } catch (...) {
                outcomes[index].failure = std::current_exception();
                control.stopping = true;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    statistics.finish();
    reporter.join();

    boost::system::error_code signal_error;
    signals.cancel(signal_error);
    signal_context.stop();
    signal_thread.join();

    for (const auto& outcome : outcomes) {
        if (outcome.failure) {
            std::rethrow_exception(outcome.failure);
        }
    }

    std::vector<client_result> client_results;
    std::vector<endpoint_result> endpoint_results;
    std::vector<std::string> infrastructure_errors;
    for (auto& outcome : outcomes) {
        if (not outcome.result) {
            throw std::runtime_error{"tester shard did not return a result"};
        }
        auto& result = *outcome.result;
        client_results.insert(client_results.end(), std::make_move_iterator(result.client_results.begin()),
            std::make_move_iterator(result.client_results.end()));
        endpoint_results.insert(endpoint_results.end(), std::make_move_iterator(result.endpoint_results.begin()),
            std::make_move_iterator(result.endpoint_results.end()));
        infrastructure_errors.insert(infrastructure_errors.end(),
            std::make_move_iterator(result.infrastructure_errors.begin()),
            std::make_move_iterator(result.infrastructure_errors.end()));
    }

    std::vector<std::string> selected_scenarios;
    selected_scenarios.reserve(config.workloads.size());
    for (const auto& selected_workload : config.workloads) {
        selected_scenarios.push_back(selected_workload.scenario_name);
    }
    return build_summary(configured_scenarios, selected_scenarios, config.duration.has_value(), std::move(client_results),
        std::move(endpoint_results), std::move(infrastructure_errors), control.stop_signal.load());
}

} // namespace tester
