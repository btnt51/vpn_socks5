#ifndef VPN_UTILS_H
#define VPN_UTILS_H

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/cobalt/result.hpp>

namespace asio_coro_utils {
inline boost::cobalt::promise<std::tuple<boost::system::error_code, std::size_t>> help_socket_reader(boost::asio::ip::tcp::socket& socket, boost::asio::streambuf& buffer, std::size_t needs_to_read_bytes) {
    co_return co_await boost::cobalt::as_tuple(boost::asio::async_read(socket, buffer,boost::asio::transfer_exactly(needs_to_read_bytes),boost::cobalt::use_op));
}

inline boost::cobalt::promise<std::tuple<boost::system::error_code, std::size_t>> help_socket_writer(boost::asio::ip::tcp::socket& socket, const std::span<uint8_t> message) {
    co_return co_await boost::cobalt::as_tuple(boost::asio::async_write(socket, boost::asio::buffer(message), boost::cobalt::use_op));
}

} // namespace asio_coro_utils

#endif //VPN_UTILS_H
