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
  bool started = false;
  bool stopped = false;

  // Rebases a chunk onto the video timeline and encodes it. Drops audio
  // until the first video frame exists.
  void encode_on_timeline(const float* samples, int frames, int64_t qpc_us) {
    const int64_t base = recorder->timeline_base_us();
    if (base < 0) {
      return;
    }
    const int64_t pts = qpc_us - base;
    if (pts < 0) {
      return;
    }
    encoder->encode(samples, frames, pts);
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
  if (settings.audio.microphone.enabled) {
    log::warn("audio: microphone capture is not implemented yet");
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

  const auto direct_sink = [imp](const win::AudioChunk& c) {
    imp->encode_on_timeline(c.samples, c.frame_count, c.timestamp_us);
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

    std::string cap_error;
    if (exclude_pid != 0) {
      auto cap = win::ProcessLoopbackCapture::create(
          exclude_pid, win::ProcessLoopbackCapture::Mode::ExcludeTree, direct_sink, &cap_error);
      if (!cap) {
        return fail("exclude capture failed: " + cap_error);
      }
      if (!make_encoder(kRate, kChannels)) {
        return fail("audio encoder failed");
      }
      im.process_captures.push_back(std::move(cap));
    } else {
      auto cap = win::DesktopLoopbackCapture::create(direct_sink, &cap_error);
      if (!cap) {
        return fail("desktop capture failed: " + cap_error);
      }
      if (!make_encoder(cap->sample_rate(), cap->channels())) {
        return fail("audio encoder failed");
      }
      im.desktop_captures.push_back(std::move(cap));
    }
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
  im.mixer = std::make_unique<AudioMixer>(
      kRate, kChannels, [imp](const float* samples, int frames, int64_t pts_qpc_us) {
        imp->encode_on_timeline(samples, frames, pts_qpc_us);
      });

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
  if (im.mixer) {
    im.mixer->stop();
  }
  if (im.encoder) {
    im.encoder->flush();
  }
}

}  // namespace clipster::app
