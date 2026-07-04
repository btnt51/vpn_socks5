#include <iostream>

#include <boost/cobalt/run.hpp>
#include <boost/cobalt/task.hpp>
#include <boost/cobalt/this_thread.hpp>
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
        boost::cobalt::this_thread::set_executor(io_service.get_executor());

        server server(boost::cobalt::this_thread::get_executor());
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
    }

    shutdown_logger();
    return exit_code;
}
