#include "audio_mixer.hpp"

#include <algorithm>

#include "clipster/logging.hpp"

namespace clipster::app {

namespace {
constexpr auto kTick = std::chrono::milliseconds(20);
}

AudioMixer::AudioMixer(int sample_rate, int channels, Sink sink)
    : sample_rate_(sample_rate), channels_(channels), sink_(std::move(sink)) {}

AudioMixer::~AudioMixer() { stop(); }

size_t AudioMixer::add_source() {
  sources_.emplace_back();
  return sources_.size() - 1;
}

void AudioMixer::submit(size_t source_index, const win::AudioChunk& chunk) {
  if (chunk.sample_rate != sample_rate_) {
    return;  // sources are created at the mixer's rate; drop misconfigured data
  }
  std::lock_guard lock(mutex_);
  auto& queue = sources_[source_index];

  // Adapt channel count sample-by-sample (mono -> dup, extra -> drop).
  for (int f = 0; f < chunk.frame_count; ++f) {
    for (int c = 0; c < channels_; ++c) {
      const int src_c = std::min(c, chunk.channels - 1);
      queue.push_back(chunk.samples[f * chunk.channels + src_c]);
    }
  }
  // Bound the queue: anything the mixer is too far behind on is stale.
  const size_t cap = static_cast<size_t>(sample_rate_) * channels_;  // 1 s
  while (queue.size() > cap) {
    queue.pop_front();
  }
  if (first_ts_us_ < 0) {
    first_ts_us_ = chunk.timestamp_us;
  }
}

void AudioMixer::start() {
  if (!thread_.joinable()) {
    thread_ = std::thread([this] { run(); });
  }
}

void AudioMixer::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AudioMixer::run() {
  std::unique_lock lock(mutex_);
  while (!cv_.wait_for(lock, kTick, [this] { return stopping_; })) {
    if (!emitting_) {
      if (first_ts_us_ < 0) {
        continue;  // nothing submitted yet
      }
      emitting_ = true;
      emit_start_ = std::chrono::steady_clock::now();
    }

    const auto elapsed = std::chrono::steady_clock::now() - emit_start_;
    const int64_t target_frames =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() * sample_rate_ /
        1'000'000;
    int64_t need = target_frames - emitted_frames_;
    // Never emit more than half a second per tick (e.g. after suspend).
    need = std::clamp<int64_t>(need, 0, sample_rate_ / 2);
    if (need == 0) {
      continue;
    }

    out_buf_.assign(static_cast<size_t>(need) * channels_, 0.0f);
    for (auto& queue : sources_) {
      const size_t avail = std::min(out_buf_.size(), queue.size());
      for (size_t i = 0; i < avail; ++i) {
        out_buf_[i] += queue[i];
      }
      queue.erase(queue.begin(), queue.begin() + avail);
    }
    for (float& s : out_buf_) {
      s = std::clamp(s, -1.0f, 1.0f);
    }

    const int64_t pts =
        first_ts_us_ + emitted_frames_ * 1'000'000 / sample_rate_;
    emitted_frames_ += need;

    // Sink runs outside the lock so submit() never blocks on encoding.
    auto buf = std::move(out_buf_);
    lock.unlock();
    sink_(buf.data(), static_cast<int>(need), pts);
    lock.lock();
    out_buf_ = std::move(buf);
  }
}

}  // namespace clipster::app
