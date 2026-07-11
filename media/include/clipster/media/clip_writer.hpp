#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "clipster/types.hpp"

namespace clipster::media {

struct ClipJob {
  std::vector<EncodedPacket> packets;  // from SegmentRingBuffer::snapshot()
  VideoStreamInfo video;
  std::optional<AudioStreamInfo> audio;
  std::filesystem::path out_path;
};

// Remuxes already-encoded packets into an MP4 — a pure copy, no re-encode,
// so a two-minute clip is written in milliseconds. Timestamps are rebased
// so the clip starts at t=0. Writes to a ".part" temp file first and
// renames on success, so an interrupted write never leaves a broken clip
// at the final path.
bool write_clip(const ClipJob& job, std::string* error);

}  // namespace clipster::media
