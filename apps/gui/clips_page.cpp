#include "clips_page.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "clipster/media/clip_editor.hpp"

namespace clipster::gui {

namespace {
constexpr int kMargin = 12;         // room for the grips to overhang the track
constexpr qint64 kMinGapMs = 100;   // smallest allowed selection
constexpr qint64 kSnapMs = 60;      // dragging this close to an edge = "no trim"
const QColor kTrimColor(230, 90, 90);
}  // namespace

// ---------------------------------------------------------------- timeline

TimelineBar::TimelineBar() {
  setMouseTracking(true);
  setMinimumHeight(34);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void TimelineBar::set_duration(qint64 ms) {
  duration_ = std::max<qint64>(ms, 0);
  if (end_ms_ >= duration_) {
    end_ms_ = -1;
  }
  if (start_ms_ > duration_) {
    start_ms_ = 0;
  }
  update();
}

void TimelineBar::set_position(qint64 ms) {
  if (drag_ == Drag::Playhead) {
    return;  // the user owns the playhead while scrubbing
  }
  position_ = ms;
  update();
}

void TimelineBar::reset_selection() {
  start_ms_ = 0;
  end_ms_ = -1;
  update();
}

QRect TimelineBar::track_rect() const {
  return QRect(kMargin, height() - 12, width() - 2 * kMargin, 6);
}

int TimelineBar::x_for(qint64 ms) const {
  const QRect tr = track_rect();
  if (duration_ <= 0 || tr.width() <= 0) {
    return tr.left();
  }
  ms = std::clamp<qint64>(ms, 0, duration_);
  return tr.left() + static_cast<int>(ms * tr.width() / duration_);
}

qint64 TimelineBar::ms_for(int x) const {
  const QRect tr = track_rect();
  if (duration_ <= 0 || tr.width() <= 0) {
    return 0;
  }
  const int cl = std::clamp(x, tr.left(), tr.right());
  return static_cast<qint64>(cl - tr.left()) * duration_ / tr.width();
}

TimelineBar::Drag TimelineBar::hit(const QPoint& pos) const {
  const QRect tr = track_rect();
  const int xs = x_for(start_ms_);
  const int xe = x_for(effective_end());
  const auto grip = [&](int x) { return QRect(x - 6, tr.top() - 14, 13, tr.height() + 18); };
  const bool in_s = grip(xs).contains(pos);
  const bool in_e = grip(xe).contains(pos);
  if (in_s && in_e) {
    return std::abs(pos.x() - xs) <= std::abs(pos.x() - xe) ? Drag::Start : Drag::End;
  }
  if (in_s) {
    return Drag::Start;
  }
  if (in_e) {
    return Drag::End;
  }
  return Drag::Playhead;
}

void TimelineBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRect tr = track_rect();
  p.setPen(Qt::NoPen);
  p.setBrush(palette().color(QPalette::Mid));
  p.drawRoundedRect(tr, 3, 3);
  if (duration_ <= 0) {
    return;
  }

  const int xp = x_for(position_);
  p.setBrush(palette().color(QPalette::Highlight));
  p.drawRoundedRect(QRect(tr.left(), tr.top(), std::max(0, xp - tr.left()), tr.height()), 3, 3);

  // Trim selection band and its grips, riding on top of the track.
  const int xs = x_for(start_ms_);
  const int xe = x_for(effective_end());
  QColor band = kTrimColor;
  band.setAlpha(has_selection() ? 110 : 55);
  p.setBrush(band);
  p.drawRect(QRect(QPoint(xs, tr.top() - 3), QPoint(xe, tr.bottom() + 3)));
  const auto grip = [&](int x) { return QRect(x - 4, tr.top() - 13, 9, tr.height() + 16); };
  p.setBrush(kTrimColor);
  p.drawRoundedRect(grip(xs), 3, 3);
  p.drawRoundedRect(grip(xe), 3, 3);
  p.setPen(QPen(Qt::white, 1));
  p.drawLine(xs, tr.top() - 9, xs, tr.bottom());
  p.drawLine(xe, tr.top() - 9, xe, tr.bottom());

  p.setPen(QPen(palette().color(QPalette::Text), 2));
  p.drawLine(xp, tr.top() - 6, xp, tr.bottom() + 5);
}

void TimelineBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || duration_ <= 0) {
    return;
  }
  drag_ = hit(event->pos());
  mouseMoveEvent(event);  // apply the press position immediately
}

