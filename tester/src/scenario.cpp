#include "scenario.h"

namespace tester {

const scenario_registry& scenarios() {
    using namespace std::chrono_literals;
    using namespace tester::literals;

    static const scenario_registry registry{
        {
            "pong",
            scenario{
                .name = "pong",
                .client_steps = {
                    socks5_negotiate{
                        .methods = {0x00},
                        .expected_method = 0x00,
                    },
                    socks5_connect{
                        .expected_reply = 0x00,
                    },
                    tls_handshake{},
                    send_http_request{
                        .method = "GET",
                        .target = "/pong",
                    },
                    expect_http_response{
                        .status = 200,
                        .body = "pong",
                    },
                },
                .endpoint = pong_behavior{
                    .status = 200,
                    .body = "pong",
                },
                .slo = {
                    .maximum_error_rate_percent = 1.0,
                    .maximum_p95 = 1500ms,
                    .maximum_p99 = 2000ms,
                },
            },
        },
        {
            "interrupted-download",
            scenario{
                .name = "interrupted-download",
                .client_steps = {
                    socks5_negotiate{
                        .methods = {0x00},
                        .expected_method = 0x00,
                    },
                    socks5_connect{
                        .expected_reply = 0x00,
                    },
                    tls_handshake{},
                    send_http_request{
                        .method = "GET",
                        .target = "/download",
                    },
                    expect_incomplete_response{
                        .declared_size = 1_GiB,
                        .minimum_received = 1_MiB,
                        .maximum_received = 10_MiB,
                    },
                },
                .endpoint = interrupted_download_behavior{
                    .declared_size = 1_GiB,
                    .close_after_bytes = 10_MiB,
                    .chunk_size = 64_KiB,
                    .chunk_delay = 10ms,
                    .close_mode =
                        connection_close_mode::tcp_reset,
                },
                .slo = {
                    .maximum_error_rate_percent = 1.0,
                    .maximum_p95 = 3500ms,
                    .maximum_p99 = 4500ms,
                },
                .session_timeout = 60s,
            },
        },
        {
            "complete-download",
            scenario{
                .name = "complete-download",
                .client_steps = {
                    socks5_negotiate{
                        .methods = {0x00},
                        .expected_method = 0x00,
                    },
                    socks5_connect{
                        .expected_reply = 0x00,
                    },
                    tls_handshake{},
                    send_http_request{
                        .method = "GET",
                        .target = "/download",
                    },
                    expect_complete_download{
                        .status = 200,
                        .declared_size = 10_MiB,
                    },
                },
                .endpoint = complete_download_behavior{
                    .size = 10_MiB,
                    .chunk_size = 64_KiB,
                    .chunk_delay = 10ms,
                },
                .slo = {
                    .maximum_error_rate_percent = 1.0,
                    .maximum_p95 = 3500ms,
                    .maximum_p99 = 4500ms,
                },
                .session_timeout = 60s,
            },
        },
    };

    return registry;
}

} // namespace tester
