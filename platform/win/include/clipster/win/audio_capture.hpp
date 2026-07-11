#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace clipster::win {

// One block of captured audio, valid only during the sink callback.
// Interleaved 32-bit float samples.
struct AudioChunk {
  const float* samples = nullptr;
  int frame_count = 0;
  int channels = 0;
  int sample_rate = 0;
  int64_t timestamp_us = 0;  // QPC-based, same clock as CapturedFrame
};

using AudioSink = std::function<void(const AudioChunk&)>;

// Captures everything the default output device plays ("desktop" audio
// mode) via classic WASAPI loopback. Works on every supported Windows.
class DesktopLoopbackCapture {
 public:
  static std::unique_ptr<DesktopLoopbackCapture> create(AudioSink sink, std::string* error);
  ~DesktopLoopbackCapture();

  void start();
  void stop();

  // The device mix format the chunks arrive in.
  int sample_rate() const;
  int channels() const;

 private:
  DesktopLoopbackCapture();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Captures ONLY a given process tree's audio (IncludeTree) or everything
// EXCEPT it (ExcludeTree) via WASAPI process loopback. This is what powers
// the "game + Discord but not Spotify" audio modes:
//
//   game_only       -> one IncludeTree capture on the game pid
//   include_list    -> one IncludeTree capture per app, mixed together
//   desktop_exclude -> ExcludeTree captures layered on desktop audio
//
// Requires Windows 10 2004+; callers should check is_supported() and fall
// back to DesktopLoopbackCapture with a warning.
class ProcessLoopbackCapture {
 public:
  enum class Mode { IncludeTree, ExcludeTree };

  static bool is_supported();
  static std::unique_ptr<ProcessLoopbackCapture> create(DWORD pid, Mode mode, AudioSink sink,
                                                        std::string* error);
  ~ProcessLoopbackCapture();

  void start();
  void stop();

 private:
  ProcessLoopbackCapture();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::win
