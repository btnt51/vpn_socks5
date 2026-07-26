#include "client.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/race.hpp>
#include <boost/variant2/variant.hpp>
#include <fmt/format.h>

#include "http_io.h"

namespace tester {

namespace {

constexpr std::uint8_t socks_version = 0x05;

termination_reason classify_transport_error(const boost::system::error_code& error) {
    if (not error) {
        return termination_reason::completed;
    }
    if (error == boost::asio::error::connection_reset) {
        return termination_reason::connection_reset;
    }
    if (error == boost::asio::ssl::error::stream_truncated) {
        return termination_reason::stream_truncated;
    }
    if (error == boost::asio::error::eof) {
        return termination_reason::partial_http_message;
    }
    return termination_reason::io_error;
}

template <std::size_t Size>
boost::cobalt::promise<std::expected<void, std::string>> write_message(socket_type& socket, const std::array<std::uint8_t, Size>& message) {
    auto [error, bytes_written] = co_await boost::asio::async_write(socket, boost::asio::buffer(message), boost::asio::as_tuple(boost::cobalt::use_op));

    if (error) {
        co_return std::unexpected{"SOCKS5 write: " + error.message()};
    }

    if (bytes_written != message.size()) {
        co_return std::unexpected{"short SOCKS5 write"};
    }

    co_return {};
}

template <std::size_t Size>
boost::cobalt::promise<std::expected<void, std::string>> read_message(socket_type& socket,std::array<std::uint8_t, Size>& message) {
    auto [error, bytes_read] = co_await boost::asio::async_read(socket, boost::asio::buffer(message), boost::asio::as_tuple(boost::cobalt::use_op));

    if (error) {
        co_return std::unexpected{"SOCKS5 read: " + error.message()};
    }

    if (bytes_read != message.size()) {
        co_return std::unexpected{"short SOCKS5 read"};
    }

    co_return {};
}

} // namespace

client::client(client_context context) : context_{std::move(context)}, socket_{context_.executor},
    result_{.run_id = context_.run_id, .scenario_name = context_.selected_scenario.name,} { }

boost::cobalt::promise<client_result> client::run() {
    const auto started_at = std::chrono::steady_clock::now();
    const auto session_deadline = started_at + context_.selected_scenario.session_timeout;

    auto connect_timeout = std::min<std::chrono::steady_clock::duration>(
        context_.selected_scenario.step_timeout, session_deadline - std::chrono::steady_clock::now());
    auto connect_result = co_await wait_with_timeout(connect_proxy(), connect_timeout, "proxy connection timeout");
    if (not connect_result) {
        result_.failed_step = 0;
        result_.error = connect_result.error();
        result_.latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at);
        co_return result_;
    }

    for (current_step_ = 0; current_step_ < context_.selected_scenario.client_steps.size(); ++current_step_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= session_deadline) {
            result_.failed_step = current_step_;
            result_.termination = termination_reason::timeout;
            result_.error = "session timeout";
            result_.latency = std::chrono::duration_cast<std::chrono::microseconds>(now - started_at);
            close_transport();
            co_return result_;
        }

        const auto remaining_session = session_deadline - now;
        const auto timeout = std::min<std::chrono::steady_clock::duration>(context_.selected_scenario.step_timeout, remaining_session);
        const auto session_timeout_first = remaining_session <= context_.selected_scenario.step_timeout;
        auto operation = std::visit([this](const auto& step) {
            return execute(step);
        }, context_.selected_scenario.client_steps[current_step_]);
        auto step_result = co_await wait_with_timeout(std::move(operation), timeout,
            session_timeout_first ? "session timeout" : "step timeout");

        if (not step_result) {
            if (result_.termination == termination_reason::completed) {
                result_.termination = termination_reason::protocol_error;
            }
            result_.failed_step = current_step_;
            result_.error = step_result.error();
            result_.latency =std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at);
            co_return result_;
        }
    }

    result_.success = true;
    result_.failed_step = context_.selected_scenario.client_steps.size();
    result_.latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at);

    co_return result_;
}

