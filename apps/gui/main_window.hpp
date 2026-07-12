#pragma once

#include <QtWidgets>

#include <filesystem>
#include <memory>

#include "clipster/settings.hpp"
#include "clipster/win/gamepad_hotkey.hpp"
#include "clipster/win/hotkey_manager.hpp"
#include "session_manager.hpp"
#include "settings_widget.hpp"

namespace clipster::gui {

// The application window: live recording status + recent clips on the
// Home page, full configuration on the Settings page. Closing the window
// minimizes to the tray so recording continues in the background.
class MainWindow : public QMainWindow {
 public:
  MainWindow(Settings settings, std::filesystem::path settings_path);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  QWidget* build_home_page();
  QWidget* build_settings_page();
  void setup_tray();
  void register_hotkeys();
  void apply_settings();
  void refresh_status();
  void refresh_clips();
  void quit_app();
  std::filesystem::path clips_dir() const;
  static QIcon dot_icon(const QColor& color);

  Settings settings_;
  std::filesystem::path settings_path_;

  std::unique_ptr<app::SessionManager> manager_;
  std::unique_ptr<win::HotkeyManager> hotkeys_;
  std::unique_ptr<win::GamepadHotkey> gamepad_;

  QLabel* status_label_ = nullptr;
  QLabel* stats_label_ = nullptr;
  QPushButton* save_clip_btn_ = nullptr;
  QListWidget* clips_list_ = nullptr;
  SettingsWidget* settings_widget_ = nullptr;
  QCheckBox* autostart_ = nullptr;
  QSystemTrayIcon* tray_ = nullptr;
  QTimer* status_timer_ = nullptr;
  QTimer* clips_timer_ = nullptr;
  bool quitting_ = false;
  bool tray_hint_shown_ = false;
};

}  // namespace clipster::gui