void TimelineBar::mouseMoveEvent(QMouseEvent* event) {
  if (drag_ == Drag::None) {
    const Drag h = hit(event->pos());
    setCursor(h == Drag::Start || h == Drag::End ? Qt::SizeHorCursor : Qt::PointingHandCursor);
    return;
  }
  const qint64 ms = ms_for(event->pos().x());
  switch (drag_) {
    case Drag::Playhead:
      position_ = ms;
      if (on_seek) {
        on_seek(ms);
      }
      break;
    case Drag::Start: {
      qint64 v = std::max<qint64>(std::min(ms, effective_end() - kMinGapMs), 0);
      if (v < kSnapMs) {
        v = 0;
      }
      start_ms_ = v;
      if (on_preview) {
        on_preview(start_ms_);
      }
      if (on_selection_changed) {
        on_selection_changed();
      }
      break;
    }
    case Drag::End: {
      const qint64 v = std::min<qint64>(std::max(ms, start_ms_ + kMinGapMs), duration_);
      end_ms_ = v > duration_ - kSnapMs ? -1 : v;
      if (on_preview) {
        on_preview(effective_end());
      }
      if (on_selection_changed) {
        on_selection_changed();
      }
      break;
    }
    case Drag::None:
      break;
  }
  update();
}

void TimelineBar::mouseReleaseEvent(QMouseEvent*) {
  const bool trim = drag_ == Drag::Start || drag_ == Drag::End;
  drag_ = Drag::None;
  if (trim && on_trim_released) {
    on_trim_released();
  }
}

void TimelineBar::leaveEvent(QEvent*) { unsetCursor(); }

// ------------------------------------------------------------ video canvas

VideoCanvas::VideoCanvas() {
  auto* scene = new QGraphicsScene(this);
  setScene(scene);
  video_item_ = new QGraphicsVideoItem;
  scene->addItem(video_item_);

  QPen pen(QColor(255, 210, 70), 2, Qt::DashLine);
  pen.setCosmetic(true);  // constant on-screen width regardless of zoom
  crop_item_ = scene->addRect(QRectF(), pen);
  crop_item_->setZValue(1);
  crop_item_->setVisible(false);

  setBackgroundBrush(Qt::black);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setRenderHint(QPainter::SmoothPixmapTransform);
  setMinimumHeight(200);

  // Size the item to the native frame so scene units == video pixels; the
  // view then scales everything to fit.
  QObject::connect(video_item_, &QGraphicsVideoItem::nativeSizeChanged, video_item_,
                   [this](const QSizeF& size) {
                     if (size.isEmpty()) {
                       return;
                     }
                     video_item_->setSize(size);
                     setSceneRect(QRectF(QPointF(0, 0), size));
                     crop_ = QRectF();
                     update_crop_item();
                     fit();
                   });
}

void VideoCanvas::set_crop_mode(bool on) {
  crop_mode_ = on;
  setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
}

void VideoCanvas::clear_crop() {
  crop_ = QRectF();
  dragging_ = moving_ = false;
  update_crop_item();
  if (on_crop_changed) {
    on_crop_changed();
  }
}

std::optional<QRect> VideoCanvas::crop_rect() const {
  if (crop_.isEmpty()) {
    return std::nullopt;
  }
  const QRect r = crop_.toRect() & sceneRect().toRect();
  if (r.width() < 2 || r.height() < 2) {
    return std::nullopt;
  }
  return r;
}

void VideoCanvas::resizeEvent(QResizeEvent* event) {
  QGraphicsView::resizeEvent(event);
  fit();
}

void VideoCanvas::fit() {
  if (!sceneRect().isEmpty()) {
    fitInView(sceneRect(), Qt::KeepAspectRatio);
  }
}

QPointF VideoCanvas::clamp_to_video(QPointF p) const {
  const QRectF r = sceneRect();
  return QPointF(std::clamp(p.x(), r.left(), r.right()),
                 std::clamp(p.y(), r.top(), r.bottom()));
}

void VideoCanvas::update_crop_item() {
  crop_item_->setRect(crop_);
  crop_item_->setVisible(!crop_.isEmpty());
}

