#include "clipster/ring_buffer.hpp"

#include <algorithm>

namespace clipster {

namespace {
constexpr int64_t kMicros = 1'000'000;
}

SegmentRingBuffer::SegmentRingBuffer(std::chrono::seconds capacity)
    : capacity_us_(capacity.count() * kMicros) {}

void SegmentRingBuffer::set_capacity(std::chrono::seconds capacity) {
  std::lock_guard lock(mutex_);
  capacity_us_ = capacity.count() * kMicros;
  evict_locked();
}

void SegmentRingBuffer::push(EncodedPacket packet) {
  std::lock_guard lock(mutex_);

  const bool opens_gop = packet.stream == StreamKind::Video && packet.keyframe;
  if (gops_.empty() && !opens_gop) {
    return;  // nothing decodable can start mid-GOP
  }

  if (opens_gop) {
    Gop gop;
    gop.start_us = packet.pts_us;
    gop.end_us = packet.pts_us;
    gops_.push_back(std::move(gop));
  }

  Gop& gop = gops_.back();
  gop.end_us = std::max(gop.end_us, packet.pts_us);
  gop.bytes += packet.size();
  total_bytes_ += packet.size();
  gop.packets.push_back(std::move(packet));

  evict_locked();
}

void SegmentRingBuffer::evict_locked() {
  // Drop the oldest GOP while the remaining ones still cover the window.
  while (gops_.size() > 1 && gops_.back().end_us - gops_[1].start_us >= capacity_us_) {
    total_bytes_ -= gops_.front().bytes;
    gops_.pop_front();
  }
}

std::vector<EncodedPacket> SegmentRingBuffer::snapshot(std::chrono::seconds duration) const {
  std::lock_guard lock(mutex_);
  if (gops_.empty()) {
    return {};
  }

  const int64_t cutoff_us = gops_.back().end_us - duration.count() * kMicros;

  // Walk back to the newest GOP that starts at or before the cutoff so the
  // clip covers at least the requested duration.
  size_t first = gops_.size();
  while (first > 0) {
    --first;
    if (gops_[first].start_us <= cutoff_us) {
      break;
    }
  }

  std::vector<EncodedPacket> out;
  for (size_t i = first; i < gops_.size(); ++i) {
    out.insert(out.end(), gops_[i].packets.begin(), gops_[i].packets.end());
  }
  return out;
}

std::chrono::microseconds SegmentRingBuffer::buffered_duration() const {
  std::lock_guard lock(mutex_);
  if (gops_.empty()) {
    return std::chrono::microseconds{0};
  }
  return std::chrono::microseconds{gops_.back().end_us - gops_.front().start_us};
}

size_t SegmentRingBuffer::buffered_bytes() const {
  std::lock_guard lock(mutex_);
  return total_bytes_;
}

void SegmentRingBuffer::clear() {
  std::lock_guard lock(mutex_);
  gops_.clear();
  total_bytes_ = 0;
}

}  // namespace clipster
