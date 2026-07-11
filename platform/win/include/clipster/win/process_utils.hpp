#pragma once

#include <windows.h>

#include <string_view>
#include <vector>

namespace clipster::win {

// Pids of processes whose executable basename matches (case-insensitive)
// and whose parent is NOT another matching process — i.e. the roots of
// each process tree. Apps like Discord and Chrome run many child
// processes; capturing the root's tree covers all of them exactly once.
std::vector<DWORD> find_root_pids_by_exe_name(std::string_view exe_basename);

}  // namespace clipster::win
