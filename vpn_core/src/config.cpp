#include <vpn/config.h>

#include <fstream>
#include <set>

#include <boost/json/parse.hpp>
#include <boost/json/value_to.hpp>

#include "boost/asio/ip/address.hpp"

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

namespace server {
config tag_invoke(boost::json::value_to_tag<config>, boost::json::value const& jv ) {
    boost::json::object const& obj = jv.as_object();
    return config {
        value_to<std::string>(obj.at("ip")),
        value_to<std::uint16_t>(obj.at("port")),
    };
}
}

namespace {
std::expected<boost::json::value, std::string> read_json_file(const std::filesystem::path &config_file_path) {
    if (not std::filesystem::exists(config_file_path)) {
        return std::unexpected{fmt::format("Logger`s config `{}` does not exist", config_file_path.string())};
    }
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

namespace logger_config {
namespace {
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

std::expected<logger::logger_config, std::string> parse_and_validate_json_logger_config_object(const boost::json::value &object) {
    if (not object.is_object()) {
        return std::unexpected{"Must be an object"};
    }
    const auto& obj = object.as_object();
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

    try {
        return boost::json::value_to<logger::logger_config>(object);
    } catch (const std::exception& e) {
        return std::unexpected{fmt::format("Failed to convert logger configuration: {}",e.what())};
    }
}

std::expected<logger::loggers_settings, std::string> parse_json(const boost::json::value &object) {
    if (not object.is_object()) {
        return std::unexpected{"Root of config must be an object"};
    }

    const auto& root_object = object.as_object();
    const auto* loggers_value = root_object.if_contains("loggers");

    if (loggers_value == nullptr) {
        return std::unexpected{"Required field '$.loggers' is missing"};
    }

    if (not loggers_value->is_array()) {
        return std::unexpected{"'$.loggers' must be an array"};
    }

    const auto& array = loggers_value->as_array();
    logger::loggers_settings loggers_settings;
    loggers_settings.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        auto config = parse_and_validate_json_logger_config_object(array[index]);
        if (not config) {
            return std::unexpected{fmt::format("[{}].{}", index, config.error())};
        }
        loggers_settings.emplace_back(std::move(*config));
    }
    return loggers_settings;
}

std::expected<void, std::string> validate_config_struct(std::set<std::string_view>& duplicates_names,
    std::set<std::string_view>& duplicates_files, const logger::logger_config &config) {

    if (config.module_name.empty()) {
        return std::unexpected{fmt::format("Module name must be not empty")};;
    }
    if (duplicates_names.contains(config.module_name)) {
        return std::unexpected{fmt::format("'module_name' `{}` already exists", config.module_name)};;
    }
    duplicates_names.insert(config.module_name);

    if (config.filename.empty()) {
        return std::unexpected{"'filename' must be not empty"};
    }
    const std::filesystem::path filename{config.filename};
    if (filename.empty() or filename.is_absolute() or filename != filename.filename()) {
        return std::unexpected{"'filename' must be a plain relative filename"};
    }
    if (duplicates_files.contains(config.filename)) {
        return std::unexpected{fmt::format("'filename' `{}` already exists at others logger settings", config.filename)};
    }
    duplicates_files.insert(config.filename);

    if (config.pattern.empty()) {
        return std::unexpected{"'pattern' must be not empty"};
    }
    if (auto res = validate_spdlog_pattern(config.pattern); not res) {
        return std::unexpected{fmt::format("'pattern' has error: '{}'", res.error())};;
    }

    if (config.level.empty()) {
        return std::unexpected{"'level' must be not empty"};
    }
    if (not map_string_logger_level.contains(config.level)) {
        return std::unexpected{"'level' must bet one of {{'debug', 'info', 'trace', 'warning', 'error'}}"};
    }

    if (config.rotation_hour < 0 or config.rotation_hour > 23) {
        return std::unexpected{"'rotation_hour' must be in range [0, 23]"};
    }
    if (config.rotation_minute < 0 or config.rotation_minute > 59) {
        return std::unexpected{"'rotation_minute' must be in range [0, 59]"};
    }
    return {};
}
}
}

std::expected<void, std::string> validate(const logger::loggers_settings &configs) {
    std::set<std::string_view> duplicates_names;
    std::set<std::string_view> duplicates_files;
    int index = 0;
    for (const auto& config : configs) {
        if (auto validation = logger_config::validate_config_struct(duplicates_names, duplicates_files, config); not validation) {
            return std::unexpected{fmt::format("[{}].{}", index, validation.error())};
        }
        index++;
    }
    return {};
}

std::expected<logger::loggers_settings, std::string> validate_and_return(logger::loggers_settings configs) {
    return validate(configs).transform([configs = std::move(configs)] mutable -> logger::loggers_settings {
        return std::move(configs);
    });
}

namespace server_config {
namespace {
std::expected<server::config, std::string> parse_and_validate_json_server_config_object(const boost::json::value &object) {
    const auto& obj = object.as_object();
    if (not obj.contains("ip")) {
        return std::unexpected{fmt::format("must contains 'ip'")};
    }
    auto ip = obj.if_contains("ip");
    if (not ip->is_string()) {
        return std::unexpected{fmt::format("'ip' must be a string ipv4 or ipv6 format")};
    }
    if (ip->as_string().empty()) {
        return std::unexpected{fmt::format("'ip' must not be empty")};
    }

    if (not obj.contains("port")) {
        return std::unexpected{fmt::format("must contains 'port'")};
    }
    auto port = obj.if_contains("port");
    if (not port->is_int64()) {
        return std::unexpected{fmt::format("'port' must be an integer in range [0, 65535]")};
    }
    if (port->as_int64() < 0 or port->as_int64() > 65535) {
        return std::unexpected{fmt::format("'port' must be in range [0, 65535]")};
    }

    try {
        return boost::json::value_to<server::config>(object);
    } catch (const std::exception& e) {
        return std::unexpected{fmt::format("Failed to convert server configuration: {}",e.what())};
    }
}

std::expected<server::config, std::string> parse_json(const boost::json::value &object) {
    if (not object.is_object()) {
        return std::unexpected{"Root of config must be an object"};
    }

    const auto& root_object = object.as_object();
    const auto* server_value = root_object.if_contains("server");

    if (server_value == nullptr) {
        return std::unexpected{"Required field '$.server' is missing"};
    }
    if (not server_value->is_object()) {
        return std::unexpected{fmt::format("'server' must be an object")};
    }
    auto config = parse_and_validate_json_server_config_object(*server_value);
    if (not config) {
        return std::unexpected{fmt::format("'server' config error: {}" ,config.error())};
    }

    return config;
}
}
}

std::expected<void, std::string> validate(const server::config &config) {
    try {
        boost::asio::ip::make_address(config.address);
    } catch (const boost::system::system_error& error) {
        return std::unexpected{fmt::format("Could not make address from: {} error: {}", config.address, error.what())};
    } catch (const std::exception& error) {
        return std::unexpected{fmt::format("Could not make address from: {} error: {}", config.address, error.what())};
    }
    return {};
}

std::expected<server::config, std::string> validate_and_return(server::config config) {
    return validate(config).transform([config = std::move(config)] mutable -> server::config {
        return std::move(config);
    });
}

std::expected<logger::loggers_settings, std::string> load_logger_config_file(const std::filesystem::path &config_folder_path) {
    if (not std::filesystem::exists(config_folder_path)) {
        return std::unexpected{fmt::format("Config folder `{}` does not exist", config_folder_path.string())};
    }
    auto config_path = std::filesystem::path{config_folder_path/"loggers.json"};
    if (not std::filesystem::exists(config_path)) {
        return std::unexpected{fmt::format("Logger`s config `{}` does not exist", config_path.string())};
    }

    return read_json_file(config_path).and_then([](const boost::json::value& json) {
        return logger_config::parse_json(json);
    })
    .and_then([](logger::loggers_settings configs) -> std::expected<logger::loggers_settings, std::string> {
        return validate_and_return(std::move(configs));
    });
}

std::expected<server::config, std::string> load_server_config_file(const std::filesystem::path &config_folder_path) {
    if (not std::filesystem::exists(config_folder_path)) {
        return std::unexpected{fmt::format("Config folder `{}` does not exist", config_folder_path.string())};
    }
    auto config_path = std::filesystem::path{config_folder_path/"server.json"};
    if (not std::filesystem::exists(config_path)) {
        return std::unexpected{fmt::format("Server`s config `{}` does not exist", config_path.string())};
    }
    return read_json_file(config_path).and_then([](const boost::json::value& json) {
        return server_config::parse_json(json);
    })
    .and_then([](server::config configs) -> std::expected<server::config, std::string> {
        return validate_and_return(std::move(configs));
    });
}
}

std::expected<config::main_config, std::string> config::load_config(const std::filesystem::path &config_folder_path) {
    auto logger_config = load_logger_config_file(config_folder_path);
    if (not logger_config) {
        return std::unexpected{fmt::format("Failed to load logger config: {}", logger_config.error())};
    }

    auto server_config = load_server_config_file(config_folder_path);
    if (not server_config) {
        return std::unexpected{fmt::format("Failed to load server config: {}", server_config.error())};
    }
    return config::main_config{std::move(*logger_config), std::move(*server_config)};
}


