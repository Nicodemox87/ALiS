// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
namespace alis {
inline QString blockWorkerPath() {
#ifdef Q_OS_WIN
    const auto name = QStringLiteral("ALiS_prepare_block.exe");
#else
    const auto name = QStringLiteral("ALiS_prepare_block");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}
inline QString pythonWorkerPath() {
    const QDir app(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    const QString bundleWorker=app.filePath(QStringLiteral("../Resources/ALiS/worker/qal_ml_worker.py"));
    if(QFileInfo::exists(bundleWorker)) return QDir::cleanPath(bundleWorker);
#endif
    const auto worker=app.filePath(QStringLiteral("worker/ALiS/qal_ml_worker.py"));
    const auto legacy=app.filePath(QStringLiteral("worker/qArchaeoLiDAR/qal_ml_worker.py"));
    return QFileInfo::exists(worker)||!QFileInfo::exists(legacy)?worker:legacy;
}
inline QStringList pythonIsolationArguments(const QString& executable) {
#ifdef Q_OS_MACOS
    const auto bundled=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../Resources/ALiS/runtime/bin/python3"));
    if(QFileInfo(executable).canonicalFilePath()==QFileInfo(bundled).canonicalFilePath()
       && QFileInfo(bundled).isExecutable()) return {QStringLiteral("-E"),QStringLiteral("-s"),QStringLiteral("-B")};
#else
    Q_UNUSED(executable);
#endif
    return {};
}
inline QString defaultPythonPath() {
    QString python=qEnvironmentVariable("ALIS_PYTHON");
    if(python.isEmpty()) python=qEnvironmentVariable("QARCHAEOLIDAR_PYTHON");
    if(!python.isEmpty()) return python;
#ifdef Q_OS_MACOS
    const auto bundled=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../Resources/ALiS/runtime/bin/python3"));
    if(QFileInfo(bundled).isExecutable()) return QDir::cleanPath(bundled);
#endif
    const QDir home(QDir::homePath());
#ifdef Q_OS_WIN
    const auto managed=home.filePath(QStringLiteral(".alis/runtime-cu128/Scripts/python.exe"));
    if(QFileInfo::exists(managed)&&QFileInfo::exists(home.filePath(QStringLiteral(".alis/runtime-cu128/alis-runtime-ready.json"))))return managed;
    python=QStandardPaths::findExecutable(QStringLiteral("python.exe"));
#else
    const auto managed=home.filePath(QStringLiteral(".alis/runtime-macos/bin/python3"));
    if(QFileInfo(managed).isExecutable())return managed;
    python=QStandardPaths::findExecutable(QStringLiteral("python3"));
#endif
    if(python.isEmpty())python=QStandardPaths::findExecutable(QStringLiteral("python"));
    return python;
}
}
