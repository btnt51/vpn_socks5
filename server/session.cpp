#include "session.h"

#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/read.hpp>
#include <ranges>
#include <boost/asio/connect.hpp>

#include "logger.h"
#include "socks5.h"
#include "utils.h"
#include <boost/cobalt/io/socket.hpp>
#include <boost/cobalt/gather.hpp>
#include <boost/cobalt/io/endpoint.hpp>

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
    g_logger.log("session", logger_levels::e_info,
        "session id: {} username: {} starting relay", session_id_, username_);
    co_await boost::cobalt::gather(client_to_upstream(), upstream_to_client());
    co_return true;
}

boost::cobalt::promise<void> session::client_to_upstream() {
    for (;;) {
        if (stopping_) {
            co_return;
        }
        auto [ec, read_bytes] = co_await asio_coro_utils::help_socket_reader_some(client_connection_, client_buffer_);
        if (ec) {
            const auto level = is_expected_disconnect(ec) ? logger_levels::e_debug : logger_levels::e_warning;
            g_logger.log("session", level,
                "session id: {} username: {} error while reading from client (client_to_upstream): {}", session_id_, username_, ec.message());
            cancel();
            co_return;
        }
        auto [write_ec, bytes_written] = co_await boost::cobalt::as_tuple(boost::cobalt::io::write(upstream_connection_, client_buffer_.data()));
        client_buffer_.consume(bytes_written);

        if (write_ec) {
            const auto level = is_expected_disconnect(write_ec) ? logger_levels::e_debug : logger_levels::e_warning;
            g_logger.log("session", level,
                "session id: {} username: {} error while sending from client to upstream (client_to_upstream): {}", session_id_, username_, write_ec.message());
            cancel();
            co_return;
        }
    }
}

boost::cobalt::promise<void> session::upstream_to_client() {
    for (;;) {
        if (stopping_) {
            co_return;
        }
        auto [ec, read_bytes] = co_await asio_coro_utils::help_socket_reader_some(upstream_connection_, upstream_buffer_);
        if (ec) {
            const auto level = is_expected_disconnect(ec) ? logger_levels::e_debug : logger_levels::e_warning;
            g_logger.log("session", level,
                "session id: {} username: {} error while reading from upstream (upstream_to_client): {}", session_id_, username_, ec.message());
            cancel();
            co_return;
        }
        auto [write_ec, bytes_written] = co_await boost::cobalt::as_tuple(boost::cobalt::io::write(client_connection_, upstream_buffer_.data()));
        upstream_buffer_.consume(bytes_written);

        if (write_ec) {
            const auto level = is_expected_disconnect(write_ec) ? logger_levels::e_debug : logger_levels::e_warning;
            g_logger.log("session", level,
                "session id: {} username: {} error while sending from upstream to client (upstream_to_client): {}", session_id_, username_, write_ec.message());
            cancel();
            co_return;
        }
    }
}

bool session::is_expected_disconnect(const boost::system::error_code &ec) const {
    return ec == boost::asio::error::eof or (stopping_ and ec == boost::asio::error::operation_aborted);
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
    g_logger.log("session", logger_levels::e_info, "session id: {} username: {} previous state: {} session state: {}",
        session_id_, username_, session_state_to_string(state_), session_state_to_string(state));
    state_ = state;
}

void session::cancel() {
    if (std::exchange(stopping_, true)) {
        return;
    }

    auto previous_state = state_;
    set_session_state(session_states::e_closing);

    g_logger.log("session",logger_levels::e_debug,
        "session id: {} username: {} cancel requested while state={}", session_id_, username_, session_state_to_string(previous_state)
    );

    resolver_.cancel();
    close_socket("client", client_connection_);
    close_socket("upstream", upstream_connection_);
}

uint64_t session::session_id() const {
    return session_id_;
}

std::string session::username() const {
    return username_;
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
        if (auto remote = socket.remote_endpoint()) {
            endpoint = asio_coro_utils::format_endpoint(*remote);
        } else {
            log_close_error(socket_name, "remote_endpoint", endpoint, remote.error());
        }
    }
    g_logger.log("session", logger_levels::e_debug,
    "session id: {} username: {} closing socket: {} endpoint: {}", session_id_, username_, socket_name, endpoint);

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

    g_logger.log("session", expected ? logger_levels::e_debug : logger_levels::e_warning, "session id: {} username: {} socket: {} endpoint: {} operation: {} close error: {} ({})",
        session_id_, username_, socket_name, endpoint, operation, ec.value(), ec.message()
    );
}

