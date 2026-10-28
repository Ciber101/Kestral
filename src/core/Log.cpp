#include "core/Log.h"
#include "core/Types.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>

namespace kst::log {

namespace {
std::mutex g_mutex;
std::deque<Line> g_lines;
std::ofstream g_file;
}

void init(const std::string& path) {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    std::filesystem::create_directories(u8path(path).parent_path(), ec);
    g_file.open(u8path(path), std::ios::out | std::ios::trunc);
}

void write(Level lvl, const std::string& msg) {
    std::lock_guard lock(g_mutex);
    const char* tag = lvl == Level::Info ? "INFO " : lvl == Level::Warn ? "WARN " : "ERROR";
    std::time_t t = std::time(nullptr);
    char ts[32] = {};
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    std::string line = std::string(ts) + " " + tag + " " + msg;
    if (g_file.is_open()) { g_file << line << '\n'; g_file.flush(); }
    std::fprintf(stderr, "%s\n", line.c_str());
    g_lines.push_back({lvl, line});
    while (g_lines.size() > 1000) g_lines.pop_front();
}

std::vector<Line> recent(size_t max) {
    std::lock_guard lock(g_mutex);
    size_t n = std::min(max, g_lines.size());
    return std::vector<Line>(g_lines.end() - (long)n, g_lines.end());
}

} // namespace kst::log
