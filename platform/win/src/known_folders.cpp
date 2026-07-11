#include "clipster/win/known_folders.hpp"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

namespace clipster::win {

namespace {

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
    out = raw;
  }
  CoTaskMemFree(raw);
  return out;
}

}  // namespace

std::filesystem::path app_data_dir() {
  return known_folder(FOLDERID_RoamingAppData) / L"Clipster";
}

std::filesystem::path default_save_dir() {
  return known_folder(FOLDERID_Videos) / L"Clipster";
}

}  // namespace clipster::win
