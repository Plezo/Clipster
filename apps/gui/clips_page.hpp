#pragma once

#include <QtMultimedia>
#include <QtMultimediaWidgets>
#include <QtWidgets>

#include <functional>
#include <optional>
#include <thread>

#include "clipster/settings.hpp"

namespace clipster::gui {

// Seek bar with a playhead plus draggable trim handles riding on top of the
// track. Dragging a handle reports the handle position so the owner can
// preview that frame; the handles can never cross (100 ms minimum gap).
class TimelineBar : public QWidget {
 public:
  TimelineBar();

  void set_duration(qint64 ms);
  void set_position(qint64 ms);
  void reset_selection();
  qint64 sel_start_ms() const { return start_ms_; }
  qint64 sel_end_ms() const { return end_ms_; }  // -1 = clip end
  bool has_selection() const { return start_ms_ > 0 || end_ms_ >= 0; }

  std::function<void(qint64)> on_seek;     // playhead pressed / dragged
  std::function<void(qint64)> on_preview;  // trim handle pressed / dragged
  std::function<void()> on_selection_changed;
  std::function<void()> on_trim_released;  // a trim handle was let go

  QSize sizeHint() const override { return {200, 34}; }

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  enum class Drag { None, Playhead, Start, End };

  QRect track_rect() const;
  int x_for(qint64 ms) const;
  qint64 ms_for(int x) const;
  qint64 effective_end() const { return end_ms_ < 0 ? duration_ : end_ms_; }
  Drag hit(const QPoint& pos) const;

  qint64 duration_ = 0;
  qint64 position_ = 0;
  qint64 start_ms_ = 0;
  qint64 end_ms_ = -1;  // -1 = clip end
  Drag drag_ = Drag::None;
};

// The video surface: a graphics view whose scene is in video-pixel
// coordinates (the video item is sized to the native frame size), so a
// crop rectangle drawn with the mouse maps 1:1 onto source pixels.
class VideoCanvas : public QGraphicsView {
 public:
  VideoCanvas();

  QGraphicsVideoItem* video_item() { return video_item_; }
  void set_crop_mode(bool on);
  void clear_crop();
  std::optional<QRect> crop_rect() const;  // in video pixels

  std::function<void()> on_crop_changed;

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  void fit();
  QPointF clamp_to_video(QPointF p) const;
  void update_crop_item();

  QGraphicsVideoItem* video_item_ = nullptr;
  QGraphicsRectItem* crop_item_ = nullptr;
  bool crop_mode_ = false;
  bool dragging_ = false;
  bool moving_ = false;  // dragging inside an existing rect moves it
  QPointF drag_origin_;
  QPointF move_offset_;
  QRectF crop_;  // empty = no crop
};

// The "Clips" tab: browse saved clips, watch them in-app, and export a
// trimmed / cropped copy (written as a new file next to the original).
class ClipsPage : public QWidget {
 public:
  explicit ClipsPage(const Settings* settings);
  ~ClipsPage() override;

  // Newest-first (label, absolute path) pairs; keeps the current selection.
  void set_clips(const QVector<QPair<QString, QString>>& clips);
  // Select the clip in the list (if present) and start playing it.
  void open_clip(const QString& path);

  std::function<void()> request_refresh;  // ask the owner to rescan the clips dir

 private:
  void load_clip(const QString& path);
  void unload_current();
  void toggle_play();
  void update_export_ui();
  void start_export();
  void rename_clip(const QString& path);
  void show_context_menu(const QPoint& pos);
  QIcon crop_icon() const;
  static QString sanitize_filename(QString name);
  static QString unique_path(const QDir& dir, const QString& base);
  static QString format_ms(qint64 ms);

  const Settings* settings_;

  QListWidget* list_ = nullptr;
  QLabel* title_label_ = nullptr;
  VideoCanvas* canvas_ = nullptr;
  QMediaPlayer* player_ = nullptr;
  QAudioOutput* audio_out_ = nullptr;
  TimelineBar* timeline_ = nullptr;
  QToolButton* play_btn_ = nullptr;
  QLabel* time_label_ = nullptr;
  QToolButton* reset_btn_ = nullptr;
  QToolButton* crop_btn_ = nullptr;
  QToolButton* clear_crop_btn_ = nullptr;
  QLabel* edit_label_ = nullptr;
  QPushButton* export_btn_ = nullptr;
  QProgressBar* export_progress_ = nullptr;

  QString current_path_;
  bool exporting_ = false;
  std::thread export_thread_;
};

}  // namespace clipster::gui
