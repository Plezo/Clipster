#pragma once

#include <windows.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace clipster::win {

// Polls the process table and reports starts/stops with full executable
// paths (UTF-8). Callbacks run on the watcher's own thread — post to your
// main thread if you need to touch UI.
//
// The initial scan also fires on_started for every process already
// running, so a game launched before Clipster is still picked up.
class ProcessWatcher {
 public:
  struct Callbacks {
    std::function<void(DWORD pid, const std::string& exe_path)> on_started;
    std::function<void(DWORD pid, const std::string& exe_path)> on_stopped;
  };

  explicit ProcessWatcher(Callbacks callbacks,
                          std::chrono::milliseconds interval = std::chrono::milliseconds(2000));
  ~ProcessWatcher();

  ProcessWatcher(const ProcessWatcher&) = delete;
  ProcessWatcher& operator=(const ProcessWatcher&) = delete;

  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::win
