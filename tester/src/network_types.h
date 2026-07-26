#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/cobalt/config.hpp>
#include <boost/cobalt/op.hpp>

namespace tester {

using executor_type = boost::cobalt::use_op_t::executor_with_default<boost::cobalt::executor>;
using socket_type = boost::asio::ip::tcp::socket::rebind_executor<executor_type>::other;
using acceptor_type = boost::asio::ip::tcp::acceptor::rebind_executor<executor_type>::other;
using tls_stream = boost::asio::ssl::stream<socket_type>;

} // namespace tester
