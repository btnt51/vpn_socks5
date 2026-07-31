#include "../include/vpn/worker_pool.h"

worker_pool::worker_pool(std::size_t worker_count, logger::logger& logger) {
    workers_.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers_.push_back(std::make_unique<worker>(logger));
    }
}

worker_pool::~worker_pool() {
    for (auto& worker : workers_) {
        worker->stop();
    }
}

worker& worker_pool::get_next_worker() {
    const auto selected_worker_it = std::ranges::min_element(workers_, [](const std::unique_ptr<worker>& lhs, const std::unique_ptr<worker>& rhs) {
        return lhs->get_session_count() < rhs->get_session_count();
    });
    (*selected_worker_it)->reserve_session();
    return **selected_worker_it;
}

statistics worker_pool::get_statistics() const {
    statistics result;
    for (auto& worker : workers_) {
        auto temp_stats = worker->get_statistics();
        result.socks_rx += temp_stats.socks_rx;
        result.socks_tx += temp_stats.socks_tx;
        result.bytes_client_to_upstream += temp_stats.bytes_client_to_upstream;
        result.bytes_upstream_to_client += temp_stats.bytes_upstream_to_client;
    }
    return result;
}

void worker_pool::stop() const {
    for (auto& worker : workers_) {
        worker->stop();
    }
}
