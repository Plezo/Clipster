#include "clipster/util.hpp"

#include <cctype>

namespace clipster::util {

std::string expand_template(std::string_view tmpl,
                            const std::map<std::string, std::string>& vars) {
  std::string out;
  out.reserve(tmpl.size());
  size_t i = 0;
  while (i < tmpl.size()) {
    if (tmpl[i] == '{') {
      const size_t close = tmpl.find('}', i + 1);
      if (close != std::string_view::npos) {
        const std::string key(tmpl.substr(i + 1, close - i - 1));
        if (auto it = vars.find(key); it != vars.end()) {
          out += it->second;
          i = close + 1;
          continue;
        }
      }
    }
    out += tmpl[i++];
  }
  return out;
}

std::string sanitize_filename(std::string_view name) {
  static constexpr std::string_view kInvalid = "<>:\"/\\|?*";
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    if (kInvalid.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20) {
      out += '-';
    } else {
      out += c;
    }
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
    out.pop_back();
  }
  return out;
}

std::string to_lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace clipster::util
