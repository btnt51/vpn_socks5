#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <boost/asio/ip/address.hpp>
#include <boost/cobalt/promise.hpp>

#include "network_types.h"
#include "results.h"
#include "scenario.h"
#include "target_endpoint.h"

namespace tester {

struct proxy_address {
    std::string host;
    std::uint16_t port{};
};

struct client_context {
    std::string run_id;
    const scenario& selected_scenario;

    proxy_address proxy;
    endpoint_address endpoint;
    std::optional<boost::asio::ip::address> source_address;

    boost::cobalt::executor executor;
    boost::asio::ssl::context& tls_context;
};

class client {
public:
    explicit client(client_context context);

    boost::cobalt::promise<client_result> run();

private:
    using step_result = std::expected<void, std::string>;

    boost::cobalt::promise<step_result> connect_proxy();
    boost::cobalt::promise<void> wait_timeout(std::chrono::steady_clock::duration timeout);
    boost::cobalt::promise<step_result> wait_with_timeout(boost::cobalt::promise<step_result> operation,
        std::chrono::steady_clock::duration timeout, std::string timeout_error);
    void close_transport();

    boost::cobalt::promise<step_result> execute(const socks5_negotiate& step);

    boost::cobalt::promise<step_result> execute(const socks5_connect& step);

    boost::cobalt::promise<step_result> execute(const tls_handshake& step);

    boost::cobalt::promise<step_result> execute(const send_http_request& step);

    boost::cobalt::promise<step_result> execute(const expect_http_response& step);

    boost::cobalt::promise<step_result> execute(const expect_incomplete_response& step);

    boost::cobalt::promise<step_result> execute(const expect_complete_download& step);

    client_context context_;
    std::size_t current_step_{};

    socket_type socket_;
    std::optional<tls_stream> tls_stream_;

    client_result result_;
};

} // namespace tester