void VideoCanvas::mousePressEvent(QMouseEvent* event) {
  if (!crop_mode_ || event->button() != Qt::LeftButton || sceneRect().isEmpty()) {
    QGraphicsView::mousePressEvent(event);
    return;
  }
  const QPointF pos = clamp_to_video(mapToScene(event->position().toPoint()));
  if (!crop_.isEmpty() && crop_.contains(pos)) {
    moving_ = true;
    move_offset_ = pos - crop_.topLeft();
  } else {
    dragging_ = true;
    drag_origin_ = pos;
    crop_ = QRectF(pos, pos);
    update_crop_item();
  }
}

void VideoCanvas::mouseMoveEvent(QMouseEvent* event) {
  if (!dragging_ && !moving_) {
    QGraphicsView::mouseMoveEvent(event);
    return;
  }
  const QPointF pos = clamp_to_video(mapToScene(event->position().toPoint()));
  if (dragging_) {
    crop_ = QRectF(drag_origin_, pos).normalized();
  } else {
    QPointF tl = pos - move_offset_;
    const QRectF r = sceneRect();
    tl.setX(std::clamp(tl.x(), 0.0, std::max(0.0, r.width() - crop_.width())));
    tl.setY(std::clamp(tl.y(), 0.0, std::max(0.0, r.height() - crop_.height())));
    crop_.moveTopLeft(tl);
  }
  update_crop_item();
}

void VideoCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (!dragging_ && !moving_) {
    QGraphicsView::mouseReleaseEvent(event);
    return;
  }
  if (dragging_ && (crop_.width() < 8 || crop_.height() < 8)) {
    crop_ = QRectF();  // a stray click clears the selection
  }
  dragging_ = moving_ = false;
  update_crop_item();
  if (on_crop_changed) {
    on_crop_changed();
  }
}

// -------------------------------------------------------------- clips page

