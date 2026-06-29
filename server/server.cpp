#include "server.h"

#include "logger.h"
#include "session.h"

#include <boost/cobalt/spawn.hpp>


server::server(boost::asio::any_io_executor io_context)
    : io_context_(std::move(io_context)),
      acceptor_(
          std::in_place,
          boost::cobalt::io::endpoint{
              boost::asio::ip::tcp::endpoint{boost::asio::ip::tcp::v4(), 1080}
          },
          io_context_
      ) {
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

        auto session = std::make_shared<class session>(io_context_, std::move(socket));
        boost::cobalt::spawn(io_context_, session->run(), [session](std::exception_ptr ep) {
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

void server::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }

    acceptor_.reset();

    for (const auto& session : sessions_) {
        session->cancel();
    }
}
