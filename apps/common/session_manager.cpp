#include "session_manager.hpp"

#include <objbase.h>

#include "clipster/logging.hpp"
#include "clipster/win/steam_locator.hpp"
#include "clipster/win/window_finder.hpp"

namespace clipster::app {

namespace {
constexpr auto kControlTick = std::chrono::seconds(1);
constexpr auto kFindWindowTimeout = std::chrono::seconds(90);
}  // namespace

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

SessionManager::SessionManager(Settings settings, Callbacks callbacks)
    : callbacks_(std::move(callbacks)), settings_(std::move(settings)) {
  matcher_.store(build_matcher(settings_));
  control_thread_ = std::thread([this] { control_loop(); });

  win::ProcessWatcher::Callbacks watcher_callbacks;
  watcher_callbacks.on_started = [this](DWORD pid, const std::string& exe) {
    if (auto matcher = matcher_.load(); matcher && matcher->is_game(exe)) {
      post_event({Event::Kind::GameStarted, pid, exe});
    }
  };
  watcher_callbacks.on_stopped = [this](DWORD pid, const std::string& exe) {
    if (auto matcher = matcher_.load(); matcher && matcher->is_game(exe)) {
      post_event({Event::Kind::GameStopped, pid, exe});
    }
  };
  watcher_ = std::make_unique<win::ProcessWatcher>(std::move(watcher_callbacks));
}

SessionManager::~SessionManager() { stop(); }

void SessionManager::stop() {
  if (watcher_) {
    watcher_->stop();  // no new events past this point
  }
  {
    std::lock_guard lock(events_mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  events_cv_.notify_all();
  if (control_thread_.joinable()) {
    control_thread_.join();
  }
  stop_session(/*game_exited=*/false);
}

void SessionManager::post_event(Event event) {
  {
    std::lock_guard lock(events_mutex_);
    events_.push_back(std::move(event));
  }
  events_cv_.notify_all();
}

Settings SessionManager::settings_copy() const {
  std::lock_guard lock(settings_mutex_);
  return settings_;
}

void SessionManager::update_settings(const Settings& settings) {
  {
    std::lock_guard lock(settings_mutex_);
    settings_ = settings;
  }
  matcher_.store(build_matcher(settings));
  {
    // Buffer length, clip length, output and sounds apply to the running
    // session immediately; encoder/audio topology waits for the next one.
    std::lock_guard lock(session_mutex_);
    if (session_) {
      session_->recorder->update_live_settings(settings);
    }
  }
  log::info("settings updated (encoder/audio settings apply to the next game session)");
}

void SessionManager::save_clip() {
  std::lock_guard lock(session_mutex_);
  if (session_) {
    session_->recorder->save_clip();
  }
}

bool SessionManager::is_recording() const {
  std::lock_guard lock(session_mutex_);
  return session_ != nullptr;
}

std::string SessionManager::current_game() const {
  std::lock_guard lock(session_mutex_);
  return session_ ? session_->game_name : std::string();
}

std::string SessionManager::stats() const {
  std::lock_guard lock(session_mutex_);
  if (!session_) {
    return {};
  }
  std::string out = session_->recorder->stats();
  if (!session_->audio) {
    out += " — audio unavailable (see clipster.log)";
  }
  return out;
}

void SessionManager::control_loop() {
  // WGC (WinRT) objects are created on this thread.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  std::unique_lock lock(events_mutex_);
  while (true) {
    events_cv_.wait_for(lock, kControlTick,
                        [this] { return stopping_ || !events_.empty(); });
    if (stopping_) {
      break;
    }
    while (!events_.empty()) {
      const Event event = std::move(events_.front());
      events_.pop_front();
      lock.unlock();
      handle_event(event);
      lock.lock();
    }
    if (!candidates_.empty()) {
      lock.unlock();
      try_begin_capture();
      lock.lock();
    }
  }
  lock.unlock();
  CoUninitialize();
}

void SessionManager::add_candidate(DWORD pid, std::string exe_path) {
  for (const Candidate& c : candidates_) {
    if (c.pid == pid) {
      return;
    }
  }
  candidates_.push_back(
      {pid, std::move(exe_path), std::chrono::steady_clock::now() + kFindWindowTimeout});
}

void SessionManager::handle_event(const Event& event) {
  switch (event.kind) {
    case Event::Kind::GameStarted: {
      {
        std::lock_guard lock(session_mutex_);
        if (session_) {
          return;  // already recording something
        }
      }
      log::info("game detected: {}", event.exe_path);
      add_candidate(event.pid, event.exe_path);
      try_begin_capture();  // the window may already exist
      break;
    }
    case Event::Kind::GameStopped: {
      std::erase_if(candidates_, [&](const Candidate& c) { return c.pid == event.pid; });
      bool ours = false;
      {
        std::lock_guard lock(session_mutex_);
        ours = session_ && session_->pid == event.pid;
      }
      if (ours) {
        stop_session(/*game_exited=*/true);
      }
      break;
    }
    case Event::Kind::EncoderFailed: {
      stop_session(/*game_exited=*/false);
      if (callbacks_.on_error) {
        callbacks_.on_error("Recording stopped: no usable video encoder. See clipster.log.");
      }
      break;
    }
    case Event::Kind::WindowClosed: {
      // Splash screens and launcher windows get destroyed and replaced by
      // the real game window: tear down and re-queue the process so we
      // attach to whatever it shows next.
      std::string exe;
      {
        std::lock_guard lock(session_mutex_);
        if (!session_ || session_->pid != event.pid) {
          return;  // stale notification from a previous session
        }
        exe = session_->exe_path;
      }
      log::info("capture window closed; waiting for a new window from {}", exe);
      stop_session(/*game_exited=*/false);
      add_candidate(event.pid, exe);  // GameStopped will clear it if the process died
      break;
    }
  }
}

void SessionManager::try_begin_capture() {
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(candidates_, [&](const Candidate& c) {
    if (now <= c.deadline) {
      return false;
    }
    log::warn("{} never showed a window — giving up", c.exe_path);
    return true;
  });

  DWORD pid = 0;
  std::string exe;
  std::optional<win::WindowInfo> window;
  for (const Candidate& c : candidates_) {
    if ((window = win::find_window_by_pid(c.pid))) {
      pid = c.pid;
      exe = c.exe_path;
      break;
    }
  }
  if (!window) {
    return;
  }
  candidates_.clear();  // the winner takes the session; siblings were helpers

  const Settings settings = settings_copy();
  const std::string game = std::filesystem::path(exe).stem().string();

  auto session = std::make_unique<Session>();
  session->pid = pid;
  session->exe_path = exe;
  session->game_name = game;
  session->recorder = std::make_unique<Recorder>(
      settings, game,
      [this, pid, exe] { post_event({Event::Kind::EncoderFailed, pid, exe}); });

  std::string error;
  auto* recorder = session->recorder.get();
  session->capture = win::WgcCapture::create_for_window(
      window->hwnd, [recorder](const win::CapturedFrame& f) { recorder->on_frame(f); }, &error);
  if (!session->capture) {
    log::error("cannot capture {}: {}", game, error);
    if (callbacks_.on_error) {
      callbacks_.on_error("Could not record " + game + ": " + error);
    }
    return;
  }
  session->capture->set_on_closed(
      [this, pid, exe] { post_event({Event::Kind::WindowClosed, pid, exe}); });

  std::string audio_error;
  session->audio = AudioPipeline::create(settings, session->pid, *recorder, &audio_error);
  if (!session->audio) {
    log::warn("recording without audio: {}", audio_error);
  }

  session->capture->start();
  if (session->audio) {
    session->audio->start();
  }
  log::info("recording {} (pid {})", game, session->pid);
  {
    std::lock_guard lock(session_mutex_);
    session_ = std::move(session);
  }
  if (callbacks_.on_recording_started) {
    callbacks_.on_recording_started(game);
  }
}

void SessionManager::stop_session(bool game_exited) {
  std::unique_ptr<Session> session;
  {
    std::lock_guard lock(session_mutex_);
    session = std::move(session_);
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
  if (callbacks_.on_recording_stopped) {
    callbacks_.on_recording_stopped(session->game_name, game_exited);
  }
}

}  // namespace clipster::app
