#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace clipster::win {

struct WindowInfo {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::string title;     // UTF-8
  std::string exe_path;  // UTF-8, may be empty if the process is protected
};

// Top-level visible windows that are plausible capture targets (have a
// title, are not tool windows).
std::vector<WindowInfo> list_capturable_windows();

// First capturable window whose title contains `needle` (case-insensitive).
std::optional<WindowInfo> find_window_by_title(const std::string& needle);

// Main visible window belonging to a process, used once game detection
// (by exe path) picks the pid to record.
std::optional<WindowInfo> find_window_by_pid(DWORD pid);

}  // namespace clipster::win
