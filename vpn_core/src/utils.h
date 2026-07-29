#ifndef VPN_UTILS_H
#define VPN_UTILS_H

#include <cstdint>
#include <span>
#include <string>
#include <tuple>

#include <boost/asio/streambuf.hpp>
#include <boost/cobalt/io/read.hpp>
#include <boost/cobalt/io/endpoint.hpp>
#include <boost/cobalt/io/stream_socket.hpp>
#include <boost/cobalt/io/write.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/cobalt/result.hpp>

namespace asio_coro_utils {
inline std::string format_endpoint(const boost::cobalt::io::endpoint& endpoint) {
    if (auto ip = get_if<boost::cobalt::io::ip>(&endpoint)) {
        const auto addr = ip->addr_str();
        if (ip->is_ipv6()) {
            return "[" + std::string{addr.c_str()} + "]:" + std::to_string(ip->port());
        }
        return std::string{addr.c_str()} + ":" + std::to_string(ip->port());
    }

    return "unknown";
}

inline boost::cobalt::promise<std::tuple<boost::system::error_code, std::size_t>> help_socket_reader(boost::cobalt::io::stream_socket& socket, boost::asio::streambuf& buffer, std::size_t needs_to_read_bytes) {
    try {
        auto prepared = buffer.prepare(needs_to_read_bytes);
        auto [ec, bytes_read] = co_await boost::cobalt::as_tuple(socket.read_some(prepared));
        buffer.commit(bytes_read);
        co_return {ec, bytes_read};
    } catch (const std::exception& e) {
        boost::system::error_code ec = boost::asio::error::no_memory;
        co_return {ec, 0};
    }
}

inline boost::cobalt::promise<std::tuple<boost::system::error_code, std::size_t>> help_socket_reader_some(boost::cobalt::io::stream_socket& socket,
                                                                                    boost::asio::streambuf& buffer) {
    constexpr std::size_t relay_buffer_size = 32 * 1024;
    try {
        auto prepared = buffer.prepare(relay_buffer_size);
        auto [ec, bytes_read] = co_await boost::cobalt::as_tuple(socket.read_some(prepared));
        buffer.commit(bytes_read);
        co_return {ec, bytes_read};
    } catch (const std::exception& e) {
        boost::system::error_code ec = boost::asio::error::no_memory;
        co_return {ec, 0};
    }
}

inline boost::cobalt::promise<std::tuple<boost::system::error_code, std::size_t>> help_socket_writer(boost::cobalt::io::stream_socket& socket,std::span<const std::uint8_t> message) {
    co_return co_await boost::cobalt::as_tuple(boost::cobalt::io::write(socket, boost::cobalt::io::buffer(message)));
}

} // namespace asio_coro_utils

#endif //VPN_UTILS_H
