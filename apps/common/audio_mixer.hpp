#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "clipster/win/audio_capture.hpp"

namespace clipster::app {

// Sums N capture streams (same sample rate/channel count) into one.
//
// Output is paced by the wall clock, not by source availability: WASAPI
// process loopback delivers NOTHING while an app is silent, so a source
// with no queued samples simply contributes silence. This is what makes
// "game + Discord" work when Discord is quiet.
//
// submit() may be called from any thread; the sink runs on the mixer's
// own thread with timestamps in the shared QPC-microsecond timeline.
class AudioMixer {
 public:
  using Sink = std::function<void(const float* interleaved, int frame_count, int64_t pts_us)>;

  AudioMixer(int sample_rate, int channels, Sink sink);
  ~AudioMixer();

  AudioMixer(const AudioMixer&) = delete;
  AudioMixer& operator=(const AudioMixer&) = delete;

  // Call before start().
  size_t add_source();

  void submit(size_t source_index, const win::AudioChunk& chunk);

  void start();
  void stop();

 private:
  void run();

  const int sample_rate_;
  const int channels_;
  Sink sink_;

  std::mutex mutex_;
  std::vector<std::deque<float>> sources_;  // interleaved sample queues
  int64_t first_ts_us_ = -1;                // timestamp of the earliest submitted chunk
  int64_t emitted_frames_ = 0;
  std::chrono::steady_clock::time_point emit_start_{};
  bool emitting_ = false;

  std::thread thread_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::vector<float> out_buf_;
};

}  // namespace clipster::app
