#pragma once

#include <cstddef>
#include <vector>

namespace clipster {

// Folds a multi-channel microphone stream down to one channel so voice
// always ends up in the centre of the clip's stereo image.
//
// Neither of the obvious approaches works on Windows capture endpoints: a
// straight copy leaves the voice wherever the driver put it (mono capsules
// are routinely exposed as a two-channel endpoint that only fills the left
// channel — the "mic only in my left ear" clip), while a plain average of
// all channels would then drop that same voice 6 dB. So channels that have
// never carried signal are excluded from the average: the fold converges to
// "left channel only" for a half-wired stereo endpoint and to a true average
// for a genuine stereo mic.
//
// Activity is decided from a running per-channel peak, so a channel only has
// to speak up once to join the mix, and is never dropped afterwards.
class MonoDownmixer {
 public:
  explicit MonoDownmixer(int channels);

  // Interleaved input, one sample per frame out. The returned buffer stays
  // valid until the next call.
  const std::vector<float>& process(const float* interleaved, int frame_count);

  int channels() const { return channels_; }
  // Whether the channel has carried signal so far, i.e. contributes to the
  // fold. Exposed for tests and logging.
  bool channel_active(int channel) const;

 private:
  int channels_;
  std::vector<float> peaks_;    // running per-channel peak amplitude
  std::vector<size_t> active_;  // scratch: indices folded into the output
  std::vector<float> out_;
};

}  // namespace clipster
