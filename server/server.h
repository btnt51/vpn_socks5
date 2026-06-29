#ifndef VPN_SERVER_H
#define VPN_SERVER_H
#include <optional>

#include <boost/asio/ip/tcp.hpp>

#include <boost/cobalt/task.hpp>

#include "boost/cobalt/io/acceptor.hpp"

class session;

class server {
public:
    server(boost::asio::any_io_executor io_context);

    ~server();

    boost::cobalt::task<void> accept();

    void cancel();
private:
    boost::asio::any_io_executor io_context_;
    std::optional<boost::cobalt::io::acceptor> acceptor_;
    std::vector<std::shared_ptr<session>> sessions_;
    bool stopping_{false};
};


#endif //VPN_SERVER_H
