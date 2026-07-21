#include <iostream>
#include <vpn/config.h>
#include <vpn/logger.h>
#include <vpn/server.h>

int main(int argc, char** argv) {
    auto configs = config::load_config();
    if (not configs) {
        std::cerr << "Failed to load config files." << configs.error() << std::endl;
    }
    const auto& [logger_config, server_config] = configs.value();
    logger::runtime runtime_logger(logger_config);
    auto server = server::runtime::create(server_config, runtime_logger.get());
    if (not server) {
        runtime_logger.get().log("server", logger_levels::e_error, "Could not initialize server error: {}", server.error());
        return 1;
    }
    return server.value()->run();
}
