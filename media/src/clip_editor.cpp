#include "clipster/media/clip_editor.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "clipster/logging.hpp"
#include "clipster/media/clip_writer.hpp"
#include "clipster/media/video_encoder.hpp"

namespace clipster::media {

namespace {

constexpr AVRational kMicroTb{1, 1'000'000};

struct InputGuard {
  AVFormatContext* ctx = nullptr;
  ~InputGuard() {
    if (ctx) {
      avformat_close_input(&ctx);
    }
  }
};

struct DecoderGuard {
  AVCodecContext* ctx = nullptr;
  ~DecoderGuard() {
    if (ctx) {
      avcodec_free_context(&ctx);
    }
  }
};

struct SwsGuard {
  SwsContext* ctx = nullptr;
  ~SwsGuard() {
    if (ctx) {
      sws_freeContext(ctx);
    }
  }
};

int64_t to_us(int64_t ts, AVRational tb) { return av_rescale_q(ts, tb, kMicroTb); }

EncodedPacket copy_packet(const AVPacket* pkt, StreamKind kind, AVRational tb) {
  EncodedPacket out;
  out.stream = kind;
  out.pts_us = to_us(pkt->pts, tb);
  out.dts_us = pkt->dts == AV_NOPTS_VALUE ? out.pts_us : to_us(pkt->dts, tb);
  out.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
  out.data = std::make_shared<const std::vector<uint8_t>>(pkt->data, pkt->data + pkt->size);
  return out;
}

}  // namespace

// Reads the source clip, applies the trim window (and optional crop via a
// decode -> re-encode of the video track; audio is always a bit-exact
// copy), then hands the packets to write_clip for the actual muxing.
bool export_edit(const EditJob& job, std::string* error) {
  const auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    log::error("clip_editor: {}", msg);
    return false;
  };

  const std::u8string in_u8 = job.in_path.u8string();
  InputGuard in;
  if (avformat_open_input(&in.ctx, reinterpret_cast<const char*>(in_u8.c_str()), nullptr,
                          nullptr) < 0) {
    return fail("could not open " + job.in_path.filename().string());
  }
  if (avformat_find_stream_info(in.ctx, nullptr) < 0) {
    return fail("could not read stream info");
  }

  const int video_idx = av_find_best_stream(in.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_idx < 0) {
    return fail("no video stream in clip");
  }
  AVStream* video_st = in.ctx->streams[video_idx];
  const AVCodecParameters* vpar = video_st->codecpar;