ClipsPage::ClipsPage(const Settings* settings) : settings_(settings) {
  auto* layout = new QHBoxLayout(this);
  auto* splitter = new QSplitter(Qt::Horizontal);
  layout->addWidget(splitter);

  list_ = new QListWidget;
  list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(list_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) { show_context_menu(pos); });
  connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
    if (item) {
      const QString path = item->data(Qt::UserRole).toString();
      if (path != current_path_) {
        load_clip(path);
      }
    }
  });
  auto* rename_key = new QShortcut(QKeySequence(Qt::Key_F2), list_);
  rename_key->setContext(Qt::WidgetShortcut);
  connect(rename_key, &QShortcut::activated, this, [this] {
    if (auto* item = list_->currentItem()) {
      rename_clip(item->data(Qt::UserRole).toString());
    }
  });
  splitter->addWidget(list_);

  auto* right = new QWidget;
  auto* rl = new QVBoxLayout(right);
  rl->setContentsMargins(10, 6, 10, 8);

  title_label_ = new QLabel(tr("Select a clip to watch or edit"));
  QFont bold = title_label_->font();
  bold.setBold(true);
  title_label_->setFont(bold);
  rl->addWidget(title_label_);

  canvas_ = new VideoCanvas;
  canvas_->on_crop_changed = [this] { update_export_ui(); };
  rl->addWidget(canvas_, 1);

  player_ = new QMediaPlayer(this);
  audio_out_ = new QAudioOutput(this);
  player_->setAudioOutput(audio_out_);
  player_->setVideoOutput(canvas_->video_item());

  timeline_ = new TimelineBar;
  timeline_->on_seek = [this](qint64 ms) { player_->setPosition(ms); };
  timeline_->on_preview = [this](qint64 ms) {
    player_->pause();  // show the frame under the handle while dragging
    player_->setPosition(ms);
  };
  timeline_->on_selection_changed = [this] { update_export_ui(); };
  timeline_->on_trim_released = [this] {
    // Letting go of either handle previews the selection from its start.
    player_->setPosition(timeline_->sel_start_ms());
    player_->play();
  };
  rl->addWidget(timeline_);

  auto* transport = new QGridLayout;
  transport->setColumnStretch(0, 1);
  transport->setColumnStretch(2, 1);

  time_label_ = new QLabel(QStringLiteral("0:00.0 / 0:00.0"));
  time_label_->setStyleSheet("color: gray");
  transport->addWidget(time_label_, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

  play_btn_ = new QToolButton;
  play_btn_->setAutoRaise(true);
  play_btn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
  play_btn_->setIconSize(QSize(28, 28));
  play_btn_->setToolTip(tr("Play / pause (Space)"));
  transport->addWidget(play_btn_, 0, 1, Qt::AlignCenter);

  auto* right_controls = new QHBoxLayout;
  right_controls->setSpacing(6);
  edit_label_ = new QLabel;
  edit_label_->setStyleSheet("color: gray");
  right_controls->addWidget(edit_label_);
  reset_btn_ = new QToolButton;
  reset_btn_->setAutoRaise(true);
  reset_btn_->setText(tr("Reset"));
  reset_btn_->setToolTip(tr("Reset cut — play the whole clip again"));
  right_controls->addWidget(reset_btn_);
  crop_btn_ = new QToolButton;
  crop_btn_->setAutoRaise(true);
  crop_btn_->setCheckable(true);
  crop_btn_->setIcon(crop_icon());
  crop_btn_->setIconSize(QSize(20, 20));
  crop_btn_->setToolTip(tr("Crop — drag a box on the video"));
  right_controls->addWidget(crop_btn_);
  clear_crop_btn_ = new QToolButton;
  clear_crop_btn_->setAutoRaise(true);
  clear_crop_btn_->setIcon(style()->standardIcon(QStyle::SP_LineEditClearButton));
  clear_crop_btn_->setToolTip(tr("Clear crop"));
  right_controls->addWidget(clear_crop_btn_);
  export_progress_ = new QProgressBar;
  export_progress_->setRange(0, 100);
  export_progress_->setFixedWidth(120);
  export_progress_->setVisible(false);
  right_controls->addWidget(export_progress_);
  export_btn_ = new QPushButton(tr("Export"));
  right_controls->addWidget(export_btn_);
  auto* right_widget = new QWidget;
  right_widget->setLayout(right_controls);
  transport->addWidget(right_widget, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
  rl->addLayout(transport);

  auto* hint = new QLabel(tr("Drag the red handles to cut, use crop to frame a region, then "
                             "Export a copy. Cuts are lossless; crops re-encode."));
  hint->setWordWrap(true);
  hint->setStyleSheet("color: gray; font-size: 11px");
  rl->addWidget(hint);

  splitter->addWidget(right);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({210, 550});

  connect(play_btn_, &QToolButton::clicked, this, [this] { toggle_play(); });
  auto* space = new QShortcut(QKeySequence(Qt::Key_Space), this);
  space->setContext(Qt::WidgetWithChildrenShortcut);
  connect(space, &QShortcut::activated, this, [this] { toggle_play(); });

  connect(player_, &QMediaPlayer::playbackStateChanged, this, [this] {
    const bool playing = player_->playbackState() == QMediaPlayer::PlayingState;
    play_btn_->setIcon(
        style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
  });
  connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
    timeline_->set_duration(ms);
    update_export_ui();
  });
  connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
    timeline_->set_position(ms);
    time_label_->setText(
        QStringLiteral("%1 / %2").arg(format_ms(ms), format_ms(player_->duration())));
    // Playback is bounded to the selection: pause at the end handle.
    if (timeline_->sel_end_ms() >= 0 && ms >= timeline_->sel_end_ms() &&
        player_->playbackState() == QMediaPlayer::PlayingState) {
      player_->pause();
    }
  });
  connect(player_, &QMediaPlayer::errorOccurred, this,
          [this](QMediaPlayer::Error, const QString& text) {
            title_label_->setText(tr("Could not play clip: %1").arg(text));
          });

  connect(reset_btn_, &QToolButton::clicked, this, [this] {
    timeline_->reset_selection();
    update_export_ui();
    player_->setPosition(0);
    player_->play();
  });
  connect(crop_btn_, &QToolButton::toggled, this,
          [this](bool on) { canvas_->set_crop_mode(on); });
  connect(clear_crop_btn_, &QToolButton::clicked, this, [this] { canvas_->clear_crop(); });
  connect(export_btn_, &QPushButton::clicked, this, [this] { start_export(); });

  update_export_ui();
}

ClipsPage::~ClipsPage() {
  if (export_thread_.joinable()) {
    export_thread_.join();
  }
}

void ClipsPage::set_clips(const QVector<QPair<QString, QString>>& clips) {
  const QSignalBlocker blocker(list_);
  list_->clear();
  QListWidgetItem* current = nullptr;
  for (const auto& [label, path] : clips) {
    auto* item = new QListWidgetItem(label, list_);
    item->setData(Qt::UserRole, path);
    item->setToolTip(path);
    if (path == current_path_) {
      current = item;
    }
  }
  if (current) {
    list_->setCurrentItem(current);
  }
}

