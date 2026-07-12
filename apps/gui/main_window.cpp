#include "main_window.hpp"

#include <algorithm>
#include <vector>

#include "clipster/logging.hpp"
#include "clipster/win/autostart.hpp"
#include "clipster/win/known_folders.hpp"

namespace clipster::gui {

MainWindow::MainWindow(Settings settings, std::filesystem::path settings_path)
    : settings_(std::move(settings)), settings_path_(std::move(settings_path)) {
  setWindowTitle(tr("Clipster"));
  resize(760, 560);

  auto* tabs = new QTabWidget;
  tabs->addTab(build_home_page(), tr("Home"));
  tabs->addTab(build_settings_page(), tr("Settings"));
  setCentralWidget(tabs);
  statusBar();  // create it so messages have somewhere to go

  setup_tray();

  // Engine callbacks arrive on the manager's control thread; marshal to
  // the UI thread before touching widgets.
  app::SessionManager::Callbacks callbacks;
  callbacks.on_recording_started = [this](const std::string& game) {
    QMetaObject::invokeMethod(
        this,
        [this, game] {
          tray_->showMessage(tr("Clipster"),
                             tr("Recording %1 — %2 saves the last %3 s")
                                 .arg(QString::fromStdString(game),
                                      QString::fromStdString(settings_.hotkeys.save_clip))
                                 .arg(settings_.clip.default_length_seconds));
          refresh_status();
        },
        Qt::QueuedConnection);
  };
  callbacks.on_recording_stopped = [this](const std::string& game, bool game_exited) {
    QMetaObject::invokeMethod(
        this,
        [this, game, game_exited] {
          if (game_exited) {
            tray_->showMessage(tr("Clipster"),
                               tr("Stopped recording %1").arg(QString::fromStdString(game)));
          }
          refresh_status();
          refresh_clips();
        },
        Qt::QueuedConnection);
  };
  callbacks.on_error = [this](const std::string& message) {
    QMetaObject::invokeMethod(
        this,
        [this, message] {
          tray_->showMessage(tr("Clipster"), QString::fromStdString(message),
                             QSystemTrayIcon::Warning);
          refresh_status();
        },
        Qt::QueuedConnection);
  };
  manager_ = std::make_unique<app::SessionManager>(settings_, std::move(callbacks));

  register_hotkeys();

  status_timer_ = new QTimer(this);
  connect(status_timer_, &QTimer::timeout, this, [this] { refresh_status(); });
  status_timer_->start(1000);

  clips_timer_ = new QTimer(this);
  connect(clips_timer_, &QTimer::timeout, this, [this] { refresh_clips(); });
  clips_timer_->start(10000);

  refresh_status();
  refresh_clips();
}

MainWindow::~MainWindow() {
  if (gamepad_) {
    gamepad_->stop();
  }
  if (hotkeys_) {
    hotkeys_->stop();
  }
  if (manager_) {
    manager_->stop();
  }
}

QWidget* MainWindow::build_home_page() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);

  status_label_ = new QLabel;
  QFont big = status_label_->font();
  big.setPointSize(15);
  big.setBold(true);
  status_label_->setFont(big);
  layout->addWidget(status_label_);

  stats_label_ = new QLabel;
  stats_label_->setStyleSheet("color: gray");
  layout->addWidget(stats_label_);

  auto* buttons = new QHBoxLayout;
  save_clip_btn_ = new QPushButton(tr("Save clip"));
  save_clip_btn_->setMinimumHeight(36);
  connect(save_clip_btn_, &QPushButton::clicked, this, [this] {
    manager_->save_clip();
    statusBar()->showMessage(tr("Clip requested…"), 2000);
    QTimer::singleShot(1500, this, [this] { refresh_clips(); });
  });
  auto* open_folder = new QPushButton(tr("Open clips folder"));
  connect(open_folder, &QPushButton::clicked, this, [this] {
    std::error_code ec;
    std::filesystem::create_directories(clips_dir(), ec);
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdWString(clips_dir().wstring())));
  });
  buttons->addWidget(save_clip_btn_);
  buttons->addWidget(open_folder);
  buttons->addStretch();
  layout->addLayout(buttons);

  layout->addSpacing(8);
  layout->addWidget(new QLabel(tr("Recent clips (double-click to play):")));
  clips_list_ = new QListWidget;
  connect(clips_list_, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem* item) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
  });
  layout->addWidget(clips_list_, 1);
  return page;
}

QWidget* MainWindow::build_settings_page() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);

  settings_widget_ = new SettingsWidget(settings_);
  layout->addWidget(settings_widget_, 1);

  auto* bottom = new QHBoxLayout;
  autostart_ = new QCheckBox(tr("Start with Windows (minimized to tray)"));
  autostart_->setChecked(win::autostart_enabled());
  bottom->addWidget(autostart_);
  bottom->addStretch();
  auto* save = new QPushButton(tr("Save settings"));
  save->setDefault(true);
  connect(save, &QPushButton::clicked, this, [this] { apply_settings(); });
  bottom->addWidget(save);
  layout->addLayout(bottom);
  return page;
}

