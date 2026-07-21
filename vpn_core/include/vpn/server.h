#ifndef VPN_SERVER_H
#define VPN_SERVER_H
#include <expected>
#include <list>
#include <memory>
#include <optional>
#include <string>

#include <boost/cobalt/task.hpp>
#include <boost/cobalt/io/acceptor.hpp>

#include <vpn/logger.h>

class session;
namespace server {
struct config {
    std::string address;
    std::uint16_t port;
};


class server {
public:
    server(boost::cobalt::executor io_context, const config& config, logger::logger& logger);

    ~server();

    boost::cobalt::task<void> accept();

    void cancel();
private:
    boost::cobalt::executor io_context_;
    std::optional<boost::cobalt::io::acceptor> acceptor_;
    std::list<std::shared_ptr<session>> sessions_;
    logger::logger& logger_;
    bool stopping_{false};
};

class runtime {
public:
    static std::expected<std::unique_ptr<runtime>, std::string> create(const config& config, logger::logger& logger);

    ~runtime() = default;

    int run();
    void stop() noexcept;

private:
    explicit runtime(const config& config, logger::logger& logger);

    boost::asio::io_context io_context_;
    logger::logger& logger_;
    server server_;
    std::exception_ptr failure_;
};
}


#endif //VPN_SERVER_H
