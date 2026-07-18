#include "config.h"

#include <fstream>
#include <set>

#include <boost/json/parse.hpp>
#include <boost/json/value_to.hpp>

namespace logger {
logger_config tag_invoke(boost::json::value_to_tag<logger_config>, boost::json::value const& jv ) {
    boost::json::object const& obj = jv.as_object();
    return logger_config {
        value_to<std::string>(obj.at("module_name")),
        value_to<std::string>(obj.at("filename")),
        value_to<std::string>(obj.at("pattern")),
        value_to<std::string>(obj.at("level")),
        value_to<int>(obj.at("rotation_hour")),
        value_to<int>(obj.at("rotation_minute")),
    };
}
}

std::expected<void, std::string> validate_spdlog_pattern(std::string_view pattern) {
    if (pattern.empty()) {
        return std::unexpected{"Logger pattern must not be empty"};
    }

    if (pattern.size() > 1024) {
        return std::unexpected{"Logger pattern is too long"};
    }

    constexpr std::string_view valid_flags =
        "+nlLtv"
        "aAbhBcCYDxmdHIMSefFEprRTXz"
        "P^$@sg#!%"
        "uioO"
        "&";

    bool contains_message = false;

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            continue;
        }

        ++i;

        if (i == pattern.size()) {
            return std::unexpected{fmt::format("Logger pattern ends with an incomplete '%' at position {}", i - 1)};
        }

        if (pattern[i] == '%') {
            continue;
        }

        if (pattern[i] == '-' or pattern[i] == '=') {
            ++i;

            if (i == pattern.size()) {
                return std::unexpected{"Logger pattern ends after a padding alignment modifier"};
            }
        }

        std::size_t width = 0;
        bool has_width = false;

        while (i < pattern.size() and std::isdigit(static_cast<unsigned char>(pattern[i]))) {
            has_width = true;

            const auto digit =
                static_cast<unsigned>(pattern[i] - '0');

            if (width > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
                return std::unexpected{"Logger pattern width is too large"};
            }

            width = width * 10 + digit;
            ++i;
        }

        if (has_width and width > 64) {
            return std::unexpected{fmt::format("Logger pattern width {} exceeds spdlog limit 64", width)};
        }

        if (has_width and i < pattern.size() and pattern[i] == '!') {
            ++i;
        }

        if (i == pattern.size()) {
            return std::unexpected{"Logger pattern ends before a format flag"};
        }

        const char flag = pattern[i];

        if (not valid_flags.contains(flag)) {
            return std::unexpected{fmt::format("Unknown spdlog pattern flag '%{}' at position {}", flag, i - 1)};
        }

        if (flag == 'v' or flag == '+') {
            contains_message = true;
        }
    }

    if (not contains_message) {
        return std::unexpected{"Logger pattern must contain '%v' or '%+'"};
    }

    return {};
}

std::expected<boost::json::value, std::string> read_json_file(
    const std::filesystem::path &config_file_path) {
    std::ifstream file(config_file_path.string(), std::ios::in);
    if (not file.is_open()) {
        return std::unexpected{fmt::format("Could not open config file `{}`", config_file_path.string())};
    }
    boost::system::error_code ec;
    std::string content((std::istreambuf_iterator<char>(file)), {});
    auto res = boost::json::parse(content, ec);
    if (ec) {
        return std::unexpected{fmt::format("Error occurred while parsing config file `{}` error: {}", config_file_path.string(), ec.message())};
    }
    return res;
}

std::expected<void, std::string> validate_json_logger_config_object(const boost::json::value &object) {
    if (not object.is_object()) {
        return std::unexpected{"Must be an object"};
    }
    const auto obj = object.as_object();
    if (not obj.contains("module_name")) {
        return std::unexpected{"'module_name' must exists in object"};
    }
    if (not obj.at("module_name").is_string()) {
        return std::unexpected{"'module_name' be string"};
    }
    if (not obj.contains("filename")) {
        return std::unexpected{"'filename' must exists in object"};
    }
    if (not obj.at("filename").is_string()) {
        return std::unexpected{"'filename' be string"};
    }
    if (not obj.contains("pattern")) {
        return std::unexpected{"'pattern' must exists in object"};
    }
    if (not obj.at("pattern").is_string()) {
        return std::unexpected{"'pattern' be string"};
    }
    if (not obj.contains("level")) {
        return std::unexpected{"'level' must exists in object"};
    }
    if (not obj.at("level").is_string()) {
        return std::unexpected{"'level' be string"};
    }
    if (not obj.contains("rotation_hour")) {
        return std::unexpected{"'rotation_hour' must exists in object"};
    }
    if (not obj.at("rotation_hour").is_int64()) {
        return std::unexpected{"'rotation_hour' be integer"};
    }
    if (not obj.contains("rotation_minute")) {
        return std::unexpected{"'rotation_minute' must exists in object"};
    }
    if (not obj.at("rotation_minute").is_int64()) {
        return std::unexpected{"'rotation_minute' be integer"};
    }
    return {};
}

