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
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

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
    if (dirty_ && hasWork(doc_) && time_ - wdSnapTime_ > 5.0) {
        wdSnapTime_ = time_;
        std::string data;
        if (serializeDocument(doc_, data)) {
            if (asKey_.empty()) asKey_ = newRecoveryKey();
            std::lock_guard<std::mutex> lock(wdSnapMu_);
            wdSnapshot_ = std::move(data);
            wdSnapKey_ = asKey_;
            wdSnapPath_ = doc_.path;
            wdSnapName_ = doc_.name;
        }
    }
}

void App::startWatchdog() {
    wdBeatMs_.store(nowMs());
    wdStop_.store(false);
    std::string recDir = recoveryDir();
    std::string logDir = platform::appDataDir();
    watchdog_ = std::thread([this, recDir, logDir] {
        int64_t nextAskMs = 0;
        bool savedThisHang = false;
        double memLogged = 0;   // GB last written to the log
        while (!wdStop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (wdStop_.load()) break;
#ifdef _WIN32
            {
                // Big memory use goes in the log with what Kestral was doing, so a runaway step can be
                // named afterwards (Kestral is capped at 60% of the RAM; past that a step fails).
                PROCESS_MEMORY_COUNTERS_EX pm{};
                if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
                    double gb = (double)pm.PrivateUsage / (1024.0 * 1024.0 * 1024.0);
                    if (gb > 6.0 && gb > memLogged + 2.0) {
                        memLogged = gb;
                        const char* ph = wdPhase_.load();
                        std::string step = recomputeStepNow();
                        char buf[64];
                        std::snprintf(buf, sizeof buf, "%.1f GB", gb);
                        log::warn(std::string("Memory use ") + buf + " (main: " + (ph ? ph : "?") +
                                  (step.empty() ? "" : ", rebuilding: " + step) + ")");
                    } else if (gb < memLogged - 4.0) {
                        memLogged = gb;
                    }
                }
            }
#endif
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
                std::string snap, key, path, name;
                {
                    std::lock_guard<std::mutex> lock(wdSnapMu_);
                    snap = wdSnapshot_;
                    key = wdSnapKey_;
                    path = wdSnapPath_;
                    name = wdSnapName_;
                }
                // The design in front, as of a few seconds ago (other tabs' copies are written when
                // they go to the back).
                if (!snap.empty() && writeRecoveryFiles(recDir, key, snap, path, name)) log::info("Watchdog saved a recovery copy");
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
