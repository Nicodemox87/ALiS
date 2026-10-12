// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef Q_OS_MACOS
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QCoreApplication>
#include <QPointer>

namespace alis {
inline QString macResource(const QString& name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/ALiS/" + name);
}
inline QString macInstalledVersion() {
    QFile f(macResource("mac-release.json"));
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("development build");
    const auto c = QJsonDocument::fromJson(f.readAll()).object();
    return c.value("version").toString() + QStringLiteral(" (Mac build %1)").arg(c.value("build").toInt());
}
inline void checkMacUpdates(QWidget* parent, bool automatic = false, bool download = false) {
    static QPointer<QProcess> active;
    if (active) {
        if (!automatic) QMessageBox::information(parent, QStringLiteral("ALiS updates"), QStringLiteral("An update check or download is already running."));
        return;
    }
    QSettings settings;
    const auto lastAttempt = settings.value("ALiS/updates/lastAttempt").toDateTime();
    if (automatic && (!settings.value("ALiS/updates/automatic", true).toBool()
        || (lastAttempt.isValid() && lastAttempt.secsTo(QDateTime::currentDateTimeUtc()) < 86400))) return;
    const QString python = macResource("runtime/bin/python3");
    if (!QFile::exists(python)) return;
    settings.setValue("ALiS/updates/lastAttempt", QDateTime::currentDateTimeUtc());
    auto* process = new QProcess(qApp);
    active = process;
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("PYTHONPATH"); env.remove("PYTHONHOME"); env.insert("PYTHONDONTWRITEBYTECODE", "1");
    process->setProcessEnvironment(env);
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, process, [process](){process->kill();});
    const QPointer<QWidget> owner(parent);
    QObject::connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), process,
        [process, owner, automatic, download](int code, QProcess::ExitStatus exitStatus) {
        active.clear();
        const auto result = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        process->deleteLater();
        const QString status = result.value("status").toString();
        if (code != 0 || exitStatus != QProcess::NormalExit || status == "error" || status.isEmpty()) {
            if (!automatic) QMessageBox::warning(owner, "ALiS updates", "Could not check or download the update. Try again later.\n" + result.value("message").toString());
            return;
        }
        if (status == "available") {
            if (download) {
                const QString path = result.value("path").toString();
                if (path.isEmpty()) return;
                // Reveal the verified package; installation requires closing ALiS first.
                QProcess::startDetached("/usr/bin/open", {"-R", path});
                QMessageBox::information(owner, "ALiS update downloaded", "The verified installer is selected in Finder. Save your work, quit ALiS, then open the package. macOS will ask permission to install it.\n\n" + path);
            } else if (QMessageBox::question(owner, "ALiS update available",
                QStringLiteral("ALiS %1 (Mac build %2) is available from the official GitHub repository. Download the installer now? Your current session will stay open.")
                    .arg(result.value("release").toObject().value("version").toString())
                    .arg(result.value("release").toObject().value("build").toInt()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
                checkMacUpdates(owner, false, true);
            }
        } else if (!automatic) {
            const QString message = status == "current" ? QStringLiteral("This is the latest compatible Mac build in the release channel.")
                : status == "incompatible" ? QStringLiteral("A newer release requires a newer macOS version.")
                : QStringLiteral("No Mac installer has been published on the official GitHub release channel yet.");
            QMessageBox::information(owner, "ALiS updates", message);
        }
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process,owner,automatic](QProcess::ProcessError error){
        if (error != QProcess::FailedToStart) return;
        active.clear(); process->deleteLater();
        if (!automatic) QMessageBox::warning(owner, "ALiS updates", "The bundled update helper could not start.");
    });
    process->start(python, {"-I", "-B", macResource("worker/alis_mac_update.py"), download ? "download" : "check"});
    timeout->start(download ? 1800000 : 60000);
}
}
#endif
