#include <iostream>
#include <vpn/config.h>
#include <vpn/logger.h>
#include <vpn/server.h>

int main(int argc, char** argv) {
    auto configs = config::load_config();
    if (not configs) {
        std::cerr << "Failed to load config files." << configs.error() << std::endl;
        return 1;
    }
    const auto& [logger_config, server_config] = configs.value();
    auto logger = logger::runtime::create(logger_config);
    if (not logger) {
        std::cerr << "Failed to create logger. Error: " << logger.error() <<  std::endl;
        return 1;
    }
    auto server = server::runtime::create(server_config, logger.value()->get());
    if (not server) {
        logger.value()->get().log("server", logger_levels::e_error, "Could not initialize server error: {}", server.error());
        return 1;
    }
    return server.value()->run();
}
