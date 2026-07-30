#pragma once

#include <QtNetwork>
#include <QtWidgets>

#include <functional>
#include <optional>

namespace clipster::gui {

struct UpdateInfo {
  QString tag;            // "v0.5.0"
  QString page_url;       // the release page, for "what's new"
  QString installer_url;  // Clipster-Setup-*.exe asset, empty if absent
  qint64 installer_size = 0;
};

// Asks GitHub for the latest release and, for installed builds, downloads
// and runs the setup exe. Callbacks rather than signals: the GUI is built
// without moc, so none of our classes declare Q_OBJECT.
//
// Callbacks fire at most once per call and always on the GUI thread.
class Updater {
 public:
  explicit Updater(QObject* parent);

  // `done` receives the release when it is newer than the running build,
  // nullopt when already up to date. `failed` gets a human-readable reason
  // (offline, rate-limited, TLS) — callers decide whether that is worth
  // showing, since the automatic check on startup keeps quiet.
  void check(std::function<void(std::optional<UpdateInfo>)> done,
             std::function<void(QString)> failed);

  // Downloads the installer and starts it. The installer closes Clipster
  // itself, but `launched` fires first so the app can shut the recorder
  // down cleanly instead of being killed mid-clip.
  void install(const UpdateInfo& info,
               std::function<void(qint64 received, qint64 total)> progress,
               std::function<void()> launched, std::function<void(QString)> failed);

  // True when this exe is the one the setup program installed. The
  // portable zip must not be "updated" by running an installer that would
  // put a second copy somewhere else.
  static bool is_installed_build();

  static QString current_version();  // without the leading "v"
  static bool is_newer(const QString& tag, const QString& current);

 private:
  QNetworkAccessManager* nam_;
  QObject* parent_;
};

}  // namespace clipster::gui
