#include "clipster/media/audio_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <vector>

#include "clipster/logging.hpp"

namespace clipster::media {

namespace {
constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;
}  // namespace

struct AudioEncoder::Impl {
  AVCodecContext* ctx = nullptr;
  SwrContext* swr = nullptr;
  AVAudioFifo* fifo = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* pkt = nullptr;
  PacketSink sink;

  int64_t first_pts_us = -1;
  int64_t samples_emitted = 0;

  ~Impl() {
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_audio_fifo_free(fifo);
    swr_free(&swr);
    avcodec_free_context(&ctx);
  }

  bool drain_packets() {
    while (true) {
      const int ret = avcodec_receive_packet(ctx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return true;
      }
      if (ret < 0) {
        log::error("audio encoder: receive_packet failed ({})", ret);
        return false;
      }
      EncodedPacket out;
      out.stream = StreamKind::Audio;
      out.pts_us = pkt->pts;
      out.dts_us = pkt->dts;
      out.keyframe = true;  // every AAC frame is independently decodable
      out.data = std::make_shared<const std::vector<uint8_t>>(pkt->data, pkt->data + pkt->size);
      av_packet_unref(pkt);
      sink(std::move(out));
    }
  }

  bool encode_fifo_frames(bool pad_final) {
    const int frame_size = ctx->frame_size;
    while (true) {
      const int available = av_audio_fifo_size(fifo);
      if (available <= 0 || (available < frame_size && !pad_final)) {
        return true;
      }
      if (av_frame_make_writable(frame) < 0) {
        return false;
      }
      const int to_read = std::min(available, frame_size);
      if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(frame->data), to_read) < to_read) {
        return false;
      }
      if (to_read < frame_size) {
        // Zero-pad the trailing partial frame at flush time.
        for (int ch = 0; ch < kOutChannels; ++ch) {
          float* plane = reinterpret_cast<float*>(frame->data[ch]);
          std::fill(plane + to_read, plane + frame_size, 0.0f);
        }
      }
      frame->pts = first_pts_us + av_rescale(samples_emitted, 1'000'000, kOutRate);
      samples_emitted += to_read;
      if (avcodec_send_frame(ctx, frame) < 0 || !drain_packets()) {
        return false;
      }
      if (to_read < frame_size) {
        return true;  // that was the padded final frame
      }
    }
  }
};

AudioEncoder::AudioEncoder() : impl_(std::make_unique<Impl>()) {}
AudioEncoder::~AudioEncoder() = default;

std::unique_ptr<AudioEncoder> AudioEncoder::create(const AudioEncoderConfig& config,
                                                   PacketSink sink, std::string* error) {
  auto fail = [&](const std::string& msg) -> std::unique_ptr<AudioEncoder> {
    if (error) *error = msg;
    return nullptr;
  };
  if (config.in_sample_rate <= 0 || config.in_channels <= 0 || !sink) {
    return fail("invalid audio encoder config");
  }

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) {
    return fail("AAC encoder not available in this FFmpeg build");
  }

  auto enc = std::unique_ptr<AudioEncoder>(new AudioEncoder());
  Impl& im = *enc->impl_;
  im.sink = std::move(sink);

  im.ctx = avcodec_alloc_context3(codec);
  if (!im.ctx) {
    return fail("could not allocate AAC context");
  }
  im.ctx->sample_rate = kOutRate;
  av_channel_layout_default(&im.ctx->ch_layout, kOutChannels);
  im.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
  im.ctx->bit_rate = static_cast<int64_t>(config.bitrate_kbps) * 1000;
  im.ctx->time_base = AVRational{1, 1'000'000};  // pts in µs, same as video
  im.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // MP4 wants AudioSpecificConfig out of band
  if (avcodec_open2(im.ctx, codec, nullptr) < 0) {
    return fail("could not open AAC encoder");
  }

  AVChannelLayout in_layout;
  av_channel_layout_default(&in_layout, config.in_channels);
  const int swr_ret =
      swr_alloc_set_opts2(&im.swr, &im.ctx->ch_layout, AV_SAMPLE_FMT_FLTP, kOutRate, &in_layout,
                          AV_SAMPLE_FMT_FLT, config.in_sample_rate, 0, nullptr);
  av_channel_layout_uninit(&in_layout);
  if (swr_ret < 0 || swr_init(im.swr) < 0) {
    return fail("could not initialize audio resampler");
  }

  im.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, kOutChannels, kOutRate);
  im.pkt = av_packet_alloc();
  im.frame = av_frame_alloc();
  if (!im.fifo || !im.pkt || !im.frame) {
    return fail("audio encoder allocation failed");
  }
  im.frame->format = AV_SAMPLE_FMT_FLTP;
  im.frame->nb_samples = im.ctx->frame_size;
  av_channel_layout_copy(&im.frame->ch_layout, &im.ctx->ch_layout);
  im.frame->sample_rate = kOutRate;
  if (av_frame_get_buffer(im.frame, 0) < 0) {
    return fail("audio frame allocation failed");
  }

  log::info("audio encoder: aac {} Hz stereo @ {} kbps (input {} Hz / {} ch)", kOutRate,
            config.bitrate_kbps, config.in_sample_rate, config.in_channels);
  return enc;
}

bool AudioEncoder::encode(const float* samples, int frame_count, int64_t pts_us) {
  Impl& im = *impl_;
  if (frame_count <= 0) {
    return true;
  }
  if (im.first_pts_us < 0) {
    im.first_pts_us = pts_us;
  }

  const int out_cap = static_cast<int>(swr_get_out_samples(im.swr, frame_count));
  uint8_t* out_data[kOutChannels] = {};
  int linesize = 0;
  if (av_samples_alloc(out_data, &linesize, kOutChannels, out_cap, AV_SAMPLE_FMT_FLTP, 0) < 0) {
    return false;
  }
  const uint8_t* in_planes[1] = {reinterpret_cast<const uint8_t*>(samples)};
  const int converted = swr_convert(im.swr, out_data, out_cap, in_planes, frame_count);
  bool ok = converted >= 0;
  if (ok && converted > 0) {
    ok = av_audio_fifo_write(im.fifo, reinterpret_cast<void**>(out_data), converted) >= converted;
  }
  av_freep(&out_data[0]);

  return ok && im.encode_fifo_frames(/*pad_final=*/false);
}

void AudioEncoder::flush() {
  Impl& im = *impl_;
  if (im.first_pts_us < 0) {
    return;  // never received any audio
  }
  im.encode_fifo_frames(/*pad_final=*/true);
  avcodec_send_frame(im.ctx, nullptr);
  im.drain_packets();
}

AudioStreamInfo AudioEncoder::stream_info() const {
  const AVCodecContext* ctx = impl_->ctx;
  AudioStreamInfo info;
  info.codec_name = "aac";
  info.sample_rate = ctx->sample_rate;
  info.channels = kOutChannels;
  if (ctx->extradata && ctx->extradata_size > 0) {
    info.extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
  }
  return info;
}

}  // namespace clipster::media
