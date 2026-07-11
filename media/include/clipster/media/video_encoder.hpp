#pragma once

#include <functional>
#include <memory>
#include <string>

#include "clipster/types.hpp"

namespace clipster::media {

struct VideoEncoderConfig {
  int width = 0;   // output size; odd values are rounded down to even
  int height = 0;
  int fps = 60;
  int bitrate_kbps = 20000;
  // "auto" | "h264" | "hevc" — expanded to a hardware-first probe list.
  std::string codec = "auto";
  int gop_seconds = 2;  // keyframe interval == ring buffer granularity
};

// Encodes BGRA frames into H.264/HEVC packets. Probes encoders in order of
// preference (NVENC -> AMF -> QSV -> software) and picks the first that
// opens on this machine.
class VideoEncoder {
 public:
  using PacketSink = std::function<void(EncodedPacket)>;

  static std::unique_ptr<VideoEncoder> create(const VideoEncoderConfig& config,
                                              PacketSink sink, std::string* error);
  ~VideoEncoder();

  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  // Encodes one frame. `data` is tightly packed BGRA rows of `stride` bytes;
  // `width`/`height` describe the source and may differ from the configured
  // output (the frame is scaled). pts must be monotonically increasing
  // microseconds. Emitted packets go to the sink on the calling thread.
  bool encode_bgra(const uint8_t* data, int width, int height, int stride, int64_t pts_us);

  // Drains any delayed packets. Call once at end of session.
  void flush();

  const std::string& encoder_name() const;
  VideoStreamInfo stream_info() const;

 private:
  VideoEncoder();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clipster::media