void MainWindow::setup_tray() {
  tray_ = new QSystemTrayIcon(dot_icon(QColor(130, 130, 130)), this);
  auto* menu = new QMenu(this);
  menu->addAction(tr("Open Clipster"), this, [this] {
    show();
    raise();
    activateWindow();
  });
  menu->addAction(tr("Save clip"), this, [this] { manager_->save_clip(); });
  menu->addSeparator();
  menu->addAction(tr("Quit"), this, [this] { quit_app(); });
  tray_->setContextMenu(menu);
  tray_->setToolTip(tr("Clipster — waiting for a game"));
  connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
    if (r == QSystemTrayIcon::DoubleClick || r == QSystemTrayIcon::Trigger) {
      show();
      raise();
      activateWindow();
    }
  });
  tray_->show();
}

void MainWindow::register_hotkeys() {
  const auto save = [this] { manager_->save_clip(); };

  hotkeys_ = std::make_unique<win::HotkeyManager>();
  std::string error;
  if (!hotkeys_->register_hotkey(settings_.hotkeys.save_clip, save, &error)) {
    log::error("{}", error);
    statusBar()->showMessage(QString::fromStdString(error), 6000);
  }

  gamepad_.reset();
  if (!settings_.hotkeys.controller_save_clip.empty()) {
    gamepad_ = win::GamepadHotkey::create(settings_.hotkeys.controller_save_clip, save, &error);
    if (!gamepad_) {
      log::warn("{}", error);
    }
  }
}

void MainWindow::apply_settings() {
  settings_ = settings_widget_->collect();
  std::string error;
  if (!settings_.save(settings_path_, &error)) {
    QMessageBox::critical(this, tr("Clipster"),
                          tr("Could not save settings:\n%1").arg(QString::fromStdString(error)));
    return;
  }
  manager_->update_settings(settings_);
  register_hotkeys();
  win::set_autostart(autostart_->isChecked(), L"--minimized");
  statusBar()->showMessage(tr("Settings saved — recording settings apply to the next game"),
                           5000);
}

void MainWindow::refresh_status() {
  const bool recording = manager_->is_recording();
  if (recording) {
    const QString game = QString::fromStdString(manager_->current_game());
    status_label_->setText(tr("● Recording %1").arg(game));
    status_label_->setStyleSheet("color: #d33");
    stats_label_->setText(QString::fromStdString(manager_->stats()));
    tray_->setToolTip(tr("Clipster — recording %1").arg(game));
    tray_->setIcon(dot_icon(QColor(210, 60, 60)));
  } else {
    status_label_->setText(tr("Waiting for a game…"));
    status_label_->setStyleSheet("");
    stats_label_->setText(
        tr("Launch a game from a watched folder and recording starts automatically."));
    tray_->setToolTip(tr("Clipster — waiting for a game"));
    tray_->setIcon(dot_icon(QColor(130, 130, 130)));
  }
  save_clip_btn_->setEnabled(recording);
}

std::filesystem::path MainWindow::clips_dir() const {
  return settings_.output.save_dir.empty()
             ? win::default_save_dir()
             : std::filesystem::path(settings_.output.save_dir);
}

void MainWindow::refresh_clips() {
  struct Entry {
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
  };
  std::vector<Entry> entries;
  std::error_code ec;
  const auto dir = clips_dir();
  if (std::filesystem::exists(dir, ec)) {
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->is_regular_file(ec) && it->path().extension() == L".mp4") {
        entries.push_back({it->path(), it->last_write_time(ec)});
      }
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
  if (entries.size() > 50) {
    entries.resize(50);
  }

  clips_list_->clear();
  for (const Entry& e : entries) {
    const auto rel = std::filesystem::relative(e.path, dir, ec);
    auto* item = new QListWidgetItem(
        QString::fromStdWString(ec ? e.path.filename().wstring() : rel.wstring()));
    item->setData(Qt::UserRole, QString::fromStdWString(e.path.wstring()));
    clips_list_->addItem(item);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (quitting_) {
    event->accept();
    return;
  }
  hide();
  event->ignore();
  if (!tray_hint_shown_) {
    tray_hint_shown_ = true;
    tray_->showMessage(tr("Clipster"),
                       tr("Still running in the background — games keep being recorded. "
                          "Right-click the tray icon to quit."));
  }
}

void MainWindow::quit_app() {
  quitting_ = true;
  tray_->hide();
  close();
  QApplication::quit();
}

QIcon MainWindow::dot_icon(const QColor& color) {
  QPixmap pm(32, 32);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(color);
  p.setPen(QPen(color.darker(130), 2));
  p.drawEllipse(4, 4, 24, 24);
  p.end();
  return QIcon(pm);
}

}  // namespace clipster::gui
