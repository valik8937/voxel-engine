#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace vox {

// A small priority job pool. Lower priority values run first.
// submit() and wait_idle() are intended to be called from the main thread.
class JobSystem {
public:
    explicit JobSystem(std::size_t worker_count = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void submit(int priority, std::function<void()> fn);

    // Runs fn(i) for i in [0, count). Blocks until all work is done.
    void parallel_for(std::size_t count, std::size_t grain, const std::function<void(std::size_t)>& fn);

    void wait_idle();

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

private:
    struct Task {
        int priority = 0;
        std::uint64_t sequence = 0;
        std::function<void()> fn;
    };

    struct TaskCompare {
        bool operator()(const Task& a, const Task& b) const noexcept {
            if (a.priority != b.priority) {
                return a.priority > b.priority;
            }
            return a.sequence > b.sequence;
        }
    };

    void worker_loop();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idle_condition_;
    std::priority_queue<Task, std::vector<Task>, TaskCompare> queue_;
    std::uint64_t next_sequence_ = 0;
    std::size_t active_tasks_ = 0;
    bool stopping_ = false;
};

} // namespace vox
