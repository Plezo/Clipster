// clipster-tray — the background application.
//
// Lives in the system tray, watches for game launches (Steam libraries are
// auto-detected; more folders/exes via settings.json), records the active
// game into the replay buffer, and saves clips on the global hotkey.
// Right-click the tray icon for everything else.

#include <winrt/base.h>

#include <windows.h>

#include <shellapi.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "clipster/game_matcher.hpp"
#include "clipster/logging.hpp"
#include "clipster/settings.hpp"
#include "clipster/win/gamepad_hotkey.hpp"
#include "clipster/win/hotkey_manager.hpp"
#include "clipster/win/known_folders.hpp"
#include "clipster/win/process_watcher.hpp"
#include "clipster/win/steam_locator.hpp"
#include "clipster/win/str_util.hpp"
#include "clipster/win/wgc_capture.hpp"
#include "clipster/win/window_finder.hpp"
#include "audio_pipeline.hpp"
#include "recorder.hpp"

namespace {

using namespace clipster;

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_EVENT = WM_APP + 2;
constexpr UINT_PTR kFindWindowTimer = 1;
constexpr UINT_PTR kSettingsWatchTimer = 2;
constexpr int kFindWindowMaxTicks = 90;  // give slow games 90 s to open a window

constexpr UINT IDM_STATUS = 100;
constexpr UINT IDM_SAVE_CLIP = 101;
constexpr UINT IDM_OPEN_FOLDER = 102;
constexpr UINT IDM_OPEN_SETTINGS = 103;
constexpr UINT IDM_AUTOSTART = 104;
constexpr UINT IDM_QUIT = 105;
constexpr UINT IDM_OPEN_UI = 106;

struct GameEvent {
  enum class Kind { Started, Stopped, EncoderFailed } kind;
  DWORD pid = 0;
  std::string exe_path;
};

// All mutated on the main thread unless noted.
Settings g_settings;
std::filesystem::path g_settings_path;
std::filesystem::file_time_type g_settings_mtime{};
// Read by the watcher thread, atomically swapped on settings reload.
std::atomic<std::shared_ptr<const GameMatcher>> g_matcher;
std::unique_ptr<win::HotkeyManager> g_hotkeys;
std::unique_ptr<win::GamepadHotkey> g_gamepad;
HWND g_hwnd = nullptr;

std::mutex g_events_mutex;
std::deque<GameEvent> g_events;  // watcher/capture threads -> main thread

// The active recording session. The mutex makes the hotkey thread's
// save_clip safe against the main thread tearing the session down.
struct Session {
  DWORD pid = 0;
  std::string game_name;
  std::unique_ptr<app::Recorder> recorder;
  std::unique_ptr<win::WgcCapture> capture;
  std::unique_ptr<app::AudioPipeline> audio;  // may be null (audio failed)
};
std::mutex g_session_mutex;
std::unique_ptr<Session> g_session;

// A game process that started but has not shown a window yet.
DWORD g_pending_pid = 0;
std::string g_pending_exe;
int g_pending_ticks = 0;

NOTIFYICONDATAW g_nid{};

void post_event(GameEvent ev) {
  {
    std::lock_guard lock(g_events_mutex);
    g_events.push_back(std::move(ev));
  }
  PostMessageW(g_hwnd, WM_APP_EVENT, 0, 0);
}

std::string game_name_from_path(const std::string& exe_path) {
  return std::filesystem::path(exe_path).stem().string();
}

void update_tooltip(const std::wstring& text) {
  wcsncpy_s(g_nid.szTip, text.c_str(), _TRUNCATE);
  g_nid.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void show_balloon(const std::wstring& title, const std::wstring& text) {
  g_nid.uFlags = NIF_INFO;
  g_nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
  wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

std::filesystem::path clips_dir() {
  return g_settings.output.save_dir.empty()
             ? win::default_save_dir()
             : std::filesystem::path(g_settings.output.save_dir);
}

// --- autostart (HKCU Run key) -----------------------------------------------

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Clipster";

bool autostart_enabled() {
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr,
                      nullptr) == ERROR_SUCCESS;
}

void set_autostart(bool enable) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  if (enable) {
    wchar_t exe[MAX_PATH * 2];
    const DWORD len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const std::wstring cmd = L"\"" + std::wstring(exe, len) + L"\"";
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                   static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

// --- settings loading / hot reload (main thread) ------------------------------

std::shared_ptr<const GameMatcher> build_matcher(const Settings& settings) {
  GameMatcherConfig cfg;
  cfg.watched_folders = settings.games.watched_folders;
  cfg.manual_exes = settings.games.manual_exes;
  cfg.ignored_exes = settings.games.ignored_exes;
  if (settings.games.auto_detect_steam) {
    for (auto& dir : win::steam_library_common_dirs()) {
      cfg.watched_folders.push_back(std::move(dir));
    }
  }
  if (cfg.watched_folders.empty() && cfg.manual_exes.empty()) {
    log::warn("no Steam libraries found and no folders configured — "
              "add watched folders or executables in Settings");
  }
  return std::make_shared<const GameMatcher>(std::move(cfg));
}

void register_save_hotkeys() {
  const auto save = [] {
    std::lock_guard lock(g_session_mutex);
    if (g_session) {
      g_session->recorder->save_clip();
    }
  };

  g_hotkeys = std::make_unique<win::HotkeyManager>();
  std::string error;
  if (!g_hotkeys->register_hotkey(g_settings.hotkeys.save_clip, save, &error)) {
    log::error("{}", error);
    show_balloon(L"Clipster", win::widen("Hotkey unavailable: " + error));
  }

  g_gamepad.reset();
  if (!g_settings.hotkeys.controller_save_clip.empty()) {
    g_gamepad = win::GamepadHotkey::create(g_settings.hotkeys.controller_save_clip, save, &error);
    if (!g_gamepad) {
      log::warn("{}", error);
    }
  }
}

std::filesystem::file_time_type settings_mtime() {
  std::error_code ec;
  return std::filesystem::last_write_time(g_settings_path, ec);
}

void reload_settings_if_changed() {
  const auto mtime = settings_mtime();
  if (mtime == g_settings_mtime) {
    return;
  }
  g_settings_mtime = mtime;

  std::string warning;
  Settings fresh = Settings::load_or_default(g_settings_path, &warning);
  if (!warning.empty()) {
    log::warn("{}", warning);
    return;  // keep the last good settings
  }
  const bool hotkeys_changed =
      fresh.hotkeys.save_clip != g_settings.hotkeys.save_clip ||
      fresh.hotkeys.controller_save_clip != g_settings.hotkeys.controller_save_clip;
  g_settings = std::move(fresh);
  g_matcher.store(build_matcher(g_settings));
  if (hotkeys_changed) {
    register_save_hotkeys();
  }
  log::info("settings reloaded (recording settings apply to the next game session)");
}

// --- session lifecycle (main thread) ----------------------------------------

void stop_session(bool game_exited) {
  std::unique_ptr<Session> session;
  {
    std::lock_guard lock(g_session_mutex);
    session = std::move(g_session);
  }
  if (!session) {
    return;
  }
  session->capture->stop();
  if (session->audio) {
    session->audio->stop();
  }
  session->recorder->finish();
  log::info("session ended: {} ({})", session->game_name,
            game_exited ? "game exited" : "stopped");
  update_tooltip(L"Clipster — waiting for a game");
  if (game_exited) {
    show_balloon(L"Clipster", win::widen("Stopped recording " + session->game_name));
  }
}

void try_begin_capture() {
  const auto window = win::find_window_by_pid(g_pending_pid);
  if (!window) {
    if (++g_pending_ticks > kFindWindowMaxTicks) {
      log::warn("{} never showed a window — giving up", g_pending_exe);
      KillTimer(g_hwnd, kFindWindowTimer);
      g_pending_pid = 0;
    }
    return;
  }
  KillTimer(g_hwnd, kFindWindowTimer);

  const std::string game = game_name_from_path(g_pending_exe);
  auto session = std::make_unique<Session>();
  session->pid = g_pending_pid;
  session->game_name = game;
  session->recorder = std::make_unique<app::Recorder>(
      g_settings, game,
      [pid = g_pending_pid, exe = g_pending_exe] {
        post_event({GameEvent::Kind::EncoderFailed, pid, exe});
      });
  g_pending_pid = 0;

  std::string error;
  auto* recorder = session->recorder.get();
  session->capture = win::WgcCapture::create_for_window(
      window->hwnd, [recorder](const win::CapturedFrame& f) { recorder->on_frame(f); }, &error);
  if (!session->capture) {
    log::error("cannot capture {}: {}", game, error);
    show_balloon(L"Clipster", win::widen("Could not record " + game + ": " + error));
    return;
  }

  std::string audio_error;
  session->audio =
      app::AudioPipeline::create(g_settings, session->pid, *session->recorder, &audio_error);
  if (!session->audio) {
    log::warn("recording without audio: {}", audio_error);
  }

  session->capture->start();
  if (session->audio) {
    session->audio->start();
  }
  log::info("recording {} (pid {})", game, session->pid);
  update_tooltip(win::widen("Clipster — recording " + game));
  show_balloon(L"Clipster", win::widen("Recording " + game + " — " +
                                       g_settings.hotkeys.save_clip + " saves the last " +
                                       std::to_string(g_settings.clip.default_length_seconds) +
                                       "s"));
  std::lock_guard lock(g_session_mutex);
  g_session = std::move(session);
}

void handle_event(const GameEvent& ev) {
  switch (ev.kind) {
    case GameEvent::Kind::Started: {
      {
        std::lock_guard lock(g_session_mutex);
        if (g_session) {
          return;  // already recording something
        }
      }
      if (g_pending_pid != 0) {
        return;  // already waiting on a launching game
      }
      log::info("game detected: {}", ev.exe_path);
      g_pending_pid = ev.pid;
      g_pending_exe = ev.exe_path;
      g_pending_ticks = 0;
      try_begin_capture();  // the window may already exist
      if (g_pending_pid != 0) {
        SetTimer(g_hwnd, kFindWindowTimer, 1000, nullptr);
      }
      break;
    }
    case GameEvent::Kind::Stopped: {
      if (g_pending_pid == ev.pid) {
        KillTimer(g_hwnd, kFindWindowTimer);
        g_pending_pid = 0;
        return;
      }
      bool ours = false;
      {
        std::lock_guard lock(g_session_mutex);
        ours = g_session && g_session->pid == ev.pid;
      }
      if (ours) {
        stop_session(/*game_exited=*/true);
      }
      break;
    }
    case GameEvent::Kind::EncoderFailed: {
      stop_session(/*game_exited=*/false);
      show_balloon(L"Clipster",
                   L"Recording stopped: no usable video encoder. See clipster.log.");
      break;
    }
  }
}

// --- tray menu ----------------------------------------------------------------

void show_menu() {
  HMENU menu = CreatePopupMenu();

  std::wstring status = L"Waiting for a game";
  bool recording = false;
  {
    std::lock_guard lock(g_session_mutex);
    if (g_session) {
      recording = true;
      status = win::widen("Recording " + g_session->game_name);
    }
  }
  AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (recording ? 0 : MF_GRAYED), IDM_SAVE_CLIP,
              win::widen("Save clip (" + g_settings.hotkeys.save_clip + ")").c_str());
  AppendMenuW(menu, MF_STRING, IDM_OPEN_FOLDER, L"Open clips folder");
  AppendMenuW(menu, MF_STRING, IDM_OPEN_UI, L"Settings…");
  AppendMenuW(menu, MF_STRING, IDM_OPEN_SETTINGS, L"Edit settings file");
  AppendMenuW(menu, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), IDM_AUTOSTART,
              L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

  // Required so the menu closes when the user clicks elsewhere.
  SetForegroundWindow(g_hwnd);
  POINT pt;
  GetCursorPos(&pt);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
  DestroyMenu(menu);
}

void save_clip_now() {
  std::lock_guard lock(g_session_mutex);
  if (g_session) {
    g_session->recorder->save_clip();
  }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_APP_TRAY:
      if (lparam == WM_RBUTTONUP) {
        show_menu();
      } else if (lparam == WM_LBUTTONDBLCLK) {
        ShellExecuteW(nullptr, L"open", clips_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      return 0;

    case WM_APP_EVENT: {
      std::deque<GameEvent> events;
      {
        std::lock_guard lock(g_events_mutex);
        events.swap(g_events);
      }
      for (const auto& ev : events) {
        handle_event(ev);
      }
      return 0;
    }

    case WM_TIMER:
      if (wparam == kFindWindowTimer && g_pending_pid != 0) {
        try_begin_capture();
      } else if (wparam == kSettingsWatchTimer) {
        reload_settings_if_changed();
      }
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case IDM_SAVE_CLIP:
          save_clip_now();
          break;
        case IDM_OPEN_FOLDER: {
          std::error_code ec;
          std::filesystem::create_directories(clips_dir(), ec);
          ShellExecuteW(nullptr, L"open", clips_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
        }
        case IDM_OPEN_UI: {
          const auto ui = app::executable_dir() / L"clipster-settings.exe";
          std::error_code ec;
          if (std::filesystem::exists(ui, ec)) {
            ShellExecuteW(nullptr, L"open", ui.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          } else {
            ShellExecuteW(nullptr, L"open", g_settings_path.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
          }
          break;
        }
        case IDM_OPEN_SETTINGS:
          ShellExecuteW(nullptr, L"open", g_settings_path.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
          break;
        case IDM_AUTOSTART:
          set_autostart(!autostart_enabled());
          break;
        case IDM_QUIT:
          DestroyWindow(hwnd);
          break;
      }
      return 0;

    case WM_DESTROY:
      Shell_NotifyIconW(NIM_DELETE, &g_nid);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Single instance: two Clipsters fighting over the same hotkey and game
  // would be chaos.
  CreateMutexW(nullptr, TRUE, L"Local\\ClipsterTrayMutex");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Clipster is already running — look for it in the system tray.",
                L"Clipster", MB_OK | MB_ICONINFORMATION);
    return 0;
  }

  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  const auto app_dir = win::app_data_dir();
  log::set_file(app_dir / L"clipster.log");
  log::info("--- clipster-tray starting ---");

  g_settings_path = app_dir / L"settings.json";
  std::string warning;
  g_settings = Settings::load_or_default(g_settings_path, &warning);
  if (!warning.empty()) {
    log::warn("{}", warning);
  }
  if (!std::filesystem::exists(g_settings_path)) {
    g_settings.save(g_settings_path);
  }

  g_settings_mtime = settings_mtime();
  g_matcher.store(build_matcher(g_settings));

  // Hidden message window + tray icon.
  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"ClipsterTrayWnd";
  RegisterClassW(&wc);
  g_hwnd = CreateWindowW(wc.lpszClassName, L"Clipster", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                         instance, nullptr);

  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = g_hwnd;
  g_nid.uID = 1;
  g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  g_nid.uCallbackMessage = WM_APP_TRAY;
  g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);  // TODO: real logo .ico
  wcsncpy_s(g_nid.szTip, L"Clipster — waiting for a game", _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &g_nid);

  // Global hotkey (works over fullscreen games) + controller combo.
  register_save_hotkeys();

  // Pick up saves from the settings UI (or hand edits) automatically.
  SetTimer(g_hwnd, kSettingsWatchTimer, 3000, nullptr);

  // Watch for game processes. The matcher filter runs on the watcher
  // thread; only interesting events reach the main thread.
  win::ProcessWatcher::Callbacks callbacks;
  callbacks.on_started = [](DWORD pid, const std::string& exe) {
    if (auto matcher = g_matcher.load(); matcher && matcher->is_game(exe)) {
      post_event({GameEvent::Kind::Started, pid, exe});
    }
  };
  callbacks.on_stopped = [](DWORD pid, const std::string& exe) {
    if (auto matcher = g_matcher.load(); matcher && matcher->is_game(exe)) {
      post_event({GameEvent::Kind::Stopped, pid, exe});
    }
  };
  win::ProcessWatcher watcher(std::move(callbacks));

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  watcher.stop();
  if (g_hotkeys) {
    g_hotkeys->stop();
  }
  if (g_gamepad) {
    g_gamepad->stop();
  }
  stop_session(/*game_exited=*/false);
  log::info("--- clipster-tray exiting ---");
  return 0;
}
