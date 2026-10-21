// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
class QWidget;
namespace alis {
void checkWindowsUpdates(QWidget* parent, bool automatic = false);
inline QVector<int> windowsVersionParts(const QString& value) {
    const auto match=QRegularExpression(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(?:-alpha\\.([\\d.]+))?$"),QRegularExpression::CaseInsensitiveOption).match(value);
    if(!match.hasMatch())return {};
    QVector<int> result{match.captured(1).toInt(),match.captured(2).toInt(),match.captured(3).toInt()};
    if(match.captured(4).isEmpty())result<<1000000;
    else for(const auto& part:match.captured(4).split('.')){bool ok=false;int n=part.toInt(&ok);if(!ok)return {};result<<n;}
    return result;
}
inline bool newerWindowsVersion(const QString& candidate,const QString& reference) {
    const auto a=windowsVersionParts(candidate),b=windowsVersionParts(reference);
    if(a.isEmpty()||b.isEmpty())return false;
    for(int i=0;i<qMax(a.size(),b.size());++i){int x=a.value(i,0),y=b.value(i,0);if(x!=y)return x>y;}
    return false;
}
inline QString windowsInstallerVersion(const QString& filename) {
    const auto match=QRegularExpression(QStringLiteral("^ALiS-(.+?)-(?:CloudCompare-.*-Setup|Windows-x64-Complete-Setup-r\\d+)\\.exe$"),QRegularExpression::CaseInsensitiveOption).match(filename);
    return match.hasMatch()&&!windowsVersionParts(match.captured(1)).isEmpty()?match.captured(1):QString();
}
// Only release pages on our repository are opened; no remote executable runs here.
inline QJsonObject newestWindowsRelease(const QJsonArray& releases, const QString& installed) {
    QJsonObject best;
    QString bestVersion=installed;
    for(const auto& item:releases) {
        const auto release=item.toObject();
        if(release.value("draft").toBool())continue;
        const QString tag=release.value("tag_name").toString();
        if(!tag.startsWith('v'))continue;
        const QString version=tag.mid(1);
        if(!newerWindowsVersion(version,bestVersion))continue;
        const QString expected="ALiS-"+version+"-Windows-x64-Complete-Setup-r1.exe";
        const QString page="https://github.com/Nicodemox87/ALiS/releases/tag/"+tag;
        if(release.value("html_url").toString()!=page)continue;
        bool found=false;
        for(const auto& a:release.value("assets").toArray()) {
            const auto asset=a.toObject();
            if(asset.value("name").toString()==expected && asset.value("size").toDouble()>0 &&
               asset.value("browser_download_url").toString()=="https://github.com/Nicodemox87/ALiS/releases/download/"+tag+"/"+expected)found=true;
        }
        if(found){best=release;bestVersion=version;}
    }
    return best;
}
}
