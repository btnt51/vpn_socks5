#ifndef VPN_CONFIG_H
#define VPN_CONFIG_H
#include <expected>
#include <filesystem>
#include <vpn/logger.h>
#include <vpn/server.h>

namespace config {
std::expected<logger::loggers_settings, std::string> load_logger_config_file(const std::filesystem::path &config_folder_path = "./config/");
std::expected<server::config, std::string> load_server_config_file(const std::filesystem::path &config_folder_path = "./config/");
}

#endif //VPN_CONFIG_H
