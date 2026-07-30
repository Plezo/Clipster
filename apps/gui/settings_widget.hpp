#pragma once

#include <QtWidgets>

#include <functional>
#include <optional>

#include "clipster/settings.hpp"
#include "updater.hpp"

namespace clipster::gui {

// The settings tabs, embeddable in the main window. Edits a copy; call
// collect() to obtain the resulting Settings (already clamped).
class SettingsWidget : public QWidget {
 public:
  explicit SettingsWidget(const Settings& initial, QWidget* parent = nullptr);

  Settings collect() const;

  // Asked to shut down cleanly once an update installer has been started,
  // so the recorder finishes its clips instead of being force-killed.
  std::function<void()> request_quit;

 private:
  QWidget* build_recording_tab();
  QWidget* build_output_tab();
  QWidget* build_audio_tab();
  QWidget* build_games_tab();
  QWidget* build_hotkeys_tab();
  QWidget* build_advanced_tab();
  QWidget* build_about_tab();

  void run_update_check();
  void show_update(const UpdateInfo& info);
  void start_update();

  Settings initial_;

  QComboBox* fps_ = nullptr;
  QSpinBox* bitrate_ = nullptr;
  QComboBox* codec_ = nullptr;
  QSpinBox* clip_len_ = nullptr;
  QLabel* ram_label_ = nullptr;
  QLineEdit* save_dir_ = nullptr;
  QLineEdit* template_ = nullptr;
  QLabel* template_example_ = nullptr;
  QComboBox* audio_mode_ = nullptr;
  QGroupBox* include_group_ = nullptr;
  QGroupBox* exclude_group_ = nullptr;
  QListWidget* include_list_ = nullptr;
  QListWidget* exclude_list_ = nullptr;
  QCheckBox* mic_enabled_ = nullptr;
  QComboBox* mic_device_ = nullptr;
  QCheckBox* mic_separate_track_ = nullptr;
  QCheckBox* steam_ = nullptr;
  QListWidget* folders_list_ = nullptr;
  QListWidget* exes_list_ = nullptr;
  QListWidget* ignored_list_ = nullptr;
  QKeySequenceEdit* hotkey_ = nullptr;
  QLineEdit* controller_ = nullptr;
  QCheckBox* sound_ = nullptr;
  QLineEdit* sound_file_ = nullptr;

  Updater* updater_ = nullptr;
  QPushButton* check_update_ = nullptr;
  QPushButton* get_update_ = nullptr;
  QLabel* update_status_ = nullptr;
  QProgressBar* update_progress_ = nullptr;
  std::optional<UpdateInfo> pending_update_;
};

}  // namespace clipster::gui
