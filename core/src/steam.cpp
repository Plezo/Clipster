#include "clipster/steam.hpp"

#include "clipster/util.hpp"

namespace clipster::steam {

namespace {

// Reads the next double-quoted token starting at or after `pos`.
// Returns false when no further token exists. VDF escapes backslashes and
// quotes with a backslash.
bool next_quoted_token(std::string_view text, size_t& pos, std::string& out) {
  const size_t open = text.find('"', pos);
  if (open == std::string_view::npos) {
    return false;
  }
  out.clear();
  size_t i = open + 1;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      out += text[i + 1];
      i += 2;
      continue;
    }
    if (c == '"') {
      pos = i + 1;
      return true;
    }
    out += c;
    ++i;
  }
  return false;  // unterminated string
}

}  // namespace

std::vector<std::string> parse_library_folders(std::string_view vdf_text) {
  std::vector<std::string> paths;
  size_t pos = 0;
  std::string token;
  bool value_is_path = false;
  while (next_quoted_token(vdf_text, pos, token)) {
    if (value_is_path) {
      paths.push_back(token);
      value_is_path = false;
    } else if (util::to_lower_ascii(token) == "path") {
      value_is_path = true;
    }
  }
  return paths;
}

std::vector<std::string> library_common_dirs(std::string_view vdf_text) {
  std::vector<std::string> dirs = parse_library_folders(vdf_text);
  for (auto& d : dirs) {
    const char sep = d.find('\\') != std::string::npos ? '\\' : '/';
    if (!d.empty() && d.back() != sep) {
      d += sep;
    }
    d += "steamapps";
    d += sep;
    d += "common";
  }
  return dirs;
}

}  // namespace clipster::steam
