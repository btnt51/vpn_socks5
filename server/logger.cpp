#include "logger.h"

#include <ranges>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>

void logger::create_async_logger(std::string module_name, logger_levels log_level) {
    auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(fmt::format("logs/{}.log", module_name), 12,30);

    auto logger = std::make_shared<spdlog::async_logger>(module_name, sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] %v");
    logger->set_level(static_cast<spdlog::level::level_enum>(log_level));
    spdlog::register_or_replace(logger);
    loggers[module_name] = logger;
}

logger::~logger() {
    for (const auto& logger : loggers | std::views::values) {
        logger->flush();
    }
}
