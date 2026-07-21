#ifndef VPN_SESSION_H
#define VPN_SESSION_H
#include <atomic>
#include <expected>
#include <memory>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/cobalt/task.hpp>
#include <boost/cobalt/io/resolver.hpp>
#include <boost/cobalt/io/stream_socket.hpp>

#include <vpn/socks5.h>
#include <vpn/logger.h>

namespace session {
enum class session_states {
    e_none = 0,
    e_started = 1,
    e_handshake = 2,
    e_request = 3,
    e_resolve = 4,
    e_active = 5,
    e_closing = 6,
    e_closed = 7,
    e_error = 8,
};

std::string_view session_state_to_string(session_states state);

inline std::uint64_t generate_id() noexcept {
    static std::atomic_uint64_t next_id{1};
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

struct statistics {
    std::uint64_t socks_rx{};
    std::uint64_t socks_tx{};
    std::uint64_t bytes_client_to_upstream{};
    std::uint64_t bytes_upstream_to_client{};
};

class session : public std::enable_shared_from_this<session> {
public:
    session(boost::asio::any_io_executor io_executor, boost::cobalt::io::stream_socket&& socket, logger::logger& logger)
        : executor_{std::move(io_executor)}, client_connection_{std::move(socket)}, upstream_connection_{executor_},
          resolver_{executor_}, logger_{logger}, session_id_{generate_id()} {
        set_session_state(session_states::e_started);
    }

    boost::cobalt::task<void> run();
    void cancel();
    uint64_t session_id() const;
    std::string username() const;
    statistics statistics() const;
private:
    boost::cobalt::promise<bool> handshake();

    boost::cobalt::promise<std::optional<socks5::auth_method>> negotiate_step();
    boost::cobalt::promise<bool> authentication_step();

    boost::cobalt::promise<void> handle_socks5_command_request_error(const socks5::parse_result<socks5::command_request>& request);

    boost::cobalt::promise<std::optional<socks5::command_request>> get_command_request();

    boost::cobalt::promise<bool> send_success_socks5_connect();

    std::array<std::uint8_t, 10> prepare_socks5_failed_command(const boost::system::error_code& connect_ec);

    boost::cobalt::promise<bool> resolve_and_connect_to_remote(const socks5::domain_endpoint& domain_endpoint);

    boost::cobalt::promise<void> handle_error_while_connecting_to_remote(const boost::system::error_code& connect_ec);

    boost::cobalt::promise<bool> connect_to_remote(const boost::cobalt::io::endpoint& endpoint);

    boost::cobalt::promise<bool> request();
    boost::cobalt::promise<std::expected<boost::cobalt::io::endpoint_sequence, boost::system::error_code>> resolve(std::string_view host, std::string_view port);
    boost::cobalt::promise<boost::system::error_code> ensure_bytes(std::size_t full_request_size);

    void close_socket(std::string_view socket_name, boost::asio::ip::tcp::socket& socket) const;
    void close_socket(std::string_view socket_name, boost::cobalt::io::stream_socket& socket) const;
    void log_close_error(std::string_view socket_name, std::string_view operation, std::string_view endpoint, const boost::system::error_code& ec) const;
    void set_session_state(session_states state);

    boost::cobalt::promise<bool> relay();

    boost::cobalt::promise<void> client_to_upstream();
    boost::cobalt::promise<void> upstream_to_client();

    boost::cobalt::promise<boost::system::error_code> send_socks_message(std::span<const std::uint8_t> message);

    bool is_expected_disconnect(const boost::system::error_code& ec) const;

    boost::cobalt::executor executor_;
    boost::cobalt::io::stream_socket client_connection_;
    boost::cobalt::io::stream_socket upstream_connection_;
    boost::cobalt::io::resolver resolver_;
    boost::asio::streambuf client_buffer_;
    boost::asio::streambuf upstream_buffer_;
    logger::logger& logger_;
    std::string username_{"unknown"};
    struct statistics session_stats_{};
    uint64_t session_id_;
    session_states state_{session_states::e_none};
    bool stopping_{false};
};
}

template<>
struct fmt::formatter<::session::statistics> {
    constexpr auto parse(fmt::format_parse_context& context) {
        return context.begin();
    }

    template<typename FormatContext>
    auto format(const ::session::statistics& stats, FormatContext& context) const {
        return fmt::format_to(context.out(), "socks_rx={} socks_tx={} bytes_client_to_upstream={} bytes_upstream_to_client={}",
            stats.socks_rx, stats.socks_tx, stats.bytes_client_to_upstream, stats.bytes_upstream_to_client
        );
    }
};

#endif //VPN_SESSION_H
