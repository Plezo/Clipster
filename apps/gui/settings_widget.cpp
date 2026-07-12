#include "settings_widget.hpp"

#include <windows.h>

#include <mmsystem.h>

#include <functional>

#include "clipster/win/audio_capture.hpp"

namespace clipster::gui {

namespace {

QStringList to_qstringlist(const std::vector<std::string>& v) {
  QStringList out;
  for (const auto& s : v) {
    out << QString::fromStdString(s);
  }
  return out;
}

std::vector<std::string> from_list_widget(const QListWidget* list) {
  std::vector<std::string> out;
  for (int i = 0; i < list->count(); ++i) {
    out.push_back(list->item(i)->text().toStdString());
  }
  return out;
}

// A QListWidget with Add/Remove buttons; `provide` returns the string to
// add (empty = cancelled).
QWidget* make_list_editor(QListWidget*& list_out, const QStringList& initial,
                          const QString& add_label, std::function<QString(QWidget*)> provide) {
  auto* container = new QWidget;
  auto* layout = new QHBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* list = new QListWidget;
  list->addItems(initial);
  list_out = list;

  auto* buttons = new QVBoxLayout;
  auto* add = new QPushButton(add_label);
  auto* remove = new QPushButton(QObject::tr("Remove"));
  buttons->addWidget(add);
  buttons->addWidget(remove);
  buttons->addStretch();

  QObject::connect(add, &QPushButton::clicked, container, [list, provide, container] {
    const QString value = provide(container);
    if (!value.isEmpty() && list->findItems(value, Qt::MatchFixedString).isEmpty()) {
      list->addItem(value);
    }
  });
  QObject::connect(remove, &QPushButton::clicked, container,
                   [list] { delete list->takeItem(list->currentRow()); });

  layout->addWidget(list, 1);
  layout->addLayout(buttons);
  return container;
}

QString ask_exe_name(QWidget* parent, const QString& title) {
  bool ok = false;
  QString name = QInputDialog::getText(parent, title,
                                       QObject::tr("Executable name (e.g. Discord.exe):"),
                                       QLineEdit::Normal, {}, &ok);
  name = name.trimmed();
  if (ok && !name.isEmpty() && !name.endsWith(".exe", Qt::CaseInsensitive)) {
    name += ".exe";
  }
  return ok ? name : QString();
}

}  // namespace

SettingsWidget::SettingsWidget(const Settings& initial, QWidget* parent)
    : QWidget(parent), initial_(initial) {
  auto* tabs = new QTabWidget;
  tabs->addTab(build_recording_tab(), tr("Recording"));
  tabs->addTab(build_output_tab(), tr("Output"));
  tabs->addTab(build_audio_tab(), tr("Audio"));
  tabs->addTab(build_games_tab(), tr("Games"));
  tabs->addTab(build_hotkeys_tab(), tr("Hotkeys && Alerts"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(tabs);
}

QWidget* SettingsWidget::build_recording_tab() {
  auto* page = new QWidget;
  auto* form = new QFormLayout(page);

  fps_ = new QComboBox;
  for (int fps : {30, 60, 120}) {
    fps_->addItem(QString::number(fps), fps);
  }
  fps_->setCurrentIndex(std::max(0, fps_->findData(initial_.recording.fps)));
  form->addRow(tr("Frame rate:"), fps_);

  bitrate_ = new QSpinBox;
  bitrate_->setRange(1, 150);
  bitrate_->setSuffix(tr(" Mbps"));
  bitrate_->setValue(initial_.recording.bitrate_kbps / 1000);
  form->addRow(tr("Bitrate:"), bitrate_);

  codec_ = new QComboBox;
  codec_->addItem(tr("Auto (recommended)"), "auto");
  codec_->addItem(tr("H.264 (max compatibility)"), "h264");
  codec_->addItem(tr("HEVC (smaller files)"), "hevc");
  codec_->setCurrentIndex(
      std::max(0, codec_->findData(QString::fromStdString(initial_.recording.codec))));
  form->addRow(tr("Codec:"), codec_);

  buffer_ = new QSpinBox;
  buffer_->setRange(5, 600);
  buffer_->setSuffix(tr(" s"));
  buffer_->setValue(initial_.recording.buffer_seconds);
  form->addRow(tr("Replay buffer length:"), buffer_);

  capture_frame_ = new QCheckBox(tr("Include window title bar and borders (windowed games)"));
  capture_frame_->setChecked(initial_.recording.capture_window_frame);
  form->addRow(QString(), capture_frame_);

  ram_label_ = new QLabel;
  ram_label_->setStyleSheet("color: gray");
  form->addRow(QString(), ram_label_);

  const auto update_ram = [this] {
    const double mb = double(buffer_->value()) * bitrate_->value() * 1000.0 / 8.0 / 1024.0;
    ram_label_->setText(tr("≈ %1 MB of RAM while recording").arg(qRound(mb)));
  };
  connect(buffer_, &QSpinBox::valueChanged, this, update_ram);
  connect(bitrate_, &QSpinBox::valueChanged, this, update_ram);
  update_ram();
  return page;
}

QWidget* SettingsWidget::build_output_tab() {
  auto* page = new QWidget;
  auto* form = new QFormLayout(page);

  save_dir_ = new QLineEdit(QString::fromStdString(initial_.output.save_dir));
  save_dir_->setPlaceholderText(tr("(default: Videos\\Clipster)"));
  auto* browse = new QPushButton(tr("Browse…"));
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Clips folder"));
    if (!dir.isEmpty()) {
      save_dir_->setText(QDir::toNativeSeparators(dir));
    }
  });
  auto* row = new QHBoxLayout;
  row->addWidget(save_dir_, 1);
  row->addWidget(browse);
  form->addRow(tr("Save clips to:"), row);

  subfolder_ = new QCheckBox(tr("Create a subfolder per game"));
  subfolder_->setChecked(initial_.output.subfolder_per_game);
  form->addRow(QString(), subfolder_);

  template_ = new QLineEdit(QString::fromStdString(initial_.output.filename_template));
  form->addRow(tr("Filename template:"), template_);
  auto* hint = new QLabel(tr("Placeholders: {game} {date} {time}"));
  hint->setStyleSheet("color: gray");
  form->addRow(QString(), hint);

  clip_len_ = new QSpinBox;
  clip_len_->setRange(5, 600);
  clip_len_->setSuffix(tr(" s"));
  clip_len_->setValue(initial_.clip.default_length_seconds);
  form->addRow(tr("Clip length (seconds saved per press):"), clip_len_);
  return page;
}

QWidget* SettingsWidget::build_audio_tab() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  auto* form = new QFormLayout;

