#include <vpn/logger.h>

#include <ranges>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>

void logger::logger::create_async_logger(const logger_config &config) {
    auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(fmt::format("./logs/{}", config.filename), config.rotation_hour, config.rotation_minute);

    auto logger = std::make_shared<spdlog::async_logger>(config.module_name, sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    logger->set_pattern(config.pattern);
    logger->set_level(static_cast<spdlog::level::level_enum>(from_string(config.level)));
    spdlog::register_or_replace(logger);
    loggers[config.module_name] = logger;
}

void logger::logger::shutdown() {
    for (const auto& module_logger : loggers | std::views::values) {
        module_logger->flush();
    }
    loggers.clear();
}

void logger::init_logger(const std::vector<logger_config>& configs) {
    spdlog::flush_every(std::chrono::seconds(15));
    for (const auto& config : configs) {
        g_logger.create_async_logger(config);
    }
}

void logger::shutdown_logger() {
    g_logger.shutdown();
    spdlog::shutdown();
}
