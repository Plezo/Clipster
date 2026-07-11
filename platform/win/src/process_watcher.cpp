#include "clipster/win/process_watcher.hpp"

#include <tlhelp32.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clipster/win/str_util.hpp"

namespace clipster::win {

namespace {

std::string image_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return {};  // protected/system process — never a game we can capture
  }
  wchar_t buf[MAX_PATH * 2];
  DWORD len = static_cast<DWORD>(std::size(buf));
  std::string out;
  if (QueryFullProcessImageNameW(process, 0, buf, &len)) {
    out = narrow(std::wstring_view(buf, len));
  }
  CloseHandle(process);
  return out;
}

std::unordered_set<DWORD> snapshot_pids() {
  std::unordered_set<DWORD> pids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return pids;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snap, &entry)) {
    do {
      pids.insert(entry.th32ProcessID);
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return pids;
}

}  // namespace

struct ProcessWatcher::Impl {
  Callbacks callbacks;
  std::chrono::milliseconds interval;
  std::unordered_map<DWORD, std::string> known;  // pid -> exe path ("" = unqueryable)

  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool stopping = false;

  void run() {
    while (true) {
      const auto pids = snapshot_pids();

      for (const DWORD pid : pids) {
        if (known.contains(pid)) {
          continue;
        }
        const std::string path = image_path(pid);
        known.emplace(pid, path);
        if (!path.empty() && callbacks.on_started) {
          callbacks.on_started(pid, path);
        }
      }

      for (auto it = known.begin(); it != known.end();) {
        if (pids.contains(it->first)) {
          ++it;
          continue;
        }
        if (!it->second.empty() && callbacks.on_stopped) {
          callbacks.on_stopped(it->first, it->second);
        }
        it = known.erase(it);
      }

      std::unique_lock lock(mutex);
      if (cv.wait_for(lock, interval, [this] { return stopping; })) {
        return;
      }
    }
  }
};

ProcessWatcher::ProcessWatcher(Callbacks callbacks, std::chrono::milliseconds interval)
    : impl_(std::make_unique<Impl>()) {
  impl_->callbacks = std::move(callbacks);
  impl_->interval = interval;
  impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

ProcessWatcher::~ProcessWatcher() { stop(); }

void ProcessWatcher::stop() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
      return;
    }
    impl_->stopping = true;
  }
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

}  // namespace clipster::win