  audio_mode_ = new QComboBox;
  audio_mode_->addItem(tr("Everything (all desktop audio)"), "desktop");
  audio_mode_->addItem(tr("Game only"), "game_only");
  audio_mode_->addItem(tr("Game + specific apps"), "include_list");
  audio_mode_->addItem(tr("Everything except specific apps"), "desktop_exclude");
  audio_mode_->setCurrentIndex(
      std::max(0, audio_mode_->findData(QString::fromStdString(initial_.audio.mode))));
  form->addRow(tr("What to record:"), audio_mode_);
  layout->addLayout(form);

  include_group_ = new QGroupBox(tr("Also record these apps"));
  auto* inc_layout = new QVBoxLayout(include_group_);
  inc_layout->addWidget(make_list_editor(
      include_list_, to_qstringlist(initial_.audio.include_apps), tr("Add app…"),
      [](QWidget* p) { return ask_exe_name(p, QObject::tr("Include app audio")); }));
  layout->addWidget(include_group_);

  exclude_group_ = new QGroupBox(tr("Never record these apps"));
  auto* exc_layout = new QVBoxLayout(exclude_group_);
  exc_layout->addWidget(make_list_editor(
      exclude_list_, to_qstringlist(initial_.audio.exclude_apps), tr("Add app…"),
      [](QWidget* p) { return ask_exe_name(p, QObject::tr("Exclude app audio")); }));
  layout->addWidget(exclude_group_);

