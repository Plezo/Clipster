#include "clipster/win/str_util.hpp"

#include <windows.h>

namespace clipster::win {

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
  return out;
}

std::string narrow(std::wstring_view utf16) {
  if (utf16.empty()) {
    return {};
  }
  const int len = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                      nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), len,
                      nullptr, nullptr);
  return out;
}

}  // namespace clipster::win
