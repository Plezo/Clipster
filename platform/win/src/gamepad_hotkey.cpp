#include "clipster/win/gamepad_hotkey.hpp"

#include <windows.h>

#include <xinput.h>

#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <thread>

#include "clipster/logging.hpp"
#include "clipster/util.hpp"

namespace clipster::win {

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(50);
// Probing empty controller slots is comparatively slow; retry them at 2 s.
constexpr int kReconnectTicks = 40;

std::optional<WORD> parse_combo(const std::string& combo) {
  static const std::map<std::string, WORD> buttons{
      {"a", XINPUT_GAMEPAD_A},
      {"b", XINPUT_GAMEPAD_B},
      {"x", XINPUT_GAMEPAD_X},
      {"y", XINPUT_GAMEPAD_Y},
      {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {"ls", XINPUT_GAMEPAD_LEFT_THUMB},
      {"l3", XINPUT_GAMEPAD_LEFT_THUMB},
      {"rs", XINPUT_GAMEPAD_RIGHT_THUMB},
      {"r3", XINPUT_GAMEPAD_RIGHT_THUMB},
      {"back", XINPUT_GAMEPAD_BACK},
      {"select", XINPUT_GAMEPAD_BACK},
      {"view", XINPUT_GAMEPAD_BACK},
      {"start", XINPUT_GAMEPAD_START},
      {"menu", XINPUT_GAMEPAD_START},
      {"dpadup", XINPUT_GAMEPAD_DPAD_UP},
      {"dpaddown", XINPUT_GAMEPAD_DPAD_DOWN},
      {"dpadleft", XINPUT_GAMEPAD_DPAD_LEFT},
      {"dpadright", XINPUT_GAMEPAD_DPAD_RIGHT},
  };

  WORD mask = 0;
  const std::string lowered = util::to_lower_ascii(combo);
  size_t start = 0;
  while (start <= lowered.size()) {
    size_t end = lowered.find('+', start);
    if (end == std::string::npos) {
      end = lowered.size();
    }
    std::string token = lowered.substr(start, end - start);
    std::erase_if(token, [](char c) { return c == ' ' || c == '_'; });
    if (!token.empty()) {
      const auto it = buttons.find(token);
      if (it == buttons.end()) {
        return std::nullopt;
      }
      mask |= it->second;
    }
    if (end == lowered.size()) {
      break;
    }
    start = end + 1;
  }
  if (mask == 0) {
    return std::nullopt;
  }
  return mask;
}

}  // namespace

bool is_valid_gamepad_combo(const std::string& combo) { return parse_combo(combo).has_value(); }

struct GamepadHotkey::Impl {
  WORD mask = 0;
  Callback callback;
  std::thread thread;
  std::atomic<bool> stopping{false};

  void run() {
    std::array<bool, XUSER_MAX_COUNT> connected{};
    std::array<bool, XUSER_MAX_COUNT> combo_down{};
    int tick = 0;

    while (!stopping.load(std::memory_order_relaxed)) {
      for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (!connected[i] && tick % kReconnectTicks != 0) {
          continue;
        }
        XINPUT_STATE state{};
        if (XInputGetState(i, &state) != ERROR_SUCCESS) {
          connected[i] = false;
          combo_down[i] = false;
          continue;
        }
        connected[i] = true;
        const bool pressed = (state.Gamepad.wButtons & mask) == mask;
        if (pressed && !combo_down[i]) {
          callback();
        }
        combo_down[i] = pressed;
      }
      ++tick;
      std::this_thread::sleep_for(kPollInterval);
    }
  }
};

GamepadHotkey::GamepadHotkey() : impl_(std::make_unique<Impl>()) {}
GamepadHotkey::~GamepadHotkey() { stop(); }

std::unique_ptr<GamepadHotkey> GamepadHotkey::create(const std::string& combo, Callback callback,
                                                     std::string* error) {
  const auto mask = parse_combo(combo);
  if (!mask) {
    if (error) *error = "cannot parse controller combo '" + combo + "'";
    return nullptr;
  }
  auto hk = std::unique_ptr<GamepadHotkey>(new GamepadHotkey());
  hk->impl_->mask = *mask;
  hk->impl_->callback = std::move(callback);
  hk->impl_->thread = std::thread([impl = hk->impl_.get()] { impl->run(); });
  log::info("gamepad: watching for {}", combo);
  return hk;
}

void GamepadHotkey::stop() {
  if (impl_->stopping.exchange(true)) {
    return;
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

}  // namespace clipster::win