boost::cobalt::promise<client::step_result> client::connect_proxy() {
    boost::system::error_code address_error;
    const auto proxy_ip = boost::asio::ip::make_address(context_.proxy.host, address_error);
    if (address_error) {
        result_.termination = termination_reason::io_error;
        co_return std::unexpected{"parse proxy address: " + address_error.message()};
    }

    if (context_.source_address) {
        if (context_.source_address->is_v4() != proxy_ip.is_v4()) {
            result_.termination = termination_reason::io_error;
            co_return std::unexpected{fmt::format("source address {} and proxy address {} use different address families",
                context_.source_address->to_string(),proxy_ip.to_string())};
        }

        boost::system::error_code open_error;
        socket_.open(proxy_ip.is_v4() ? boost::asio::ip::tcp::v4() : boost::asio::ip::tcp::v6(),open_error);
        if (open_error) {
            result_.termination = termination_reason::io_error;
            co_return std::unexpected{"open proxy socket: " + open_error.message()};
        }

        boost::system::error_code bind_error;
        socket_.bind(boost::asio::ip::tcp::endpoint{*context_.source_address,0},bind_error);
        if (bind_error) {
            result_.termination = bind_error == boost::system::errc::address_not_available ?
                termination_reason::resource_exhausted : termination_reason::io_error;
            co_return std::unexpected{fmt::format("bind source address {}: {}",
                context_.source_address->to_string(),bind_error.message())};
        }
    }

    auto [connect_error] = co_await socket_.async_connect(boost::asio::ip::tcp::endpoint{proxy_ip, context_.proxy.port}, boost::asio::as_tuple(boost::cobalt::use_op));
    if (connect_error) {
        result_.termination = connect_error == boost::system::errc::address_not_available ?
            termination_reason::resource_exhausted : termination_reason::io_error;
        co_return std::unexpected{context_.source_address ?
            fmt::format("connect to proxy from {}: {}",context_.source_address->to_string(),connect_error.message()) :
            "connect to proxy: " + connect_error.message()};
    }

    co_return {};
}

boost::cobalt::promise<void> client::wait_timeout(const std::chrono::steady_clock::duration timeout) {
    boost::asio::basic_waitable_timer<std::chrono::steady_clock, boost::asio::wait_traits<std::chrono::steady_clock>, executor_type>
        timer{context_.executor, timeout};
    auto [error] = co_await timer.async_wait(boost::asio::as_tuple(boost::cobalt::use_op));
    if (error and error != boost::asio::error::operation_aborted) {
        throw boost::system::system_error{error};
    }
}

boost::cobalt::promise<client::step_result> client::wait_with_timeout(boost::cobalt::promise<step_result> operation,
    const std::chrono::steady_clock::duration timeout, std::string timeout_error) {

    if (timeout <= std::chrono::steady_clock::duration::zero()) {
        result_.termination = termination_reason::timeout;
        close_transport();
        co_return std::unexpected{std::move(timeout_error)};
    }

    auto outcome = co_await boost::cobalt::race(std::move(operation), wait_timeout(timeout));
    if (outcome.index() == 1) {
        result_.termination = termination_reason::timeout;
        close_transport();
        co_return std::unexpected{std::move(timeout_error)};
    }
    co_return boost::variant2::get<0>(std::move(outcome));
}

void client::close_transport() {
    boost::system::error_code error;
    if (tls_stream_) {
        tls_stream_->next_layer().cancel(error);
        tls_stream_->next_layer().close(error);
    } else {
        socket_.cancel(error);
        socket_.close(error);
    }
}

