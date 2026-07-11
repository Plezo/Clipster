#pragma once

#include <string>
#include <string_view>

namespace clipster::win {

// UTF-8 <-> UTF-16 conversion. All clipster-internal strings are UTF-8;
// conversion happens only at Win32 API boundaries.
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

}  // namespace clipster::win
