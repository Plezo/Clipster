#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace clipster::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level level);

// Additionally appends log lines to a file — for frontends without a
// console (the tray app). Returns false if the file cannot be opened.
bool set_file(const std::filesystem::path& path);

void write(Level level, std::string_view message);

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace clipster::log
