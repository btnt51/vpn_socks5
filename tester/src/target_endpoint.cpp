#include "target_endpoint.h"

#include <algorithm>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/cobalt/io/sleep.hpp>
#include <boost/cobalt/op.hpp>
#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "http_io.h"

namespace tester {

namespace {

constexpr std::string_view scenario_header = "X-Tester-Scenario";
constexpr std::string_view run_id_header = "X-Tester-Run-Id";

template <typename T, void (*Deleter)(T*)>
using openssl_ptr = std::unique_ptr<T, decltype(Deleter)>;

void configure_server_certificate(boost::asio::ssl::context& context) {
    openssl_ptr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> key_context{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};

    if (not key_context or EVP_PKEY_keygen_init(key_context.get()) <= 0 or
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0) {

        throw std::runtime_error{"cannot initialize TLS key generation"};
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0) {
        throw std::runtime_error{"cannot generate TLS private key"};
    }

    openssl_ptr<EVP_PKEY, EVP_PKEY_free> key{raw_key, EVP_PKEY_free};

    openssl_ptr<X509, X509_free> certificate{X509_new(), X509_free};

    if (not certificate) {
        throw std::runtime_error{"cannot allocate TLS certificate"};
    }

    X509_set_version(certificate.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
    X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0);
    X509_gmtime_adj(X509_get_notAfter(certificate.get()), 24 * 60 * 60);

    if (X509_set_pubkey(certificate.get(), key.get()) != 1) {
        throw std::runtime_error{"cannot attach TLS public key"};
    }

    auto* subject = X509_get_subject_name(certificate.get());
    constexpr unsigned char common_name[] = "localhost";
    if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, common_name, -1, -1, 0) != 1 or
        X509_set_issuer_name(certificate.get(), subject) != 1 or  X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error{"cannot sign TLS certificate"};
    }

    auto* native_context = context.native_handle();
    if (SSL_CTX_use_certificate(native_context,certificate.get()) != 1 or
        SSL_CTX_use_PrivateKey(native_context, key.get()) != 1 or
        SSL_CTX_check_private_key(native_context) != 1) {

        throw std::runtime_error{"cannot configure TLS certificate"};
    }
}

std::optional<std::string> request_header(const http::request<http::string_body>& request, const std::string_view name) {
    const auto iterator = request.find(name);
    if (iterator == request.end()) {
        return std::nullopt;
    }
    return std::string{iterator->value()};
}

std::string expected_target(const scenario& scenario) {
    for (const auto& step : scenario.client_steps) {
        if (const auto* request = std::get_if<send_http_request>(&step)) {
            return request->target;
        }
    }
    return {};
}

class endpoint_session {
public:
    endpoint_session(socket_type socket, boost::asio::ssl::context& tls_context,
        const scenario_registry& scenarios, endpoint_result_collector& results, const bool measured)
        : stream_{std::move(socket), tls_context}, scenarios_{scenarios}, results_{results}, measured_{measured} { }

    ~endpoint_session() {
        cancel();
    }

    boost::cobalt::promise<void> run() {
        endpoint_result result{.measured = measured_};

        try {
            auto [handshake_error] = co_await stream_.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::as_tuple(boost::cobalt::use_op));
            if (handshake_error) {
                result.termination = termination_reason::io_error;
                result.error = "TLS handshake: " + handshake_error.message();
                results_.submit(std::move(result));
                co_return;
            }

            auto request_result = co_await read_http_request(stream_);
            if (not request_result) {
                result.termination = termination_reason::protocol_error;
                result.error = request_result.error();
                results_.submit(std::move(result));
                co_return;
            }

            const auto& request = request_result.value();
            const auto scenario_name = request_header(request, scenario_header);
            const auto run_id = request_header(request, run_id_header);

            if (not scenario_name or not run_id) {
                result.termination = termination_reason::protocol_error;
                result.error = "missing X-Tester-Scenario or X-Tester-Run-Id";
                results_.submit(std::move(result));
                co_return;
            }

            result.scenario_name = *scenario_name;
            result.run_id = *run_id;

            const auto scenario_iterator = scenarios_.find(*scenario_name);
            if (scenario_iterator == scenarios_.end()) {
                result.termination = termination_reason::protocol_error;
                result.error = "unknown scenario: " + *scenario_name;
                results_.submit(std::move(result));
                co_return;
            }

            const auto& selected_scenario = scenario_iterator->second;
            if (request.target() != expected_target(selected_scenario)) {
                result.termination = termination_reason::protocol_error;
                result.error = fmt::format("unexpected request target: {}", request.target());
                results_.submit(std::move(result));
                co_return;
            }

            co_await std::visit([this, &result](const auto& behavior) {
                return execute(behavior, result);
            }, selected_scenario.endpoint);
        } catch (const std::exception& error) {
            result.termination = termination_reason::io_error;
            result.error = error.what();
        }

