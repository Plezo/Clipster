#pragma once

#include <QtWidgets>

#include "clipster/settings.hpp"

namespace clipster::gui {

// The settings tabs, embeddable in the main window. Edits a copy; call
// collect() to obtain the resulting Settings (already clamped).
class SettingsWidget : public QWidget {
 public:
  explicit SettingsWidget(const Settings& initial, QWidget* parent = nullptr);

  Settings collect() const;

 private:
  QWidget* build_recording_tab();
  QWidget* build_output_tab();
  QWidget* build_audio_tab();
  QWidget* build_games_tab();
  QWidget* build_hotkeys_tab();

  Settings initial_;

  QComboBox* fps_ = nullptr;
  QSpinBox* bitrate_ = nullptr;
  QComboBox* codec_ = nullptr;
  QSpinBox* buffer_ = nullptr;
  QCheckBox* capture_frame_ = nullptr;
  QLabel* ram_label_ = nullptr;
  QLineEdit* save_dir_ = nullptr;
  QCheckBox* subfolder_ = nullptr;
  QLineEdit* template_ = nullptr;
  QSpinBox* clip_len_ = nullptr;
  QComboBox* audio_mode_ = nullptr;
  QGroupBox* include_group_ = nullptr;
  QGroupBox* exclude_group_ = nullptr;
  QListWidget* include_list_ = nullptr;
  QListWidget* exclude_list_ = nullptr;
  QCheckBox* steam_ = nullptr;
  QListWidget* folders_list_ = nullptr;
  QListWidget* exes_list_ = nullptr;
  QListWidget* ignored_list_ = nullptr;
  QKeySequenceEdit* hotkey_ = nullptr;
  QLineEdit* controller_ = nullptr;
  QCheckBox* sound_ = nullptr;
  QLineEdit* sound_file_ = nullptr;
};

}  // namespace clipster::gui
