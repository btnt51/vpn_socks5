#ifndef VPN_SERVER_H
#define VPN_SERVER_H
#include <optional>
#include <boost/cobalt/task.hpp>
#include <boost/cobalt/io/acceptor.hpp>

class session;

namespace server {
struct config {
    std::string address;
    std::uint16_t port;
};


class server {
public:
    server(boost::cobalt::executor io_context, const config& config);

    ~server();

    boost::cobalt::task<void> accept();

    void cancel();
private:
    boost::cobalt::executor io_context_;
    std::optional<boost::cobalt::io::acceptor> acceptor_;
    std::vector<std::shared_ptr<session>> sessions_;
    bool stopping_{false};
};
}


#endif //VPN_SERVER_H
