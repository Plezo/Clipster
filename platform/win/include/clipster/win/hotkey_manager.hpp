#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clipster::win {

// System-wide hotkeys via RegisterHotKey — these fire even while a
// fullscreen game has focus. Combos are strings like "Ctrl+Alt+F10" or
// "Ctrl+Shift+S" so they can live in settings.json and, later, a UI
// key-capture widget.
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

  // False if the combo cannot be parsed or the OS rejected it (usually
  // because another application already owns it).
  bool register_hotkey(const std::string& combo, Callback callback, std::string* error);

  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Exposed for reuse by the future settings UI: validates a combo string.
bool is_valid_hotkey_combo(const std::string& combo);

}  // namespace clipster::win
