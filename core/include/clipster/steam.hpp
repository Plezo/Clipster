#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace clipster::steam {

// Extracts every library path from Steam's libraryfolders.vdf
// (e.g. "C:\\Program Files (x86)\\Steam" and any extra library drives).
// Tolerant of format drift: it simply collects the value after every
// "path" key anywhere in the file.
std::vector<std::string> parse_library_folders(std::string_view vdf_text);

// Same, but with "/steamapps/common" appended — the directories game
// executables actually live under, ready for GameMatcherConfig.
std::vector<std::string> library_common_dirs(std::string_view vdf_text);

}  // namespace clipster::steam
