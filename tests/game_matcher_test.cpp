#include "clipster/game_matcher.hpp"

#include "test_framework.hpp"

using namespace clipster;

namespace {

GameMatcher make_matcher() {
  GameMatcherConfig cfg;
  cfg.watched_folders = {"C:\\Program Files (x86)\\Steam\\steamapps\\common",
                         "D:\\SteamLibrary\\steamapps\\common"};
  cfg.manual_exes = {"C:\\Games\\retro\\emu.exe", "standalone.exe"};
  cfg.ignored_exes = {"MyLauncher.exe"};
  return GameMatcher(std::move(cfg));
}

}  // namespace

TEST(matches_exe_inside_watched_folder_case_insensitively) {
  const auto m = make_matcher();
  CHECK(m.is_game(
      "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Hades II\\Hades2.exe"));
  CHECK(m.is_game("d:\\steamlibrary\\STEAMAPPS\\common\\Elden Ring\\eldenring.exe"));
}

TEST(rejects_exe_outside_watched_folders) {
  const auto m = make_matcher();
  CHECK(!m.is_game("C:\\Windows\\notepad.exe"));
  CHECK(!m.is_game("C:\\Program Files (x86)\\Steam\\steam_helper.exe"));
  // Prefix that is not a real path component boundary must not match.
  CHECK(!m.is_game("D:\\SteamLibrary\\steamapps\\commonEvil\\bad.exe"));
}

TEST(manual_exes_match_by_path_or_basename) {
  const auto m = make_matcher();
  CHECK(m.is_game("C:\\Games\\retro\\emu.exe"));
  CHECK(m.is_game("c:/games/RETRO/emu.exe"));  // separators + case
  CHECK(m.is_game("E:\\anywhere\\Standalone.exe"));
  CHECK(!m.is_game("C:\\Games\\other\\thing.exe"));
}

TEST(default_and_user_ignore_lists_win_over_watched_folders) {
  const auto m = make_matcher();
  // Helper processes inside a game folder must not trigger recording.
  CHECK(!m.is_game(
      "D:\\SteamLibrary\\steamapps\\common\\SomeGame\\UnityCrashHandler64.exe"));
  CHECK(!m.is_game(
      "D:\\SteamLibrary\\steamapps\\common\\SomeGame\\crashpad_handler.exe"));
  CHECK(!m.is_game("D:\\SteamLibrary\\steamapps\\common\\SomeGame\\MyLauncher.exe"));
}

TEST(path_normalization_utilities) {
  CHECK_EQ(path_util::normalize("C:\\Foo\\Bar\\"), "c:/foo/bar");
  CHECK(path_util::is_under("c:/foo", "c:/foo/bar.exe"));
  CHECK(!path_util::is_under("c:/foo", "c:/foobar/baz.exe"));
  CHECK(!path_util::is_under("c:/foo", "c:/foo"));
  CHECK_EQ(path_util::basename("c:/foo/bar.exe"), "bar.exe");
  CHECK_EQ(path_util::basename("bar.exe"), "bar.exe");
}
