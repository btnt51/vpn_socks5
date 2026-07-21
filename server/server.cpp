#include "server.h"

#include <iostream>

#include "logger.h"
#include "session.h"

#include <boost/cobalt/spawn.hpp>

namespace server {
using namespace boost::cobalt::io;
server::server(boost::cobalt::executor io_context, const config& config) : io_context_(std::move(io_context)),
    acceptor_(std::in_place, endpoint{tcp, config.address, config.port}, io_context_) {
    g_logger.log("server", logger_levels::e_info,
                "Server acceptor started at ip: {}, port: {}", config.address, config.port);
    sessions_.reserve(1024);
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
            g_logger.log("server", logger_levels::e_warning,
                "Could not accept connection from acceptor with error: {}" ,error.message());
            continue;
        }
        auto session = std::make_shared<::session>(io_context_, std::move(socket));
        g_logger.log("server", logger_levels::e_info,
                "Accepting new session session id:{} username: {}", session->session_id(), session->username());
        boost::cobalt::spawn(io_context_, session->run(), [session](const std::exception_ptr &ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (std::exception& e) {
                    g_logger.log("session", logger_levels::e_warning,
                        "While session was running there was thrown an exception: {}", e.what());
                }
                session->cancel();
            }
        });
        sessions_.push_back(session);
    }
}

void server::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }
    g_logger.log("server", logger_levels::e_info, "Stopped server");
    acceptor_.reset();

    for (const auto& session : sessions_) {
        session->cancel();
    }
}
}
