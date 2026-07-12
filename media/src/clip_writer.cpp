#include "clipster/media/clip_writer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cstring>

#include "clipster/logging.hpp"

namespace clipster::media {

namespace {

AVCodecID codec_id_from_name(const std::string& name) {
  const AVCodecDescriptor* desc = avcodec_descriptor_get_by_name(name.c_str());
  return desc ? desc->id : AV_CODEC_ID_NONE;
}

bool set_extradata(AVCodecParameters* par, const std::vector<uint8_t>& extradata) {
  if (extradata.empty()) {
    return true;
  }
  par->extradata =
      static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!par->extradata) {
    return false;
  }
  memcpy(par->extradata, extradata.data(), extradata.size());
  par->extradata_size = static_cast<int>(extradata.size());
  return true;
}

struct FormatCtxGuard {
  AVFormatContext* ctx = nullptr;
  ~FormatCtxGuard() {
    if (ctx) {
      if (ctx->pb) {
        avio_closep(&ctx->pb);
      }
      avformat_free_context(ctx);
    }
  }
};

}  // namespace

bool write_clip(const ClipJob& job, std::string* error) {
  const auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    log::error("clip_writer: {}", msg);
    return false;
  };

  const auto first_video =
      std::find_if(job.packets.begin(), job.packets.end(),
                   [](const EncodedPacket& p) { return p.stream == StreamKind::Video; });
  if (first_video == job.packets.end()) {
    return fail("no video packets in clip");
  }
  const int64_t base_us = first_video->pts_us;

  const std::filesystem::path tmp_path = job.out_path.native() +
#ifdef _WIN32
                                         L".part";
#else
                                         ".part";
#endif
  // FFmpeg expects UTF-8 paths on every platform (it converts to UTF-16
  // internally on Windows); path::string() would use the ANSI code page.
  const std::u8string tmp_u8 = tmp_path.u8string();
  const char* tmp_cstr = reinterpret_cast<const char*>(tmp_u8.c_str());

  FormatCtxGuard fmt;
  if (avformat_alloc_output_context2(&fmt.ctx, nullptr, "mp4", tmp_cstr) < 0) {
    return fail("could not allocate mp4 muxer");
  }

  AVStream* video_st = avformat_new_stream(fmt.ctx, nullptr);
  if (!video_st) {
    return fail("could not create video stream");
  }
  video_st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  video_st->codecpar->codec_id = codec_id_from_name(job.video.codec_name);
  video_st->codecpar->width = job.video.width;
  video_st->codecpar->height = job.video.height;
  video_st->time_base = AVRational{1, 1'000'000};
  if (video_st->codecpar->codec_id == AV_CODEC_ID_NONE ||
      !set_extradata(video_st->codecpar, job.video.extradata)) {
    return fail("bad video codec parameters (" + job.video.codec_name + ")");
  }

  const auto make_audio_stream = [&](const AudioStreamInfo& info, const char* title,
                                     AVStream** out) {
    AVStream* st = avformat_new_stream(fmt.ctx, nullptr);
    if (!st) {
      return false;
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = codec_id_from_name(info.codec_name);
    st->codecpar->sample_rate = info.sample_rate;
    av_channel_layout_default(&st->codecpar->ch_layout, info.channels);
    st->time_base = AVRational{1, info.sample_rate};
    av_dict_set(&st->metadata, "title", title, 0);
    if (st->codecpar->codec_id == AV_CODEC_ID_NONE ||
        !set_extradata(st->codecpar, info.extradata)) {
      return false;
    }
    *out = st;
    return true;
  };

  AVStream* audio_st = nullptr;
  if (job.audio && !make_audio_stream(*job.audio, "Game Audio", &audio_st)) {
    return fail("bad audio codec parameters (" + job.audio->codec_name + ")");
  }
  AVStream* mic_st = nullptr;
  if (job.microphone && !make_audio_stream(*job.microphone, "Microphone", &mic_st)) {
    return fail("bad microphone codec parameters (" + job.microphone->codec_name + ")");
  }

  if (avio_open(&fmt.ctx->pb, tmp_cstr, AVIO_FLAG_WRITE) < 0) {
    return fail(std::string("could not open output file: ") + tmp_cstr);
  }

  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "movflags", "+faststart", 0);
  const int header_ret = avformat_write_header(fmt.ctx, &opts);
  av_dict_free(&opts);
  if (header_ret < 0) {
    return fail("could not write mp4 header");
  }

  static const AVRational kMicroTb{1, 1'000'000};
  for (const EncodedPacket& src : job.packets) {
    AVStream* st = nullptr;
    switch (src.stream) {
      case StreamKind::Video:
        st = video_st;
        break;
      case StreamKind::Audio:
        st = audio_st;
        break;
      case StreamKind::Microphone:
        st = mic_st;
        break;
    }
    if (!st) {
      continue;  // captured but not configured for this clip
    }
    if (src.pts_us < base_us) {
      continue;  // audio that predates the clip's first keyframe
    }
    AVPacket* pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, static_cast<int>(src.size())) < 0) {
      av_packet_free(&pkt);
      return fail("out of memory building packet");
    }
    memcpy(pkt->data, src.data->data(), src.size());
    pkt->pts = av_rescale_q(src.pts_us - base_us, kMicroTb, st->time_base);
    pkt->dts = av_rescale_q(std::min(src.dts_us, src.pts_us) - base_us, kMicroTb, st->time_base);
    pkt->dts = std::max<int64_t>(pkt->dts, 0);
    pkt->stream_index = st->index;
    if (src.keyframe) {
      pkt->flags |= AV_PKT_FLAG_KEY;
    }

    const int ret = av_interleaved_write_frame(fmt.ctx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
      return fail("failed writing packet");
    }
  }

  if (av_write_trailer(fmt.ctx) < 0) {
    return fail("failed writing mp4 trailer");
  }
  avio_closep(&fmt.ctx->pb);

  std::error_code ec;
  std::filesystem::rename(tmp_path, job.out_path, ec);
  if (ec) {
    return fail("could not rename temp clip: " + ec.message());
  }
  return true;
}

}  // namespace clipster::media
