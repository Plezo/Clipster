#include "clipster/audio_downmix.hpp"

#include <cmath>
#include <vector>

#include "test_framework.hpp"

using namespace clipster;

namespace {

// Interleaves per-channel constant levels into `frames` frames.
std::vector<float> tone(const std::vector<float>& levels, int frames) {
  std::vector<float> out;
  out.reserve(levels.size() * static_cast<size_t>(frames));
  for (int f = 0; f < frames; ++f) {
    // Alternate the sign so the "peak" is a real amplitude, not a DC offset.
    const float sign = (f % 2 == 0) ? 1.0f : -1.0f;
    for (const float level : levels) {
      out.push_back(level * sign);
    }
  }
  return out;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

TEST(downmix_passes_mono_through) {
  MonoDownmixer mixer(1);
  const auto in = tone({0.5f}, 4);
  const auto& out = mixer.process(in.data(), 4);
  CHECK_EQ(out.size(), 4u);
  CHECK(near(out[0], 0.5f));
  CHECK(near(out[1], -0.5f));
}

TEST(downmix_keeps_level_when_only_one_channel_is_wired) {
  // The common Windows case: a mono capsule on a two-channel endpoint. The
  // voice must stay at full level, not be halved by averaging in silence.
  MonoDownmixer mixer(2);
  const auto in = tone({0.5f, 0.0f}, 8);
  const auto& out = mixer.process(in.data(), 8);
  CHECK_EQ(out.size(), 8u);
  CHECK(near(out[0], 0.5f));
  CHECK(mixer.channel_active(0));
  CHECK(!mixer.channel_active(1));
}

TEST(downmix_averages_a_real_stereo_mic) {
  MonoDownmixer mixer(2);
  const auto in = tone({0.6f, 0.2f}, 8);
  const auto& out = mixer.process(in.data(), 8);
  CHECK(near(out[0], 0.4f));
  CHECK(mixer.channel_active(0));
  CHECK(mixer.channel_active(1));
}

TEST(downmix_ignores_hiss_and_crosstalk) {
  MonoDownmixer mixer(2);
  const auto in = tone({0.8f, 0.001f}, 8);  // right channel ~ -60 dBFS
  const auto& out = mixer.process(in.data(), 8);
  CHECK(near(out[0], 0.8f));
  CHECK(!mixer.channel_active(1));
}

TEST(downmix_lets_a_late_channel_join) {
  MonoDownmixer mixer(2);
  const auto left_only = tone({0.5f, 0.0f}, 8);
  mixer.process(left_only.data(), 8);
  CHECK(!mixer.channel_active(1));

  const auto both = tone({0.5f, 0.5f}, 8);
  const auto& out = mixer.process(both.data(), 8);
  CHECK(mixer.channel_active(1));
  CHECK(near(out[0], 0.5f));  // averaged, not summed to 1.0
}

TEST(downmix_of_pure_silence_stays_silent) {
  MonoDownmixer mixer(2);
  const auto in = tone({0.0f, 0.0f}, 4);
  const auto& out = mixer.process(in.data(), 4);
  CHECK_EQ(out.size(), 4u);
  CHECK(near(out[0], 0.0f));
  // Nothing is locked out: a channel that speaks up later still joins.
  const auto right_only = tone({0.0f, 0.4f}, 8);
  mixer.process(right_only.data(), 8);
  CHECK(mixer.channel_active(1));
  CHECK(!mixer.channel_active(0));
}

TEST(downmix_clamps_a_summed_overload) {
  MonoDownmixer mixer(2);
  const auto loud = tone({1.0f, 1.0f}, 4);
  const auto& out = mixer.process(loud.data(), 4);
  CHECK(near(out[0], 1.0f));
  CHECK(out[0] <= 1.0f);
}

TEST(downmix_handles_empty_input) {
  MonoDownmixer mixer(2);
  CHECK(mixer.process(nullptr, 4).empty());
  const auto in = tone({0.5f, 0.5f}, 1);
  CHECK(mixer.process(in.data(), 0).empty());
}

TEST(downmix_folds_more_than_two_channels) {
  MonoDownmixer mixer(4);
  const auto in = tone({0.4f, 0.0f, 0.2f, 0.0f}, 8);
  const auto& out = mixer.process(in.data(), 8);
  CHECK(near(out[0], 0.3f));  // only the two channels carrying signal
  CHECK(mixer.channel_active(2));
  CHECK(!mixer.channel_active(3));
}
