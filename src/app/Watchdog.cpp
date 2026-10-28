// Hang watchdog. Crashes are already caught (structured exceptions, guarded frames, crash save);
// this covers the other way a program "dies": the main loop getting stuck in a long computation.
// A separate thread watches a heartbeat. If the viewport stops for 10 seconds it writes the last
// good copy of the design to the recovery file and asks whether to close Kestral. Nothing is ever
// lost beyond a few seconds, and you never need Alt+F4 or Task Manager.
#include "app/App.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "platform/Platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace kst {

namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void App::watchdogBeat(const char* phase) {
    wdBeatMs_.store(nowMs());
    wdPhase_.store(phase);
    // Keep a fresh copy of the design for the watchdog (structure only, a fraction of a ms).
    if (dirty_ && time_ - wdSnapTime_ > 5.0) {
        wdSnapTime_ = time_;
        std::string data;
        if (serializeDocument(doc_, data)) {
            std::lock_guard<std::mutex> lock(wdSnapMu_);
            wdSnapshot_ = std::move(data);
        }
    }
}

void App::startWatchdog() {
    wdBeatMs_.store(nowMs());
    wdStop_.store(false);
    std::string recPath = platform::appDataDir() + "/autosave/crash.kst";
    std::string logDir = platform::appDataDir();
    watchdog_ = std::thread([this, recPath, logDir] {
        int64_t nextAskMs = 0;
        bool savedThisHang = false;
        while (!wdStop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (wdStop_.load()) break;
            int64_t now = nowMs(), stuck = now - wdBeatMs_.load();
            if (platform::inNativeDialog() || stuck < 10000 || (stuck < 60000 && platform::mouseButtonHeld())) {
                savedThisHang = false;
                nextAskMs = 0;
                continue;
            }
            const char* phase = wdPhase_.load();
            if (!savedThisHang) {
                savedThisHang = true;
                log::warn(std::string("Main loop stuck for ") + std::to_string(stuck / 1000) + " s in: " + (phase ? phase : "?"));
                std::string snap;
                {
                    std::lock_guard<std::mutex> lock(wdSnapMu_);
                    snap = wdSnapshot_;
                }
                if (!snap.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(u8path(recPath).parent_path(), ec);
                    if (writeFileAtomic(recPath, snap)) log::info("Watchdog saved a recovery copy");
                }
                nextAskMs = now;
            }
            if (now < nextAskMs) continue;
            // --run-tools runs unattended: nobody can answer the question, and a long tool call or the
            // first draw of a big imported design is normal there. Keep waiting instead of asking.
            if (!toolScript_.empty()) {
                nextAskMs = nowMs() + 30000;
                continue;
            }
            std::string msg = std::string("Kestral has been busy for ") + std::to_string(stuck / 1000) +
                              " seconds (in: " + (phase ? phase : "?") + ").\n\n"
                              "Your design is saved in the recovery copy and will be offered the next time "
                              "Kestral starts.\n\nClose Kestral now?\n(No keeps waiting.)";
            bool close = platform::askYesNo("Kestral is not responding", msg);
            // It may have come back while the box was up: then never kill a healthy app.
            if (close && nowMs() - wdBeatMs_.load() > 5000) {
                log::warn("Closed by the user from the hang watchdog");
                platform::hardExit(3);
            }
            nextAskMs = nowMs() + 30000;
        }
    });
}

void App::stopWatchdog() {
    wdStop_.store(true);
    if (watchdog_.joinable()) watchdog_.join();
}

} // namespace kst
