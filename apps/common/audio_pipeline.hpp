#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "clipster/settings.hpp"

namespace clipster::app {

class Recorder;

// Assembles the audio side of a recording session according to
// settings.audio.mode:
//
//   desktop         one WASAPI desktop loopback -> AAC
//   game_only       process loopback (include game tree) -> AAC
//   include_list    process loopback per app (game + Discord + ...) ->
//                   mixer -> AAC
//   desktop_exclude process loopback excluding one app's tree -> AAC
//                   (Windows can only exclude a single process tree per
//                   capture; the first *running* app in exclude_apps wins)
//
// Encoded AAC packets are pushed into the Recorder's ring buffer on the
// shared video timeline; audio arriving before the first video frame is
// dropped. On systems older than Windows 10 2004 every per-app mode
// falls back to desktop capture with a warning.
class AudioPipeline {
 public:
  static std::unique_ptr<AudioPipeline> create(const Settings& settings, DWORD game_pid,
                                               Recorder& recorder, std::string* error);
  ~AudioPipeline();

  AudioPipeline(const AudioPipeline&) = delete;
  AudioPipeline& operator=(const AudioPipeline&) = delete;

  void start();
  void stop();  // stops captures and flushes the encoder

 private:
  AudioPipeline();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::app
