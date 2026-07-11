#include "clipster/game_matcher.hpp"

#include <algorithm>

#include "clipster/util.hpp"

namespace clipster {

namespace path_util {

std::string normalize(std::string_view path) {
  std::string out = util::to_lower_ascii(path);
  std::replace(out.begin(), out.end(), '\\', '/');
  while (out.size() > 1 && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

bool is_under(std::string_view folder, std::string_view path) {
  if (folder.empty() || path.size() <= folder.size()) {
    return false;
  }
  return path.substr(0, folder.size()) == folder && path[folder.size()] == '/';
}

std::string basename(std::string_view path) {
  const size_t pos = path.find_last_of('/');
  return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

}  // namespace path_util

const std::vector<std::string>& GameMatcher::default_ignored_exes() {
  static const std::vector<std::string> ignored{
      // Store/launcher processes
      "steam.exe", "steamwebhelper.exe", "steamservice.exe", "steamerrorreporter.exe",
      "epicgameslauncher.exe", "epicwebhelper.exe", "galaxyclient.exe", "gog galaxy.exe",
      "battle.net.exe", "riotclientservices.exe", "eadesktop.exe", "upc.exe",
      // Common helper/crash processes shipped inside game folders
      "crashpad_handler.exe", "crashreportclient.exe", "unitycrashhandler.exe",
      "unitycrashhandler64.exe", "ueprereqsetup_x64.exe", "vc_redist.x64.exe",
      "easyanticheat.exe", "easyanticheat_setup.exe", "installscript.vdf",
      "dxsetup.exe", "launcher.exe",
  };
  return ignored;
}

GameMatcher::GameMatcher(GameMatcherConfig config) {
  for (const auto& f : config.watched_folders) {
    watched_folders_.push_back(path_util::normalize(f));
  }
  for (const auto& e : config.manual_exes) {
    const std::string norm = path_util::normalize(e);
    if (norm.find('/') != std::string::npos) {
      manual_paths_.push_back(norm);
    } else {
      manual_names_.push_back(norm);
    }
  }
  for (const auto& e : default_ignored_exes()) {
    ignored_names_.push_back(path_util::normalize(e));
  }
  for (const auto& e : config.ignored_exes) {
    ignored_names_.push_back(path_util::normalize(path_util::basename(path_util::normalize(e))));
  }
}

bool GameMatcher::is_game(std::string_view exe_path) const {
  const std::string path = path_util::normalize(exe_path);
  const std::string name = path_util::basename(path);

  if (std::find(ignored_names_.begin(), ignored_names_.end(), name) != ignored_names_.end()) {
    return false;
  }
  if (std::find(manual_paths_.begin(), manual_paths_.end(), path) != manual_paths_.end() ||
      std::find(manual_names_.begin(), manual_names_.end(), name) != manual_names_.end()) {
    return true;
  }
  return std::any_of(watched_folders_.begin(), watched_folders_.end(),
                     [&](const std::string& folder) { return path_util::is_under(folder, path); });
}

}  // namespace clipster
