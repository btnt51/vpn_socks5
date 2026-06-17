#ifndef VPN_LOGGER_H
#define VPN_LOGGER_H

#include <cassert>
#include <string>
#include <unordered_map>

#include <spdlog/async_logger.h>
#include <spdlog/common.h>

namespace spdlog {
class async_logger;
}
enum class logger_levels {
    e_debug = SPDLOG_LEVEL_DEBUG,
    e_info = SPDLOG_LEVEL_INFO,
    e_trace = SPDLOG_LEVEL_TRACE,
    e_warning = SPDLOG_LEVEL_WARN,
    e_error = SPDLOG_LEVEL_ERROR,
};


class logger {
public:
    void create_async_logger(std::string module_name, logger_levels log_level);

    template <typename... Args>
    void log(const std::string& module_name, logger_levels log_level, fmt::format_string<Args...> fmt, Args&&... args);

    ~logger();

private:
    std::unordered_map<std::string,std::shared_ptr<spdlog::async_logger>> loggers;
};

template<typename ... Args>
void logger::log(const std::string& module_name, logger_levels log_level, fmt::format_string<Args...> fmt, Args &&...args) {
    const auto logger_it = loggers.find(module_name);
    if (logger_it == loggers.end()) {
        assert(false && fmt::format("there is no logger with name {}", module_name).c_str());
        return;
    }
    const auto& logger = logger_it->second;
    logger->log(static_cast<spdlog::level::level_enum>(log_level), std::forward<fmt::format_string<Args...>>(fmt), std::forward<Args>(args)...);
}

inline logger g_logger{};


#endif //VPN_LOGGER_H
