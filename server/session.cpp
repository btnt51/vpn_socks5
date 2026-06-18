#include "session.h"

#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/read.hpp>
#include <ranges>
#include <boost/asio/connect.hpp>

#include "logger.h"
#include "socks5.h"
#include "utils.h"
#include "boost/cobalt/io/socket.hpp"
#include "boost/cobalt/io/endpoint.hpp"

std::string_view session_state_to_string(session_states state) {
    using std::string_view_literals::operator ""sv;
    switch (state) {
        case session_states::e_none:
            return "none"sv;
        case session_states::e_started:
            return "started"sv;
        case session_states::e_handshake:
            return "handshake"sv;
        case session_states::e_request:
            return "request"sv;
        case session_states::e_resolve:
            return "resolve"sv;
        case session_states::e_active:
            return "active"sv;
        case session_states::e_closing:
            return "closing"sv;
        case session_states::e_closed:
            return "closed"sv;
        case session_states::e_error:
            return "error"sv;
        default:
            return "unknown"sv;
    }
}

boost::cobalt::promise<bool> session::relay() {
    co_return true;
}

boost::cobalt::task<void> session::run() {
    auto self = shared_from_this();
    self->set_session_state(session_states::e_handshake);
    auto res = co_await handshake();
    if (not res) {
        cancel();
        co_return;
    }
    self->set_session_state(session_states::e_request);
    res = co_await request();
    if (not res) {
        cancel();
        co_return;
    }


    self->set_session_state(session_states::e_active);
    res = co_await relay();
    if (not res) {
        cancel();
        co_return;
    }
}

void session::set_session_state(session_states state) {
    g_logger.log("session", logger_levels::e_info, "session id: {}  previous state: {} session state: {}",
        session_id_, session_state_to_string(state_), session_state_to_string(state));
    state_ = state;
}

void session::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }

    auto previous_state = state_;
    set_session_state(session_states::e_closing);

    g_logger.log("session",logger_levels::e_debug,
        "session id: {} cancel requested while state={}", session_id_, session_state_to_string(previous_state)
    );

    resolver_.cancel();
    close_socket("client", client_connection_);
    close_socket("upstream", upstream_connection_);
}

void session::close_socket(std::string_view socket_name, boost::asio::ip::tcp::socket &socket) const {
    boost::system::error_code ec;

    auto endpoint = std::string{"unknown"};
    if (socket.is_open()) {
        auto ep = socket.remote_endpoint(ec);
        if (!ec) {
            endpoint = fmt::format("{}:{}", ep.address().to_string(), ep.port());
        }
    }

    socket.cancel(ec);
    log_close_error(socket_name, "cancel", endpoint, ec);

    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    log_close_error(socket_name, "shutdown", endpoint, ec);

    socket.close(ec);
    log_close_error(socket_name, "close", endpoint, ec);
}

void session::close_socket(std::string_view socket_name, boost::cobalt::io::stream_socket &socket) const {
    auto endpoint = std::string{"unknown"};

    if (socket.is_open()) {
        auto remote = socket.remote_endpoint();

        if (remote) {
            if (auto ip = get_if<boost::cobalt::io::ip>(&*remote)) {
                auto addr = ip->addr_str();

                if (ip->is_ipv6()) {
                    endpoint = fmt::format("[{}]:{}", addr.c_str(), ip->port());
                } else {
                    endpoint = fmt::format("{}:{}", addr.c_str(), ip->port());
                }
            }
        } else {
            log_close_error(socket_name, "remote_endpoint", endpoint, remote.error());
        }
    }
    g_logger.log("session", logger_levels::e_debug,
    "session id: {} closing socket: {} endpoint: {}", session_id_, socket_name, endpoint);

    if (auto cancel_result = socket.cancel(); not cancel_result) {
        log_close_error(socket_name, "cancel", endpoint, cancel_result.error());
    }

    ;
    if (auto shutdown_result = socket.shutdown(); not shutdown_result) {
        log_close_error(socket_name, "shutdown", endpoint, shutdown_result.error());
    }


    if (auto close_result = socket.close(); not close_result) {
        log_close_error(socket_name, "close", endpoint, close_result.error());
    }
}

void session::log_close_error(std::string_view socket_name, std::string_view operation, std::string_view endpoint, const boost::system::error_code& ec) const {
    if (!ec) {
        return;
    }

    const bool expected = ec == boost::asio::error::bad_descriptor or ec == boost::asio::error::not_connected or ec == boost::asio::error::operation_aborted;

    g_logger.log("session", expected ? logger_levels::e_debug : logger_levels::e_warning, "session id: {} socket: {} endpoint: {} operation: {} close error: {} ({})",
        session_id_, socket_name, endpoint, operation, ec.value(), ec.message()
    );
}

boost::cobalt::promise<std::optional<socks5::auth_method>> session::negotiate_step() {
    auto ec = co_await ensure_bytes(2);
    if (ec) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} error while reading SOCKS5 negotiation header: {}", session_id_, ec.message());
        co_return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(client_buffer_.size());
    boost::asio::buffer_copy(boost::asio::buffer(bytes), client_buffer_.data());

    auto request = socks5::parse_negotiation_req(std::span{bytes});

    if (request.status == socks5::parse_status::need_more) {
        ec = co_await ensure_bytes(request.required);
        if (ec) {
            g_logger.log("session", logger_levels::e_warning,
                "session id: {} error while reading SOCKS5 negotiation: {}", session_id_, ec.message());
            cancel();
            co_return std::nullopt;
        }

        bytes.resize(client_buffer_.size());
        boost::asio::buffer_copy(boost::asio::buffer(bytes), client_buffer_.data());

        request = socks5::parse_negotiation_req(std::span{bytes});
    }

    if (request.status != socks5::parse_status::ok) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} invalid SOCKS5 negotiation request",
            session_id_);
        cancel();
        co_return std::nullopt;
    }

    const auto method = socks5::choose_auth_method(request.request.methods);
    auto response = socks5::build_method_selection(method);

    client_buffer_.consume(request.consumed);

    auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);

    if (write_ec) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} error while sending SOCKS5 auth method: {}", session_id_, write_ec.message());
        co_return std::nullopt;
    }

    if (method == socks5::auth_method::no_acceptable_methods) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} no acceptable SOCKS5 auth method", session_id_);
        co_return std::nullopt;
    }

    g_logger.log("session", logger_levels::e_debug,
        "session id: {} selected SOCKS5 auth method: {}", session_id_, static_cast<std::uint8_t>(method));

    co_return method;
}

