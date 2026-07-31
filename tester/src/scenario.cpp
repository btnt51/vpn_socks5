#include "scenario.h"

#include <algorithm>
#include <type_traits>

namespace tester {

std::string_view step_name(const client_step& step) {
    return std::visit([]<typename Step>(const Step&) -> std::string_view {
        if constexpr (std::is_same_v<Step, socks5_negotiate>) {
            return "socks5-negotiate";
        } else if constexpr (std::is_same_v<Step, socks5_connect>) {
            return "socks5-connect";
        } else if constexpr (std::is_same_v<Step, tls_handshake>) {
            return "tls-handshake";
        } else if constexpr (std::is_same_v<Step, send_http_request>) {
            return "send-http-request";
        } else if constexpr (std::is_same_v<Step, expect_http_response>) {
            return "expect-http-response";
        } else if constexpr (std::is_same_v<Step, expect_incomplete_response>) {
            return "expect-incomplete-response";
        } else {
            return "expect-complete-download";
        }
    }, step);
}

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

void set_payload_size(scenario_registry& scenarios, const std::uint64_t payload_size) {
    for (auto& [name, selected_scenario] : scenarios) {
        std::visit([&](auto& behavior) {
            using behavior_type = std::decay_t<decltype(behavior)>;
            if constexpr (std::is_same_v<behavior_type, interrupted_download_behavior>) {
                behavior.declared_size = payload_size * 2;
                behavior.close_after_bytes = payload_size;
            } else if constexpr (std::is_same_v<behavior_type, complete_download_behavior>) {
                behavior.size = payload_size;
            }
        }, selected_scenario.endpoint);

        for (auto& step : selected_scenario.client_steps) {
            std::visit([&](auto& selected_step) {
                using step_type = std::decay_t<decltype(selected_step)>;
                if constexpr (std::is_same_v<step_type, expect_incomplete_response>) {
                    selected_step.declared_size = payload_size * 2;
                    selected_step.minimum_received = std::min(selected_step.minimum_received,payload_size);
                    selected_step.maximum_received = payload_size;
                } else if constexpr (std::is_same_v<step_type, expect_complete_download>) {
                    selected_step.declared_size = payload_size;
                }
            }, step);
        }
    }
}

} // namespace tester
