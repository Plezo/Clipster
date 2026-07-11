#include "clipster/ring_buffer.hpp"

#include <memory>

#include "test_framework.hpp"

using namespace clipster;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kSec = 1'000'000;

EncodedPacket packet(StreamKind stream, int64_t pts_us, bool keyframe, size_t bytes = 1000) {
  EncodedPacket p;
  p.stream = stream;
  p.pts_us = pts_us;
  p.dts_us = pts_us;
  p.keyframe = keyframe;
  p.data = std::make_shared<const std::vector<uint8_t>>(bytes);
  return p;
}

// Simulates `seconds` of 30 fps video with a keyframe every 2 s, starting
// at `start_us`.
void feed_video(SegmentRingBuffer& rb, int64_t start_us, int seconds) {
  for (int64_t f = 0; f < seconds * 30; ++f) {
    const int64_t pts = start_us + f * kSec / 30;
    rb.push(packet(StreamKind::Video, pts, f % 60 == 0));
  }
}

}  // namespace

TEST(drops_everything_before_first_keyframe) {
  SegmentRingBuffer rb(60s);
  rb.push(packet(StreamKind::Video, 0, false));
  rb.push(packet(StreamKind::Audio, 0, false));
  CHECK_EQ(rb.buffered_bytes(), 0u);
  CHECK(rb.snapshot(30s).empty());

  rb.push(packet(StreamKind::Video, kSec, true));
  CHECK_EQ(rb.buffered_bytes(), 1000u);
}

TEST(evicts_old_gops_beyond_capacity) {
  SegmentRingBuffer rb(10s);
  feed_video(rb, 0, 60);

  const auto duration = rb.buffered_duration();
  // Should hold roughly the window: at least 10 s minus one GOP of slack,
  // at most 10 s plus one 2 s GOP.
  CHECK(duration >= 8s);
  CHECK(duration <= 12s + 1s);

  // ~12 s of 30 fps @1000 bytes -> far less than the full 60 s.
  CHECK(rb.buffered_bytes() < 60u * 30u * 1000u / 3u);
}

TEST(snapshot_starts_on_keyframe_and_covers_duration) {
  SegmentRingBuffer rb(60s);
  feed_video(rb, 0, 30);

  const auto packets = rb.snapshot(10s);
  CHECK(!packets.empty());
  CHECK(packets.front().keyframe);
  CHECK_EQ(static_cast<int>(packets.front().stream), static_cast<int>(StreamKind::Video));

  const int64_t covered = packets.back().pts_us - packets.front().pts_us;
  CHECK(covered >= 10 * kSec - kSec / 30);  // at least the request
  CHECK(covered <= 12 * kSec);              // at most one extra GOP
}

TEST(snapshot_longer_than_buffer_returns_everything) {
  SegmentRingBuffer rb(60s);
  feed_video(rb, 0, 6);
  const auto all = rb.snapshot(300s);
  CHECK_EQ(all.size(), 6u * 30u);
}

TEST(audio_packets_ride_along_with_gops) {
  SegmentRingBuffer rb(60s);
  rb.push(packet(StreamKind::Video, 0, true));
  rb.push(packet(StreamKind::Audio, 10'000, false, 200));
  rb.push(packet(StreamKind::Video, kSec / 30, false));

  const auto packets = rb.snapshot(60s);
  CHECK_EQ(packets.size(), 3u);
  CHECK_EQ(rb.buffered_bytes(), 2200u);
}

TEST(clear_empties_buffer) {
  SegmentRingBuffer rb(60s);
  feed_video(rb, 0, 4);
  rb.clear();
  CHECK_EQ(rb.buffered_bytes(), 0u);
  CHECK(rb.snapshot(60s).empty());
  // And it only accepts data again from the next keyframe.
  rb.push(packet(StreamKind::Video, 100 * kSec, false));
  CHECK_EQ(rb.buffered_bytes(), 0u);
}

TEST(shrinking_capacity_evicts_immediately) {
  SegmentRingBuffer rb(60s);
  feed_video(rb, 0, 40);
  CHECK(rb.buffered_duration() >= 30s);
  rb.set_capacity(10s);
  CHECK(rb.buffered_duration() <= 13s);
}
