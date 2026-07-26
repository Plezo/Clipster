#include "clipster/media/audio_resampler.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cstdint>

#include "clipster/logging.hpp"

namespace clipster::media {

struct AudioResampler::Impl {
  SwrContext* swr = nullptr;
  int out_channels = 2;
  std::vector<float> buf;

  ~Impl() { swr_free(&swr); }
};

AudioResampler::AudioResampler() : impl_(std::make_unique<Impl>()) {}
AudioResampler::~AudioResampler() = default;

std::unique_ptr<AudioResampler> AudioResampler::create(int in_sample_rate, int in_channels,
                                                       int out_sample_rate, int out_channels,
                                                       std::string* error) {
  auto fail = [&](const std::string& msg) -> std::unique_ptr<AudioResampler> {
    if (error) *error = msg;
    return nullptr;
  };
  if (in_sample_rate <= 0 || in_channels <= 0 || out_sample_rate <= 0 || out_channels <= 0) {
    return fail("invalid resampler config");
  }

  auto res = std::unique_ptr<AudioResampler>(new AudioResampler());
  Impl& im = *res->impl_;
  im.out_channels = out_channels;

  AVChannelLayout in_layout;
  AVChannelLayout out_layout;
  av_channel_layout_default(&in_layout, in_channels);
  av_channel_layout_default(&out_layout, out_channels);
  const int ret =
      swr_alloc_set_opts2(&im.swr, &out_layout, AV_SAMPLE_FMT_FLT, out_sample_rate, &in_layout,
                          AV_SAMPLE_FMT_FLT, in_sample_rate, 0, nullptr);
  av_channel_layout_uninit(&in_layout);
  av_channel_layout_uninit(&out_layout);
  if (ret < 0 || swr_init(im.swr) < 0) {
    return fail("could not initialize resampler");
  }

  log::info("audio: resampling {} Hz / {} ch -> {} Hz / {} ch", in_sample_rate, in_channels,
            out_sample_rate, out_channels);
  return res;
}

const std::vector<float>& AudioResampler::convert(const float* samples, int frame_count) {
  Impl& im = *impl_;
  im.buf.clear();
  if (!samples || frame_count <= 0) {
    return im.buf;
  }
  const int64_t capacity = swr_get_out_samples(im.swr, frame_count);
  if (capacity <= 0) {
    return im.buf;
  }
  im.buf.resize(static_cast<size_t>(capacity) * im.out_channels);

  uint8_t* out_planes[1] = {reinterpret_cast<uint8_t*>(im.buf.data())};
  const uint8_t* in_planes[1] = {reinterpret_cast<const uint8_t*>(samples)};
  const int converted =
      swr_convert(im.swr, out_planes, static_cast<int>(capacity), in_planes, frame_count);
  im.buf.resize(converted > 0 ? static_cast<size_t>(converted) * im.out_channels : 0);
  return im.buf;
}

int AudioResampler::out_channels() const { return impl_->out_channels; }

}  // namespace clipster::media
