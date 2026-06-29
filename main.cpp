#include <iostream>

#include <boost/cobalt/run.hpp>
#include <boost/cobalt/task.hpp>
#include "boost/asio/co_spawn.hpp"
#include "boost/asio/detached.hpp"
#include "server/logger.h"
#include "server/server.h"
#include "spdlog/async.h"


int main(int argc, char** argv) {
    spdlog::init_thread_pool(8192, 1);
    init_logger({"server", "session"});

    auto exit_code = 0;
    {
        boost::asio::io_context io_service;
        server server(io_service.get_executor());
        std::exception_ptr failure;
        boost::cobalt::spawn(
            io_service,
            server.accept(),
            boost::asio::detached);

        io_service.run();

        if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& ex) {
                std::cerr << "part1.day02 failed: " << ex.what() << '\n';
                exit_code = 1;
            }
        }
    }

    shutdown_logger();
    return exit_code;
}
