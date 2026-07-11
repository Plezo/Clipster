#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "clipster/media/video_encoder.hpp"
#include "clipster/ring_buffer.hpp"
#include "clipster/settings.hpp"
#include "clipster/win/wgc_capture.hpp"

namespace clipster::app {

std::filesystem::path executable_dir();

// Plays the clip-saved chime: user-configured .wav, else the bundled
// assets/clip_saved.wav next to the executable, else the system beep.
void play_clip_saved_sound(const Settings& settings);

// The recording engine shared by every frontend (CLI, tray): encodes
// incoming frames at the configured fps, keeps the ring buffer filled, and
// snapshots it into clip files on demand.
//
// Thread contract: on_frame() is called from the capture thread;
// save_clip()/stats() may be called from any thread; finish() joins
// in-flight clip writes and must run after capture has stopped.
class Recorder {
 public:
  // `on_fatal` fires (from the capture thread) when recording cannot
  // continue, e.g. no usable encoder; keep it short and non-blocking.
  Recorder(Settings settings, std::string game_name, std::function<void()> on_fatal);

  void on_frame(const win::CapturedFrame& frame);

  // --- audio (fed by AudioPipeline) ---
  // The QPC-µs timestamp of the first captured video frame, or -1 until
  // one arrives. Audio pts must be rebased against this so both streams
  // share a timeline.
  int64_t timeline_base_us() const {
    return timeline_base_us_.load(std::memory_order_acquire);
  }
  // Call once, before audio packets start flowing.
  void set_audio_info(AudioStreamInfo info);
  // Thread-safe; pts already rebased to the shared timeline.
  void push_audio_packet(EncodedPacket packet);

  void save_clip(std::chrono::seconds length);
  void save_clip();  // settings' default clip length

  void finish();

  std::string stats();

 private:
  std::filesystem::path build_output_path() const;

  Settings settings_;
  std::string game_name_;
  std::function<void()> on_fatal_;
  SegmentRingBuffer ring_;
  std::unique_ptr<media::VideoEncoder> encoder_;  // created on first frame
  std::atomic<bool> encoder_ready_{false};
  AudioStreamInfo audio_info_;
  std::atomic<bool> has_audio_{false};
  std::atomic<int64_t> timeline_base_us_{-1};
  int64_t first_pts_us_ = -1;
  int64_t next_due_us_ = 0;
  const int64_t frame_interval_us_;
  std::atomic<uint64_t> frames_encoded_{0};
  std::mutex writers_mutex_;
  std::vector<std::thread> writers_;
};

}  // namespace clipster::app
