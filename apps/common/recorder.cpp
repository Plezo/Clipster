#include "recorder.hpp"

#include <windows.h>

#include <mmsystem.h>

#include <format>

#include "clipster/logging.hpp"
#include "clipster/media/clip_writer.hpp"
#include "clipster/util.hpp"
#include "clipster/win/known_folders.hpp"

namespace clipster::app {

std::filesystem::path executable_dir() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return std::filesystem::path(std::wstring_view(buf, len)).parent_path();
}

void play_clip_saved_sound(const Settings& settings) {
  if (!settings.notifications.sound_enabled) {
    return;
  }
  std::filesystem::path wav = settings.notifications.sound_file.empty()
                                  ? executable_dir() / L"assets" / L"clip_saved.wav"
                                  : std::filesystem::path(settings.notifications.sound_file);
  std::error_code ec;
  if (std::filesystem::exists(wav, ec) &&
      PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
    return;
  }
  MessageBeep(MB_OK);
}

Recorder::Recorder(Settings settings, std::string game_name, std::function<void()> on_fatal)
    : settings_(std::move(settings)),
      game_name_(std::move(game_name)),
      on_fatal_(std::move(on_fatal)),
      ring_(std::chrono::seconds(settings_.recording.buffer_seconds)),
      frame_interval_us_(1'000'000 / settings_.recording.fps) {}

void Recorder::on_frame(const win::CapturedFrame& frame) {
  if (first_pts_us_ < 0) {
    first_pts_us_ = frame.timestamp_us;
  }
  const int64_t pts = frame.timestamp_us - first_pts_us_;

  // Pace the display-rate frame stream down to the target fps.
  if (pts < next_due_us_) {
    return;
  }
  next_due_us_ += frame_interval_us_;
  if (pts > next_due_us_ + 1'000'000) {
    next_due_us_ = pts + frame_interval_us_;  // fell behind (alt-tab, load screen)
  }

  if (!encoder_) {
    media::VideoEncoderConfig cfg;
    cfg.width = frame.width;
    cfg.height = frame.height;
    cfg.fps = settings_.recording.fps;
    cfg.bitrate_kbps = settings_.recording.bitrate_kbps;
    cfg.codec = settings_.recording.codec;
    std::string error;
    encoder_ = media::VideoEncoder::create(
        cfg, [this](EncodedPacket pkt) { ring_.push(std::move(pkt)); }, &error);
    if (!encoder_) {
      log::error("cannot create encoder: {}", error);
      if (on_fatal_) {
        on_fatal_();
      }
      return;
    }
    encoder_ready_.store(true, std::memory_order_release);
  }

  encoder_->encode_bgra(frame.data, frame.width, frame.height, frame.stride, pts);
  frames_encoded_.fetch_add(1, std::memory_order_relaxed);
}

void Recorder::save_clip() {
  save_clip(std::chrono::seconds(settings_.clip.default_length_seconds));
}

void Recorder::save_clip(std::chrono::seconds length) {
  if (!encoder_ready_.load(std::memory_order_acquire)) {
    log::warn("no frames captured yet — nothing to clip");
    return;
  }

  media::ClipJob job;
  job.packets = ring_.snapshot(length);
  if (job.packets.empty()) {
    log::warn("ring buffer is empty — nothing to clip");
    return;
  }
  job.video = encoder_->stream_info();
  job.out_path = build_output_path();

  // Owned, not detached: finish() joins these before shutdown, so quitting
  // mid-write cannot race static destruction or truncate the clip.
  std::lock_guard lock(writers_mutex_);
  writers_.emplace_back([job = std::move(job), settings = settings_] {
    std::string error;
    if (media::write_clip(job, &error)) {
      log::info("clip saved: {}", job.out_path.string());
      play_clip_saved_sound(settings);
    } else {
      log::error("clip failed: {}", error);
      MessageBeep(MB_ICONERROR);
    }
  });
}

void Recorder::finish() {
  std::vector<std::thread> writers;
  {
    std::lock_guard lock(writers_mutex_);
    writers.swap(writers_);
  }
  for (std::thread& t : writers) {
    t.join();
  }
}

std::string Recorder::stats() {
  const auto dur = std::chrono::duration_cast<std::chrono::seconds>(ring_.buffered_duration());
  return std::format("buffered {}s / {} MB, {} frames encoded", dur.count(),
                     ring_.buffered_bytes() / (1024 * 1024),
                     frames_encoded_.load(std::memory_order_relaxed));
}

std::filesystem::path Recorder::build_output_path() const {
  std::filesystem::path dir = settings_.output.save_dir.empty()
                                  ? win::default_save_dir()
                                  : std::filesystem::path(settings_.output.save_dir);
  if (settings_.output.subfolder_per_game) {
    dir /= util::sanitize_filename(game_name_);
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &t);
  const std::string name = util::expand_template(
      settings_.output.filename_template,
      {{"game", game_name_},
       {"date", std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)},
       {"time", std::format("{:02}-{:02}-{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec)}});
  return dir / (util::sanitize_filename(name) + ".mp4");
}

}  // namespace clipster::app
