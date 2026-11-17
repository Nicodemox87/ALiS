// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ProgressiveFeatureStore.h"
#include "MemorySafety.h"
#include <QBitArray>
#include <QSet>

namespace alis {
// Validate every payload before allocating/scattering scalar fields.
inline QJsonObject inspectFeatureRecovery(ccPointCloud& cloud,const QString& folder,
 const QString& coordinateHash,const std::function<void()>& tick={}) {
    QFile file(QDir(folder).filePath("blocks.json"));
    checkpointCheck(file.open(QIODevice::ReadOnly)&&file.size()<64*1024*1024,"Cannot read blocks.json");
    const auto manifest=QJsonDocument::fromJson(file.readAll()).object();const auto config=manifest.value("config").toObject();
    checkpointCheck(manifest.value("schema").toString()=="alis-spatial-blocks-2"&&config.value("point_count").toDouble()==cloud.size()&&!coordinateHash.isEmpty()&&config.value("coordinate_sha256").toString()==coordinateHash,"Recovery refused: cloud coordinates/order/count do not match the saved job");
    const auto identity=manifest.value("identity").toString();QStringList keys;for(auto key:config.value("features").toArray())keys<<key.toString();
    checkpointCheck(!identity.isEmpty()&&!keys.isEmpty(),"Missing recovery schema");
    const auto expected=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(config).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    checkpointCheck(identity==expected,"Recovery configuration identity mismatch");
    checkpointCheck(cloud.size()<=unsigned(std::numeric_limits<int>::max()),"Recovery ownership exceeds supported point count");
    QBitArray ownership(int(cloud.size()),false);QJsonArray validated;QSet<QString> names;
    for(auto value:manifest.value("blocks").toArray()) {
        const auto block=value.toObject();if(block.value("status").toString()!="Completed")continue;
        const auto id=block.value("id").toString();const auto path=QFileInfo(block.value("file").toString()).canonicalFilePath();
        checkpointCheck(!id.isEmpty()&&!names.contains(id)&&!path.isEmpty()&&QFileInfo(path).absolutePath()==QFileInfo(folder).canonicalFilePath(),"Missing, repeated or external checkpoint");names.insert(id);
        const quint64 count=quint64(block.value("core_points").toDouble());
        const auto hash=validateFeatureCheckpoint(path,checkpointMetadata(identity,id,keys),count,cloud.size(),tick);
        FeatureCheckpointReader reader(path);quint64 n=0;
        while(reader.valid){checkpointCheck(!ownership.testBit(int(reader.row)),"Duplicate ID across recovered cores");ownership.setBit(int(reader.row));reader.advance();if(++n%32768==0&&tick)tick();}
        validated.append(QJsonObject{{"id",id},{"file",path},{"rows",double(count)},{"sha256",hash}});
    }
    checkpointCheck(!validated.isEmpty(),"No completed checkpoint to recover");
    return {{"manifest",manifest},{"validated",validated},{"covered",double(ownership.count(true))}};
}
}
