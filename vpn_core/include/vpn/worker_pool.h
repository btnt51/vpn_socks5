#ifndef VPN_WORKER_POOL_H
#define VPN_WORKER_POOL_H
#include <vector>

#include "../../src/worker.h"

class worker_pool {
public:
    explicit worker_pool(std::size_t worker_count, logger::logger& logger);
    ~worker_pool();

    worker& get_next_worker();

    statistics get_statistics() const;

    void stop() const;

private:
    std::vector<std::unique_ptr<worker>> workers_;
};


#endif //VPN_WORKER_POOL_H
