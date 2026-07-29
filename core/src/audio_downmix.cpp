#include "clipster/audio_downmix.hpp"

#include <algorithm>
#include <cmath>

namespace clipster {

namespace {
// A channel counts as active once it peaks above roughly -50 dBFS (below
// that it is dead silence, preamp hiss or crosstalk) *and* comes within
// 26 dB of the loudest channel, which keeps a hissy dead channel out of the
// fold even next to a loud one.
constexpr float kSilenceFloor = 0.003f;
constexpr float kRelativeFloor = 0.05f;
}  // namespace

MonoDownmixer::MonoDownmixer(int channels)
    : channels_(std::max(channels, 1)), peaks_(static_cast<size_t>(channels_), 0.0f) {
  active_.reserve(static_cast<size_t>(channels_));
}

bool MonoDownmixer::channel_active(int channel) const {
  if (channel < 0 || channel >= channels_) {
    return false;
  }
  const float loudest = *std::max_element(peaks_.begin(), peaks_.end());
  const float peak = peaks_[static_cast<size_t>(channel)];
  return peak >= kSilenceFloor && peak >= loudest * kRelativeFloor;
}

const std::vector<float>& MonoDownmixer::process(const float* interleaved, int frame_count) {
  out_.clear();
  if (!interleaved || frame_count <= 0) {
    return out_;
  }
  const size_t frames = static_cast<size_t>(frame_count);
  const size_t channels = static_cast<size_t>(channels_);

  for (size_t f = 0; f < frames; ++f) {
    for (size_t c = 0; c < channels; ++c) {
      peaks_[c] = std::max(peaks_[c], std::fabs(interleaved[f * channels + c]));
    }
  }

  // Recomputed per chunk: the active set can only grow, and a chunk is short
  // enough that acting on it one buffer late is inaudible.
  active_.clear();
  for (size_t c = 0; c < channels; ++c) {
    if (channel_active(static_cast<int>(c))) {
      active_.push_back(c);
    }
  }
  if (active_.empty()) {
    // Nothing has made a sound yet — fold everything: the result is silence
    // either way, and no channel gets locked out.
    for (size_t c = 0; c < channels; ++c) {
      active_.push_back(c);
    }
  }

  const float scale = 1.0f / static_cast<float>(active_.size());
  out_.resize(frames);
  for (size_t f = 0; f < frames; ++f) {
    float sum = 0.0f;
    for (const size_t c : active_) {
      sum += interleaved[f * channels + c];
    }
    out_[f] = std::clamp(sum * scale, -1.0f, 1.0f);
  }
  return out_;
}

}  // namespace clipster
