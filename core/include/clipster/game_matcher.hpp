#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace clipster {

// Decides whether a newly started process is a game we should record.
// Pure logic (string/path matching) so it is unit-testable everywhere; the
// platform layer feeds it process start events with full executable paths.
struct GameMatcherConfig {
  // Folders whose executables count as games (e.g. Steam library
  // steamapps/common dirs, Epic install dir).
  std::vector<std::string> watched_folders;
  // Executables that always count, matched by full path or basename.
  std::vector<std::string> manual_exes;
  // Basenames that never count, merged with default_ignored_exes().
  std::vector<std::string> ignored_exes;
};

class GameMatcher {
 public:
  explicit GameMatcher(GameMatcherConfig config);

  bool is_game(std::string_view exe_path) const;

  // Launchers, crash handlers and helper processes that live inside game
  // folders but must never trigger recording.
  static const std::vector<std::string>& default_ignored_exes();

 private:
  std::vector<std::string> watched_folders_;  // normalized
  std::vector<std::string> manual_paths_;     // normalized full paths
  std::vector<std::string> manual_names_;     // normalized basenames
  std::vector<std::string> ignored_names_;    // normalized basenames
};

namespace path_util {

// Lowercases (ASCII) and converts backslashes to forward slashes; collapses
// trailing slashes. Sufficient for matching Windows paths case-insensitively.
std::string normalize(std::string_view path);

// True if `path` is inside `folder` (both already normalized).
bool is_under(std::string_view folder, std::string_view path);

std::string basename(std::string_view path);

}  // namespace path_util

}  // namespace clipster
