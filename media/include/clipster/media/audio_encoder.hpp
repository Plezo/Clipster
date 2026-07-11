#pragma once

#include <functional>
#include <memory>
#include <string>

#include "clipster/types.hpp"

namespace clipster::media {

struct AudioEncoderConfig {
  int in_sample_rate = 48000;  // format of the samples fed to encode()
  int in_channels = 2;
  int bitrate_kbps = 160;
};

// Encodes interleaved float PCM into AAC packets. Input is resampled and
// downmixed to the output format (48 kHz stereo) internally, so desktop
// loopback at whatever the device mix format is Just Works.
//
// Timestamps: pass the pts of the FIRST sample of each call in the shared
// microsecond timeline. Output pts is derived from the first call's pts
// plus the running sample count, so it can never drift or jitter.
class AudioEncoder {
 public:
  using PacketSink = std::function<void(EncodedPacket)>;

  static std::unique_ptr<AudioEncoder> create(const AudioEncoderConfig& config, PacketSink sink,
                                              std::string* error);
  ~AudioEncoder();

  AudioEncoder(const AudioEncoder&) = delete;
  AudioEncoder& operator=(const AudioEncoder&) = delete;

  // `samples` = frame_count * in_channels interleaved floats. Not
  // thread-safe: feed from one thread.
  bool encode(const float* samples, int frame_count, int64_t pts_us);

  // Pads and drains the final partial frame. Call once at session end.
  void flush();

  AudioStreamInfo stream_info() const;

 private:
  AudioEncoder();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::media
