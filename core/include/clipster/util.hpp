#pragma once

#include <map>
#include <string>
#include <string_view>

namespace clipster::util {

// Replaces {key} placeholders with values from `vars`. Unknown placeholders
// are left as-is so typos in a filename template are visible instead of
// silently vanishing.
std::string expand_template(std::string_view tmpl,
                            const std::map<std::string, std::string>& vars);

// Replaces characters that are invalid in Windows/Linux filenames with '-'
// and trims trailing dots/spaces (invalid on Windows).
std::string sanitize_filename(std::string_view name);

std::string to_lower_ascii(std::string_view s);

}  // namespace clipster::util