std::expected<logger::loggers_settings, std::string> parse_json(const boost::json::value &object) {
    if (not object.is_array()) {
        return std::unexpected{"Root of config's settings must be an array"};
    }
    int index = 0;
    for (const auto& entry : object.as_array()) {
        if (auto validate_result = validate_json_logger_config_object(entry); not validate_result) {
            return std::unexpected{fmt::format("[{}].{}", index, validate_result.error())};
        }
        index++;
    }
    return boost::json::value_to<logger::loggers_settings>(object);
}

std::expected<void, std::string> validate(const logger::loggers_settings &configs) {
    std::set<std::string_view> duplicates_names;
    std::set<std::string_view> duplicates_files;
    int index = 0;
    for (const auto& config : configs) {
        if (config.module_name.empty()) {
            return std::unexpected{fmt::format("Module name must be not empty, check object at index: {}", index)};
        }
        if (duplicates_names.contains(config.module_name)) {
            return std::unexpected{fmt::format("Settings for logger `{}` already exists, check object at index: {}", config.module_name, index)};
        }
        duplicates_names.insert(config.module_name);
        if (config.filename.empty()) {
            return std::unexpected{fmt::format("Filename must be not empty for module: {}, check object at index: {}", config.module_name, index)};
        }
        if (duplicates_files.contains(config.filename)) {
            return std::unexpected{fmt::format("Logger filename: `{}` already exists, check object at index: {}", config.filename, index)};
        }
        duplicates_files.insert(config.filename);
        if (config.pattern.empty()) {
            return std::unexpected{fmt::format("Pattern must be not empty for module: {}, check object at index: {}", config.module_name, index)};
        }
        if (auto res = validate_spdlog_pattern(config.pattern); not res) {
            return std::unexpected{fmt::format("Pattern for module: {} has error: '{}', check object at index: {}", config.module_name, res.error(), index)};
        }
        if (config.level.empty()) {
            return std::unexpected{fmt::format("Level must be not empty for module: {}, check object at index: {}", config.module_name, index)};
        }
        if (not map_string_logger_level.contains(config.level)) {
            return std::unexpected{fmt::format("Level for module: {} must bet one of {{'debug', 'info', 'trace', 'warning', 'error'}}, check object at index: {}", config.module_name, index)};
        }
        if (config.rotation_hour < 0 or config.rotation_hour > 23) {
            return std::unexpected{fmt::format("rotation_hour for module '{}' must be in range [0, 23], check object at index: {}", config.module_name, index)};
        }

        if (config.rotation_minute < 0 or config.rotation_minute > 59) {
            return std::unexpected{fmt::format("rotation_minute for module '{}' must be in range [0, 59], check object at index: {}", config.module_name, index)};
        }

        index++;
    }
    return {};
}

std::expected<logger::loggers_settings, std::string> validate_and_return(logger::loggers_settings configs) {
    return validate(configs).transform([configs = std::move(configs)]() mutable {
        return std::move(configs);
    });
}

std::expected<logger::loggers_settings, std::string> config::load_config_file(
    const std::filesystem::path &config_folder_path) {
    if (not std::filesystem::exists(config_folder_path)) {
        return std::unexpected{fmt::format("Config folder `{}` does not exist", config_folder_path.string())};
    }
    auto config_path = std::filesystem::path{config_folder_path/"loggers.json"};
    if (not std::filesystem::exists(config_path)) {
        return std::unexpected{fmt::format("Logger`s config `{}` does not exist", config_path.string())};
    }


    return read_json_file(config_path).and_then([](const boost::json::value& json) {
        return parse_json(json);
    })
    .and_then([](const logger::loggers_settings& configs) {
        return validate_and_return(configs);
    });
}


