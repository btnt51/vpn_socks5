#ifndef VPN_WORKER_H
#define VPN_WORKER_H
#include <list>
#include <boost/asio/io_context.hpp>

#include "session.h"
#include <vpn/logger.h>
#include <__thread/jthread.h>

struct statistics {
    std::uint64_t socks_rx{};
    std::uint64_t socks_tx{};
    std::uint64_t bytes_client_to_upstream{};
    std::uint64_t bytes_upstream_to_client{};
    std::uint64_t amount_of_sessions{};
};

struct atomic_statistics {
    std::atomic_uint64_t socks_rx{};
    std::atomic_uint64_t socks_tx{};
    std::atomic_uint64_t bytes_client_to_upstream{};
    std::atomic_uint64_t bytes_upstream_to_client{};
};

constexpr  int next_worker_id() {
    static int id = 1;
    return id++;
}

class worker {
public:
    worker(logger::logger& logger);

    ~worker();

    void stop();

    int get_id() const;

    std::uint64_t get_session_count() const;

    statistics get_statistics() const;
    boost::cobalt::executor get_executor();
    boost::cobalt::task<void> run_session(boost::cobalt::io::stream_socket socket);
    void reserve_session();
    void release_session();
private:
    void run();
    void append_session_statistic(const session::statistics& statistics);
    using work_guard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    int worker_id_{};
    boost::asio::io_context io_context_;
    work_guard work_guard_;
    std::list<std::shared_ptr<session::session>> sessions_;
    std::atomic_bool running_{true};
    atomic_statistics worker_statistics_{};
    std::atomic_uint64_t current_amount_of_sessions_{0};
    logger::logger& logger_;
    std::jthread thread_;
};


#endif //VPN_WORKER_H
