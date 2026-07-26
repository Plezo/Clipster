#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace clipster::media {

// Crop rectangle in source-video pixels.
struct CropRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// A trim/crop export of an existing clip to a new MP4.
struct EditJob {
  std::filesystem::path in_path;
  std::filesystem::path out_path;
  int64_t start_us = 0;  // trim in
  int64_t end_us = -1;   // trim out; -1 = to end of clip
  // Cropping forces a decode -> encode round trip (and makes the start cut
  // frame-accurate). Without it the export is a pure remux, so the start
  // snaps back to the previous keyframe and quality is untouched.
  std::optional<CropRect> crop;
  // Re-encode parameters, only used when `crop` is set.
  std::string codec = "auto";
  int bitrate_kbps = 20000;
  std::function<void(float)> progress;  // optional, called with 0..1
};

bool export_edit(const EditJob& job, std::string* error);

}  // namespace clipster::media
