#pragma once

#include <string>

namespace clipster::win {

// "Start with Windows" via the HKCU Run key, pointing at the current
// executable with the given arguments.
bool autostart_enabled();
void set_autostart(bool enable, const std::wstring& args);

}  // namespace clipster::win
