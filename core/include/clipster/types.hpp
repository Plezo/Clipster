#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace clipster {

enum class StreamKind { Video, Audio, Microphone };

// One encoded media packet (an H.264/HEVC access unit or an AAC frame).
// The payload is shared and immutable so ring-buffer snapshots are cheap:
// taking a clip copies refcounted pointers, never the compressed bytes.
struct EncodedPacket {
  StreamKind stream = StreamKind::Video;
  int64_t pts_us = 0;
  int64_t dts_us = 0;
  bool keyframe = false;
  std::shared_ptr<const std::vector<uint8_t>> data;

  size_t size() const { return data ? data->size() : 0; }
};

// Codec parameters needed to mux packets into a container later.
// `codec_name` is an FFmpeg codec name ("h264", "hevc", "aac") so core
// stays free of FFmpeg types.
struct VideoStreamInfo {
  std::string codec_name;
  int width = 0;
  int height = 0;
  int fps = 0;
  std::vector<uint8_t> extradata;  // SPS/PPS etc.
};

struct AudioStreamInfo {
  std::string codec_name;
  int sample_rate = 0;
  int channels = 0;
  std::vector<uint8_t> extradata;
};

}  // namespace clipster
