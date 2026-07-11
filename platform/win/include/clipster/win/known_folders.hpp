#pragma once

#include <filesystem>

namespace clipster::win {

// %APPDATA%/Clipster — settings and logs.
std::filesystem::path app_data_dir();

// <user Videos>/Clipster — default clip output location.
std::filesystem::path default_save_dir();

}  // namespace clipster::win
