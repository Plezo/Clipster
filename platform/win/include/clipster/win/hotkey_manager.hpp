#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clipster::win {

// System-wide hotkeys via a low-level keyboard hook — these fire even
// while a fullscreen game with raw input has focus, and cannot conflict
// with combos owned by other applications. Combos are strings like
// "Ctrl+Del" or "Ctrl+Shift+S" so they can live in settings.json and the
// settings UI. Known limitation: if the foreground app runs elevated and
// Clipster does not, Windows withholds the input (run Clipster as admin
// in that case).
//
// Callbacks run on the manager's internal message-loop thread; keep them
// short (signal the real work, don't do it there).
class HotkeyManager {
 public:
  using Callback = std::function<void()>;

  HotkeyManager();
  ~HotkeyManager();

  HotkeyManager(const HotkeyManager&) = delete;
  HotkeyManager& operator=(const HotkeyManager&) = delete;

  // False only if the combo cannot be parsed.
  bool register_hotkey(const std::string& combo, Callback callback, std::string* error);

  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Exposed for reuse by the future settings UI: validates a combo string.
bool is_valid_hotkey_combo(const std::string& combo);

}  // namespace clipster::win