  // First two audio tracks map onto the Game Audio / Microphone slots the
  // muxer knows about (Clipster's own clips are written in that order).
  std::vector<int> audio_idx;
  for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
    if (in.ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audio_idx.size() < 2) {
      audio_idx.push_back(static_cast<int>(i));
    }
  }
  const auto audio_kind = [&](int stream_index) {
    return stream_index == audio_idx[0] ? StreamKind::Audio : StreamKind::Microphone;
  };

  const int64_t duration_us = in.ctx->duration > 0 ? in.ctx->duration
                                                   : std::numeric_limits<int64_t>::max();
  const int64_t start_us = std::clamp<int64_t>(job.start_us, 0, duration_us);
  const int64_t end_us =
      job.end_us < 0 ? duration_us : std::clamp<int64_t>(job.end_us, 0, duration_us);
  if (end_us <= start_us) {
    return fail("selection is empty");
  }
  const auto report = [&](int64_t pts_us) {
    if (job.progress) {
      const float span = static_cast<float>(end_us - start_us);
      job.progress(std::clamp(static_cast<float>(pts_us - start_us) / span, 0.0f, 1.0f));
    }
  };

  // Land on the last keyframe at or before the trim start; the decode path
  // then drops frames until the exact start, the remux path keeps them.
  if (start_us > 0 &&
      av_seek_frame(in.ctx, video_idx, av_rescale_q(start_us, kMicroTb, video_st->time_base),
                    AVSEEK_FLAG_BACKWARD) < 0) {
    return fail("could not seek in clip");
  }

  // The re-encode pipeline (crop only): decode -> full-frame BGRA -> hand
  // the crop window to VideoEncoder via pointer offset + stride.
  DecoderGuard dec;
  SwsGuard sws;
  std::unique_ptr<VideoEncoder> encoder;
  std::vector<uint8_t> bgra;
  CropRect crop;
  std::vector<EncodedPacket> packets;
  if (job.crop) {
    crop = *job.crop;
    crop.x = std::clamp(crop.x, 0, std::max(0, vpar->width - 2));
    crop.y = std::clamp(crop.y, 0, std::max(0, vpar->height - 2));
    crop.width = std::clamp(crop.width, 2, vpar->width - crop.x);
    crop.height = std::clamp(crop.height, 2, vpar->height - crop.y);

    const AVCodec* codec = avcodec_find_decoder(vpar->codec_id);
    if (!codec) {
      return fail(std::string("no decoder for ") + avcodec_get_name(vpar->codec_id));
    }
    dec.ctx = avcodec_alloc_context3(codec);
    if (!dec.ctx || avcodec_parameters_to_context(dec.ctx, vpar) < 0 ||
        avcodec_open2(dec.ctx, codec, nullptr) < 0) {
      return fail("could not open video decoder");
    }

    VideoEncoderConfig cfg;
    cfg.width = crop.width;
    cfg.height = crop.height;
    const AVRational fr = video_st->avg_frame_rate;
    cfg.fps = fr.num > 0 && fr.den > 0 ? std::max(1, fr.num / fr.den) : 60;
    cfg.codec = job.codec;
    cfg.bitrate_kbps = job.bitrate_kbps;
    encoder = VideoEncoder::create(
        cfg, [&packets](EncodedPacket p) { packets.push_back(std::move(p)); }, error);
    if (!encoder) {
      return false;  // create() filled *error
    }
  }

  AVFrame* frame = av_frame_alloc();
  AVPacket* pkt = av_packet_alloc();
  const auto cleanup = [&] {
    av_frame_free(&frame);
    av_packet_free(&pkt);
  };
  if (!frame || !pkt) {
    cleanup();
    return fail("out of memory");
  }

  // Decoded frames outside [start, end] are dropped; returns false once the
  // trim end has been passed. Conversion problems set convert_error.
  std::string convert_error;
  const auto encode_decoded = [&]() -> bool {
    while (avcodec_receive_frame(dec.ctx, frame) == 0) {
      const int64_t frame_ts =
          frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
                                                         : frame->pts;
      if (frame_ts == AV_NOPTS_VALUE) {
        av_frame_unref(frame);
        continue;
      }
      const int64_t pts_us = to_us(frame_ts, video_st->time_base);
      if (pts_us > end_us) {
        av_frame_unref(frame);
        return false;
      }
      if (pts_us >= start_us) {
        sws.ctx = sws_getCachedContext(sws.ctx, frame->width, frame->height,
                                       static_cast<AVPixelFormat>(frame->format), frame->width,
                                       frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                       nullptr, nullptr);
        const int stride = frame->width * 4;
        bgra.resize(static_cast<size_t>(stride) * frame->height);
        uint8_t* dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {stride, 0, 0, 0};
        if (!sws.ctx ||
            sws_scale(sws.ctx, frame->data, frame->linesize, 0, frame->height, dst,
                      dst_stride) <= 0) {
          convert_error = "could not convert frame for cropping";
          av_frame_unref(frame);
          return false;
        }
        encoder->encode_bgra(
            bgra.data() + static_cast<size_t>(crop.y) * stride + static_cast<size_t>(crop.x) * 4,
            crop.width, crop.height, stride, pts_us);
        report(pts_us);
      }
      av_frame_unref(frame);
    }
    return true;
  };

  // Keep reading past the video trim end until every audio stream has
  // crossed it too — the file interleaves audio slightly behind video.
  bool video_done = false;
  std::vector<bool> audio_done(audio_idx.size(), false);
  const auto all_audio_done = [&] {
    return std::all_of(audio_done.begin(), audio_done.end(), [](bool d) { return d; });
  };
  while (!(video_done && all_audio_done()) && convert_error.empty() &&
         av_read_frame(in.ctx, pkt) >= 0) {
    AVStream* st = in.ctx->streams[pkt->stream_index];
    if (pkt->pts == AV_NOPTS_VALUE) {
      av_packet_unref(pkt);
      continue;
    }
    const int64_t pts_us = to_us(pkt->pts, st->time_base);
    if (pkt->stream_index == video_idx) {
      if (job.crop) {
        if (!video_done && avcodec_send_packet(dec.ctx, pkt) == 0 && !encode_decoded()) {
          video_done = true;
        }
      } else if (pts_us <= end_us) {
        packets.push_back(copy_packet(pkt, StreamKind::Video, st->time_base));
        report(pts_us);
      } else {
        video_done = true;
      }
    } else {
      const auto it = std::find(audio_idx.begin(), audio_idx.end(), pkt->stream_index);
      if (it != audio_idx.end()) {
        if (pts_us <= end_us) {
          // write_clip drops audio that predates the first video packet, so
          // over-collecting at the front is fine.
          packets.push_back(copy_packet(pkt, audio_kind(pkt->stream_index), st->time_base));
        } else {
          audio_done[it - audio_idx.begin()] = true;
        }
      }
    }
    av_packet_unref(pkt);
  }

  if (job.crop && convert_error.empty()) {
    if (!video_done) {  // hit EOF: drain the decoder's delayed frames
      avcodec_send_packet(dec.ctx, nullptr);
      encode_decoded();
    }
    encoder->flush();
  }
  cleanup();
  if (!convert_error.empty()) {
    return fail(convert_error);
  }

  const bool has_video =
      std::any_of(packets.begin(), packets.end(),
                  [](const EncodedPacket& p) { return p.stream == StreamKind::Video; });
  if (!has_video) {
    return fail("selection contains no video");
  }

  // Interleave for the muxer: packets arrive video-then-audio interleaved
  // from the file, but the re-encode path appends encoder output in bursts.
  std::stable_sort(packets.begin(), packets.end(),
                   [](const EncodedPacket& a, const EncodedPacket& b) {
                     return std::min(a.dts_us, a.pts_us) < std::min(b.dts_us, b.pts_us);
                   });

  ClipJob out;
  out.packets = std::move(packets);
  out.out_path = job.out_path;
  if (job.crop) {
    out.video = encoder->stream_info();
  } else {
    out.video.codec_name = avcodec_get_name(vpar->codec_id);
    out.video.width = vpar->width;
    out.video.height = vpar->height;
    const AVRational fr = video_st->avg_frame_rate;
    out.video.fps = fr.num > 0 && fr.den > 0 ? std::max(1, fr.num / fr.den) : 0;
    if (vpar->extradata && vpar->extradata_size > 0) {
      out.video.extradata.assign(vpar->extradata, vpar->extradata + vpar->extradata_size);
    }
  }
  for (size_t i = 0; i < audio_idx.size(); ++i) {
    const AVCodecParameters* apar = in.ctx->streams[audio_idx[i]]->codecpar;
    AudioStreamInfo info;
    info.codec_name = avcodec_get_name(apar->codec_id);
    info.sample_rate = apar->sample_rate;
    info.channels = apar->ch_layout.nb_channels;
    if (apar->extradata && apar->extradata_size > 0) {
      info.extradata.assign(apar->extradata, apar->extradata + apar->extradata_size);
    }
    (i == 0 ? out.audio : out.microphone) = std::move(info);
  }

  if (!write_clip(out, error)) {
    return false;
  }
  if (job.progress) {
    job.progress(1.0f);
  }
  return true;
}

}  // namespace clipster::media