boost::cobalt::promise<std::optional<socks5::auth_method>> session::negotiate_step() {
    auto ec = co_await ensure_bytes(2);
    if (ec) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} username: {} error while reading SOCKS5 negotiation header: {}", session_id_, username_, ec.message());
        co_return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(client_buffer_.size());
    boost::asio::buffer_copy(boost::asio::buffer(bytes), client_buffer_.data());

    auto request = socks5::parse_negotiation_req(std::span{bytes});

    if (request.status == socks5::parse_status::need_more) {
        ec = co_await ensure_bytes(request.required);
        if (ec) {
            g_logger.log("session", logger_levels::e_warning,
                "session id: {} username: {} error while reading SOCKS5 negotiation: {}", session_id_, username_, ec.message());
            cancel();
            co_return std::nullopt;
        }

        bytes.resize(client_buffer_.size());
        boost::asio::buffer_copy(boost::asio::buffer(bytes), client_buffer_.data());

        request = socks5::parse_negotiation_req(std::span{bytes});
    }

    if (request.status != socks5::parse_status::ok) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} username: {} invalid SOCKS5 negotiation request",
            session_id_, username_);
        cancel();
        co_return std::nullopt;
    }

    const auto method = socks5::choose_auth_method(request.request.methods);
    auto response = socks5::build_method_selection(method);

    client_buffer_.consume(request.consumed);

    auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);

    if (write_ec) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} username: {} error while sending SOCKS5 auth method: {}", session_id_, username_, write_ec.message());
        co_return std::nullopt;
    }

    if (method == socks5::auth_method::no_acceptable_methods) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} username: {} no acceptable SOCKS5 auth method", session_id_, username_);
        co_return std::nullopt;
    }

    co_return method;
}

boost::cobalt::promise<bool> session::authentication_step() {
    co_return true;
}

boost::cobalt::promise<void> session::handle_socks5_command_request_error(const socks5::parse_result<socks5::command_request>& request) {
    g_logger.log("session", logger_levels::e_warning,
                 "session id: {} username: {} invalid SOCKS5 command request",
                 session_id_, username_);
    if (not request.silent_exit) {
        auto response = socks5::build_failed_command_response(request.reply_code);
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} username: {} error while sending SOCKS5 command response: {}",
                         session_id_, username_, write_ec.message());
        }
    }
}

boost::cobalt::promise<std::optional<socks5::command_request>> session::get_command_request() {
    socks5::parse_result<socks5::command_request> request;
    g_logger.log("session", logger_levels::e_info,
        "session id: {} username: {} getting socks5 command request", session_id_, username_);
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
                         "session id: {} username: {}  error while reading SOCKS5 request: {}",
                         session_id_, username_, ec.message());
            co_return std::nullopt;
        }
    }
    if (request.status == socks5::parse_status::error) {
        co_await handle_socks5_command_request_error(request);
        co_return std::nullopt;
    }
    client_buffer_.consume(request.consumed);
    co_return request.request;
}

boost::cobalt::promise<bool> session::send_success_socks5_connect() {
    auto ep = upstream_connection_.local_endpoint();
    if (ep.has_error()) {
        g_logger.log("session", logger_levels::e_warning,
                         "session id: {} username: {} upstream connection doesn`t have local endpoint: {}",
                         session_id_, username_, ep.error().message());
        auto response = prepare_socks5_failed_command(ep.error());
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} username: {} error while sending failed SOCKS5 command response: {}",
                         session_id_, username_, write_ec.message());
        }
        cancel();
        co_return false;
    }
    auto local_endpoint = ep.value();
    if (auto ip = get_if<boost::cobalt::io::ip>(&local_endpoint)) {
        std::vector<uint8_t> response;
        auto addr = ip->addr();
        if (ip->is_ipv4()) {
            response = socks5::build_success_command_response(socks5::reply_code::succeeded, std::span(addr).last<4>(), ip->port());
        } else {
            response = socks5::build_success_command_response(socks5::reply_code::succeeded, std::span(addr), ip->port());
        }

        g_logger.log("session", logger_levels::e_info,
            "session id: {} username: {} sending socks5 succeeded command response failed reply code: {:02x}",
            session_id_, username_, static_cast<uint8_t>(socks5::reply_code::succeeded));

        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} username: {} error while sending success SOCKS5 command response: {}",
                         session_id_, username_, write_ec.message());
            cancel();
            co_return false;
        }
        co_return true;
    }
    g_logger.log("session", logger_levels::e_warning,
                 "session id: {} username: {} upstream connection doesn`t have local endpoint",
                 session_id_, username_);
    cancel();
    co_return false;
}

