#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_pipeline.hpp"
#include "clipster/game_matcher.hpp"
#include "clipster/settings.hpp"
#include "clipster/win/process_watcher.hpp"
#include "clipster/win/wgc_capture.hpp"
#include "recorder.hpp"

namespace clipster::app {

// Watches for game launches and drives the recording session lifecycle
// (capture + audio + ring buffer) on its own control thread. UI-agnostic:
// frontends observe state via callbacks, which run on the control thread —
// marshal to your UI thread before touching widgets.
class SessionManager {
 public:
  struct Callbacks {
    std::function<void(const std::string& game)> on_recording_started;
    std::function<void(const std::string& game, bool game_exited)> on_recording_stopped;
    std::function<void(const std::string& message)> on_error;
  };

  SessionManager(Settings settings, Callbacks callbacks);
  ~SessionManager();

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  // Applies to game detection immediately and to the NEXT session's
  // recording/audio parameters (an active session keeps its config).
  void update_settings(const Settings& settings);

  // Thread-safe; no-op while idle.
  void save_clip();

  bool is_recording() const;
  std::string current_game() const;   // empty while idle
  std::string stats() const;          // empty while idle

  void stop();

 private:
  struct Event {
    enum class Kind { GameStarted, GameStopped, EncoderFailed, WindowClosed } kind;
    DWORD pid = 0;
    std::string exe_path;
  };

  struct Session {
    DWORD pid = 0;
    std::string exe_path;
    std::string game_name;
    std::unique_ptr<Recorder> recorder;
    std::unique_ptr<win::WgcCapture> capture;
    std::unique_ptr<AudioPipeline> audio;  // may be null
  };

  // A matched game process that has not shown a window yet. Games often
  // launch through windowless bootstrap exes (Unreal's ReadyOrNot.exe
  // spawns ReadyOrNotSteam-Win64-Shipping.exe), so several candidates can
  // be pending at once — whichever produces a window first wins.
  struct Candidate {
    DWORD pid = 0;
    std::string exe_path;
    std::chrono::steady_clock::time_point deadline;
  };

  void post_event(Event event);
  void control_loop();
  void handle_event(const Event& event);
  void add_candidate(DWORD pid, std::string exe_path);
  void try_begin_capture();
  void stop_session(bool game_exited);
  Settings settings_copy() const;

  Callbacks callbacks_;

  mutable std::mutex settings_mutex_;
  Settings settings_;

  std::atomic<std::shared_ptr<const GameMatcher>> matcher_;

  mutable std::mutex session_mutex_;
  std::unique_ptr<Session> session_;

  // Control thread state (touched only on the control thread).
  std::vector<Candidate> candidates_;

  std::mutex events_mutex_;
  std::condition_variable events_cv_;
  std::deque<Event> events_;
  bool stopping_ = false;

  std::thread control_thread_;
  std::unique_ptr<win::ProcessWatcher> watcher_;  // last member: stopped first
};

// Builds the matcher from settings + auto-detected Steam libraries.
std::shared_ptr<const GameMatcher> build_matcher(const Settings& settings);

}  // namespace clipster::app
