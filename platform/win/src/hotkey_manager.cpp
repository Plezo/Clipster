#include "clipster/win/hotkey_manager.hpp"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "clipster/logging.hpp"
#include "clipster/util.hpp"

namespace clipster::win {

namespace {

struct ParsedCombo {
  UINT modifiers = 0;  // MOD_CONTROL / MOD_ALT / MOD_SHIFT / MOD_WIN
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

bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Exact modifier match: the combo fires only when precisely the requested
// modifier classes are held, so Ctrl+Del does not fire during Ctrl+Alt+Del.
bool modifiers_match(UINT wanted) {
  return key_down(VK_CONTROL) == ((wanted & MOD_CONTROL) != 0) &&
         key_down(VK_MENU) == ((wanted & MOD_ALT) != 0) &&
         key_down(VK_SHIFT) == ((wanted & MOD_SHIFT) != 0) &&
         (key_down(VK_LWIN) || key_down(VK_RWIN)) == ((wanted & MOD_WIN) != 0);
}

}  // namespace

bool is_valid_hotkey_combo(const std::string& combo) { return parse_combo(combo).has_value(); }

// Implementation: a WH_KEYBOARD_LL low-level keyboard hook instead of
// RegisterHotKey. RegisterHotKey is starved by fullscreen games that use
// raw input, and fails outright when another app owns the combo; the
// low-level hook sees every keystroke first (the approach used by OBS,
// Discord, etc.). The hook callback only posts a message so the hook
// chain is never delayed. Limitation (shared with RegisterHotKey): input
// destined for an elevated window is not delivered to a non-elevated
// Clipster — run Clipster as administrator if the game runs elevated.
struct HotkeyManager::Impl {
  struct Def {
    ParsedCombo combo;
    Callback callback;
    bool held = false;  // suppress auto-repeat while the combo stays down
  };

  std::mutex mutex;
  std::vector<Def> defs;
  std::thread thread;
  DWORD thread_id = 0;
  std::shared_future<void> ready;
  bool stopped = false;

  // The LL hook procedure has no context parameter; managers are created
  // one at a time (frontends replace theirs on settings changes), so a
  // process-wide current-instance pointer is sufficient.
  static std::atomic<Impl*> current;

  static LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
      Impl* self = current.load(std::memory_order_acquire);
      if (self) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
        std::lock_guard lock(self->mutex);
        for (size_t i = 0; i < self->defs.size(); ++i) {
          Def& def = self->defs[i];
          if (info->vkCode != def.combo.vk) {
            continue;
          }
          if (up) {
            def.held = false;
          } else if (down && !def.held && modifiers_match(def.combo.modifiers)) {
            def.held = true;
            // Fire from the message loop; the hook must return quickly.
            PostThreadMessageW(self->thread_id, WM_APP, i, 0);
          }
        }
      }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
  }

  void thread_main(std::promise<void> ready_promise) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // force-create the queue
    thread_id = GetCurrentThreadId();

    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &hook_proc, GetModuleHandleW(nullptr), 0);
    if (!hook) {
      log::error("hotkeys: SetWindowsHookEx failed ({})", GetLastError());
    }
    current.store(this, std::memory_order_release);
    ready_promise.set_value();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == WM_APP) {
        Callback cb;
        {
          std::lock_guard lock(mutex);
          const size_t index = msg.wParam;
          if (index < defs.size()) {
            cb = defs[index].callback;
          }
        }
        if (cb) {
          cb();
        }
      }
    }

    current.store(nullptr, std::memory_order_release);
    if (hook) {
      UnhookWindowsHookEx(hook);
    }
  }
};

std::atomic<HotkeyManager::Impl*> HotkeyManager::Impl::current{nullptr};

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
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
      if (error) *error = "hotkey manager stopped";
      return false;
    }
    impl_->defs.push_back({*parsed, std::move(callback), false});
  }
  log::info("hotkeys: watching {}", combo);
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
