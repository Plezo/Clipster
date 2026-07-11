#include "clipster/steam.hpp"

#include "test_framework.hpp"

using namespace clipster;

namespace {

// Trimmed-down copy of a real libraryfolders.vdf.
constexpr const char* kVdf = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"contentid"		"1234567890"
		"apps"
		{
			"228980"		"1234"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"label"		"games"
	}
}
)";

}  // namespace

TEST(steam_parses_all_library_paths) {
  const auto paths = steam::parse_library_folders(kVdf);
  CHECK_EQ(paths.size(), 2u);
  CHECK_EQ(paths[0], "C:\\Program Files (x86)\\Steam");
  CHECK_EQ(paths[1], "D:\\SteamLibrary");
}

TEST(steam_common_dirs_append_steamapps_common) {
  const auto dirs = steam::library_common_dirs(kVdf);
  CHECK_EQ(dirs.size(), 2u);
  CHECK_EQ(dirs[0], "C:\\Program Files (x86)\\Steam\\steamapps\\common");
  CHECK_EQ(dirs[1], "D:\\SteamLibrary\\steamapps\\common");
}

TEST(steam_parser_tolerates_garbage) {
  CHECK(steam::parse_library_folders("").empty());
  CHECK(steam::parse_library_folders("not vdf at all { } \"").empty());
  const auto one = steam::parse_library_folders(R"("path" "/home/user/.steam/steam")");
  CHECK_EQ(one.size(), 1u);
  CHECK_EQ(one[0], "/home/user/.steam/steam");
}

TEST(steam_common_dirs_use_matching_separator) {
  const auto dirs = steam::library_common_dirs(R"("path" "/home/user/.steam/steam")");
  CHECK_EQ(dirs.size(), 1u);
  CHECK_EQ(dirs[0], "/home/user/.steam/steam/steamapps/common");
}
