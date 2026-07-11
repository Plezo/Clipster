#include "clipster/media/video_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <string_view>
#include <vector>

#include "clipster/logging.hpp"

namespace clipster::media {

namespace {

std::vector<const char*> candidate_encoders(const std::string& codec) {
  if (codec == "hevc") {
    return {"hevc_nvenc", "hevc_amf", "hevc_qsv", "libx265"};
  }
  // "auto" defaults to H.264: every hardware vendor has an encoder for it
  // and the resulting clips play everywhere (Discord embeds, browsers).
  return {"h264_nvenc", "h264_amf", "h264_qsv", "libopenh264", "libx264"};
}

// The AVCodec::pix_fmts field was deprecated in FFmpeg 7.1 (lavc 61.13)
// in favour of avcodec_get_supported_config().
const AVPixelFormat* supported_pix_fmts(const AVCodec* codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void* list = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list,
                                   &count) < 0) {
    return nullptr;
  }
  return static_cast<const AVPixelFormat*>(list);  // null == "no restriction"
#else
  return codec->pix_fmts;
#endif
}

// Prefers a format we can reach cheaply from BGRA capture frames: BGR0 is a
// straight memcpy, NV12/YUV420P need one swscale conversion.
AVPixelFormat pick_pix_fmt(const AVCodec* codec) {
  const AVPixelFormat* fmts = supported_pix_fmts(codec);
  if (!fmts) {
    return AV_PIX_FMT_YUV420P;  // universally accepted by H.264/HEVC encoders
  }
  for (AVPixelFormat wanted : {AV_PIX_FMT_BGR0, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
      if (*p == wanted) {
        return wanted;
      }
    }
  }
  return fmts[0];
}

}  // namespace

struct VideoEncoder::Impl {
  AVCodecContext* ctx = nullptr;
  SwsContext* sws = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* pkt = nullptr;
  std::string name;
  PacketSink sink;

  ~Impl() {
    av_packet_free(&pkt);
    av_frame_free(&frame);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
  }

  bool drain_packets() {
    while (true) {
      const int ret = avcodec_receive_packet(ctx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return true;
      }
      if (ret < 0) {
        log::error("encoder: receive_packet failed ({})", ret);
        return false;
      }
      EncodedPacket out;
      out.stream = StreamKind::Video;
      out.pts_us = pkt->pts;
      out.dts_us = pkt->dts;
      out.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      out.data = std::make_shared<const std::vector<uint8_t>>(pkt->data, pkt->data + pkt->size);
      av_packet_unref(pkt);
      sink(std::move(out));
    }
  }
};

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() = default;

std::unique_ptr<VideoEncoder> VideoEncoder::create(const VideoEncoderConfig& config,
                                                   PacketSink sink, std::string* error) {
  const int width = config.width & ~1;  // encoders require even dimensions
  const int height = config.height & ~1;
  if (width <= 0 || height <= 0 || !sink) {
    if (error) *error = "invalid encoder config";
    return nullptr;
  }

  for (const char* name : candidate_encoders(config.codec)) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
      continue;  // not compiled into this FFmpeg build
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      continue;
    }
    ctx->width = width;
    ctx->height = height;
    // Microsecond timebase end to end: capture timestamps go in as-is and
    // packet pts comes out in the same unit the ring buffer expects.
    ctx->time_base = AVRational{1, 1'000'000};
    ctx->framerate = AVRational{config.fps, 1};
    ctx->pix_fmt = pick_pix_fmt(codec);
    ctx->bit_rate = static_cast<int64_t>(config.bitrate_kbps) * 1000;
    ctx->gop_size = config.fps * config.gop_seconds;
    // No B-frames: dts == pts, minimal latency, and GOPs stay simple for
    // the ring buffer.
    ctx->max_b_frames = 0;
    // MP4 needs extradata (SPS/PPS) out of band.
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts = nullptr;
    if (std::string_view(name).find("nvenc") != std::string_view::npos) {
      av_dict_set(&opts, "preset", "p4", 0);
      av_dict_set(&opts, "tune", "hq", 0);
    }

    const int ret = avcodec_open2(ctx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
      log::debug("encoder: {} unavailable ({})", name, ret);
      avcodec_free_context(&ctx);
      continue;
    }

    auto enc = std::unique_ptr<VideoEncoder>(new VideoEncoder());
    enc->impl_->ctx = ctx;
    enc->impl_->name = name;
    enc->impl_->sink = std::move(sink);
    enc->impl_->pkt = av_packet_alloc();
    enc->impl_->frame = av_frame_alloc();
    if (!enc->impl_->pkt || !enc->impl_->frame) {
      if (error) *error = "packet/frame allocation failed";
      return nullptr;
    }
    enc->impl_->frame->format = ctx->pix_fmt;
    enc->impl_->frame->width = ctx->width;
    enc->impl_->frame->height = ctx->height;
    if (av_frame_get_buffer(enc->impl_->frame, 0) < 0) {
      if (error) *error = "failed to allocate frame buffer";
      return nullptr;
    }
    log::info("encoder: using {} {}x{} @{} fps, {} kbps", name, ctx->width, ctx->height,
              config.fps, config.bitrate_kbps);
    return enc;
  }

  if (error) {
    *error = "no usable encoder found (tried NVENC/AMF/QSV/software)";
  }
  return nullptr;
}

bool VideoEncoder::encode_bgra(const uint8_t* data, int width, int height, int stride,
                               int64_t pts_us) {
  Impl& im = *impl_;
  AVCodecContext* ctx = im.ctx;

  if (av_frame_make_writable(im.frame) < 0) {
    return false;
  }

  const bool direct = ctx->pix_fmt == AV_PIX_FMT_BGR0 && width == ctx->width &&
                      height == ctx->height;
  if (direct) {
    for (int y = 0; y < ctx->height; ++y) {
      memcpy(im.frame->data[0] + y * im.frame->linesize[0], data + y * stride,
             static_cast<size_t>(ctx->width) * 4);
    }
  } else {
    im.sws = sws_getCachedContext(im.sws, width, height, AV_PIX_FMT_BGRA, ctx->width,
                                  ctx->height, ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
                                  nullptr);
    if (!im.sws) {
      return false;
    }
    const uint8_t* src_data[4] = {data, nullptr, nullptr, nullptr};
    const int src_stride[4] = {stride, 0, 0, 0};
    sws_scale(im.sws, src_data, src_stride, 0, height, im.frame->data, im.frame->linesize);
  }

  im.frame->pts = pts_us;
  if (avcodec_send_frame(ctx, im.frame) < 0) {
    return false;
  }
  return im.drain_packets();
}

void VideoEncoder::flush() {
  avcodec_send_frame(impl_->ctx, nullptr);
  impl_->drain_packets();
}

const std::string& VideoEncoder::encoder_name() const { return impl_->name; }

VideoStreamInfo VideoEncoder::stream_info() const {
  const AVCodecContext* ctx = impl_->ctx;
  VideoStreamInfo info;
  info.codec_name = avcodec_get_name(ctx->codec_id);
  info.width = ctx->width;
  info.height = ctx->height;
  info.fps = ctx->framerate.num;
  if (ctx->extradata && ctx->extradata_size > 0) {
    info.extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
  }
  return info;
}

}  // namespace clipster::media
