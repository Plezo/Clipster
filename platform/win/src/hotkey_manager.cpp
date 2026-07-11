#include "clipster/win/hotkey_manager.hpp"

#include <windows.h>

#include <cstdlib>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "clipster/logging.hpp"
#include "clipster/util.hpp"

namespace clipster::win {

namespace {

struct ParsedCombo {
  UINT modifiers = 0;
  UINT vk = 0;
};

std::optional<UINT> key_name_to_vk(const std::string& key) {
  if (key.size() == 1) {
    const char c = key[0];
    if (c >= 'a' && c <= 'z') return static_cast<UINT>(c - 'a' + 'A');
    if (c >= '0' && c <= '9') return static_cast<UINT>(c);
  }
  if (key.size() >= 2 && key[0] == 'f') {
    const int n = std::atoi(key.c_str() + 1);
    if (n >= 1 && n <= 24) return static_cast<UINT>(VK_F1 + n - 1);
  }
  if (key.rfind("numpad", 0) == 0 && key.size() == 7 && key[6] >= '0' && key[6] <= '9') {
    return static_cast<UINT>(VK_NUMPAD0 + (key[6] - '0'));
  }
  // Includes Qt QKeySequence spellings (Ins, PgUp, Print, ...) so combos
  // written by the settings UI parse without translation.
  static const std::map<std::string, UINT> named{
      {"space", VK_SPACE},      {"tab", VK_TAB},         {"enter", VK_RETURN},
      {"return", VK_RETURN},    {"backspace", VK_BACK},  {"escape", VK_ESCAPE},
      {"esc", VK_ESCAPE},       {"insert", VK_INSERT},   {"ins", VK_INSERT},
      {"delete", VK_DELETE},    {"del", VK_DELETE},      {"home", VK_HOME},
      {"end", VK_END},          {"pageup", VK_PRIOR},    {"pgup", VK_PRIOR},
      {"pagedown", VK_NEXT},    {"pgdown", VK_NEXT},     {"pgdn", VK_NEXT},
      {"up", VK_UP},            {"down", VK_DOWN},       {"left", VK_LEFT},
      {"right", VK_RIGHT},      {"printscreen", VK_SNAPSHOT}, {"print", VK_SNAPSHOT},
      {"pause", VK_PAUSE},
  };
  if (auto it = named.find(key); it != named.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<ParsedCombo> parse_combo(const std::string& combo) {
  ParsedCombo out;
  size_t start = 0;
  const std::string lowered = util::to_lower_ascii(combo);
  while (start <= lowered.size()) {
    size_t end = lowered.find('+', start);
    if (end == std::string::npos) {
      end = lowered.size();
    }
    std::string token = lowered.substr(start, end - start);
    // trim spaces
    while (!token.empty() && token.front() == ' ') token.erase(token.begin());
    while (!token.empty() && token.back() == ' ') token.pop_back();

    if (token == "ctrl" || token == "control") {
      out.modifiers |= MOD_CONTROL;
    } else if (token == "alt") {
      out.modifiers |= MOD_ALT;
    } else if (token == "shift") {
      out.modifiers |= MOD_SHIFT;
    } else if (token == "win" || token == "super" || token == "meta") {
      out.modifiers |= MOD_WIN;
    } else if (!token.empty()) {
      if (out.vk != 0) {
        return std::nullopt;  // two non-modifier keys
      }
      const auto vk = key_name_to_vk(token);
      if (!vk) {
        return std::nullopt;
      }
      out.vk = *vk;
    }
    if (end == lowered.size()) {
      break;
    }
    start = end + 1;
  }
  if (out.vk == 0) {
    return std::nullopt;
  }
  return out;
}

}  // namespace

bool is_valid_hotkey_combo(const std::string& combo) { return parse_combo(combo).has_value(); }

struct HotkeyManager::Impl {
  struct Request {
    ParsedCombo combo;
    Callback callback;
    std::promise<bool> done;
  };

  std::thread thread;
  DWORD thread_id = 0;
  std::shared_future<void> ready;
  std::mutex mutex;
  std::deque<Request> requests;
  std::map<int, Callback> callbacks;  // hotkey id -> action
  int next_id = 1;
  bool stopped = false;

  void thread_main(std::promise<void> ready_promise) {
    // Force-create the message queue before signalling readiness so
    // PostThreadMessage from other threads cannot be lost.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    thread_id = GetCurrentThreadId();
    ready_promise.set_value();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == WM_APP) {
        std::lock_guard lock(mutex);
        while (!requests.empty()) {
          Request req = std::move(requests.front());
          requests.pop_front();
          const int id = next_id++;
          if (RegisterHotKey(nullptr, id, req.combo.modifiers | MOD_NOREPEAT, req.combo.vk)) {
            callbacks[id] = std::move(req.callback);
            req.done.set_value(true);
          } else {
            req.done.set_value(false);
          }
        }
      } else if (msg.message == WM_HOTKEY) {
        Callback cb;
        {
          std::lock_guard lock(mutex);
          if (auto it = callbacks.find(static_cast<int>(msg.wParam)); it != callbacks.end()) {
            cb = it->second;
          }
        }
        if (cb) {
          cb();
        }
      }
    }

    std::lock_guard lock(mutex);
    // A registration racing with stop() may have queued a request that the
    // loop never saw (WM_QUIT drains first) — fail it so the caller's
    // future.get() cannot block forever.
    while (!requests.empty()) {
      requests.front().done.set_value(false);
      requests.pop_front();
    }
    for (const auto& [id, cb] : callbacks) {
      UnregisterHotKey(nullptr, id);
    }
    callbacks.clear();
  }
};

HotkeyManager::HotkeyManager() : impl_(std::make_unique<Impl>()) {
  std::promise<void> ready_promise;
  impl_->ready = ready_promise.get_future().share();
  impl_->thread = std::thread(&Impl::thread_main, impl_.get(), std::move(ready_promise));
}

HotkeyManager::~HotkeyManager() { stop(); }

bool HotkeyManager::register_hotkey(const std::string& combo, Callback callback,
                                    std::string* error) {
  const auto parsed = parse_combo(combo);
  if (!parsed) {
    if (error) *error = "cannot parse hotkey '" + combo + "'";
    return false;
  }

  impl_->ready.wait();
  std::future<bool> done;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
      if (error) *error = "hotkey manager stopped";
      return false;
    }
    Impl::Request req;
    req.combo = *parsed;
    req.callback = std::move(callback);
    done = req.done.get_future();
    impl_->requests.push_back(std::move(req));
  }
  PostThreadMessageW(impl_->thread_id, WM_APP, 0, 0);

  if (!done.get()) {
    if (error) {
      *error = "RegisterHotKey failed for '" + combo +
               "' — the combination is probably taken by another application";
    }
    return false;
  }
  log::info("hotkeys: registered {}", combo);
  return true;
}

void HotkeyManager::stop() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
      return;
    }
    impl_->stopped = true;
  }
  impl_->ready.wait();
  PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

}  // namespace clipster::win
