// clipster CLI — capture one window picked by title substring. Mainly a
// development/testing harness; the tray app (clipster-tray) is the way to
// actually use Clipster.
//
//   clipster --list-windows
//   clipster --window "Hades"
//   clipster --window "Hades" --clip-seconds 60

#include <winrt/base.h>

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#include "clipster/logging.hpp"
#include "clipster/settings.hpp"
#include "clipster/win/hotkey_manager.hpp"
#include "clipster/win/known_folders.hpp"
#include "clipster/win/wgc_capture.hpp"
#include "clipster/win/window_finder.hpp"
#include "recorder.hpp"

namespace {

using namespace clipster;

std::mutex g_stop_mutex;
std::condition_variable g_stop_cv;
bool g_stop = false;

void request_stop() {
  {
    std::lock_guard lock(g_stop_mutex);
    g_stop = true;
  }
  g_stop_cv.notify_all();
}

void print_usage() {
  std::fprintf(stderr,
               "Usage:\n"
               "  clipster --list-windows\n"
               "  clipster --window <title substring> [--clip-seconds N] [--verbose]\n");
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);  // window titles and paths are UTF-8

  std::string window_needle;
  int clip_seconds_override = 0;
  bool list_windows = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--list-windows") {
      list_windows = true;
    } else if (arg == "--window" && i + 1 < argc) {
      window_needle = argv[++i];
    } else if (arg == "--clip-seconds" && i + 1 < argc) {
      clip_seconds_override = std::atoi(argv[++i]);
    } else if (arg == "--verbose") {
      log::set_level(log::Level::Debug);
    } else {
      print_usage();
      return 2;
    }
  }

  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  if (list_windows) {
    for (const auto& w : win::list_capturable_windows()) {
      std::fprintf(stdout, "%6lu  %-50.50s  %s\n", static_cast<unsigned long>(w.pid),
                   w.title.c_str(), w.exe_path.c_str());
    }
    return 0;
  }
  if (window_needle.empty()) {
    print_usage();
    return 2;
  }

  // Shared with the tray app so all frontends see the same configuration.
  const auto settings_path = win::app_data_dir() / "settings.json";
  std::string warning;
  Settings settings = Settings::load_or_default(settings_path, &warning);
  if (!warning.empty()) {
    log::warn("{}", warning);
  }
  if (!std::filesystem::exists(settings_path)) {
    std::string error;
    if (settings.save(settings_path, &error)) {
      log::info("wrote default settings to {}", settings_path.string());
    } else {
      log::warn("could not write default settings: {}", error);
    }
  }
  if (clip_seconds_override > 0) {
    settings.clip.default_length_seconds = clip_seconds_override;
    settings.clamp();
  }

  const auto target = win::find_window_by_title(window_needle);
  if (!target) {
    log::error("no visible window matching '{}' — try --list-windows", window_needle);
    return 1;
  }
  const std::string game_name =
      target->exe_path.empty()
          ? "Unknown"
          : std::filesystem::path(target->exe_path).stem().string();
  log::info("capturing '{}' ({})", target->title, game_name);

  app::Recorder recorder(settings, game_name, [] { request_stop(); });

  std::string error;
  auto capture = win::WgcCapture::create_for_window(
      target->hwnd, [&recorder](const win::CapturedFrame& f) { recorder.on_frame(f); }, &error);
  if (!capture) {
    log::error("capture setup failed: {}", error);
    return 1;
  }

  win::HotkeyManager hotkeys;
  if (!hotkeys.register_hotkey(
          settings.hotkeys.save_clip, [&recorder] { recorder.save_clip(); }, &error)) {
    log::error("{}", error);
    return 1;
  }

  SetConsoleCtrlHandler(
      [](DWORD) -> BOOL {
        request_stop();
        return TRUE;
      },
      TRUE);

  capture->start();
  log::info("recording — press {} to save the last {}s, Ctrl+C to quit",
            settings.hotkeys.save_clip, settings.clip.default_length_seconds);

  // Periodic status until Ctrl+C.
  std::unique_lock lock(g_stop_mutex);
  while (!g_stop_cv.wait_for(lock, std::chrono::seconds(15), [] { return g_stop; })) {
    log::info("{}", recorder.stats());
  }
  lock.unlock();

  capture->stop();
  hotkeys.stop();
  recorder.finish();  // let in-flight clip writes complete
  log::info("bye");
  return 0;
}
