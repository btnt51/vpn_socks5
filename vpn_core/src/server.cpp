#include <vpn/server.h>

#include <iostream>

#include <vpn/logger.h>
#include "session.h"

#include <boost/cobalt/spawn.hpp>
#include "../include/vpn/worker_pool.h"

namespace server {
using namespace boost::cobalt::io;
server::server(boost::cobalt::executor io_context, const config& config, logger::logger& logger) : io_context_(std::move(io_context)),
    acceptor_(std::in_place, endpoint{tcp, config.address, config.port}, io_context_), logger_(logger), worker_pool_(config.threads, logger_) {
    logger_.log("server", logger_levels::e_info,
                "Server acceptor started at ip: {}, port: {}", config.address, config.port);
}

server::~server() {
    cancel();
}

void server::final_stat_log() {
    if (stopping_) {
        logger_.log("server", logger_levels::e_info, "Server stopped total statistic: {}", completed_server_statistics_);
    }
}

void server::append_workers_statistics() {
    auto temp_stat = worker_pool_.get_statistics();
    completed_server_statistics_.socks_rx.fetch_add(temp_stat.socks_rx, std::memory_order_relaxed);
    completed_server_statistics_.socks_tx.fetch_add(temp_stat.socks_tx, std::memory_order_relaxed);
    completed_server_statistics_.bytes_client_to_upstream.fetch_add(temp_stat.bytes_client_to_upstream, std::memory_order_relaxed);
    completed_server_statistics_.bytes_upstream_to_client.fetch_add(temp_stat.bytes_upstream_to_client, std::memory_order_relaxed);
    completed_server_statistics_.total_created_sessions.fetch_add(temp_stat.amount_of_sessions, std::memory_order_relaxed);
}

boost::cobalt::task<void>server::accept() {
    std::size_t cycle = 0;
    while (not stopping_) {
        worker* selected_worker = &worker_pool_.get_next_worker();
        boost::cobalt::io::stream_socket socket{selected_worker->get_executor()};
        auto [error] = co_await boost::cobalt::as_tuple(acceptor_->accept(socket));
        if (error) {
            selected_worker->release_session();
            logger_.log("server", logger_levels::e_warning,
                "Could not accept connection from acceptor with error: {}" ,error.message());
            if (not stopping_) {
                continue;
            } else {
                break;
            }
        }
        boost::cobalt::spawn(selected_worker->get_executor(), selected_worker->run_session(std::move(socket)),
        [this, selected_worker](std::exception_ptr error) {
            selected_worker->release_session();
            if (not error) {
                return;
            }

            try {
                std::rethrow_exception(error);
            } catch (const std::exception& exception) {
                logger_.log("worker", logger_levels::e_error,
                    "Unhandled exception in worker #{}: {}", selected_worker->get_id(), exception.what());
            } catch (...) {
                logger_.log("worker", logger_levels::e_error, "Unknown exception in worker #{}", selected_worker->get_id());
            }
        });
    }
}

void server::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }
    acceptor_.reset();
    append_workers_statistics();
    worker_pool_.stop();
    final_stat_log();
}

std::expected<std::unique_ptr<runtime>, std::string> runtime::create(const config& config, logger::logger& logger) {
    try {
        return std::unique_ptr<runtime>{new runtime{config, logger}};
    } catch (const boost::system::system_error& error) {
        return std::unexpected{error.what()};
    } catch (const std::exception& error) {
        return std::unexpected{error.what()};
    }
}

int runtime::run() {
    boost::cobalt::spawn(io_context_.get_executor(),server_.accept(), [this](const std::exception_ptr &error) {
        failure_ = error;

        if (error) {
            io_context_.stop();
        }
    });

    io_context_.run();

    if (not failure_) {
        return 0;
    }

    try {
        std::rethrow_exception(failure_);
    } catch (const std::exception& error) {
        logger_.log("server", logger_levels::e_error, "Server stopped with error: {}", error.what());
    }

    return 1;
}

void runtime::stop() noexcept {
    logger_.log("server", logger_levels::e_info, "Server stopping");
    server_.cancel();
    logger_.log("server", logger_levels::e_info, "Server stopped");
}

namespace {
boost::cobalt::executor prepare_executor(boost::asio::io_context& context) {
    auto executor = context.get_executor();
    boost::cobalt::this_thread::set_executor(executor);
    return executor;
}
}

runtime::runtime(const config& config, logger::logger& logger) : io_context_{}, logger_{logger},
        server_{prepare_executor(io_context_), config, logger_} {}
}
