#include "clipster/win/window_finder.hpp"

#include <algorithm>

#include "clipster/util.hpp"
#include "clipster/win/str_util.hpp"

namespace clipster::win {

namespace {

std::string process_image_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return {};
  }
  wchar_t buf[MAX_PATH * 2];
  DWORD len = static_cast<DWORD>(std::size(buf));
  std::string out;
  if (QueryFullProcessImageNameW(process, 0, buf, &len)) {
    out = narrow(std::wstring_view(buf, len));
  }
  CloseHandle(process);
  return out;
}

bool is_capturable(HWND hwnd) {
  if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
    return false;
  }
  if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
    return false;
  }
  return GetWindowTextLengthW(hwnd) > 0;
}

WindowInfo make_info(HWND hwnd) {
  WindowInfo info;
  info.hwnd = hwnd;
  GetWindowThreadProcessId(hwnd, &info.pid);
  wchar_t title[512];
  const int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
  info.title = narrow(std::wstring_view(title, static_cast<size_t>(len)));
  info.exe_path = process_image_path(info.pid);
  return info;
}

}  // namespace

std::vector<WindowInfo> list_capturable_windows() {
  std::vector<WindowInfo> windows;
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        if (is_capturable(hwnd)) {
          reinterpret_cast<std::vector<WindowInfo>*>(param)->push_back(make_info(hwnd));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows));
  return windows;
}

std::optional<WindowInfo> find_window_by_title(const std::string& needle) {
  const std::string lowered = util::to_lower_ascii(needle);
  for (const WindowInfo& w : list_capturable_windows()) {
    if (util::to_lower_ascii(w.title).find(lowered) != std::string::npos) {
      return w;
    }
  }
  return std::nullopt;
}

std::optional<WindowInfo> find_window_by_pid(DWORD pid) {
  for (const WindowInfo& w : list_capturable_windows()) {
    if (w.pid == pid) {
      return w;
    }
  }
  return std::nullopt;
}

}  // namespace clipster::win
