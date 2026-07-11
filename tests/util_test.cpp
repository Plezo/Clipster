#include "clipster/util.hpp"

#include "test_framework.hpp"

using namespace clipster;

TEST(template_expansion_replaces_known_keys) {
  const auto out = util::expand_template(
      "{game} {date} {time}", {{"game", "Hades II"}, {"date", "2026-07-11"}, {"time", "21-04"}});
  CHECK_EQ(out, "Hades II 2026-07-11 21-04");
}

TEST(template_expansion_keeps_unknown_placeholders_visible) {
  const auto out = util::expand_template("{game} {oops}", {{"game", "X"}});
  CHECK_EQ(out, "X {oops}");
}

TEST(filename_sanitization) {
  CHECK_EQ(util::sanitize_filename("Half-Life 2: Episode Two"), "Half-Life 2- Episode Two");
  CHECK_EQ(util::sanitize_filename("a<b>c|d?e*f"), "a-b-c-d-e-f");
  CHECK_EQ(util::sanitize_filename("trailing dots..."), "trailing dots");
  CHECK_EQ(util::sanitize_filename("path\\in/name"), "path-in-name");
}
