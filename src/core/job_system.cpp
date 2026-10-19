#include "core/job_system.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace vox {

JobSystem::JobSystem(std::size_t worker_count) {
    if (worker_count == 0) {
        worker_count = std::max(1u, std::thread::hardware_concurrency());
    }

    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

JobSystem::~JobSystem() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void JobSystem::submit(int priority, std::function<void()> fn) {
    {
        const std::lock_guard lock(mutex_);
        queue_.push(Task{priority, next_sequence_++, std::move(fn)});
    }
    condition_.notify_one();
}

void JobSystem::parallel_for(std::size_t count, std::size_t grain,
                             const std::function<void(std::size_t)>& fn) {
    if (count == 0) {
        return;
    }
    grain = std::max<std::size_t>(1, grain);
    const std::size_t chunks = (count + grain - 1) / grain;
    const std::size_t parallel = std::min(chunks, worker_count());

    auto next = std::make_shared<std::atomic<std::size_t>>(0);
    for (std::size_t worker = 0; worker < parallel; ++worker) {
        submit(0, [next, count, grain, &fn] {
            for (;;) {
                const std::size_t start = next->fetch_add(grain, std::memory_order_relaxed);
                if (start >= count) {
                    break;
                }
                const std::size_t end = std::min(count, start + grain);
                for (std::size_t i = start; i < end; ++i) {
                    fn(i);
                }
            }
        });
    }
    wait_idle();
}

void JobSystem::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_condition_.wait(lock, [this] {
        return queue_.empty() && active_tasks_ == 0;
    });
}

void JobSystem::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            task = std::move(const_cast<Task&>(queue_.top()));
            queue_.pop();
            ++active_tasks_;
        }

        task.fn();

        {
            const std::lock_guard lock(mutex_);
            --active_tasks_;
            if (queue_.empty() && active_tasks_ == 0) {
                idle_condition_.notify_all();
            }
        }
    }
}

} // namespace vox
