#include <iostream>

#include <boost/cobalt/run.hpp>
#include <boost/cobalt/task.hpp>
#include "boost/asio/co_spawn.hpp"
#include "boost/asio/detached.hpp"
#include "server/server.h"


int main(int argc, char** argv) {
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
            return 1;
        }
    }

    return 0;
}