boost::cobalt::promise<client::step_result> client::execute(const socks5_negotiate& step) {
    if (step.methods.size() > 255) {
        co_return std::unexpected{"too many SOCKS5 authentication methods"};
    }

    std::string request;
    request.reserve(step.methods.size() + 2);
    request.push_back(static_cast<char>(socks_version));
    request.push_back(static_cast<char>(step.methods.size()));
    for (const auto method : step.methods) {
        request.push_back(static_cast<char>(method));
    }

    auto [write_error, bytes_written] = co_await boost::asio::async_write(socket_, boost::asio::buffer(request), boost::asio::as_tuple(boost::cobalt::use_op));
    if (write_error) {
        co_return std::unexpected{"write SOCKS5 negotiation: " + write_error.message()};
    }
    result_.bytes_sent += bytes_written;

    std::array<std::uint8_t, 2> response{};
    auto read_result = co_await read_message(socket_, response);

    if (not read_result) {
        co_return std::unexpected{read_result.error()};
    }

    result_.bytes_received += response.size();
    if (response[0] != socks_version or response[1] != step.expected_method) {
        co_return std::unexpected{fmt::format("unexpected SOCKS5 negotiation response: version={} method={}", response[0], response[1])};
    }

    co_return {};
}

boost::cobalt::promise<client::step_result> client::execute(const socks5_connect& step) {
    const auto port = context_.endpoint.port;
    const std::array<std::uint8_t, 10> request{socks_version,
        0x01, 0x00, 0x01, 127, 0, 0, 1, static_cast<std::uint8_t>((port >> 8) & 0xff), static_cast<std::uint8_t>(port & 0xff)};

    auto write_result = co_await write_message(socket_, request);
    if (not write_result) { co_return std::unexpected{write_result.error()}; }
    result_.bytes_sent += request.size();

    std::array<std::uint8_t, 10> response{};
    auto read_result = co_await read_message(socket_, response);
    if (not read_result) {
        co_return std::unexpected{read_result.error()};
    }
    result_.bytes_received += response.size();

    if (response[0] != socks_version or response[1] != step.expected_reply) {
        co_return std::unexpected{fmt::format("unexpected SOCKS5 CONNECT response: version={} reply={}", response[0], response[1])};
    }

    co_return {};
}

boost::cobalt::promise<client::step_result> client::execute(const tls_handshake&) {
    tls_stream_.emplace(std::move(socket_), context_.tls_context);
    auto [handshake_error] = co_await tls_stream_->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::as_tuple(boost::cobalt::use_op));

    if (handshake_error) {
        result_.termination = termination_reason::io_error;
        co_return std::unexpected{"TLS handshake: " + handshake_error.message()};
    }

    co_return {};
}

boost::cobalt::promise<client::step_result> client::execute(const send_http_request& step) {
    if (not tls_stream_) {
        co_return std::unexpected{"HTTP request requires an established TLS stream"};
    }

    const auto method = http::string_to_verb(step.method);
    if (method == http::verb::unknown) {
        co_return std::unexpected{"unknown HTTP method: " + step.method};
    }

    http::request<http::string_body> request{method, step.target, 11};
    request.set(http::field::host, fmt::format("{}:{}", context_.endpoint.host, context_.endpoint.port));
    request.set("X-Tester-Scenario", context_.selected_scenario.name);
    request.set("X-Tester-Run-Id", context_.run_id);
    request.keep_alive(false);
    request.body() = step.body;
    request.prepare_payload();

    auto [write_error, bytes_written] = co_await http::async_write(*tls_stream_, request, boost::asio::as_tuple(boost::cobalt::use_op));
    if (write_error) {
        result_.termination = termination_reason::io_error;
        co_return std::unexpected{"write HTTP request: " + write_error.message()};
    }

    result_.bytes_sent += bytes_written;
    co_return {};
}

