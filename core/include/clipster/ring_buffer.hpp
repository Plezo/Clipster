#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "clipster/types.hpp"

namespace clipster {

// Thread-safe rolling window of encoded media packets.
//
// Packets are grouped into GOPs (video keyframe to keyframe) so that both
// eviction and clip extraction always happen on independently decodable
// boundaries. Memory is bounded by `capacity` seconds of media: once the
// buffered duration exceeds it, whole GOPs are dropped from the front.
class SegmentRingBuffer {
 public:
  explicit SegmentRingBuffer(std::chrono::seconds capacity);

  void set_capacity(std::chrono::seconds capacity);

  // Packets must arrive with non-decreasing pts per stream. Everything
  // received before the first video keyframe is discarded (it could not be
  // decoded anyway).
  void push(EncodedPacket packet);

  // Returns packets covering at least the last `duration` of media, oldest
  // first, always starting on a video keyframe. May return slightly more
  // than requested (rounded up to a GOP boundary), or less if the buffer
  // has not filled yet.
  std::vector<EncodedPacket> snapshot(std::chrono::seconds duration) const;

  std::chrono::microseconds buffered_duration() const;
  size_t buffered_bytes() const;
  void clear();

 private:
  struct Gop {
    std::vector<EncodedPacket> packets;
    int64_t start_us = 0;  // pts of the opening video keyframe
    int64_t end_us = 0;    // highest pts seen in this GOP
    size_t bytes = 0;
  };

  void evict_locked();

  mutable std::mutex mutex_;
  std::deque<Gop> gops_;
  int64_t capacity_us_;
  size_t total_bytes_ = 0;
};

}  // namespace clipster
