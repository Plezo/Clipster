#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace clipster {

struct RecordingSettings {
  int fps = 60;
  int bitrate_kbps = 20000;
  // "auto" probes hardware encoders (NVENC -> AMF -> QSV) and falls back to
  // software. "h264" / "hevc" force a codec family.
  std::string codec = "auto";
};

struct OutputSettings {
  // Empty means "<Videos>/Clipster" resolved at runtime. Clips always go
  // into a per-game subfolder underneath.
  std::string save_dir;
  // Available placeholders: {game} {date} {time}
  std::string filename_template = "{game} {date} {time}";
};

struct ClipSettings {
  // Seconds saved when the hotkey fires. This is also the replay window
  // kept in RAM (cost ≈ seconds * bitrate_kbps / 8 KB: 30 s @ 20 Mbps
  // ~= 75 MB), so one number answers "how far back can I clip?".
  int default_length_seconds = 30;
};

// How audio sources are selected while a game session is active.
//
//   desktop         - everything the system plays
//   game_only       - only the detected game's process tree
//   include_list    - the game (if include_game) plus the apps listed in
//                     include_apps, e.g. Discord, mixed together
//   desktop_exclude - everything EXCEPT the apps listed in exclude_apps,
//                     e.g. keep Spotify out of your clips
//
// Implemented with WASAPI process loopback on Windows (10 2004+); on older
// systems the app degrades to "desktop" with a warning.
// The microphone is always written as its own audio track (titled
// "Microphone") so voice can be balanced or muted after the fact.
struct MicrophoneSettings {
  bool enabled = false;
  // Friendly-name substring, or "default" for the system communications mic.
  std::string device = "default";
};

struct AudioSettings {
  std::string mode = "include_list";
  bool include_game = true;
  std::vector<std::string> include_apps{"Discord.exe"};
  std::vector<std::string> exclude_apps{"Spotify.exe"};
  int sample_rate = 48000;
  int bitrate_kbps = 160;
  MicrophoneSettings microphone;

  static const std::vector<std::string>& valid_modes();
};

struct GameDetectionSettings {
  // Parse Steam's libraryfolders.vdf and watch every steamapps/common dir.
  bool auto_detect_steam = true;
  // Additional folders whose executables count as games (Epic, GOG, ...).
  std::vector<std::string> watched_folders;
  // Executables that are always treated as games regardless of location.
  std::vector<std::string> manual_exes;
  // Basenames that never count (launchers, crash handlers). Merged with a
  // built-in default list; see GameMatcher.
  std::vector<std::string> ignored_exes;
};

struct HotkeySettings {
  std::string save_clip = "Ctrl+Del";
  // XInput button combo, "+"-separated (wired up in a later milestone).
  std::string controller_save_clip = "Back+RB";
};

struct NotificationSettings {
  bool sound_enabled = true;
  // Custom .wav to play when a clip is saved; empty uses the bundled
  // assets/clip_saved.wav next to the executable.
  std::string sound_file;
};

struct Settings {
  int version = 1;
  RecordingSettings recording;
  OutputSettings output;
  ClipSettings clip;
  AudioSettings audio;
  GameDetectionSettings games;
  HotkeySettings hotkeys;
  NotificationSettings notifications;

  // Loads settings from `path`. Returns defaults if the file does not
  // exist; on a malformed file, returns defaults and sets *warning.
  // Out-of-range values are clamped, unknown keys ignored, missing keys
  // defaulted — so the file survives app upgrades in both directions.
  static Settings load_or_default(const std::filesystem::path& path,
                                  std::string* warning = nullptr);

  // Serializes to pretty JSON, creating parent directories as needed.
  bool save(const std::filesystem::path& path, std::string* error = nullptr) const;

  std::string to_json_string() const;
  static Settings from_json_string(const std::string& json_text);

  void clamp();
};

}  // namespace clipster
