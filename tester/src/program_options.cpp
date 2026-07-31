#include "program_options.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <fmt/format.h>

namespace tester {

namespace {

namespace po = boost::program_options;

std::expected<std::size_t, std::string> parse_size(std::string_view value, std::string_view name) {
    std::size_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} or end != value.data() + value.size()) {
        return std::unexpected{fmt::format("invalid {}: {}", name, value)};
    }
    return result;
}

std::expected<std::uint64_t, std::string> parse_byte_size(std::string_view value) {
    const auto suffix_at = value.find_first_not_of("0123456789");
    const auto number = value.substr(0,suffix_at);
    const auto suffix = suffix_at == std::string_view::npos ? std::string_view{} : value.substr(suffix_at);
    std::uint64_t parsed{};
    const auto [end,error] = std::from_chars(number.data(),number.data() + number.size(),parsed);
    if (number.empty() or error != std::errc{} or end != number.data() + number.size()) {
        return std::unexpected{fmt::format("invalid payload size: {}",value)};
    }

    std::uint64_t multiplier{1};
    if (suffix.empty() or suffix == "B") {
        multiplier = 1;
    } else if (suffix == "KiB") {
        multiplier = 1024ULL;
    } else if (suffix == "MiB") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "GiB") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else {
        return std::unexpected{fmt::format("invalid payload size '{}', expected suffix B, KiB, MiB or GiB",value)};
    }
    if (parsed == 0) {
        return std::unexpected{"payload size must be greater than zero"};
    }
    if (parsed > std::numeric_limits<std::uint64_t>::max() / multiplier / 2) {
        return std::unexpected{"payload size is too large"};
    }
    return parsed * multiplier;
}

std::expected<std::chrono::milliseconds, std::string> parse_duration(std::string_view value, std::string_view name, bool allow_zero) {
    const auto suffix_at = value.find_first_not_of("0123456789");
    const auto number = value.substr(0, suffix_at);
    const auto suffix = suffix_at == std::string_view::npos ? std::string_view{} : value.substr(suffix_at);
    auto parsed = parse_size(number, name);
    if (not parsed) {
        return std::unexpected{parsed.error()};
    }
    if (*parsed == 0 and not allow_zero) {
        return std::unexpected{fmt::format("{} must be greater than zero", name)};
    }

    std::uint64_t multiplier{};
    if (suffix == "ms") {
        multiplier = 1;
    } else if (suffix == "s") {
        multiplier = 1'000;
    } else if (suffix == "m") {
        multiplier = 60'000;
    } else if (suffix == "h") {
        multiplier = 3'600'000;
    } else {
        return std::unexpected{fmt::format("invalid {} '{}', expected suffix ms, s, m or h", name, value)};
    }
    if (*parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / multiplier) {
        return std::unexpected{fmt::format("{} is too large", name)};
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(*parsed * multiplier)};
}

std::expected<workload, std::string> parse_workload(std::string_view value, const bool duration_mode) {
    const auto first_separator = value.find(':');
    const auto second_separator = first_separator == std::string_view::npos ? std::string_view::npos : value.find(':', first_separator + 1);
    const auto extra_separator = second_separator == std::string_view::npos ? std::string_view::npos : value.find(':', second_separator + 1);

    if (first_separator == std::string_view::npos or extra_separator != std::string_view::npos or
        (duration_mode and second_separator != std::string_view::npos) or (not duration_mode and second_separator == std::string_view::npos)) {

        return std::unexpected{fmt::format("invalid scenario '{}', expected {}", value,
            duration_mode ? "name:concurrency" : "name:connections:concurrency")};
    }

    const auto scenario_name = value.substr(0, first_separator);
    if (scenario_name.empty()) {
        return std::unexpected{"scenario name must not be empty"};
    }

    const auto connections_value = duration_mode ? std::string_view{} : value.substr(first_separator + 1, second_separator - first_separator - 1);
    const auto concurrency_value = duration_mode ? value.substr(first_separator + 1) : value.substr(second_separator + 1);
    auto connections = duration_mode ? std::expected<std::size_t, std::string>{0} : parse_size(connections_value, "connections");
    auto concurrency = parse_size(concurrency_value, "concurrency");

    if (not concurrency) {
        return std::unexpected{concurrency.error()};
    }
    if (not duration_mode and not connections) {
        return std::unexpected{connections.error()};
    }
    if (not duration_mode and *connections == 0) {
        return std::unexpected{fmt::format("scenario '{}' connections must be greater than zero", scenario_name)};
    }
    if (*concurrency == 0) {
        return std::unexpected{fmt::format("scenario '{}' concurrency must be greater than zero", scenario_name)};
    }

    return workload{.scenario_name = std::string{scenario_name}, .connections = *connections, .concurrency = *concurrency};
}

} // namespace

program_options::program_options() : options_{"Options"} {
    options_.add_options()
        ("help,h", "show help")
        ("command", po::value<std::string>(), "command: list or run")
        ("proxy-address", po::value<std::string>()->default_value("127.0.0.1"), "SOCKS5 proxy address")
        ("proxy-port", po::value<unsigned int>()->default_value(1080), "SOCKS5 proxy port")
        ("source-address", po::value<std::vector<std::string>>()->composing(), "local source IP address, may be repeated")
        ("duration", po::value<std::string>(), "measurement duration, for example 30s or 5m")
        ("payload-size", po::value<std::string>(), "download payload size, for example 64KiB, 10MiB or 1GiB")
        ("rate", po::value<std::size_t>(), "maximum number of new connections per second")
        ("threads", po::value<std::size_t>()->default_value(1), "number of independent network threads")
        ("warmup", po::value<std::string>()->default_value("0s"), "warm-up duration excluded from statistics")
        ("report-interval", po::value<std::string>()->default_value("1s"), "runtime statistics interval")
        ("failure-backoff", po::value<std::string>()->default_value("100ms"), "delay before retry after a client failure")
        ("scenario,s", po::value<std::vector<std::string>>()->composing(), "name:concurrency with --duration or name:connections:concurrency");

    positional_.add("command", 1);
}