boost::cobalt::promise<bool> session::authentication_step() {
    co_return true;
}

boost::cobalt::promise<std::optional<socks5::command_request>> session::get_command_request() {
    socks5::parse_result<socks5::command_request> request;
    while (true) {
        std::vector<std::uint8_t> bytes(client_buffer_.size());
        boost::asio::buffer_copy(boost::asio::buffer(bytes), client_buffer_.data());
        request = socks5::parse_connection_req(std::span{bytes});
        if (request.status != socks5::parse_status::need_more) {
            break;
        }
        auto ec = co_await ensure_bytes(request.required);
        if (ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} error while reading SOCKS5 request: {}",
                         session_id_, ec.message());
            co_return std::nullopt;
        }
    }
    if (request.status == socks5::parse_status::error) {
        g_logger.log("session", logger_levels::e_warning,
                     "session id: {} invalid SOCKS5 command request",
                     session_id_);
        if (not request.silent_exit) {
            auto response = socks5::build_failed_command_response(request.reply_code);
            auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
            if (write_ec) {
                g_logger.log("session", logger_levels::e_warning,
                             "session id: {} error while sending SOCKS5 command response: {}",
                             session_id_, write_ec.message());
            }
        }
        co_return std::nullopt;
    }
    client_buffer_.consume(request.consumed);
    co_return request.request;
}

boost::cobalt::promise<bool> session::resolve_and_connect_to_remote(const socks5::domain_endpoint& domain_endpoint) {
    set_session_state(session_states::e_resolve);
    auto results = co_await resolve(domain_endpoint.host, std::to_string(domain_endpoint.port));
    if (auto err = results.error(); not results) {
        auto response = socks5::build_failed_command_response(socks5::to_reply_code(err));
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} error while sending SOCKS5 command response: {}",
                         session_id_, write_ec.message());
        }
        cancel();
        co_return false;
    }
    auto endpoints = results.value();
    auto [connect_ec, endpoint] = co_await boost::cobalt::as_tuple(upstream_connection_.connect(endpoints));

    if (connect_ec) {
        auto response = socks5::build_failed_command_response(socks5::to_reply_code(connect_ec));
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} error while sending SOCKS5 command response: {}",
                         session_id_, write_ec.message());
        }
        cancel();
        co_return false;
    }
    co_return false;
}

boost::cobalt::promise<bool> session::connect_to_remote(const boost::cobalt::io::endpoint& endpoint) {
    auto [connect_ec] = co_await boost::cobalt::as_tuple(upstream_connection_.connect(endpoint));
    if (connect_ec) {
        g_logger.log("session", logger_levels::e_warning,
                         "session id: {} error while connecting to remote: {}",
                         session_id_, connect_ec.message());
        auto response = socks5::build_failed_command_response(socks5::to_reply_code(connect_ec));
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} error while sending SOCKS5 command response: {}",
                         session_id_, write_ec.message());
        }
        cancel();
        co_return false;
    }
    co_return true;
}

boost::cobalt::promise<bool> session::request() {
    auto request = co_await get_command_request();
    if (not request) {
        cancel();
        co_return false;
    }
    if (auto* domain_endpoint = std::get_if<socks5::domain_endpoint>(&request.value().target)) {
        co_return co_await resolve_and_connect_to_remote(*domain_endpoint);
    }
    if (auto* endpoint = std::get_if<boost::asio::ip::tcp::endpoint>(&request.value().target)) {
        co_return co_await connect_to_remote(*endpoint);
    }
    co_return true;
}

boost::cobalt::promise<std::expected<boost::cobalt::io::endpoint_sequence, boost::system::error_code>> session::resolve(std::string_view host, std::string_view port) {
    auto [err, endpoints] = co_await boost::cobalt::as_tuple(resolver_.resolve(host, port));
    if (not err) {
        co_return endpoints;
    }
    g_logger.log("session", logger_levels::e_warning,
            "session id: {} couldn`r resolve host: [domain: {}, port: {}] err: {}",
            session_id_, host, port, err.message());
    co_return std::unexpected{err};
}

boost::cobalt::promise<bool> session::handshake() {
    auto method = co_await negotiate_step();
    if (not method) {
        co_return false;
    }

    switch (method.value()) {
        case socks5::auth_method::no_auth:
            co_return true;

        case socks5::auth_method::username_password:
            co_return co_await authentication_step();

        case socks5::auth_method::no_acceptable_methods:
            co_return false;
    }

    co_return false;
}


boost::cobalt::promise<boost::system::error_code> session::ensure_bytes(const std::size_t full_request_size) {
    if (client_buffer_.size() >= full_request_size)
        co_return {};

    const auto missing = full_request_size - client_buffer_.size();

    auto [ec, n] = co_await asio_coro_utils::help_socket_reader(client_connection_,
        client_buffer_, missing);

    co_return ec;
}
