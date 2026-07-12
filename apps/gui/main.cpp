// Clipster — the application. Main window with live status, recent clips
// and settings; minimizes to the tray and keeps recording. Launch with
// --minimized (used by "Start with Windows") to start in the tray.

#include <QtWidgets>

#include <windows.h>

#include "clipster/logging.hpp"
#include "clipster/settings.hpp"
#include "clipster/win/known_folders.hpp"
#include "main_window.hpp"

int main(int argc, char** argv) {
  // Two Clipsters would fight over the hotkey and the game.
  CreateMutexW(nullptr, TRUE, L"Local\\ClipsterAppMutex");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Clipster is already running — look for it in the system tray.",
                L"Clipster", MB_OK | MB_ICONINFORMATION);
    return 0;
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName("Clipster");
  QApplication::setQuitOnLastWindowClosed(false);  // closing hides to tray

  using namespace clipster;
  const auto app_dir = win::app_data_dir();
  log::set_file(app_dir / L"clipster.log");
  log::info("--- Clipster starting ---");

  const auto settings_path = app_dir / L"settings.json";
  std::string warning;
  Settings settings = Settings::load_or_default(settings_path, &warning);
  if (!warning.empty()) {
    log::warn("{}", warning);
  }
  if (!std::filesystem::exists(settings_path)) {
    settings.save(settings_path);
  }

  gui::MainWindow window(std::move(settings), settings_path);
  if (!app.arguments().contains("--minimized")) {
    window.show();
  }

  const int rc = app.exec();
  log::info("--- Clipster exiting ---");
  return rc;
}
