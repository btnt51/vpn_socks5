#include <iostream>

#include <boost/cobalt/run.hpp>
#include <boost/cobalt/task.hpp>
#include <boost/cobalt/this_thread.hpp>
#include "boost/asio/co_spawn.hpp"
#include "boost/asio/detached.hpp"
#include "server/config.h"
#include "server/logger.h"
#include "server/server.h"
#include "spdlog/async.h"


int main(int argc, char** argv) {
    spdlog::init_thread_pool(8192, 1);
    auto logger_config = config::load_logger_config_file("./config");
    if (not logger_config) {
        std::cerr << "Failed to load config file: " << logger_config.error() << std::endl;
        return 1;
    }
    logger::init_logger(logger_config.value());

    auto server_config = config::load_server_config_file("./config");
    if (not server_config) {
        std::cerr << "Failed to load config file: " << server_config.error() << std::endl;
        return 1;
    }
    auto exit_code = 0;
    {
        boost::asio::io_context io_service;
        boost::cobalt::this_thread::set_executor(io_service.get_executor());
        try {
            server::server server(boost::cobalt::this_thread::get_executor(), server_config.value());
            std::exception_ptr failure;
            boost::cobalt::spawn(boost::cobalt::this_thread::get_executor(),server.accept(),[&](std::exception_ptr ep) {
                failure = ep;
                if (ep) {
                    io_service.stop();
                }
            });

            io_service.run();

            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& ex) {
                    std::cerr << "part1.day02 failed: " << ex.what() << '\n';
                    exit_code = 1;
                }
            }
        } catch (const boost::system::system_error& error) {
            std::cerr << "Cannot start server: " << error.code() << ": " << error.what() << '\n';
            return 1;
        } catch (const std::exception& error) {
            std::cerr << "Invalid server configuration: " << error.what() << '\n';
            return 1;
        }
    }

    logger::shutdown_logger();
    return exit_code;
}