        if (const auto submitted = results_.submit(std::move(result)); not submitted) {
            throw std::runtime_error{submitted.error()};
        }
    }

    void cancel() {
        boost::system::error_code error;
        stream_.next_layer().cancel(error);
        stream_.next_layer().close(error);
    }

private:
    boost::cobalt::promise<void> execute(const pong_behavior& behavior, endpoint_result& result) {
        http::response<http::string_body> response{static_cast<http::status>(behavior.status), 11};
        response.set(http::field::content_type, "text/plain");
        response.set(run_id_header, result.run_id);
        response.keep_alive(false);
        response.body() = behavior.body;
        response.prepare_payload();

        auto [write_error, bytes_written] = co_await http::async_write(stream_, response, boost::asio::as_tuple(boost::cobalt::use_op));
        if (write_error) {
            result.termination = termination_reason::io_error;
            result.error = "write HTTP response: " + write_error.message();
            co_return;
        }

        result.bytes_sent = behavior.body.size();
        result.termination = termination_reason::completed;

        auto [shutdown_error] = co_await stream_.async_shutdown(boost::asio::as_tuple(boost::cobalt::use_op));
        if (shutdown_error and shutdown_error != boost::asio::error::eof) {
            result.error = "TLS shutdown: " + shutdown_error.message();
        }
        boost::system::error_code close_error;
        stream_.next_layer().close(close_error);
    }

    boost::cobalt::promise<void> execute(const interrupted_download_behavior& behavior, endpoint_result& result) {
        http::response<http::empty_body> response{http::status::ok, 11};
        response.set(http::field::content_type, "application/octet-stream");
        response.set(run_id_header, result.run_id);
        response.content_length(behavior.declared_size);
        response.keep_alive(false);
        http::response_serializer<http::empty_body> serializer{response};

        auto [header_error, bytes_written] = co_await http::async_write_header(stream_, serializer, boost::asio::as_tuple(boost::cobalt::use_op));
        if (header_error) {
            result.termination = termination_reason::io_error;
            result.error = "write HTTP response headers: " + header_error.message();
            co_return;
        }

        std::string chunk(behavior.chunk_size, 'x');
        while (result.bytes_sent < behavior.close_after_bytes) {
            const auto remaining = behavior.close_after_bytes - result.bytes_sent;
            const auto chunk_size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));

            auto write_result = co_await write_bytes(stream_, std::string_view{chunk}.substr(0, chunk_size));
            if (not write_result) {
                result.termination = termination_reason::io_error;
                result.error = write_result.error();
                co_return;
            }

            result.bytes_sent += write_result.value();

            if (behavior.chunk_delay.count() > 0) {
                co_await boost::cobalt::io::sleep(behavior.chunk_delay);
            }
        }

        switch (behavior.close_mode) {
            case connection_close_mode::tls_shutdown: {
                auto [shutdown_error] = co_await stream_.async_shutdown(boost::asio::as_tuple(boost::cobalt::use_op));
                if (shutdown_error and  shutdown_error != boost::asio::error::eof) {
                    result.error = "TLS shutdown: " + shutdown_error.message();
                }
                result.termination = termination_reason::tls_shutdown;
                break;
            }
            case connection_close_mode::tcp_close:
                result.termination = termination_reason::stream_truncated;
                break;
            case connection_close_mode::tcp_reset: {
                boost::system::error_code linger_error;
                stream_.next_layer().set_option(boost::asio::socket_base::linger{true, 0}, linger_error);
                result.termination = termination_reason::connection_reset;
                break;
            }
        }

        boost::system::error_code close_error;
        stream_.next_layer().close(close_error);
    }

    boost::cobalt::promise<void> execute(const complete_download_behavior& behavior, endpoint_result& result) {
        http::response<http::empty_body> response{http::status::ok, 11};
        response.set(http::field::content_type, "application/octet-stream");
        response.set(run_id_header, result.run_id);
        response.content_length(behavior.size);
        response.keep_alive(false);
        http::response_serializer<http::empty_body> serializer{response};

        auto [header_error, bytes_written] = co_await http::async_write_header(stream_, serializer, boost::asio::as_tuple(boost::cobalt::use_op));
        if (header_error) {
            result.termination = termination_reason::io_error;
            result.error = "write HTTP response headers: " + header_error.message();
            co_return;
        }

        std::string chunk(behavior.chunk_size, 'x');
        while (result.bytes_sent < behavior.size) {
            const auto remaining = behavior.size - result.bytes_sent;
            const auto chunk_size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
            auto write_result = co_await write_bytes(stream_, std::string_view{chunk}.substr(0, chunk_size));
            if (not write_result) {
                result.termination = termination_reason::io_error;
                result.error = write_result.error();
                co_return;
            }

            result.bytes_sent += write_result.value();
            if (behavior.chunk_delay.count() > 0) {
                co_await boost::cobalt::io::sleep(behavior.chunk_delay);
            }
        }

        auto [shutdown_error] = co_await stream_.async_shutdown(boost::asio::as_tuple(boost::cobalt::use_op));
        if (shutdown_error and shutdown_error != boost::asio::error::eof) {
            result.termination = termination_reason::io_error;
            result.error = "TLS shutdown: " + shutdown_error.message();
        } else {
            result.termination = termination_reason::completed;
        }
        boost::system::error_code close_error;
        stream_.next_layer().close(close_error);
    }

    tls_stream stream_;
    const scenario_registry& scenarios_;
    endpoint_result_collector& results_;
    bool measured_;
};

