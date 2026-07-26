#pragma once

#include <memory>
#include <string>
#include <vector>

namespace clipster::media {

// Converts interleaved float PCM between sample rates and channel counts
// (libswresample). The encoder resamples its own input, so this exists for
// the PCM paths only: a capture device whose mix format does not match the
// mixer's cannot be summed with the other sources without it.
//
// Not thread-safe: feed from one thread (swr carries filter state).
class AudioResampler {
 public:
  static std::unique_ptr<AudioResampler> create(int in_sample_rate, int in_channels,
                                                int out_sample_rate, int out_channels,
                                                std::string* error);
  ~AudioResampler();

  AudioResampler(const AudioResampler&) = delete;
  AudioResampler& operator=(const AudioResampler&) = delete;

  // `samples` = frame_count * in_channels interleaved floats. Returns the
  // converted interleaved samples, valid until the next convert() call.
  // May be empty when swr is still buffering (rate conversion has latency).
  const std::vector<float>& convert(const float* samples, int frame_count);

  int out_channels() const;

 private:
  AudioResampler();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::media
