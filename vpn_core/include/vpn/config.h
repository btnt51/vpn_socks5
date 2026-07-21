#ifndef VPN_CONFIG_H
#define VPN_CONFIG_H
#include <expected>
#include <filesystem>
#include <vpn/logger.h>
#include <vpn/server.h>

namespace config {
struct main_config {
    logger::loggers_settings loggers;
    server::config server;
};
std::expected<config::main_config, std::string> load_config(const std::filesystem::path &config_folder_path = "./config/");
}

#endif //VPN_CONFIG_H
