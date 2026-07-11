#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clipster::win {

// Fires a callback when a controller button combo (e.g. "Back+RB") is
// pressed on any connected XInput controller — so you can clip from the
// couch. Edge-triggered: holding the combo fires once.
//
// Button names: A B X Y LB RB LS RS Back Start DpadUp DpadDown DpadLeft
// DpadRight (aliases: Select/View = Back, Menu = Start, L3/R3 = LS/RS).
// Analog triggers are not supported as combo keys.
//
// The callback runs on the poller's thread; keep it short.
class GamepadHotkey {
 public:
  using Callback = std::function<void()>;

  static std::unique_ptr<GamepadHotkey> create(const std::string& combo, Callback callback,
                                               std::string* error);
  ~GamepadHotkey();

  GamepadHotkey(const GamepadHotkey&) = delete;
  GamepadHotkey& operator=(const GamepadHotkey&) = delete;

  void stop();

 private:
  GamepadHotkey();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool is_valid_gamepad_combo(const std::string& combo);

}  // namespace clipster::win