  const auto update_groups = [this] {
    const QString mode = audio_mode_->currentData().toString();
    include_group_->setVisible(mode == "include_list");
    exclude_group_->setVisible(mode == "desktop_exclude");
  };
  connect(audio_mode_, &QComboBox::currentIndexChanged, this, update_groups);
  update_groups();

  auto* mic_group = new QGroupBox(tr("Microphone"));
  auto* mic_form = new QFormLayout(mic_group);

  mic_enabled_ = new QCheckBox(tr("Record my microphone"));
  mic_enabled_->setChecked(initial_.audio.microphone.enabled);
  mic_form->addRow(QString(), mic_enabled_);

  mic_device_ = new QComboBox;
  mic_device_->addItem(tr("Default (communications device)"));
  const QString current = QString::fromStdString(initial_.audio.microphone.device);
  for (const auto& name : win::list_capture_devices()) {
    mic_device_->addItem(QString::fromStdString(name));
  }
  if (!current.isEmpty() && current != "default") {
    int idx = mic_device_->findText(current, Qt::MatchFixedString);
    if (idx < 0) {
      mic_device_->addItem(current);  // currently unplugged device
      idx = mic_device_->count() - 1;
    }
    mic_device_->setCurrentIndex(idx);
  }
  mic_form->addRow(tr("Device:"), mic_device_);

  mic_separate_ = new QCheckBox(
      tr("Keep the microphone on its own audio track (best for editing; "
         "some players only play the first track)"));
  mic_separate_->setChecked(initial_.audio.microphone.separate_track);
  mic_form->addRow(QString(), mic_separate_);

  layout->addWidget(mic_group);

  layout->addStretch();
  return page;
}

QWidget* SettingsWidget::build_games_tab() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);

  steam_ = new QCheckBox(tr("Automatically watch all Steam libraries"));
  steam_->setChecked(initial_.games.auto_detect_steam);
  layout->addWidget(steam_);

  auto* folders_group = new QGroupBox(tr("Also watch these folders (Epic, GOG, …)"));
  auto* fg = new QVBoxLayout(folders_group);
  fg->addWidget(make_list_editor(
      folders_list_, to_qstringlist(initial_.games.watched_folders), tr("Add folder…"),
      [](QWidget* p) {
        return QDir::toNativeSeparators(
            QFileDialog::getExistingDirectory(p, QObject::tr("Watch folder")));
      }));
  layout->addWidget(folders_group, 1);

  auto* exes_group = new QGroupBox(tr("Always record these executables"));
  auto* eg = new QVBoxLayout(exes_group);
  eg->addWidget(make_list_editor(
      exes_list_, to_qstringlist(initial_.games.manual_exes), tr("Add exe…"),
      [](QWidget* p) {
        return QDir::toNativeSeparators(QFileDialog::getOpenFileName(
            p, QObject::tr("Game executable"), {}, QObject::tr("Programs (*.exe)")));
      }));
  layout->addWidget(exes_group, 1);

  auto* ignored_group = new QGroupBox(tr("Never record these executables"));
  auto* ig = new QVBoxLayout(ignored_group);
  ig->addWidget(make_list_editor(
      ignored_list_, to_qstringlist(initial_.games.ignored_exes), tr("Add name…"),
      [](QWidget* p) { return ask_exe_name(p, QObject::tr("Ignore executable")); }));
  layout->addWidget(ignored_group, 1);
  return page;
}

