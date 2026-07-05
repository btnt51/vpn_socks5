#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "server/socks5.h"

namespace {

TEST(Socks5NegotiationParser, ParsesNoAuthGreeting) {
    std::array<std::uint8_t, 3> bytes{0x05, 0x01, 0x00};

    const auto result = socks5::parse_negotiation_req(std::span{bytes});

    ASSERT_EQ(result.status, socks5::parse_status::ok);
    EXPECT_EQ(result.consumed, bytes.size());
    ASSERT_EQ(result.request.methods.size(), 1);
    EXPECT_EQ(result.request.methods.front(), static_cast<std::uint8_t>(socks5::auth_method::no_auth));
}

TEST(Socks5NegotiationParser, ReportsNeedMoreForPartialGreeting) {
    std::array<std::uint8_t, 3> bytes{0x05, 0x02, 0x00};

    const auto result = socks5::parse_negotiation_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 4);
}

TEST(Socks5NegotiationParser, ChoosesNoAuthWhenOffered) {
    std::array<std::uint8_t, 2> methods{0x02, 0x00};

    const auto method = socks5::choose_auth_method(std::span{methods});

    EXPECT_EQ(method, socks5::auth_method::no_auth);
}

TEST(Socks5NegotiationParser, RejectsUnsupportedMethods) {
    std::array<std::uint8_t, 1> methods{0x02};

    const auto method = socks5::choose_auth_method(std::span{methods});

    EXPECT_EQ(method, socks5::auth_method::no_acceptable_methods);
}

TEST(Socks5ConnectionParser, ParsesIpv4ConnectRequest) {
    std::array<std::uint8_t, 10> bytes{
            0x05, 0x01, 0x00, 0x01,
            127, 0, 0, 1,
            0x04, 0x38
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    ASSERT_EQ(result.status, socks5::parse_status::ok);
    EXPECT_EQ(result.consumed, bytes.size());
    EXPECT_EQ(result.request.command, socks5::command_request::command::connect);
    const auto* endpoint = std::get_if<boost::asio::ip::tcp::endpoint>(&result.request.target);
    ASSERT_NE(endpoint, nullptr);
    EXPECT_EQ(endpoint->address().to_string(), "127.0.0.1");
    EXPECT_EQ(endpoint->port(), 1080);
}

TEST(Socks5ConnectionParser, ParsesDomainConnectRequest) {
    std::vector<std::uint8_t> bytes{
            0x05, 0x01, 0x00, 0x03,
            0x0b,
            'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
            0x01, 0xbb
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    ASSERT_EQ(result.status, socks5::parse_status::ok);
    EXPECT_EQ(result.consumed, bytes.size());
    EXPECT_EQ(result.request.command, socks5::command_request::command::connect);
    const auto* endpoint = std::get_if<socks5::domain_endpoint>(&result.request.target);
    ASSERT_NE(endpoint, nullptr);
    EXPECT_EQ(endpoint->host, "example.com");
    EXPECT_EQ(endpoint->port, 443);
}

TEST(Socks5ConnectionParser, RejectsUnsupportedCommand) {
    std::array<std::uint8_t, 10> bytes{
            0x05, 0x02, 0x00, 0x01,
            127, 0, 0, 1,
            0x04, 0x38
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::error);
    EXPECT_EQ(result.reply_code, socks5::reply_code::command_not_supported);
}

TEST(Socks5CommandResponse, BuildsRfcCompatibleReplyShape) {
    const auto response = socks5::build_failed_command_response(socks5::reply_code::succeeded);

    EXPECT_EQ(response[0], 0x05);
    EXPECT_EQ(response[1], static_cast<std::uint8_t>(socks5::reply_code::succeeded));
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x01);
    EXPECT_EQ(response.size(), 10);
}

} // namespace