void ClipsPage::open_clip(const QString& path) {
  for (int i = 0; i < list_->count(); ++i) {
    if (list_->item(i)->data(Qt::UserRole).toString() == path) {
      list_->setCurrentItem(list_->item(i));  // triggers load_clip
      return;
    }
  }
  load_clip(path);
}

void ClipsPage::load_clip(const QString& path) {
  current_path_ = path;
  title_label_->setText(QFileInfo(path).fileName());
  timeline_->reset_selection();
  timeline_->set_duration(0);
  crop_btn_->setChecked(false);
  canvas_->clear_crop();
  update_export_ui();
  player_->setSource(QUrl::fromLocalFile(path));
  player_->play();
}

void ClipsPage::unload_current() {
  player_->stop();
  player_->setSource(QUrl());
  current_path_.clear();
  title_label_->setText(tr("Select a clip to watch or edit"));
  update_export_ui();
}

void ClipsPage::toggle_play() {
  if (current_path_.isEmpty()) {
    return;
  }
  if (player_->playbackState() == QMediaPlayer::PlayingState) {
    player_->pause();
  } else {
    // Play only the selected span: start over from the start handle when
    // the playhead sits outside it (or the clip already finished).
    const qint64 start = timeline_->sel_start_ms();
    const qint64 end =
        timeline_->sel_end_ms() >= 0 ? timeline_->sel_end_ms() : player_->duration();
    const qint64 pos = player_->position();
    if (player_->mediaStatus() == QMediaPlayer::EndOfMedia || pos < start || pos >= end) {
      player_->setPosition(start);
    }
    player_->play();
  }
}

void ClipsPage::update_export_ui() {
  const bool cut = timeline_->has_selection();
  const auto crop = canvas_->crop_rect();
  QStringList parts;
  if (cut) {
    const qint64 end =
        timeline_->sel_end_ms() >= 0 ? timeline_->sel_end_ms() : player_->duration();
    parts << tr("Cut %1 – %2").arg(format_ms(timeline_->sel_start_ms()), format_ms(end));
  }
  if (crop) {
    parts << tr("Crop %1×%2").arg(crop->width()).arg(crop->height());
  }
  edit_label_->setText(parts.join(QStringLiteral("  ·  ")));
  reset_btn_->setEnabled(cut);
  clear_crop_btn_->setEnabled(crop.has_value());
  export_btn_->setEnabled(!exporting_ && !current_path_.isEmpty() && (cut || crop));
}

QString ClipsPage::sanitize_filename(QString name) {
  static const QString bad = QStringLiteral("\\/:*?\"<>|");
  QString out;
  for (const QChar c : name) {
    if (!bad.contains(c)) {
      out += c;
    }
  }
  return out.trimmed();
}

QString ClipsPage::unique_path(const QDir& dir, const QString& base) {
  for (int n = 1;; ++n) {
    const QString name = n == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(n);
    const QString candidate = dir.filePath(name + QStringLiteral(".mp4"));
    if (!QFileInfo::exists(candidate)) {
      return candidate;
    }
  }
}

void ClipsPage::start_export() {
  if (exporting_ || current_path_.isEmpty()) {
    return;
  }
  const QFileInfo info(current_path_);
  const QString suggestion =
      QFileInfo(unique_path(info.dir(), info.completeBaseName() + tr(" edit")))
          .completeBaseName();
  bool accepted = false;
  QString name = QInputDialog::getText(this, tr("Export clip"), tr("Name for the new clip:"),
                                       QLineEdit::Normal, suggestion, &accepted)
                     .trimmed();
  if (!accepted) {
    return;
  }
  name = sanitize_filename(name);
  if (name.isEmpty()) {
    name = suggestion;
  }
  const QString out = unique_path(info.dir(), name);

  media::EditJob job;
  job.in_path = std::filesystem::path(current_path_.toStdWString());
  job.out_path = std::filesystem::path(out.toStdWString());
  job.start_us = timeline_->sel_start_ms() * 1000;
  job.end_us = timeline_->sel_end_ms() < 0 ? -1 : timeline_->sel_end_ms() * 1000;
  if (const auto crop = canvas_->crop_rect()) {
    job.crop = media::CropRect{crop->x(), crop->y(), crop->width(), crop->height()};
  }
  job.codec = settings_->recording.codec;
  job.bitrate_kbps = settings_->recording.bitrate_kbps;
  auto last_pct = std::make_shared<std::atomic<int>>(-1);
  job.progress = [this, last_pct](float f) {
    const int pct = static_cast<int>(f * 100.0f);
    if (last_pct->exchange(pct) == pct) {
      return;  // called per frame from the worker; only forward changes
    }
    QMetaObject::invokeMethod(
        this, [this, pct] { export_progress_->setValue(pct); }, Qt::QueuedConnection);
  };

  exporting_ = true;
  export_progress_->setValue(0);
  export_progress_->setVisible(true);
  update_export_ui();

  if (export_thread_.joinable()) {
    export_thread_.join();
  }
  export_thread_ = std::thread([this, job = std::move(job), out] {
    std::string error;
    const bool ok = media::export_edit(job, &error);
    QMetaObject::invokeMethod(
        this,
        [this, ok, error, out] {
          exporting_ = false;
          export_progress_->setVisible(false);
          update_export_ui();
          if (!ok) {
            QMessageBox::warning(this, tr("Clipster"),
                                 tr("Export failed:\n%1").arg(QString::fromStdString(error)));
            return;
          }
          if (request_refresh) {
            request_refresh();  // repopulates the lists, including the new file
          }
          open_clip(out);
        },
        Qt::QueuedConnection);
  });
}

