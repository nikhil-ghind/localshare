#include "thread_pool.h"

namespace localshare {

ThreadPool::ThreadPool(size_t n) {
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        try {
            job();
        } catch (...) {
            // swallow — workers must keep running
        }
    }
}

}  // namespace localshare
