#include "clipster/win/process_utils.hpp"

#include <tlhelp32.h>

#include <unordered_set>

#include "clipster/util.hpp"
#include "clipster/win/str_util.hpp"

namespace clipster::win {

std::vector<DWORD> find_root_pids_by_exe_name(std::string_view exe_basename) {
  const std::string wanted = util::to_lower_ascii(exe_basename);

  struct Match {
    DWORD pid;
    DWORD parent_pid;
  };
  std::vector<Match> matches;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return {};
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snap, &entry)) {
    do {
      if (util::to_lower_ascii(narrow(entry.szExeFile)) == wanted) {
        matches.push_back({entry.th32ProcessID, entry.th32ParentProcessID});
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);

  std::unordered_set<DWORD> match_pids;
  for (const Match& m : matches) {
    match_pids.insert(m.pid);
  }
  std::vector<DWORD> roots;
  for (const Match& m : matches) {
    if (!match_pids.contains(m.parent_pid)) {
      roots.push_back(m.pid);
    }
  }
  return roots;
}

}  // namespace clipster::win
