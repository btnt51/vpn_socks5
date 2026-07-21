#ifndef VPN_SERVER_H
#define VPN_SERVER_H
#include <atomic>
#include <expected>
#include <list>
#include <memory>
#include <optional>
#include <string>

#include <boost/cobalt/task.hpp>
#include <boost/cobalt/io/acceptor.hpp>

#include <vpn/logger.h>

namespace session{
struct statistics;
class session;
}
namespace server {
struct config {
    std::string address;
    std::uint16_t port;
};

struct statistics {
    std::atomic_uint64_t socks_rx{};
    std::atomic_uint64_t socks_tx{};
    std::atomic_uint64_t bytes_client_to_upstream{};
    std::atomic_uint64_t bytes_upstream_to_client{};
    std::atomic_uint64_t total_created_sessions{};
};

class server {
public:
    server(boost::cobalt::executor io_context, const config& config, logger::logger& logger);

    ~server();

    boost::cobalt::task<void> accept();

    void cancel();
private:
    void append_session_statistic(const session::statistics& statistics);

    void final_stat_log();

    boost::cobalt::executor io_context_;
    std::optional<boost::cobalt::io::acceptor> acceptor_;
    std::list<std::shared_ptr<session::session>> sessions_;
    logger::logger& logger_;
    statistics completed_server_statistics_;
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

template<>
struct fmt::formatter<::server::statistics> {
    constexpr auto parse(fmt::format_parse_context& context) {
        return context.begin();
    }

    template<typename FormatContext>
    auto format(const ::server::statistics& stats, FormatContext& context) const {
        return fmt::format_to(context.out(), "socks_rx={} socks_tx={} bytes_client_to_upstream={} bytes_upstream_to_client={} total_created_sessions={}",
            stats.socks_rx.load(), stats.socks_tx.load(), stats.bytes_client_to_upstream.load(), stats.bytes_upstream_to_client.load(), stats.total_created_sessions.load()
        );
    }
};


#endif //VPN_SERVER_H
