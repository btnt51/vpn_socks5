#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <vpn/socks5.h>

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

TEST(Socks5NegotiationParser, ReportsNeedMoreForMissingHeader) {
    std::vector<std::uint8_t> bytes;

    const auto result = socks5::parse_negotiation_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 2);
}

TEST(Socks5NegotiationParser, RejectsWrongProtocolVersion) {
    std::array<std::uint8_t, 3> bytes{0x04, 0x01, 0x00};

    const auto result = socks5::parse_negotiation_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::error);
    EXPECT_EQ(result.reply_code, socks5::reply_code::general_failure);
    EXPECT_TRUE(result.silent_exit);
}

TEST(Socks5NegotiationParser, RejectsGreetingWithoutMethods) {
    std::array<std::uint8_t, 2> bytes{0x05, 0x00};

    const auto result = socks5::parse_negotiation_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::error);
    EXPECT_TRUE(result.silent_exit);
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

TEST(Socks5ConnectionParser, ParsesIpv6ConnectRequest) {
    std::array<std::uint8_t, 22> bytes{
            0x05, 0x01, 0x00, 0x04,
            0x20, 0x01, 0x0d, 0xb8,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x01,
            0x01, 0xbb
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    ASSERT_EQ(result.status, socks5::parse_status::ok);
    EXPECT_EQ(result.consumed, bytes.size());
    EXPECT_EQ(result.request.command, socks5::command_request::command::connect);
    const auto* endpoint = std::get_if<boost::asio::ip::tcp::endpoint>(&result.request.target);
    ASSERT_NE(endpoint, nullptr);
    EXPECT_TRUE(endpoint->address().is_v6());
    EXPECT_EQ(endpoint->address().to_string(), "2001:db8::1");
    EXPECT_EQ(endpoint->port(), 443);
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

TEST(Socks5ConnectionParser, ReportsNeedMoreForIncompleteHeader) {
    std::array<std::uint8_t, 3> bytes{0x05, 0x01, 0x00};

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 4);
}

TEST(Socks5ConnectionParser, RejectsWrongProtocolVersion) {
    std::array<std::uint8_t, 10> bytes{
            0x04, 0x01, 0x00, 0x01,
            127, 0, 0, 1,
            0x04, 0x38
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::error);
    EXPECT_EQ(result.reply_code, socks5::reply_code::general_failure);
    EXPECT_TRUE(result.silent_exit);
}

TEST(Socks5ConnectionParser, RejectsNonZeroReservedByte) {
    std::array<std::uint8_t, 10> bytes{
            0x05, 0x01, 0x01, 0x01,
            127, 0, 0, 1,
            0x04, 0x38
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::error);
    EXPECT_EQ(result.reply_code, socks5::reply_code::general_failure);
}

TEST(Socks5ConnectionParser, RejectsUnknownAddressType) {
    std::array<std::uint8_t, 4> bytes{0x05, 0x01, 0x00, 0x02};

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::error);
    EXPECT_EQ(result.reply_code, socks5::reply_code::address_type_not_supported);
}

TEST(Socks5ConnectionParser, ReportsFullIpv4RequestSizeForTruncatedAddress) {
    std::array<std::uint8_t, 9> bytes{
            0x05, 0x01, 0x00, 0x01,
            127, 0, 0, 1,
            0x04
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 10);
}

TEST(Socks5ConnectionParser, ReportsDomainLengthByteAsRequired) {
    std::array<std::uint8_t, 4> bytes{0x05, 0x01, 0x00, 0x03};

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 5);
}

TEST(Socks5ConnectionParser, ReportsFullDomainRequestSizeForTruncatedPort) {
    std::vector<std::uint8_t> bytes{
            0x05, 0x01, 0x00, 0x03,
            0x03, 'v', 'p', 'n', 0x01
    };

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 10);
}

TEST(Socks5ConnectionParser, ReportsFullIpv6RequestSizeForTruncatedAddress) {
    std::array<std::uint8_t, 21> bytes{};
    bytes[0] = 0x05;
    bytes[1] = 0x01;
    bytes[3] = 0x04;

    const auto result = socks5::parse_connection_req(std::span{bytes});

    EXPECT_EQ(result.status, socks5::parse_status::need_more);
    EXPECT_EQ(result.required, 22);
}

TEST(Socks5CommandResponse, BuildsRfcCompatibleReplyShape) {
    const auto response = socks5::build_failed_command_response(socks5::reply_code::succeeded);

    EXPECT_EQ(response[0], 0x05);
    EXPECT_EQ(response[1], static_cast<std::uint8_t>(socks5::reply_code::succeeded));
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x01);
    EXPECT_EQ(response.size(), 10);
}

TEST(Socks5CommandResponse, BuildsIpv6SuccessReply) {
    std::array<std::uint8_t, 16> address{
            0x20, 0x01, 0x0d, 0xb8,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x01
    };

    const auto response = socks5::build_success_command_response(
            socks5::reply_code::succeeded,
            std::span<std::uint8_t, 16>{address},
            0x01bb);

    ASSERT_EQ(response.size(), 22);
    EXPECT_EQ(response[0], 0x05);
    EXPECT_EQ(response[1], static_cast<std::uint8_t>(socks5::reply_code::succeeded));
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x04);
    EXPECT_TRUE(std::equal(address.begin(), address.end(), response.begin() + 4));
    EXPECT_EQ(response[20], 0x01);
    EXPECT_EQ(response[21], 0xbb);
}

}  // namespace
