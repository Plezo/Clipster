#include "clipster/settings.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "clipster/logging.hpp"

namespace clipster {

using nlohmann::json;

namespace {

template <typename T>
void read(const json& j, const char* key, T& out) {
  if (auto it = j.find(key); it != j.end()) {
    try {
      out = it->get<T>();
    } catch (const json::exception&) {
      log::warn("settings: ignoring '{}' (wrong type)", key);
    }
  }
}

}  // namespace

const std::vector<std::string>& AudioSettings::valid_modes() {
  static const std::vector<std::string> modes{"desktop", "game_only", "include_list",
                                              "desktop_exclude"};
  return modes;
}

// --- serialization ---------------------------------------------------------

static json to_json(const Settings& s) {
  return json{
      {"version", s.version},
      {"recording",
       {{"fps", s.recording.fps},
        {"bitrate_kbps", s.recording.bitrate_kbps},
        {"codec", s.recording.codec},
        {"buffer_seconds", s.recording.buffer_seconds}}},
      {"output",
       {{"save_dir", s.output.save_dir},
        {"filename_template", s.output.filename_template},
        {"subfolder_per_game", s.output.subfolder_per_game}}},
      {"clip",
       {{"default_length_seconds", s.clip.default_length_seconds},
        {"quick_lengths", s.clip.quick_lengths}}},
      {"audio",
       {{"mode", s.audio.mode},
        {"include_game", s.audio.include_game},
        {"include_apps", s.audio.include_apps},
        {"exclude_apps", s.audio.exclude_apps},
        {"sample_rate", s.audio.sample_rate},
        {"bitrate_kbps", s.audio.bitrate_kbps},
        {"microphone",
         {{"enabled", s.audio.microphone.enabled},
          {"device", s.audio.microphone.device},
          {"separate_track", s.audio.microphone.separate_track}}}}},
      {"games",
       {{"auto_detect_steam", s.games.auto_detect_steam},
        {"watched_folders", s.games.watched_folders},
        {"manual_exes", s.games.manual_exes},
        {"ignored_exes", s.games.ignored_exes}}},
      {"hotkeys",
       {{"save_clip", s.hotkeys.save_clip},
        {"controller_save_clip", s.hotkeys.controller_save_clip}}},
      {"notifications",
       {{"sound_enabled", s.notifications.sound_enabled},
        {"sound_file", s.notifications.sound_file}}},
  };
}

static void from_json(const json& j, Settings& s) {
  read(j, "version", s.version);
  if (auto it = j.find("recording"); it != j.end() && it->is_object()) {
    read(*it, "fps", s.recording.fps);
    read(*it, "bitrate_kbps", s.recording.bitrate_kbps);
    read(*it, "codec", s.recording.codec);
    read(*it, "buffer_seconds", s.recording.buffer_seconds);
  }
  if (auto it = j.find("output"); it != j.end() && it->is_object()) {
    read(*it, "save_dir", s.output.save_dir);
    read(*it, "filename_template", s.output.filename_template);
    read(*it, "subfolder_per_game", s.output.subfolder_per_game);
  }
  if (auto it = j.find("clip"); it != j.end() && it->is_object()) {
    read(*it, "default_length_seconds", s.clip.default_length_seconds);
    read(*it, "quick_lengths", s.clip.quick_lengths);
  }
  if (auto it = j.find("audio"); it != j.end() && it->is_object()) {
    read(*it, "mode", s.audio.mode);
    read(*it, "include_game", s.audio.include_game);
    read(*it, "include_apps", s.audio.include_apps);
    read(*it, "exclude_apps", s.audio.exclude_apps);
    read(*it, "sample_rate", s.audio.sample_rate);
    read(*it, "bitrate_kbps", s.audio.bitrate_kbps);
    if (auto mic = it->find("microphone"); mic != it->end() && mic->is_object()) {
      read(*mic, "enabled", s.audio.microphone.enabled);
      read(*mic, "device", s.audio.microphone.device);
      read(*mic, "separate_track", s.audio.microphone.separate_track);
    }
  }
  if (auto it = j.find("games"); it != j.end() && it->is_object()) {
    read(*it, "auto_detect_steam", s.games.auto_detect_steam);
    read(*it, "watched_folders", s.games.watched_folders);
    read(*it, "manual_exes", s.games.manual_exes);
    read(*it, "ignored_exes", s.games.ignored_exes);
  }
  if (auto it = j.find("hotkeys"); it != j.end() && it->is_object()) {
    read(*it, "save_clip", s.hotkeys.save_clip);
    read(*it, "controller_save_clip", s.hotkeys.controller_save_clip);
  }
  if (auto it = j.find("notifications"); it != j.end() && it->is_object()) {
    read(*it, "sound_enabled", s.notifications.sound_enabled);
    read(*it, "sound_file", s.notifications.sound_file);
  }
}

// --- validation -------------------------------------------------------------

void Settings::clamp() {
  recording.fps = std::clamp(recording.fps, 15, 240);
  recording.bitrate_kbps = std::clamp(recording.bitrate_kbps, 1000, 150000);
  recording.buffer_seconds = std::clamp(recording.buffer_seconds, 5, 600);
  if (recording.codec != "auto" && recording.codec != "h264" && recording.codec != "hevc") {
    log::warn("settings: unknown codec '{}', using 'auto'", recording.codec);
    recording.codec = "auto";
  }

  clip.default_length_seconds =
      std::clamp(clip.default_length_seconds, 5, recording.buffer_seconds);
  std::erase_if(clip.quick_lengths,
                [&](int l) { return l < 5 || l > recording.buffer_seconds; });

  const auto& modes = AudioSettings::valid_modes();
  if (std::find(modes.begin(), modes.end(), audio.mode) == modes.end()) {
    log::warn("settings: unknown audio mode '{}', using 'desktop'", audio.mode);
    audio.mode = "desktop";
  }
  audio.sample_rate = (audio.sample_rate == 44100) ? 44100 : 48000;
  audio.bitrate_kbps = std::clamp(audio.bitrate_kbps, 64, 320);
}

// --- I/O ---------------------------------------------------------------------

std::string Settings::to_json_string() const { return to_json(*this).dump(2) + "\n"; }

Settings Settings::from_json_string(const std::string& json_text) {
  Settings s;
  from_json(json::parse(json_text), s);
  s.clamp();
  return s;
}

Settings Settings::load_or_default(const std::filesystem::path& path, std::string* warning) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    Settings s;
    s.clamp();
    return s;
  }

  std::ifstream in(path, std::ios::binary);
  std::stringstream buf;
  buf << in.rdbuf();
  try {
    return from_json_string(buf.str());
  } catch (const json::exception& e) {
    if (warning) {
      *warning = "settings file is not valid JSON (" + std::string(e.what()) +
                 "); using defaults without overwriting it";
    }
    Settings s;
    s.clamp();
    return s;
  }
}

bool Settings::save(const std::filesystem::path& path, std::string* error) const {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }

  // Write to a sibling temp file first so a crash mid-write can never
  // truncate the real settings file.
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "cannot open " + tmp + " for writing";
      return false;
    }
    out << to_json_string();
    if (!out.good()) {
      if (error) *error = "write failed for " + tmp;
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    if (error) *error = "rename to " + path.string() + " failed: " + ec.message();
    return false;
  }
  return true;
}

}  // namespace clipster
