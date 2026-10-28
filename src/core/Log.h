#pragma once
#include <string>
#include <vector>

namespace kst::log {

enum class Level { Info, Warn, Error };

void init(const std::string& logFilePath);
void write(Level lvl, const std::string& msg);
inline void info(const std::string& m) { write(Level::Info, m); }
inline void warn(const std::string& m) { write(Level::Warn, m); }
inline void error(const std::string& m) { write(Level::Error, m); }

struct Line { Level level; std::string text; };
std::vector<Line> recent(size_t max = 200);

} // namespace kst::log
