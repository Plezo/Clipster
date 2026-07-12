#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace clipster::win {

// One captured frame, valid only for the duration of the sink callback.
// BGRA, top-down, `stride` bytes per row (stride >= width * 4).
struct CapturedFrame {
  const uint8_t* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  int64_t timestamp_us = 0;  // QPC-based, monotonic across video and audio
};

// Captures a window with the Windows.Graphics.Capture API (Windows 10
// 1903+). Frames arrive on a capture worker thread; the sink must copy or
// consume the data before returning.
//
// The caller must have initialized COM/WinRT (winrt::init_apartment) on the
// creating thread.
class WgcCapture {
 public:
  using FrameSink = std::function<void(const CapturedFrame&)>;

  static bool is_supported();
  // With client_area_only (the default), frames are cropped to the
  // window's client area so windowed games record without the title bar
  // and borders.
  static std::unique_ptr<WgcCapture> create_for_window(HWND hwnd, FrameSink sink,
                                                       std::string* error,
                                                       bool client_area_only = true);
  ~WgcCapture();

  WgcCapture(const WgcCapture&) = delete;
  WgcCapture& operator=(const WgcCapture&) = delete;

  // Invoked (from a WinRT worker thread) when the captured window is
  // destroyed — e.g. a splash screen replaced by the real game window.
  // Set before start().
  void set_on_closed(std::function<void()> callback);

  void start();
  void stop();

 private:
  WgcCapture();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::win