void ClipsPage::rename_clip(const QString& path) {
  const QFileInfo info(path);
  bool accepted = false;
  QString name = QInputDialog::getText(this, tr("Rename clip"), tr("New name:"),
                                       QLineEdit::Normal, info.completeBaseName(), &accepted)
                     .trimmed();
  if (!accepted) {
    return;
  }
  name = sanitize_filename(name);
  if (name.isEmpty() || name == info.completeBaseName()) {
    return;
  }
  const QString new_path = info.dir().filePath(name + QStringLiteral(".mp4"));
  if (QFileInfo::exists(new_path)) {
    QMessageBox::warning(this, tr("Clipster"), tr("A clip with that name already exists."));
    return;
  }
  const bool was_current = path == current_path_;
  if (was_current) {
    unload_current();  // release the player's file handle first
  }
  if (!QFile::rename(path, new_path)) {
    QMessageBox::warning(this, tr("Clipster"), tr("Could not rename the file."));
    if (was_current) {
      open_clip(path);
    }
    return;
  }
  if (request_refresh) {
    request_refresh();
  }
  if (was_current) {
    open_clip(new_path);
  }
}

void ClipsPage::show_context_menu(const QPoint& pos) {
  QListWidgetItem* item = list_->itemAt(pos);
  if (!item) {
    return;
  }
  const QString path = item->data(Qt::UserRole).toString();
  QMenu menu(this);
  menu.addAction(tr("Rename… (F2)"), [this, path] { rename_clip(path); });
  menu.addAction(tr("Open in default player"),
                 [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
  menu.addAction(tr("Show in Explorer"), [path] {
    QProcess::startDetached(QStringLiteral("explorer"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
  });
  menu.addSeparator();
  menu.addAction(tr("Delete clip"), [this, path] {
    if (QMessageBox::question(this, tr("Clipster"),
                              tr("Delete %1?").arg(QFileInfo(path).fileName())) !=
        QMessageBox::Yes) {
      return;
    }
    if (path == current_path_) {
      unload_current();  // release the file handle before deleting
    }
    QFile file(path);
    if (!file.moveToTrash() && !file.remove()) {
      QMessageBox::warning(this, tr("Clipster"), tr("Could not delete the file."));
    }
    if (request_refresh) {
      request_refresh();
    }
  });
  menu.exec(list_->viewport()->mapToGlobal(pos));
}

QIcon ClipsPage::crop_icon() const {
  QPixmap pm(32, 32);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QPen pen(palette().color(QPalette::ButtonText), 3);
  pen.setCapStyle(Qt::FlatCap);
  p.setPen(pen);
  QPolygon a;
  a << QPoint(5, 9) << QPoint(23, 9) << QPoint(23, 27);
  QPolygon b;
  b << QPoint(9, 5) << QPoint(9, 23) << QPoint(27, 23);
  p.drawPolyline(a);
  p.drawPolyline(b);
  return QIcon(pm);
}

QString ClipsPage::format_ms(qint64 ms) {
  ms = std::max<qint64>(ms, 0);
  return QStringLiteral("%1:%2.%3")
      .arg(ms / 60000)
      .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
      .arg((ms % 1000) / 100);
}

}  // namespace clipster::gui
