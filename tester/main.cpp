#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <utility>

#include <fmt/base.h>

#include "program_options.h"
#include "runner.h"
#include "scenario.h"

namespace {

void print_scenarios() {
    fmt::println("Available scenarios:");
    for (const auto& [name, scenario] : tester::scenarios()) {
        fmt::println("  {:<24} client-steps={} slo-error<={:.1f}% slo-p95<={}ms slo-p99<={}ms", name,
            scenario.client_steps.size(), scenario.slo.maximum_error_rate_percent,
            scenario.slo.maximum_p95.count(), scenario.slo.maximum_p99.count());
    }
}

void print_summary(const tester::run_summary& summary) {
    fmt::println("Tester result: {}",not summary.infrastructure_valid ? "INVALID" : summary.success ? "PASS" : "FAIL");

    for (const auto& [name, scenario] : summary.scenarios) {
        const auto average_latency = scenario.succeeded == 0 ? 0 :
            scenario.successful_latency.count() / static_cast<std::int64_t>(scenario.succeeded);

        fmt::println("  {}: success={} failure={} success-avg={}us success-p50={:.1f}ms success-p90={:.1f}ms "
            "success-p95={:.1f}ms success-p99={:.1f}ms success-max={:.1f}ms failure-p95={:.1f}ms tx={} rx={}",
            name, scenario.succeeded, scenario.failed, average_latency,
            static_cast<double>(scenario.success_latency.p50.count()) / 1'000.0,
            static_cast<double>(scenario.success_latency.p90.count()) / 1'000.0,
            static_cast<double>(scenario.success_latency.p95.count()) / 1'000.0,
            static_cast<double>(scenario.success_latency.p99.count()) / 1'000.0,
            static_cast<double>(scenario.success_latency.maximum.count()) / 1'000.0,
            static_cast<double>(scenario.failure_latency.p95.count()) / 1'000.0,
            scenario.bytes_sent, scenario.bytes_received);
        for (const auto& [reason, count] : scenario.errors) {
            fmt::println("    error {}={}", tester::to_string(reason), count);
        }
        if (scenario.slo.evaluated) {
            fmt::println("    SLO {} error-rate={:.2f}%<={:.2f}% success-p95={:.1f}ms<={}ms success-p99={:.1f}ms<={}ms",
                scenario.slo.passed ? "PASS" : "FAIL", scenario.slo.error_rate_percent, scenario.slo.limits.maximum_error_rate_percent,
                static_cast<double>(scenario.success_latency.p95.count()) / 1'000.0, scenario.slo.limits.maximum_p95.count(),
                static_cast<double>(scenario.success_latency.p99.count()) / 1'000.0, scenario.slo.limits.maximum_p99.count());
            for (const auto& violation : scenario.slo.violations) {
                fmt::println("      {}", violation);
            }
        }
    }

    constexpr std::size_t maximum_error_details = 20;
    std::size_t printed_errors{};
    std::size_t total_errors{};
    for (const auto& result : summary.client_results) {
        if (result.success) {
            continue;
        }
        ++total_errors;
        if (printed_errors < maximum_error_details) {
            fmt::println("  {} {} step={} step-index={} termination={} error={}",
                result.termination == tester::termination_reason::resource_exhausted ? "INFRASTRUCTURE" : "FAIL", result.run_id,
                result.failed_step_name, result.failed_step, tester::to_string(result.termination), result.error);
            ++printed_errors;
        }
    }
    if (total_errors > printed_errors) {
        fmt::println("  ... {} more client failures omitted", total_errors - printed_errors);
    }

    const auto infrastructure_details = std::min(summary.infrastructure_errors.size(), maximum_error_details);
    for (std::size_t index = 0; index < infrastructure_details; ++index) {
        fmt::println("  INFRASTRUCTURE: {}", summary.infrastructure_errors[index]);
    }
    if (summary.infrastructure_errors.size() > infrastructure_details) {
        fmt::println("  ... {} more infrastructure failures omitted", summary.infrastructure_errors.size() - infrastructure_details);
    }
    if (summary.stop_signal != 0) {
        fmt::println("  Stopped by signal {}", summary.stop_signal);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    tester::program_options options;
    auto settings = options.parse(argc, argv);
    if (not settings) {
        fmt::println(stderr, "tester failed: {}\n\n{}", settings.error(), options.help());
        return 2;
    }

    if (settings->selected_command == tester::command::help) {
        fmt::print("{}", options.help());
        return 0;
    }
    if (settings->selected_command == tester::command::list) {
        print_scenarios();
        return 0;
    }
    for (const auto& workload : settings->config.workloads) {
        if (not tester::scenarios().contains(workload.scenario_name)) {
            fmt::println(stderr, "tester failed: unknown scenario '{}'", workload.scenario_name);
            return 2;
        }
    }

    try {
        const auto summary = tester::run_tester(tester::scenarios(),std::move(settings->config));
        print_summary(summary);

        if (summary.stop_signal != 0) {
            return 128 + summary.stop_signal;
        }
        if (not summary.infrastructure_valid) {
            return 3;
        }
        return summary.success ? 0 : 1;
    } catch (const std::exception& error) {
        fmt::println(stderr, "tester failed: {}", error.what());
        return 1;
    }
}
