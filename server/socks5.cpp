#include "socks5.h"

#include <algorithm>
#include <cstring>

socks5::reply_code socks5::to_reply_code(const boost::system::error_code &ec) {
    using boost::asio::error::connection_refused;
    using boost::asio::error::host_unreachable;
    using boost::asio::error::network_unreachable;

    if (ec == connection_refused)
        return reply_code::connection_refused;

    if (ec == host_unreachable)
        return reply_code::host_unreachable;

    if (ec == network_unreachable)
        return reply_code::network_unreachable;

    return reply_code::general_failure;
}

socks5::reply_code socks5::to_reply_code(connection_error err) {
    switch (err) {
        case connection_error::ruleset_denied:
            return reply_code::not_allowed;

        case connection_error::network_unreachable:
            return reply_code::network_unreachable;

        case connection_error::host_unreachable:
        case connection_error::resolve_failed:
            return reply_code::host_unreachable;

        case connection_error::connection_refused:
            return reply_code::connection_refused;

        case connection_error::unsupported_command:
            return reply_code::command_not_supported;

        case connection_error::unsupported_address_type:
            return reply_code::address_type_not_supported;

        case connection_error::timeout:
        case connection_error::io_error:
        case connection_error::internal_error:
            return reply_code::general_failure;
    }

    return reply_code::general_failure;
}

socks5::parse_result<socks5::negotiation_req> socks5::parse_negotiation_req(std::span<uint8_t> req) {
    parse_result<socks5::negotiation_req> res;
    if (req.size() < 2) {
        res.status = parse_status::need_more;
        res.required = 2;
        return res;
    }
    auto version = req[0];
    if (version != 0x05) {
        res.status = parse_status::error;
        res.reply_code = reply_code::general_failure;
        res.silent_exit = true;
        return res;
    }
    auto nmethods = req[1];
    if (nmethods == 0x00) {
        res.status = parse_status::error;
        res.reply_code = reply_code::general_failure;
        res.silent_exit = true;
        return res;
    }
    if (req.size() < static_cast<std::size_t>(nmethods) + 2) {
        res.required = static_cast<std::size_t>(nmethods) + 2;
        res.status = parse_status::need_more;
        return res;
    }
    res.request.methods = req.subspan(2, static_cast<std::size_t>(nmethods));
    res.consumed = 2 + static_cast<std::size_t>(nmethods);
    res.reply_code = reply_code::succeeded;
    res.status = parse_status::ok;
    return res;
}

socks5::auth_method socks5::choose_auth_method(std::span<const std::uint8_t> client_methods) {
    if (std::ranges::contains(client_methods, static_cast<std::uint8_t>(auth_method::no_auth))) {
        return auth_method::no_auth;
    }

    return auth_method::no_acceptable_methods;
}

std::array<std::uint8_t, 2> socks5::build_method_selection(auth_method method) {
    return {0x05, static_cast<std::uint8_t>(method)};
}

socks5::parse_result<socks5::auth_req> socks5::parse_auth_req(std::span<uint8_t> req) {
    parse_result<socks5::auth_req> res;

    return res;
}

socks5::parse_result<socks5::command_request> socks5::parse_connection_req(std::span<uint8_t> req) {
    parse_result<socks5::command_request> res;
    if (req.size() < 4) {
        res.status = parse_status::need_more;
        res.required = 4;
        return res;
    }
    if (req[0] != 0x05) {
        res.status = parse_status::error;
        res.reply_code = reply_code::general_failure;
        res.silent_exit = true;
        return res;
    }
    res.request.command = static_cast<enum socks5::command_request::command>(req[1]);
    if (res.request.command != socks5::command_request::command::connect) {
        res.status = parse_status::error;
        res.reply_code = reply_code::command_not_supported;
        return res;
    }
    if (req[2] != 0x00) {
        res.status = parse_status::error;
        res.reply_code = reply_code::general_failure;
        return res;
    }

    auto atyp = req[3];
    switch (atyp) {
    case 0x01: {
        if (req.size() < 10) {
            res.required = 10;
            res.status = parse_status::need_more;
            break;
        }

        std::array<std::uint8_t, 4> addr;
        std::memcpy(addr.data(), req.data()+4, 4);
        std::uint16_t port = (static_cast<std::uint16_t>(req[8]) << 8) | static_cast<std::uint16_t>(req[9]);

        res.request.target.emplace<boost::asio::ip::tcp::endpoint>(boost::asio::ip::address_v4{addr}, port);
        res.reply_code = reply_code::succeeded;
        res.consumed = 10;
        res.status = parse_status::ok;
        return res;
    }
    case 0x03: {
        if (req.size() < 5) {
            res.status = parse_status::need_more;
            res.required = 5;
            break;
        }
        auto domain_length = req[4];
        if (req.size() < 5 + domain_length +2) {
            res.status = parse_status::need_more;
            res.required = 5 + domain_length + 2;
            break;
        }

        std::string domain{reinterpret_cast<const char*>(req.data() + 5),domain_length};
        auto port_offset = 5 + domain_length;
        std::uint16_t port = (static_cast<std::uint16_t>(req[port_offset]) << 8) | static_cast<std::uint16_t>(req[port_offset + 1]);

        res.request.target.emplace<socks5::domain_endpoint>(std::move(domain), port);
        res.consumed = 5 + domain_length + 2;
        res.reply_code = reply_code::succeeded;
        res.status = parse_status::ok;
        return res;
    }
    case 0x04: {
        if (req.size() < 22) {
            res.required = 22;
            res.status = parse_status::need_more;
            break;
        }
        std::array<std::uint8_t, 16> addr;
        std::memcpy(addr.data(), req.data() + 4, 16);
        std::uint16_t port = (static_cast<std::uint16_t>(req[20]) << 8) | static_cast<std::uint16_t>(req[21]);

        res.request.target.emplace<boost::asio::ip::tcp::endpoint>(boost::asio::ip::address_v6{addr}, port);
        res.reply_code = reply_code::succeeded;
        res.consumed = 22;
        res.status = parse_status::ok;
        return res;
    }
    default:
        res.status = parse_status::error;
        res.reply_code = reply_code::address_type_not_supported;
        return res;
    }
    return res;
}

std::array<std::uint8_t, 10> socks5::build_failed_command_response(reply_code reply_code) {
    return {0x05, static_cast<std::uint8_t>(reply_code),
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> socks5::build_success_command_response(reply_code reply_code,
    const std::span<std::uint8_t, 4> &ip_v4_addr, uint16_t port) {

    std::vector<std::uint8_t> res(10);
    res[0] = 0x05;
    res[1] = static_cast<std::uint8_t>(reply_code);
    res[2] = 0x00;
    res[3] = 0x01;
    std::ranges::copy(ip_v4_addr, res.begin() + 4);
    res[8] = static_cast<std::uint8_t>(port >> 8);
    res[9] = static_cast<std::uint8_t>(port);
    return res;
}

std::vector<std::uint8_t> socks5::build_success_command_response(reply_code reply_code,
    const std::span<std::uint8_t, 16> &ip_v6_addr, uint16_t port) {
    std::vector<std::uint8_t> res(22);
    res[0] = 0x05;
    res[1] = static_cast<std::uint8_t>(reply_code);
    res[2] = 0x00;
    res[3] = 0x04;
    std::ranges::copy(ip_v6_addr, res.begin() + 4);
    res[20] = static_cast<std::uint8_t>(port >> 8);
    res[21] = static_cast<std::uint8_t>(port);
    return res;
}
