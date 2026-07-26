#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/system/error_code.hpp>

#include "network_types.h"

namespace tester {

namespace http = boost::beast::http;

struct http_response_read {
    unsigned status{};
    std::optional<std::uint64_t> content_length;
    std::optional<std::string> run_id;

    std::uint64_t body_bytes{};
    std::string captured_body;

    bool complete{};
    boost::system::error_code transport_error;
    boost::system::error_code parser_error;
};

boost::cobalt::promise<std::expected<std::size_t, std::string>> write_bytes(tls_stream& stream, std::string_view bytes);

boost::cobalt::promise<std::expected<http::request<http::string_body>, std::string>> read_http_request(tls_stream& stream);

boost::cobalt::promise<std::expected<http_response_read, std::string>> read_http_response(tls_stream& stream, std::size_t capture_limit);

} // namespace tester
