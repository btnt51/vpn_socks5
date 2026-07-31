#include "worker.h"

#include "boost/cobalt/spawn.hpp"

worker::worker(logger::logger &logger) : work_guard_(boost::asio::make_work_guard(io_context_)),
        worker_id_{next_worker_id()}, logger_{logger}, thread_([this] {run();}) {}

worker::~worker() {
    stop();
}

void worker::run() {
    boost::cobalt::this_thread::set_executor(
        io_context_.get_executor()
    );

    try {
        io_context_.run();
    } catch (const std::exception& error) {
        logger_.log("server", logger_levels::e_error,"Worker#{} stopped with exception: {}", worker_id_, error.what());
    }
}

void worker::stop() {
    if (not running_.exchange(false)) {
        return;
    }

    boost::asio::post(io_context_, [this] {
        for (auto& session : sessions_) {
            session->cancel();
        }
        work_guard_.reset();
    });
    thread_.join();
}

boost::cobalt::task<void> worker::run_session(boost::cobalt::io::stream_socket socket) {
    auto session = std::make_shared<::session::session>(io_context_.get_executor(), std::move(socket), logger_);
    auto it = sessions_.insert(sessions_.end(), session);
    logger_.log("server", logger_levels::e_info,
            "Accepting new session session id: {} username: {}", session->session_id(), session->username());
    try {
        co_await session->run();
    } catch (const std::exception& e) {
        logger_.log("session", logger_levels::e_warning,
                    "While session [session id: {} username: {}] was running there was thrown an exception: {}",
                    session->session_id(), session->username(), e.what());
        session->cancel();
    }
    auto session_stats = session->statistics();
    append_session_statistic(session_stats);
    logger_.log("session", logger_levels::e_info, "Session [session id: {} username: {}] statistic: [{}]",
                session->session_id(), session->username(), session_stats);
    sessions_.erase(it);
    co_return;
}

void worker::reserve_session() {
    current_amount_of_sessions_.fetch_add(1,std::memory_order_relaxed);
}

void worker::release_session() {
    current_amount_of_sessions_.fetch_sub(1,std::memory_order_relaxed);
}

int worker::get_id() const {
    return worker_id_;
}

std::uint64_t worker::get_session_count() const {
    return current_amount_of_sessions_.load();
}

statistics worker::get_statistics() const {
    return {
        .socks_rx = worker_statistics_.socks_rx.load(std::memory_order_relaxed),
        .socks_tx = worker_statistics_.socks_tx.load(std::memory_order_relaxed),
        .bytes_client_to_upstream = worker_statistics_.bytes_client_to_upstream.load(std::memory_order_relaxed),
        .bytes_upstream_to_client = worker_statistics_.bytes_upstream_to_client.load(std::memory_order_relaxed),
        .amount_of_sessions = current_amount_of_sessions_.load(std::memory_order_relaxed),
    };
}

boost::cobalt::executor worker::get_executor() {
    return io_context_.get_executor();
}

void worker::append_session_statistic(const session::statistics& stats) {
    worker_statistics_.socks_rx.fetch_add(stats.socks_rx,std::memory_order_relaxed);

    worker_statistics_.socks_tx.fetch_add(stats.socks_tx,std::memory_order_relaxed);

    worker_statistics_.bytes_client_to_upstream.fetch_add(stats.bytes_client_to_upstream,std::memory_order_relaxed);

    worker_statistics_.bytes_upstream_to_client.fetch_add(stats.bytes_upstream_to_client,std::memory_order_relaxed);
}

