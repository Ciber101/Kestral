#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kst {

// Worker pool for all kernel work. The UI thread only submits and receives results.
class JobSystem {
public:
    explicit JobSystem(unsigned threads);
    ~JobSystem();

    // Runs work on a worker. Exceptions and hardware faults are caught and reported via onError
    // (which runs on the main thread). Never lets a failure escape.
    void submit(std::string label, std::function<void()> work,
                std::function<void(const std::string&)> onError = {});

    // Queue a callback to run on the main thread (call from workers to deliver results).
    void post(std::function<void()> fn);

    // Main thread: run queued callbacks. Returns number executed.
    int pumpMain();

    int active() const { return active_.load(); }
    std::string status() const;
    void setStatus(const std::string& s);

private:
    struct Job { std::string label; std::function<void()> work; std::function<void(const std::string&)> onError; };
    void workerLoop();

    std::vector<std::thread> threads_;
    std::deque<Job> queue_;
    std::mutex qm_;
    std::condition_variable cv_;
    bool stop_ = false;

    std::deque<std::function<void()>> mainQueue_;
    std::mutex mm_;

    std::atomic<int> active_{0};
    mutable std::mutex sm_;
    std::string status_;
};

} // namespace kst
