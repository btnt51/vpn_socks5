#ifndef VPN_SOCKS5_H
#define VPN_SOCKS5_H
#include <array>
#include <cstdint>
#include <string>
#include <span>
#include <variant>
#include <vector>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>


namespace socks5 {
enum class parse_status {
    need_more,
    ok,
    error,
};

enum class reply_code : std::uint8_t {
    succeeded = 0x00,
    general_failure = 0x01,
    not_allowed = 0x02,
    network_unreachable = 0x03,
    host_unreachable = 0x04,
    connection_refused = 0x05,
    ttl_expired = 0x06,
    command_not_supported = 0x07,
    address_type_not_supported = 0x08,
};

enum class connection_error {
    resolve_failed,
    connection_refused,
    network_unreachable,
    host_unreachable,
    timeout,
    ruleset_denied,
    unsupported_command,
    unsupported_address_type,
    io_error,
    internal_error
};

reply_code to_reply_code(const boost::system::error_code &ec);
reply_code to_reply_code(connection_error err);

enum class auth_method : std::uint8_t {
    no_auth = 0x00,
    username_password = 0x02,
    no_acceptable_methods = 0xFF,
};

constexpr std::string_view to_string(auth_method method) {
    switch (method) {
        case auth_method::no_auth:
            return "no_auth";
        case auth_method::username_password:
            return "username_password";
        case auth_method::no_acceptable_methods:
            return "no_acceptable_methods";
        default:
            return "unknown";
    }
}


struct negotiation_req {
    std::span<uint8_t> methods;
};

struct auth_req {
    std::string username;
    std::string password;
};

struct domain_endpoint {
    domain_endpoint(const std::string& name, std::uint16_t port) : host{name}, port{port} {};
    std::string host;
    std::uint16_t port;
};

using target = std::variant<boost::asio::ip::tcp::endpoint, domain_endpoint>;

struct command_request {
    enum class command {
        connect = 0x01,
        bind = 0x02,
        udp_associate = 0x03,
    };

    command command;
    target target;
};

template <typename T>
struct parse_result {
    T request{};
    std::size_t consumed = 0;
    std::size_t required = 0;
    parse_status status{parse_status::ok};
    reply_code reply_code{reply_code::succeeded};
    bool silent_exit{false};
};

parse_result<negotiation_req> parse_negotiation_req(std::span<uint8_t> req);
auth_method choose_auth_method(std::span<const std::uint8_t> client_methods);
std::array<std::uint8_t, 2> build_method_selection(auth_method method);

parse_result<auth_req> parse_auth_req(std::span<uint8_t> req);
parse_result<command_request> parse_connection_req(std::span<uint8_t> req);

std::array<std::uint8_t, 10> build_failed_command_response(reply_code reply_code);
std::vector<std::uint8_t> build_success_command_response(reply_code reply_code, const std::span<std::uint8_t, 4>& ip_v4_addr, uint16_t port);
std::vector<std::uint8_t> build_success_command_response(reply_code reply_code, const std::span<std::uint8_t, 16>& ip_v6_addr, uint16_t port);
}


#endif //VPN_SOCKS5_H
