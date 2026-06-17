#ifndef VPN_SERVER_H
#define VPN_SERVER_H
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/cobalt/detached.hpp>
#include <utility>

#include <boost/cobalt/task.hpp>

class session;

class server {
public:
    server(boost::asio::any_io_executor io_context)
        : io_context_(std::move(io_context)),
          acceptor_(io_context_) {
        sessions_.reserve(1024);
    }

    boost::cobalt::task<void> accept();

    void log_error_on_cancel(const boost::system::error_code & ec, std::string_view operation) const;

    void cancel();
private:
    boost::asio::any_io_executor io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<session>> sessions_;
    bool stopping_{false};
};


#endif //VPN_SERVER_H
