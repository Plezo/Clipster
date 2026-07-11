#include "clipster/win/steam_locator.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "clipster/logging.hpp"
#include "clipster/steam.hpp"
#include "clipster/win/str_util.hpp"

namespace clipster::win {

namespace {

std::filesystem::path steam_root_from_registry() {
  wchar_t buf[MAX_PATH];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                   nullptr, buf, &size) != ERROR_SUCCESS) {
    return {};
  }
  return std::filesystem::path(buf);
}

}  // namespace

std::vector<std::string> steam_library_common_dirs() {
  const auto root = steam_root_from_registry();
  if (root.empty()) {
    return {};
  }

  const auto vdf_path = root / L"steamapps" / L"libraryfolders.vdf";
  std::ifstream in(vdf_path, std::ios::binary);
  if (!in) {
    // Steam installed but no libraries file: fall back to the root library.
    return {narrow((root / L"steamapps" / L"common").wstring())};
  }
  std::stringstream buf;
  buf << in.rdbuf();

  auto dirs = steam::library_common_dirs(buf.str());
  if (dirs.empty()) {
    dirs.push_back(narrow((root / L"steamapps" / L"common").wstring()));
  }
  for (const auto& d : dirs) {
    log::info("steam: watching library {}", d);
  }
  return dirs;
}

}  // namespace clipster::win
