#include "audio_pipeline.hpp"

#include <vector>

#include "audio_mixer.hpp"
#include "clipster/logging.hpp"
#include "clipster/media/audio_encoder.hpp"
#include "clipster/win/audio_capture.hpp"
#include "clipster/win/process_utils.hpp"
#include "recorder.hpp"

namespace clipster::app {

namespace {
constexpr int kRate = 48000;
constexpr int kChannels = 2;
}  // namespace

struct AudioPipeline::Impl {
  Recorder* recorder = nullptr;
  std::unique_ptr<media::AudioEncoder> encoder;
  std::unique_ptr<AudioMixer> mixer;
  std::vector<std::unique_ptr<win::DesktopLoopbackCapture>> desktop_captures;
  std::vector<std::unique_ptr<win::ProcessLoopbackCapture>> process_captures;
  std::unique_ptr<win::MicrophoneCapture> mic_capture;
  std::unique_ptr<media::AudioEncoder> mic_encoder;  // only for the separate-track path
  bool started = false;
  bool stopped = false;
  bool offset_logged = false;
  int64_t early_drops = 0;

  // Rebases a chunk onto the video timeline and encodes it. Drops audio
  // until the first video frame exists.
  void encode_on_timeline(const float* samples, int frames, int64_t qpc_us) {
    const int64_t base = recorder->timeline_base_us();
    if (base < 0) {
      return;
    }
    const int64_t pts = qpc_us - base;
    if (!offset_logged) {
      offset_logged = true;
      log::info("audio: first audio {} ms after video start", pts / 1000);
    }
    if (pts < 0) {
      // A few early chunks are normal; a stream of them means the device
      // is reporting stream-relative instead of QPC timestamps.
      if (++early_drops == 250) {
        log::warn("audio: dropping many pre-video chunks — device timestamps look "
                  "stream-relative; audio may be missing (please report this)");
      }
      return;
    }
    encoder->encode(samples, frames, pts);
  }

  // Where microphone chunks go; chosen after the capture's format is
  // known (own encoder for the separate track, mixer source when merged).
  std::function<void(const win::AudioChunk&)> mic_sink;

