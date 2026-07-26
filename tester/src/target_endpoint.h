#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <boost/cobalt/config.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/cobalt/wait_group.hpp>

#include "network_types.h"
#include "results.h"
#include "scenario.h"

namespace tester {

struct endpoint_address {
    std::string host;
    std::uint16_t port{};
};

class target_endpoint {
public:
    target_endpoint(boost::cobalt::executor executor, const scenario_registry& scenarios, endpoint_result_collector& results);

    target_endpoint(const target_endpoint&) = delete;
    target_endpoint& operator=(const target_endpoint&) = delete;

    ~target_endpoint();

    endpoint_address address() const;
    void set_measurement_start(std::chrono::steady_clock::time_point measurement_start);

    boost::cobalt::promise<void> run();
    void stop();

private:
    boost::cobalt::executor executor_;
    const scenario_registry& scenarios_;
    endpoint_result_collector& results_;
    std::chrono::steady_clock::time_point measurement_start_;

    boost::asio::ssl::context tls_context_;
    std::optional<acceptor_type> acceptor_;
    boost::cobalt::wait_group sessions_;

    endpoint_address address_;
    bool stopping_{};
};

} // namespace tester
