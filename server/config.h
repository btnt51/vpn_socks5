#ifndef VPN_CONFIG_H
#define VPN_CONFIG_H
#include <expected>
#include <filesystem>
#include "logger.h"



namespace config {
std::expected<logger::loggers_settings, std::string> load_config_file(const std::filesystem::path &config_folder_path = "./config/");
}

#endif //VPN_CONFIG_H
