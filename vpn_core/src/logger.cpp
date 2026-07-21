#include <vpn/logger.h>

#include <ranges>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>

namespace {
logger_levels from_string(std::string_view log_level) {
    if (log_level.empty()) {
        return logger_levels::e_debug;
    }
    if (not map_string_logger_level.contains(log_level)) {
        return logger_levels::e_debug;
    }
    return map_string_logger_level.at(log_level);
}
}

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

logger::runtime::runtime(const loggers_settings &settings) {
    spdlog::init_thread_pool(8192, 1);
    spdlog::flush_every(std::chrono::seconds{15});

    for (const auto& config : settings) {
        logger_.create_async_logger(config);
    }
}

logger::runtime::~runtime() noexcept {
    logger_.shutdown();
    spdlog::shutdown();
}

logger::logger & logger::runtime::get() noexcept {
    return logger_;
}