  // Same rebase for the separate microphone track. Mic capture is a
  // continuous stream, so sample-count-derived pts stays gapless.
  void encode_mic_on_timeline(const win::AudioChunk& c) {
    const int64_t base = recorder->timeline_base_us();
    if (base < 0) {
      return;
    }
    const int64_t pts = c.timestamp_us - base;
    if (pts < 0) {
      return;
    }
    mic_encoder->encode(c.samples, c.frame_count, pts);
  }
};

AudioPipeline::AudioPipeline() : impl_(std::make_unique<Impl>()) {}
AudioPipeline::~AudioPipeline() { stop(); }

std::unique_ptr<AudioPipeline> AudioPipeline::create(const Settings& settings, DWORD game_pid,
                                                     Recorder& recorder, std::string* error) {
  auto fail = [&](const std::string& msg) -> std::unique_ptr<AudioPipeline> {
    if (error) *error = msg;
    return nullptr;
  };

  std::string mode = settings.audio.mode;
  if (mode != "desktop" && !win::ProcessLoopbackCapture::is_supported()) {
    log::warn("audio: per-app capture needs Windows 10 2004+; falling back to desktop audio");
    mode = "desktop";
  }
  if (mode == "game_only" && game_pid == 0) {
    log::warn("audio: no game pid available; falling back to desktop audio");
    mode = "desktop";
  }

  auto pipeline = std::unique_ptr<AudioPipeline>(new AudioPipeline());
  Impl& im = *pipeline->impl_;
  im.recorder = &recorder;
  Impl* imp = &im;  // stable pointer for capture callbacks

  const auto make_encoder = [&](int in_rate, int in_channels) {
    media::AudioEncoderConfig cfg;
    cfg.in_sample_rate = in_rate;
    cfg.in_channels = in_channels;
    cfg.bitrate_kbps = settings.audio.bitrate_kbps;
    std::string enc_error;
    im.encoder = media::AudioEncoder::create(
        cfg, [imp](EncodedPacket pkt) { imp->recorder->push_audio_packet(std::move(pkt)); },
        &enc_error);
    return im.encoder != nullptr;
  };

  // Every mode goes through the mixer, even single-source ones: WASAPI
  // delivers nothing while the source is silent, and the wall-clock-paced
  // mixer turns those delivery gaps into explicit silence. Feeding the
  // encoder directly would splice silent periods out of the timeline and
  // make audio drift ahead of video.
  const auto make_mixer = [&](int rate, int channels) {
    im.mixer = std::make_unique<AudioMixer>(
        rate, channels, [imp](const float* samples, int frames, int64_t pts_qpc_us) {
          imp->encode_on_timeline(samples, frames, pts_qpc_us);
        });
  };

  // Optional microphone: its own AAC track by default, or mixed into the
  // main track when the user asked for it and the sample rates line up.
  const auto setup_microphone = [&](int mix_rate) {
    if (!settings.audio.microphone.enabled) {
      return;
    }
    std::string mic_error;
    auto cap = win::MicrophoneCapture::create(
        settings.audio.microphone.device,
        [imp](const win::AudioChunk& c) {
          if (imp->mic_sink) {
            imp->mic_sink(c);
          }
        },
        &mic_error);
    if (!cap) {
      log::warn("audio: microphone disabled: {}", mic_error);
      return;
    }

    bool separate = settings.audio.microphone.separate_track;
    if (!separate && cap->sample_rate() != mix_rate) {
      log::warn("audio: mic rate {} != mix rate {} — keeping the microphone on its own track",
                cap->sample_rate(), mix_rate);
      separate = true;
    }

    if (separate) {
      media::AudioEncoderConfig cfg;
      cfg.in_sample_rate = cap->sample_rate();
      cfg.in_channels = cap->channels();
      cfg.bitrate_kbps = settings.audio.bitrate_kbps;
      cfg.stream_kind = StreamKind::Microphone;
      std::string enc_error;
      im.mic_encoder = media::AudioEncoder::create(
          cfg, [imp](EncodedPacket pkt) { imp->recorder->push_audio_packet(std::move(pkt)); },
          &enc_error);
      if (!im.mic_encoder) {
        log::warn("audio: microphone disabled: {}", enc_error);
        return;
      }
      im.mic_sink = [imp](const win::AudioChunk& c) { imp->encode_mic_on_timeline(c); };
      recorder.set_microphone_info(im.mic_encoder->stream_info());
      log::info("audio: microphone on its own track");
    } else {
      const size_t source = im.mixer->add_source();
      im.mic_sink = [imp, source](const win::AudioChunk& c) { imp->mixer->submit(source, c); };
      log::info("audio: microphone mixed into the main track");
    }
    im.mic_capture = std::move(cap);
  };

  if (mode == "desktop" || mode == "desktop_exclude") {
    DWORD exclude_pid = 0;
    if (mode == "desktop_exclude") {
      std::string excluded_name;
      for (const auto& app_name : settings.audio.exclude_apps) {
        const auto roots = win::find_root_pids_by_exe_name(app_name);
        if (roots.empty()) {
          continue;
        }
        if (exclude_pid == 0) {
          exclude_pid = roots.front();
          excluded_name = app_name;
        } else {
          log::warn("audio: Windows can only exclude one app per capture — "
                    "excluding {}, but {} is also running and WILL be heard",
                    excluded_name, app_name);
        }
      }
      if (exclude_pid != 0) {
        log::info("audio: desktop minus {} (pid {})", excluded_name, exclude_pid);
      }
    }

    const auto single_source_sink = [imp](const win::AudioChunk& c) {
      imp->mixer->submit(0, c);
    };

    std::string cap_error;
    int in_rate = kRate;
    int in_channels = kChannels;
    if (exclude_pid != 0) {
      auto cap = win::ProcessLoopbackCapture::create(
          exclude_pid, win::ProcessLoopbackCapture::Mode::ExcludeTree, single_source_sink,
          &cap_error);
      if (!cap) {
        return fail("exclude capture failed: " + cap_error);
      }
      im.process_captures.push_back(std::move(cap));
    } else {
      auto cap = win::DesktopLoopbackCapture::create(single_source_sink, &cap_error);
      if (!cap) {
        return fail("desktop capture failed: " + cap_error);
      }
      in_rate = cap->sample_rate();
      in_channels = cap->channels();  // swr downmixes 5.1/7.1 properly
      im.desktop_captures.push_back(std::move(cap));
    }
    if (!make_encoder(in_rate, in_channels)) {
      return fail("audio encoder failed");
    }
    make_mixer(in_rate, in_channels);
    im.mixer->add_source();  // the captures above submit to source 0
    setup_microphone(in_rate);
    recorder.set_audio_info(im.encoder->stream_info());
    return pipeline;
  }

  // game_only / include_list: one include-tree capture per target, mixed.
  struct Target {
    DWORD pid;
    std::string label;
  };
  std::vector<Target> targets;
  if (game_pid != 0 && (mode == "game_only" || settings.audio.include_game)) {
    targets.push_back({game_pid, "game"});
  }
  if (mode == "include_list") {
    for (const auto& app_name : settings.audio.include_apps) {
      const auto roots = win::find_root_pids_by_exe_name(app_name);
      if (roots.empty()) {
        log::info("audio: {} is not running — skipped for this session", app_name);
      }
      for (const DWORD pid : roots) {
        if (pid != game_pid) {
          targets.push_back({pid, app_name});
        }
      }
    }
  }
  if (targets.empty()) {
    return fail("no audio sources available (game pid unknown and no listed apps running)");
  }

  if (!make_encoder(kRate, kChannels)) {
    return fail("audio encoder failed");
  }
  make_mixer(kRate, kChannels);

  for (const Target& target : targets) {
    const size_t source = im.mixer->add_source();
    std::string cap_error;
    auto cap = win::ProcessLoopbackCapture::create(
        target.pid, win::ProcessLoopbackCapture::Mode::IncludeTree,
        [imp, source](const win::AudioChunk& c) { imp->mixer->submit(source, c); }, &cap_error);
    if (!cap) {
      log::warn("audio: could not capture {} (pid {}): {}", target.label, target.pid, cap_error);
      continue;
    }
    log::info("audio: capturing {} (pid {})", target.label, target.pid);
    im.process_captures.push_back(std::move(cap));
  }
  if (im.process_captures.empty()) {
    return fail("all audio captures failed");
  }
  setup_microphone(kRate);
  recorder.set_audio_info(im.encoder->stream_info());
  return pipeline;
}

void AudioPipeline::start() {
  Impl& im = *impl_;
  if (im.started) {
    return;
  }
  im.started = true;
  for (auto& cap : im.desktop_captures) {
    cap->start();
  }
  for (auto& cap : im.process_captures) {
    cap->start();
  }
  if (im.mic_capture) {
    im.mic_capture->start();
  }
  if (im.mixer) {
    im.mixer->start();
  }
}

void AudioPipeline::stop() {
  Impl& im = *impl_;
  if (im.stopped) {
    return;
  }
  im.stopped = true;
  for (auto& cap : im.desktop_captures) {
    cap->stop();
  }
  for (auto& cap : im.process_captures) {
    cap->stop();
  }
  if (im.mic_capture) {
    im.mic_capture->stop();
  }
  if (im.mixer) {
    im.mixer->stop();
  }
  if (im.encoder) {
    im.encoder->flush();
  }
  if (im.mic_encoder) {
    im.mic_encoder->flush();
  }
}

}  // namespace clipster::app
