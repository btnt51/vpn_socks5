#include <vpn/server.h>

#include <iostream>

#include <vpn/logger.h>
#include "session.h"

#include <boost/cobalt/spawn.hpp>

namespace server {
using namespace boost::cobalt::io;
server::server(boost::cobalt::executor io_context, const config& config, logger::logger& logger) : io_context_(std::move(io_context)),
    acceptor_(std::in_place, endpoint{tcp, config.address, config.port}, io_context_), logger_(logger) {
    logger_.log("server", logger_levels::e_info,
                "Server acceptor started at ip: {}, port: {}", config.address, config.port);
}

server::~server() {
    cancel();
}

boost::cobalt::task<void>server::accept() {
    while (not stopping_) {
        auto [error, socket] = co_await boost::cobalt::as_tuple(acceptor_->accept());
        if (stopping_)
            break;
        if (error) {
            logger_.log("server", logger_levels::e_warning,
                "Could not accept connection from acceptor with error: {}" ,error.message());
            continue;
        }
        auto session = std::make_shared<::session::session>(io_context_, std::move(socket), logger_);
        auto it = sessions_.insert(sessions_.end(), session);
        logger_.log("server", logger_levels::e_info,
                "Accepting new session session id: {} username: {}", session->session_id(), session->username());
        boost::cobalt::spawn(io_context_, session->run(), [this, session, it](const std::exception_ptr &ep) mutable {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (std::exception& e) {
                    logger_.log("session", logger_levels::e_warning,
                        "While session [session id: {} username: {}] was running there was thrown an exception: {}",
                        session->session_id(), session->username(), e.what());
                }
                session->cancel();
            }
            logger_.log("session", logger_levels::e_info, "Session [session id: {} username: {}] statistic: [{}]",
                        session->session_id(), session->username(), session->statistics());
            sessions_.erase(it);
        });
    }
}

void server::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }
    logger_.log("server", logger_levels::e_info, "Stopped server");
    acceptor_.reset();

    for (const auto& session : sessions_) {
        session->cancel();
    }
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

    if (!failure_) {
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