std::array<std::uint8_t, 10> session::prepare_socks5_failed_command(const boost::system::error_code& connect_ec) {
    auto reply_code = socks5::to_reply_code(connect_ec);
    auto response = socks5::build_failed_command_response(reply_code);
    g_logger.log("session", logger_levels::e_warning,
                 "session id: {} username: {} sending socks5 failed command response failed reply code: {:02x}",
                 session_id_, username_, static_cast<uint8_t>(reply_code));
    return response;
}

boost::cobalt::promise<bool> session::resolve_and_connect_to_remote(const socks5::domain_endpoint& domain_endpoint) {
    set_session_state(session_states::e_resolve);
    g_logger.log("session", logger_levels::e_info,
        "session id: {} username: {} trying to resolve domain_endpoint: {}",
        session_id_, username_, domain_endpoint.host);
    auto results = co_await resolve(domain_endpoint.host, std::to_string(domain_endpoint.port));
    if (not results) {
        auto err = results.error();
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} username: {} error while resolving domain_endpoint: {} error: {}",
            session_id_, username_, domain_endpoint.host, err.message());
        auto response = prepare_socks5_failed_command(err);
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} username: {} error while sending failed SOCKS5 command response: {}",
                         session_id_, username_, write_ec.message());
        }
        cancel();
        co_return false;
    }
    const auto& endpoints = results.value();
    auto [connect_ec, endpoint] = co_await boost::cobalt::as_tuple(upstream_connection_.connect(endpoints));

    if (connect_ec) {
        g_logger.log("session", logger_levels::e_warning,
            "session id: {} username: {} error while trying to connect domain_endpoint: {} error: {}",
            session_id_, username_, domain_endpoint.host, connect_ec.message());
        auto response = prepare_socks5_failed_command(connect_ec);
        auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
        if (write_ec) {
            g_logger.log("session", logger_levels::e_warning,
                         "session id: {} username: {} error while sending failed SOCKS5 command response: {}",
                         session_id_, username_, write_ec.message());
        }
        cancel();
        co_return false;
    }
    g_logger.log("session", logger_levels::e_info,
                 "session id: {} username: {} connected to remote endpoint: {}",
                 session_id_, username_, asio_coro_utils::format_endpoint(endpoint));

    co_return co_await send_success_socks5_connect();
}

boost::cobalt::promise<void> session::handle_error_while_connecting_to_remote(const boost::system::error_code& connect_ec) {
    g_logger.log("session", logger_levels::e_warning,
                 "session id: {} username: {} error while connecting to remote: {}",
                 session_id_, username_, connect_ec.message());
    auto response = prepare_socks5_failed_command(connect_ec);
    auto [write_ec, written] = co_await asio_coro_utils::help_socket_writer(client_connection_, response);
    if (write_ec) {
        g_logger.log("session", logger_levels::e_warning,
                     "session id: {} username: {} error while sending failed SOCKS5 command response: {}",
                     session_id_, username_, write_ec.message());
    }
    cancel();
}

boost::cobalt::promise<bool> session::connect_to_remote(const boost::cobalt::io::endpoint& endpoint) {
    const auto remote_endpoint = asio_coro_utils::format_endpoint(endpoint);
    g_logger.log("session", logger_levels::e_info,
        "session id: {} username: {} connecting to remote endpoint: {}",
        session_id_, username_, remote_endpoint);

    auto [connect_ec] = co_await boost::cobalt::as_tuple(upstream_connection_.connect(endpoint));
    if (connect_ec) {
        co_await handle_error_while_connecting_to_remote(connect_ec);
        co_return false;
    }
    g_logger.log("session", logger_levels::e_info,
        "session id: {} username: {} connected to remote endpoint: {}",
        session_id_, username_, remote_endpoint);

    co_return co_await send_success_socks5_connect();
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
            "session id: {} username: {} couldn`t resolve host: [domain: {}, port: {}] err: {}",
            session_id_, username_, host, port, err.message());
    co_return std::unexpected{err};
}

boost::cobalt::promise<bool> session::handshake() {
    auto method = co_await negotiate_step();
    if (not method) {
        co_return false;
    }
    g_logger.log("session", logger_levels::e_debug,
        "session id: {} username: {} selected SOCKS5 auth method: {}", session_id_, username_, socks5::to_string(method.value()));

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