boost::cobalt::promise<void> run_endpoint_session(std::shared_ptr<endpoint_session> session) {
    co_await session->run();
}

} // namespace

target_endpoint::target_endpoint(boost::cobalt::executor executor, const scenario_registry& scenarios, endpoint_result_collector& results)
    : executor_{std::move(executor)}, scenarios_{scenarios}, results_{results}, tls_context_{boost::asio::ssl::context::tls_server},
      acceptor_{std::in_place, executor_, boost::asio::ip::tcp::endpoint{boost::asio::ip::address_v4::loopback(), 0}} {

    tls_context_.set_options( boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3 );
    configure_server_certificate(tls_context_);

    const auto local_endpoint = acceptor_->local_endpoint();
    address_ = endpoint_address{.host = "127.0.0.1", .port = local_endpoint.port(),};
}

target_endpoint::~target_endpoint() {
    stop();
}

endpoint_address target_endpoint::address() const {
    return address_;
}

void target_endpoint::set_measurement_start(const std::chrono::steady_clock::time_point measurement_start) {
    measurement_start_ = measurement_start;
}

boost::cobalt::promise<void> target_endpoint::run() {
    while (not stopping_) {
        auto [accept_error, socket] = co_await acceptor_->async_accept(boost::asio::as_tuple(boost::cobalt::use_op));

        if (stopping_) {
            break;
        }
        if (accept_error) {
            continue;
        }

        const auto measured = std::chrono::steady_clock::now() >= measurement_start_;
        auto session = std::make_shared<endpoint_session>(std::move(socket), tls_context_, scenarios_, results_, measured);
        sessions_.push_back(run_endpoint_session(std::move(session)));
        sessions_.reap();
    }

    co_await sessions_;
}

void target_endpoint::stop() {
    if (std::exchange(stopping_, true)) {
        return;
    }
    boost::system::error_code error;
    acceptor_->close(error);
}

} // namespace tester