boost::cobalt::promise<client::step_result> client::execute(const expect_http_response& step) {
    if (not tls_stream_) {
        co_return std::unexpected{"HTTP response requires an established TLS stream"};
    }

    auto response_result = co_await read_http_response(*tls_stream_, step.body.size());
    if (not response_result) {
        result_.termination = termination_reason::protocol_error;
        co_return std::unexpected{response_result.error()};
    }

    const auto& response = response_result.value();
    result_.bytes_received += response.body_bytes;
    result_.termination = classify_transport_error(response.transport_error);

    if (response.run_id != context_.run_id) {
        co_return std::unexpected{"HTTP response run_id does not match the client"};
    }
    if (response.status != step.status) {
        co_return std::unexpected{fmt::format("unexpected HTTP status: {}", response.status)};
    }
    if (not response.complete) {
        co_return std::unexpected{"HTTP response was not completed"};
    }
    if (response.captured_body != step.body) {
        co_return std::unexpected{"HTTP response body does not match"};
    }

    auto [shutdown_error] = co_await tls_stream_->async_shutdown(boost::asio::as_tuple(boost::cobalt::use_op));
    if (shutdown_error and shutdown_error != boost::asio::error::eof) {
        result_.termination = termination_reason::io_error;
        co_return std::unexpected{"TLS shutdown: " + shutdown_error.message()};
    }
    boost::system::error_code close_error;
    tls_stream_->next_layer().close(close_error);

    result_.termination = termination_reason::completed;
    co_return {};
}

boost::cobalt::promise<client::step_result>client::execute(const expect_incomplete_response& step) {
    if (not tls_stream_) {
        co_return std::unexpected{"HTTP response requires an established TLS stream"};
    }

    auto response_result = co_await read_http_response(*tls_stream_, 0);
    if (not response_result) {
        result_.termination = termination_reason::protocol_error;
        co_return std::unexpected{response_result.error()};
    }

    const auto& response = response_result.value();
    result_.bytes_received += response.body_bytes;
    result_.termination = classify_transport_error(response.transport_error);

    if (response.run_id != context_.run_id) {
        co_return std::unexpected{"HTTP response run_id does not match the client"};
    }
    if (not response.content_length or *response.content_length != step.declared_size) {
        co_return std::unexpected{"unexpected download Content-Length"};
    }
    if (response.complete) {
        co_return std::unexpected{"download unexpectedly completed"};
    }
    if (response.body_bytes < step.minimum_received or response.body_bytes > step.maximum_received) {
        co_return std::unexpected{fmt::format("received {} bytes, expected range [{}, {}]",
            response.body_bytes, step.minimum_received, step.maximum_received)};
    }

    co_return {};
}

boost::cobalt::promise<client::step_result> client::execute(const expect_complete_download& step) {
    if (not tls_stream_) {
        co_return std::unexpected{"HTTP response requires an established TLS stream"};
    }

    auto response_result = co_await read_http_response(*tls_stream_, 0);
    if (not response_result) {
        result_.termination = termination_reason::protocol_error;
        co_return std::unexpected{response_result.error()};
    }

    const auto& response = response_result.value();
    result_.bytes_received += response.body_bytes;
    result_.termination = classify_transport_error(response.transport_error);

    if (response.run_id != context_.run_id) {
        co_return std::unexpected{"HTTP response run_id does not match the client"};
    }
    if (response.status != step.status) {
        co_return std::unexpected{fmt::format("unexpected HTTP status: {}", response.status)};
    }
    if (not response.content_length or *response.content_length != step.declared_size) {
        co_return std::unexpected{"unexpected download Content-Length"};
    }
    if (not response.complete or response.body_bytes != step.declared_size) {
        co_return std::unexpected{fmt::format("received {} bytes, expected {}", response.body_bytes, step.declared_size)};
    }

    auto [shutdown_error] = co_await tls_stream_->async_shutdown(boost::asio::as_tuple(boost::cobalt::use_op));
    if (shutdown_error and shutdown_error != boost::asio::error::eof) {
        result_.termination = termination_reason::io_error;
        co_return std::unexpected{"TLS shutdown: " + shutdown_error.message()};
    }
    boost::system::error_code close_error;
    tls_stream_->next_layer().close(close_error);

    result_.termination = termination_reason::completed;
    co_return {};
}

} // namespace tester
