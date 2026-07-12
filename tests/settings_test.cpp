#include "clipster/settings.hpp"

#include "test_framework.hpp"

using namespace clipster;

TEST(settings_defaults_are_sane) {
  Settings s;
  s.clamp();
  CHECK_EQ(s.recording.fps, 60);
  CHECK_EQ(s.recording.buffer_seconds, 120);
  CHECK_EQ(s.audio.mode, "include_list");
  CHECK(s.audio.include_game);
  CHECK_EQ(s.audio.include_apps.size(), 1u);
  CHECK_EQ(s.audio.include_apps[0], "Discord.exe");
}

TEST(settings_round_trip_through_json) {
  Settings s;
  s.recording.fps = 30;
  s.recording.codec = "hevc";
  s.recording.capture_window_frame = true;
  s.audio.mode = "desktop_exclude";
  s.audio.exclude_apps = {"Spotify.exe", "chrome.exe"};
  s.audio.microphone.enabled = true;
  s.audio.microphone.device = "Blue Yeti";
  s.audio.microphone.separate_track = false;
  s.games.manual_exes = {"C:\\Games\\retro\\emu.exe"};
  s.hotkeys.save_clip = "Ctrl+Shift+F9";
  s.notifications.sound_enabled = false;
  s.notifications.sound_file = "C:\\sounds\\ding.wav";

  const Settings back = Settings::from_json_string(s.to_json_string());
  CHECK_EQ(back.recording.fps, 30);
  CHECK_EQ(back.recording.codec, "hevc");
  CHECK(back.recording.capture_window_frame);
  CHECK_EQ(back.audio.mode, "desktop_exclude");
  CHECK_EQ(back.audio.exclude_apps.size(), 2u);
  CHECK(back.audio.microphone.enabled);
  CHECK_EQ(back.audio.microphone.device, "Blue Yeti");
  CHECK(!back.audio.microphone.separate_track);
  CHECK_EQ(back.games.manual_exes[0], "C:\\Games\\retro\\emu.exe");
  CHECK_EQ(back.hotkeys.save_clip, "Ctrl+Shift+F9");
  CHECK(!back.notifications.sound_enabled);
  CHECK_EQ(back.notifications.sound_file, "C:\\sounds\\ding.wav");
}

TEST(settings_missing_and_unknown_keys_are_tolerated) {
  const Settings s = Settings::from_json_string(
      R"({"version": 1, "recording": {"fps": 120}, "some_future_section": {"x": 1}})");
  CHECK_EQ(s.recording.fps, 120);
  CHECK_EQ(s.recording.bitrate_kbps, 20000);  // default preserved
  CHECK_EQ(s.audio.mode, "include_list");
}

TEST(settings_wrong_types_fall_back_to_defaults) {
  const Settings s = Settings::from_json_string(
      R"({"recording": {"fps": "fast"}, "audio": {"include_apps": 7}})");
  CHECK_EQ(s.recording.fps, 60);
  CHECK_EQ(s.audio.include_apps.size(), 1u);
}

TEST(settings_out_of_range_values_are_clamped) {
  const Settings s = Settings::from_json_string(
      R"({"recording": {"fps": 999, "bitrate_kbps": 5, "buffer_seconds": 100000},
          "clip": {"default_length_seconds": 100000},
          "audio": {"mode": "telepathy", "sample_rate": 1234}})");
  CHECK_EQ(s.recording.fps, 240);
  CHECK_EQ(s.recording.bitrate_kbps, 1000);
  CHECK_EQ(s.recording.buffer_seconds, 600);
  CHECK_EQ(s.clip.default_length_seconds, 600);  // clamped to buffer length
  CHECK_EQ(s.audio.mode, "desktop");
  CHECK_EQ(s.audio.sample_rate, 48000);
}

TEST(settings_load_missing_file_returns_defaults) {
  std::string warning;
  const Settings s = Settings::load_or_default("does/not/exist.json", &warning);
  CHECK_EQ(s.recording.fps, 60);
  CHECK(warning.empty());
}