std::expected<settings, std::string> program_options::parse(int argc, char* argv[]) const {
    try {
        po::variables_map variables;
        po::store(po::command_line_parser(argc, argv).options(options_).positional(positional_).run(), variables);
        po::notify(variables);

        if (variables.contains("help") or not variables.contains("command")) {
            return settings{.selected_command = command::help};
        }

        const auto command_name = variables["command"].as<std::string>();
        if (command_name == "list") {
            return settings{.selected_command = command::list};
        }
        if (command_name != "run") {
            return std::unexpected{fmt::format("unknown command: {}", command_name)};
        }
        if (not variables.contains("scenario")) {
            return std::unexpected{"run requires at least one --scenario"};
        }

        const auto proxy_port = variables["proxy-port"].as<unsigned int>();
        if (proxy_port > std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected{"proxy port is outside uint16 range"};
        }
        boost::system::error_code proxy_address_error;
        auto proxy_address = boost::asio::ip::make_address(variables["proxy-address"].as<std::string>(),proxy_address_error);
        if (proxy_address_error) {
            return std::unexpected{"invalid proxy address: " + proxy_address_error.message()};
        }

        settings result{
            .selected_command = command::run,
            .config = {
                .proxy = {
                    .host = proxy_address.to_string(),
                    .port = static_cast<std::uint16_t>(proxy_port),
                },
            },
        };

        if (variables.contains("source-address")) {
            std::unordered_set<std::string> source_addresses;
            for (const auto& value : variables["source-address"].as<std::vector<std::string>>()) {
                boost::system::error_code error;
                auto address = boost::asio::ip::make_address(value, error);
                if (error) {
                    return std::unexpected{fmt::format("invalid source address '{}': {}",value,error.message())};
                }
                if (address.is_unspecified()) {
                    return std::unexpected{fmt::format("source address '{}' must not be unspecified",value)};
                }
                if (address.is_v4() != proxy_address.is_v4()) {
                    return std::unexpected{fmt::format("source address '{}' and proxy address '{}' use different address families",
                        value,proxy_address.to_string())};
                }
                const auto normalized = address.to_string();
                if (not source_addresses.emplace(normalized).second) {
                    return std::unexpected{fmt::format("source address '{}' is specified more than once",normalized)};
                }
                result.config.source_addresses.push_back(std::move(address));
            }
        }

        const auto duration_mode = variables.contains("duration");
        if (duration_mode) {
            auto duration = parse_duration(variables["duration"].as<std::string>(), "duration", false);
            if (not duration) {
                return std::unexpected{duration.error()};
            }
            result.config.duration = *duration;
        }
        if (variables.contains("rate")) {
            const auto rate = variables["rate"].as<std::size_t>();
            if (rate == 0) {
                return std::unexpected{"rate must be greater than zero"};
            }
            result.config.connection_rate = rate;
        }
        if (variables.contains("payload-size")) {
            auto payload_size = parse_byte_size(variables["payload-size"].as<std::string>());
            if (not payload_size) {
                return std::unexpected{payload_size.error()};
            }
            result.config.payload_size = *payload_size;
        }
        result.config.threads = variables["threads"].as<std::size_t>();
        if (result.config.threads == 0) {
            return std::unexpected{"threads must be greater than zero"};
        }

        auto warmup = parse_duration(variables["warmup"].as<std::string>(), "warmup", true);
        auto report_interval = parse_duration(variables["report-interval"].as<std::string>(), "report interval", false);
        auto failure_backoff = parse_duration(variables["failure-backoff"].as<std::string>(), "failure backoff", true);
        if (not warmup) {
            return std::unexpected{warmup.error()};
        }
        if (not report_interval) {
            return std::unexpected{report_interval.error()};
        }
        if (not failure_backoff) {
            return std::unexpected{failure_backoff.error()};
        }
        if (not duration_mode and warmup->count() != 0) {
            return std::unexpected{"warmup requires --duration"};
        }
        result.config.warmup = *warmup;
        result.config.report_interval = *report_interval;
        result.config.failure_backoff = *failure_backoff;

        std::unordered_set<std::string> scenario_names;
        for (const auto& value : variables["scenario"].as<std::vector<std::string>>()) {
            auto selected_workload = parse_workload(value, duration_mode);
            if (not selected_workload) {
                return std::unexpected{selected_workload.error()};
            }
            if (not scenario_names.emplace(selected_workload->scenario_name).second) {
                return std::unexpected{fmt::format("scenario '{}' is specified more than once", selected_workload->scenario_name)};
            }
            result.config.workloads.push_back(std::move(*selected_workload));
        }

        return result;
    } catch (const po::error& error) {
        return std::unexpected{error.what()};
    }
}

std::string program_options::help() const {
    std::ostringstream stream;
    stream << "Usage:\n"
           << "  tester list\n"
           << "  tester run [options] --scenario name:connections:concurrency [...]\n"
           << "  tester run --duration <time> [options] --scenario name:concurrency [...]\n\n"
           << "Examples:\n"
           << "  tester run --scenario pong:10:4\n"
           << "  tester run --duration 5m --warmup 30s --report-interval 1s --rate 100 --threads 4 "
              "--scenario pong:200 --scenario complete-download:20 --scenario interrupted-download:20\n\n"
           << options_;
    return stream.str();
}

} // namespace tester
