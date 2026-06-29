#include "logger.h"

#include <ranges>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>

void logger::create_async_logger(std::string module_name, logger_levels log_level) {
    auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(fmt::format("./logs/{}.log", module_name), 12,30);

    auto logger = std::make_shared<spdlog::async_logger>(module_name, sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] %v");
    logger->set_level(static_cast<spdlog::level::level_enum>(log_level));
    spdlog::register_or_replace(logger);
    loggers[module_name] = logger;
}

void logger::shutdown() {
    for (const auto& module_logger : loggers | std::views::values) {
        module_logger->flush();
    }
    loggers.clear();
}

void init_logger(const std::vector<std::string> &module_names) {
    for (const auto& module_name : module_names) {
        g_logger.create_async_logger(module_name, logger_levels::e_debug);
    }
}

void shutdown_logger() {
    g_logger.shutdown();
    spdlog::shutdown();
}
