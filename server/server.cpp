#include "server.h"

#include "logger.h"
#include "session.h"

#include <boost/asio/detached.hpp>
#include <boost/cobalt/spawn.hpp>


boost::cobalt::task<void>server::accept() {
    std::exception_ptr failure;
    const auto endpoint = boost::asio::ip::tcp::endpoint{boost::asio::ip::tcp::v4(), 1080};
    acceptor_.open(endpoint.protocol());
    acceptor_.bind(endpoint);
    acceptor_.listen();
    while (acceptor_.is_open() and not stopping_) {
        auto [error, socket] = co_await boost::cobalt::as_tuple(acceptor_.async_accept(boost::cobalt::use_op));
        if (stopping_)
            break;
        if (error) {
            g_logger.log("server", logger_levels::e_warning,
                "Could not accept connection from acceptor with error: {}" ,error.message());
            continue;
        }
        auto exec = co_await boost::cobalt::this_coro::executor;
        auto session = std::make_shared<class session>(exec, std::move(socket));
        boost::cobalt::spawn(exec, session->run(), [session](std::exception_ptr ep) {
            if (ep) {
                // log crash
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

void server::log_error_on_cancel(const boost::system::error_code &ec, std::string_view operation) const {
    if (!ec) {
        return;
    }

    const bool expected = ec == boost::asio::error::bad_descriptor or ec == boost::asio::error::not_connected or ec == boost::asio::error::operation_aborted;

    g_logger.log("server", expected ? logger_levels::e_debug : logger_levels::e_warning,
        "error while canceling accept operation: {} error code: {} error message: {}",
        operation, ec.value(), ec.message());
}

void server::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }

    boost::system::error_code ec;
    acceptor_.cancel(ec);
    log_error_on_cancel(ec, "cancel");

    acceptor_.close(ec);
    log_error_on_cancel(ec, "close");

    for (const auto& session : sessions_) {
        session->cancel();
    }
}
