#include "updater.hpp"

#include "clipster/logging.hpp"

#ifndef CLIPSTER_VERSION
#define CLIPSTER_VERSION "0.0.0"
#endif

namespace clipster::gui {

namespace {

constexpr auto kLatestReleaseUrl =
    "https://api.github.com/repos/Plezo/Clipster/releases/latest";
constexpr int kTimeoutMs = 15000;
// The setup exe is ~29 MB; anything tiny is an error page, anything huge is
// not ours. Bounds are deliberately loose — this is a sanity check, not a
// signature.
constexpr qint64 kMinInstallerBytes = 1 << 20;         // 1 MB
constexpr qint64 kMaxInstallerBytes = 512ll << 20;     // 512 MB

std::vector<int> parse_version(QString s) {
  s.remove(QRegularExpression(QStringLiteral("^[vV]")));
  std::vector<int> parts;
  for (const QString& p : s.split(QLatin1Char('.'))) {
    parts.push_back(p.section(QRegularExpression(QStringLiteral("[^0-9]")), 0, 0).toInt());
  }
  parts.resize(3, 0);
  return parts;
}

// The download must come from GitHub over TLS: `browser_download_url`
// arrives as data from the network, and this code hands what it fetches to
// the OS to execute.
bool trusted_download(const QUrl& url) {
  if (url.scheme() != QStringLiteral("https")) {
    return false;
  }
  const QString host = url.host().toLower();
  return host == QStringLiteral("github.com") ||
         host.endsWith(QStringLiteral(".github.com")) ||
         host.endsWith(QStringLiteral(".githubusercontent.com"));
}

QNetworkRequest make_request(const QUrl& url) {
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("Clipster/" CLIPSTER_VERSION));
  request.setTransferTimeout(kTimeoutMs);
  return request;
}

}  // namespace

Updater::Updater(QObject* parent)
    : nam_(new QNetworkAccessManager(parent)), parent_(parent) {}

QString Updater::current_version() { return QStringLiteral(CLIPSTER_VERSION); }

bool Updater::is_newer(const QString& tag, const QString& current) {
  return !tag.isEmpty() && parse_version(tag) > parse_version(current);
}

bool Updater::is_installed_build() {
  // Inno Setup records the install directory under the AppId from
  // installer/clipster.iss; a portable unzip (or a dev build) has no such
  // key, or points somewhere else.
  const QSettings key(
      QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\"
                     "Uninstall\\{8F4E9A21-6C37-4B5D-9E12-D3A47C10BE55}_is1"),
      QSettings::NativeFormat);
  const QString installed = key.value(QStringLiteral("InstallLocation")).toString();
  if (installed.isEmpty()) {
    return false;
  }
  const QString here = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
  return QDir(installed).canonicalPath().compare(here, Qt::CaseInsensitive) == 0;
}

void Updater::check(std::function<void(std::optional<UpdateInfo>)> done,
                    std::function<void(QString)> failed) {
  QNetworkReply* reply = nam_->get(make_request(QUrl(QLatin1String(kLatestReleaseUrl))));
  QObject::connect(reply, &QNetworkReply::finished, parent_,
                   [reply, done = std::move(done), failed = std::move(failed)] {
                     reply->deleteLater();
                     if (reply->error() != QNetworkReply::NoError) {
                       if (failed) {
                         failed(reply->errorString());
                       }
                       return;
                     }
                     const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                     UpdateInfo info;
                     info.tag = doc[QStringLiteral("tag_name")].toString();
                     info.page_url = doc[QStringLiteral("html_url")].toString();
                     for (const QJsonValue& asset : doc[QStringLiteral("assets")].toArray()) {
                       const QString name = asset[QStringLiteral("name")].toString();
                       if (name.startsWith(QStringLiteral("Clipster-Setup"),
                                           Qt::CaseInsensitive) &&
                           name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
                         info.installer_url =
                             asset[QStringLiteral("browser_download_url")].toString();
                         info.installer_size =
                             asset[QStringLiteral("size")].toVariant().toLongLong();
                         break;
                       }
                     }
                     if (!done) {
                       return;
                     }
                     if (!is_newer(info.tag, current_version())) {
                       done(std::nullopt);
                       return;
                     }
                     log::info("update available: {} (running {})", info.tag.toStdString(),
                               current_version().toStdString());
                     done(info);
                   });
}

void Updater::install(const UpdateInfo& info,
                      std::function<void(qint64, qint64)> progress,
                      std::function<void()> launched, std::function<void(QString)> failed) {
  const auto fail = [failed](const QString& why) {
    log::warn("update install failed: {}", why.toStdString());
    if (failed) {
      failed(why);
    }
  };
  const QUrl url(info.installer_url);
  if (info.installer_url.isEmpty() || !trusted_download(url)) {
    fail(QObject::tr("This release has no installer to download."));
    return;
  }
  if (info.installer_size > kMaxInstallerBytes) {
    fail(QObject::tr("The installer is unexpectedly large — download it yourself."));
    return;
  }

  const QString path =
      QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
          .filePath(QStringLiteral("Clipster-Setup-%1.exe").arg(info.tag));
  auto file = std::make_shared<QFile>(path);
  if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    fail(QObject::tr("Could not write to %1.").arg(QDir::toNativeSeparators(path)));
    return;
  }

  QNetworkReply* reply = nam_->get(make_request(url));
  if (progress) {
    QObject::connect(reply, &QNetworkReply::downloadProgress, parent_, progress);
  }
  QObject::connect(reply, &QNetworkReply::readyRead, parent_,
                   [reply, file] { file->write(reply->readAll()); });
  QObject::connect(
      reply, &QNetworkReply::finished, parent_,
      [reply, file, path, fail, launched = std::move(launched)] {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        if (reply->error() != QNetworkReply::NoError) {
          QFile::remove(path);
          fail(reply->errorString());
          return;
        }
        // Redirects are followed by default, but a redirect off GitHub
        // would land the payload somewhere untrusted.
        if (!trusted_download(reply->url())) {
          QFile::remove(path);
          fail(QObject::tr("The download was redirected off GitHub."));
          return;
        }
        if (file->size() < kMinInstallerBytes) {
          QFile::remove(path);
          fail(QObject::tr("The download was incomplete."));
          return;
        }
        // /SILENT shows progress but asks nothing; the installer closes
        // Clipster itself and relaunches it when it is done.
        if (!QProcess::startDetached(path, {QStringLiteral("/SILENT")})) {
          QFile::remove(path);
          fail(QObject::tr("Could not start the installer."));
          return;
        }
        log::info("update installer started: {}", path.toStdString());
        if (launched) {
          launched();
        }
      });
}

}  // namespace clipster::gui
