#include "http_io.h"

#include <algorithm>
#include <array>
#include <charconv>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/cobalt/op.hpp>

#include <fmt/format.h>

namespace tester {

namespace {

constexpr std::size_t io_buffer_size = 16 * 1024;

std::string error_message(std::string_view operation, const boost::system::error_code& error) {
    return fmt::format("{}: {}", operation, error.message());
}

void capture_body(http_response_read& response, const boost::beast::flat_buffer& buffer, const std::size_t capture_limit) {
    const auto bytes = boost::asio::buffer_size(buffer.data());
    response.body_bytes += bytes;

    const auto remaining_capture = capture_limit > response.captured_body.size() ? capture_limit - response.captured_body.size() : 0;
    const auto to_capture = std::min(bytes, remaining_capture);

    if (to_capture == 0) {
        return;
    }

    const auto old_size = response.captured_body.size();
    response.captured_body.resize(old_size + to_capture);
    boost::asio::buffer_copy(boost::asio::buffer(response.captured_body.data() + old_size, to_capture), buffer.data(), to_capture);
}

} // namespace

boost::cobalt::promise<std::expected<std::size_t, std::string>> write_bytes(tls_stream& stream, const std::string_view bytes) {
    auto [error, written] = co_await boost::asio::async_write(stream, boost::asio::buffer(bytes), boost::asio::as_tuple(boost::cobalt::use_op));

    if (error) {
        co_return std::unexpected{error_message("write", error)};
    }

    co_return written;
}

boost::cobalt::promise<std::expected<http::request<http::string_body>, std::string>> read_http_request(tls_stream& stream) {
    boost::beast::flat_buffer buffer;
    http::request<http::string_body> request;
    auto [read_error, bytes_read] = co_await http::async_read(stream, buffer, request, boost::asio::as_tuple(boost::cobalt::use_op));

    if (read_error) {
        co_return std::unexpected{error_message("read HTTP request", read_error)};
    }

    co_return request;
}

boost::cobalt::promise<std::expected<http_response_read, std::string>> read_http_response(tls_stream& stream, const std::size_t capture_limit) {
    boost::beast::flat_buffer buffer;
    http::response_parser<http::empty_body> parser;
    parser.skip(true);

    auto [read_error, bytes_read] = co_await http::async_read_header(stream, buffer, parser, boost::asio::as_tuple(boost::cobalt::use_op));
    if (read_error) {
        co_return std::unexpected{error_message("read HTTP response headers", read_error)};
    }

    http_response_read response;
    const auto& message = parser.get();
    response.status = message.result_int();
    const auto content_length = message.find(http::field::content_length);

    if (content_length != message.end()) {
        std::uint64_t parsed_content_length{};
        const auto value = content_length->value();
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(),parsed_content_length);
        if (error != std::errc{} or  end != value.data() + value.size()) {
            co_return std::unexpected{"invalid HTTP Content-Length"};
        }
        response.content_length = parsed_content_length;
    }
    if (const auto run_id = message.find("X-Tester-Run-Id"); run_id != message.end()) {
        response.run_id = std::string{run_id->value()};
    }

    capture_body(response, buffer, capture_limit);
    buffer.consume(buffer.size());

    const auto expected_body_size = response.content_length.value_or(0);
    if (response.body_bytes >= expected_body_size) {
        response.complete = true;
        co_return response;
    }

    std::array<char, io_buffer_size> body_buffer{};

    while (response.body_bytes < expected_body_size) {
        auto [read_error, bytes_read] = co_await stream.async_read_some(boost::asio::buffer(body_buffer), boost::asio::as_tuple(boost::cobalt::use_op));

        response.body_bytes += bytes_read;

        const auto remaining_capture = capture_limit > response.captured_body.size() ? capture_limit - response.captured_body.size() : 0;
        const auto to_capture = std::min(bytes_read, remaining_capture);

        response.captured_body.append(body_buffer.data(), to_capture);

        if (read_error) {
            response.transport_error = read_error;
            break;
        }
    }

    response.complete = response.body_bytes >= expected_body_size;

    co_return response;
}

} // namespace tester
