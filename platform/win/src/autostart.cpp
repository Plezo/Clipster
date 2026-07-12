#include "clipster/win/autostart.hpp"

#include <windows.h>

namespace clipster::win {

namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Clipster";
}  // namespace

bool autostart_enabled() {
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr,
                      nullptr) == ERROR_SUCCESS;
}

void set_autostart(bool enable, const std::wstring& args) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  if (enable) {
    wchar_t exe[MAX_PATH * 2];
    const DWORD len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring cmd = L"\"" + std::wstring(exe, len) + L"\"";
    if (!args.empty()) {
      cmd += L" " + args;
    }
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                   static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

}  // namespace clipster::win
