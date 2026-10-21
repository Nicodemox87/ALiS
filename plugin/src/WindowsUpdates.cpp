// SPDX-License-Identifier: GPL-2.0-or-later
#include "WindowsUpdates.h"
#include "ReleaseVersion.h"
#ifdef Q_OS_WIN
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QMessageBox>
#include <QDir>
#include <QProcess>
#include <QPointer>
#include <QSettings>
#include <QTimer>

namespace alis {
void checkWindowsUpdates(QWidget* parent, bool automatic) {
    static QPointer<QProcess> active;
    QSettings settings;
    const auto last=settings.value("ALiS/updates/windowsLastSuccess").toDateTime();
    if(automatic && (!settings.value("ALiS/updates/automatic",true).toBool() ||
       (last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc())>=0 && last.secsTo(QDateTime::currentDateTimeUtc())<86400)))return;
    if(active) {
        if(!automatic)QMessageBox::information(parent,"ALiS updates","An update check is already running.");
        return;
    }
    // Use Windows' TLS trust store, not a missing/obsolete host OpenSSL DLL.
    // The command and URL are constants: no release/project content is executed.
    auto* process=new QProcess(parent);active=process;
    process->setProgram(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/WindowsPowerShell/v1.0/powershell.exe"));
    process->setArguments({"-NoProfile","-NonInteractive","-Command",QStringLiteral(
        "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; "
        "$ProgressPreference='SilentlyContinue'; try { "
        "$r=Invoke-WebRequest -UseBasicParsing -MaximumRedirection 0 -TimeoutSec 20 "
        "-Uri 'https://api.github.com/repos/Nicodemox87/ALiS/releases?per_page=100' "
        "-Headers @{'User-Agent'='ALiS-updater';'Accept'='application/vnd.github+json'}; "
        "if($r.Content.Length -gt 4194304){exit 1}; [Console]::OutputEncoding=[Text.Encoding]::UTF8; [Console]::Out.Write($r.Content) "
        "} catch {exit 1}")});
    auto* timer=new QTimer(process);timer->setSingleShot(true);timer->start(25000);
    QObject::connect(timer,&QTimer::timeout,process,&QProcess::kill);
    QObject::connect(process,&QProcess::readyReadStandardOutput,process,[process](){if(process->bytesAvailable()>4*1024*1024)process->kill();});
    QPointer<QWidget> owner(parent);
    QObject::connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),process,[process,owner,automatic](int code,QProcess::ExitStatus status){
        const auto raw=process->readAllStandardOutput();
        QJsonParseError error;
        const auto json=QJsonDocument::fromJson(raw,&error);
        const bool ok=code==0 && status==QProcess::NormalExit &&
            raw.size()<=4*1024*1024 && error.error==QJsonParseError::NoError && json.isArray();
        process->deleteLater();
        if(!owner)return;
        if(!ok){
            if(!automatic)QMessageBox::warning(owner,"ALiS updates","GitHub could not be checked (offline, rate limit or invalid response). This does not mean ALiS is up to date. Try again later.");
            return;
        }
        QSettings().setValue("ALiS/updates/windowsLastSuccess",QDateTime::currentDateTimeUtc());
        const auto release=newestWindowsRelease(json.array(),QStringLiteral(ALIS_RELEASE_VERSION));
        if(release.isEmpty()){
            if(!automatic)QMessageBox::information(owner,"ALiS updates","No newer compatible Windows complete installer was found on the official channel (including testing previews).");
            return;
        }
        if(QMessageBox::question(owner,"ALiS update available",
            QString("%1 is available; installed: %2.\n\nOpen the official release page to download the complete installer? Save your work and close CloudCompare before installing. Nothing is downloaded or installed automatically.")
            .arg(release.value("tag_name").toString(),QStringLiteral(ALIS_RELEASE_VERSION)),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes)
            QDesktopServices::openUrl(QUrl(release.value("html_url").toString()));
    });
    QObject::connect(process,&QProcess::errorOccurred,process,[process,owner,automatic](QProcess::ProcessError e){
        if(e==QProcess::FailedToStart){if(owner && !automatic)QMessageBox::warning(owner,"ALiS updates","Windows could not start the update check. No update status is available.");process->deleteLater();}
    });
    process->start();
}
}
#endif