QWidget* SettingsWidget::build_hotkeys_tab() {
  auto* page = new QWidget;
  auto* form = new QFormLayout(page);

  hotkey_ =
      new QKeySequenceEdit(QKeySequence(QString::fromStdString(initial_.hotkeys.save_clip)));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  hotkey_->setMaximumSequenceLength(1);  // the global hotkey is one chord
#endif
  form->addRow(tr("Save clip hotkey:"), hotkey_);

  controller_ = new QLineEdit(QString::fromStdString(initial_.hotkeys.controller_save_clip));
  controller_->setPlaceholderText(tr("e.g. Back+RB (empty = disabled)"));
  form->addRow(tr("Controller combo:"), controller_);
  auto* pad_hint =
      new QLabel(tr("Buttons: A B X Y LB RB LS RS Back Start DpadUp/Down/Left/Right"));
  pad_hint->setStyleSheet("color: gray");
  form->addRow(QString(), pad_hint);

  sound_ = new QCheckBox(tr("Play a sound when a clip is saved"));
  sound_->setChecked(initial_.notifications.sound_enabled);
  form->addRow(QString(), sound_);

  sound_file_ = new QLineEdit(QString::fromStdString(initial_.notifications.sound_file));
  sound_file_->setPlaceholderText(tr("(default chime)"));
  auto* browse = new QPushButton(tr("Browse…"));
  auto* test = new QPushButton(tr("Test"));
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString wav =
        QFileDialog::getOpenFileName(this, tr("Clip sound"), {}, tr("Wave audio (*.wav)"));
    if (!wav.isEmpty()) {
      sound_file_->setText(QDir::toNativeSeparators(wav));
    }
  });
  connect(test, &QPushButton::clicked, this, [this] {
    const QString file = sound_file_->text();
    if (!file.isEmpty()) {
      PlaySoundW(reinterpret_cast<const wchar_t*>(file.utf16()), nullptr,
                 SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    } else {
      MessageBeep(MB_OK);
    }
  });
  auto* row = new QHBoxLayout;
  row->addWidget(sound_file_, 1);
  row->addWidget(browse);
  row->addWidget(test);
  form->addRow(tr("Custom sound:"), row);
  return page;
}

Settings SettingsWidget::collect() const {
  Settings s = initial_;

  s.recording.fps = fps_->currentData().toInt();
  s.recording.bitrate_kbps = bitrate_->value() * 1000;
  s.recording.codec = codec_->currentData().toString().toStdString();
  s.recording.buffer_seconds = buffer_->value();
  s.recording.capture_window_frame = capture_frame_->isChecked();

  s.output.save_dir = save_dir_->text().trimmed().toStdString();
  s.output.filename_template = template_->text().toStdString();
  s.output.subfolder_per_game = subfolder_->isChecked();
  s.clip.default_length_seconds = clip_len_->value();

  s.audio.mode = audio_mode_->currentData().toString().toStdString();
  s.audio.include_apps = from_list_widget(include_list_);
  s.audio.exclude_apps = from_list_widget(exclude_list_);
  s.audio.microphone.enabled = mic_enabled_->isChecked();
  s.audio.microphone.device =
      mic_device_->currentIndex() == 0 ? "default" : mic_device_->currentText().toStdString();
  s.audio.microphone.separate_track = mic_separate_->isChecked();

  s.games.auto_detect_steam = steam_->isChecked();
  s.games.watched_folders = from_list_widget(folders_list_);
  s.games.manual_exes = from_list_widget(exes_list_);
  s.games.ignored_exes = from_list_widget(ignored_list_);

  const QKeySequence seq = hotkey_->keySequence();
  if (!seq.isEmpty()) {
    // Keep only the first chord — the Win32 hotkey backend cannot
    // represent multi-chord sequences like "Ctrl+K, Ctrl+D".
    s.hotkeys.save_clip = QKeySequence(seq[0]).toString(QKeySequence::PortableText).toStdString();
  }
  s.hotkeys.controller_save_clip = controller_->text().trimmed().toStdString();
  s.notifications.sound_enabled = sound_->isChecked();
  s.notifications.sound_file = sound_file_->text().trimmed().toStdString();

  s.clamp();
  return s;
}

}  // namespace clipster::gui
