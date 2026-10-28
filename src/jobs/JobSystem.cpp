#include "jobs/JobSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"

namespace kst {

JobSystem::JobSystem(unsigned n) {
    if (n < 2) n = 2;
    for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { workerLoop(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(qm_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

void JobSystem::submit(std::string label, std::function<void()> work, std::function<void(const std::string&)> onError) {
    {
        std::lock_guard lock(qm_);
        queue_.push_back({std::move(label), std::move(work), std::move(onError)});
    }
    active_++;
    cv_.notify_one();
}

void JobSystem::post(std::function<void()> fn) {
    std::lock_guard lock(mm_);
    mainQueue_.push_back(std::move(fn));
}

int JobSystem::pumpMain() {
    std::deque<std::function<void()>> q;
    {
        std::lock_guard lock(mm_);
        q.swap(mainQueue_);
    }
    int n = 0;
    for (auto& fn : q) {
        std::string err;
        platform::guarded("main thread callback", fn, err);
        ++n;
    }
    return n;
}

void JobSystem::workerLoop() {
    platform::installThreadGuard();
    for (;;) {
        Job job;
        {
            std::unique_lock lock(qm_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string err;
        bool ok = platform::guarded(job.label.c_str(), job.work, err);
        if (!ok) {
            auto cb = job.onError;
            std::string label = job.label;
            post([cb, err, label] {
                if (cb) cb(err);
                else log::error(label + ": " + err);
            });
        }
        active_--;
    }
}

std::string JobSystem::status() const {
    std::lock_guard lock(sm_);
    return status_;
}

void JobSystem::setStatus(const std::string& s) {
    std::lock_guard lock(sm_);
    status_ = s;
}

} // namespace kst
