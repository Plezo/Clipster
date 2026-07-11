#pragma once

#include <string>
#include <vector>

namespace clipster::win {

// Finds every Steam "steamapps/common" directory on this machine: reads
// the Steam install path from the registry, then parses
// libraryfolders.vdf for all library drives. Returns an empty vector when
// Steam is not installed.
std::vector<std::string> steam_library_common_dirs();

}  // namespace clipster::win
