#include "clipster/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#include <share.h>
#endif

namespace clipster::log {

namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;
std::FILE* g_file = nullptr;  // guarded by g_mutex

const char* level_tag(Level level) {
  switch (level) {
    case Level::Debug:
      return "DEBUG";
    case Level::Info:
      return "INFO ";
    case Level::Warn:
      return "WARN ";
    case Level::Error:
      return "ERROR";
  }
  return "?";
}

}  // namespace

void set_level(Level level) { g_level.store(level, std::memory_order_relaxed); }

bool set_file(const std::filesystem::path& path) {
#ifdef _WIN32
  // _wfopen_s denies sharing, which would lock the log against Notepad
  // (and the developer) while the app runs; _wfsopen lets readers in.
  std::FILE* f = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
#else
  std::FILE* f = std::fopen(path.c_str(), "ab");
#endif
  if (!f) {
    return false;
  }
  std::lock_guard lock(g_mutex);
  if (g_file) {
    std::fclose(g_file);
  }
  g_file = f;
  return true;
}

void write(Level level, std::string_view message) {
  if (level < g_level.load(std::memory_order_relaxed)) {
    return;
  }
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto secs = floor<seconds>(now);
  const auto millis = duration_cast<milliseconds>(now - secs).count();
  const auto t = system_clock::to_time_t(secs);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif

  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "[%02d:%02d:%02d.%03d] [%s] %.*s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
               static_cast<int>(millis), level_tag(level), static_cast<int>(message.size()),
               message.data());
  if (g_file) {
    std::fprintf(g_file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %.*s\n", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                 static_cast<int>(millis), level_tag(level), static_cast<int>(message.size()),
                 message.data());
    std::fflush(g_file);
  }
}

}  // namespace clipster::log
