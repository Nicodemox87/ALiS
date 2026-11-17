// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ProgressiveFeatureStore.h"
#include <QSet>
#include <QDateTime>
#include <algorithm>
namespace alis {
inline QString scalarCanonicalName(QString name){return name.toLower().remove(' ').remove('_');}
inline bool protectedScalarField(const QString& name){
    const auto key=scalarCanonicalName(name);
    return key.startsWith("qal")||key=="classification"||key=="returnnumber"||key=="numberofreturns"||key=="originalcloudindex";
}
struct ScalarFieldStats {
    quint64 finite=0,missing=0;
    double minimum=std::numeric_limits<double>::quiet_NaN();
    double maximum=std::numeric_limits<double>::quiet_NaN();
    double mean=std::numeric_limits<double>::quiet_NaN();
};
inline ScalarFieldStats inspectScalarField(const CCCoreLib::ScalarField& field,const std::function<void()>& tick={}){
    ScalarFieldStats result;for(unsigned i=0;i<field.currentSize();++i){const double value=field.getValue(i);if(std::isfinite(value)){++result.finite;if(result.finite==1)result.minimum=result.maximum=result.mean=value;else{result.minimum=std::min(result.minimum,value);result.maximum=std::max(result.maximum,value);result.mean+=(value-result.mean)/double(result.finite);}}else ++result.missing;if(tick&&i%65536==0)tick();}return result;
}
// Only loaded columns are removed, never source/checkpoint/model files.
inline QJsonObject removeSelectedScalarFields(ccPointCloud& cloud,const QStringList& selected){
    checkpointCheck(!selected.isEmpty(),"Select scalar fields to remove");QSet<QString> names;QVector<int> indices;
    for(const auto& name:selected){checkpointCheck(!protectedScalarField(name),"Ground / classes / return metadata are protected");int index=cloud.getScalarFieldIndexByName(name.toUtf8());checkpointCheck(index>=0&&!names.contains(name),"Missing/repeated scalar field");names.insert(name);indices<<index;}
    auto references=[&](const QJsonObject& job){const auto fields=job.value("fields").toObject();for(auto it=fields.begin();it!=fields.end();++it)if(names.contains(it.value().toString()))return true;return false;};
    auto detached=QJsonDocument::fromJson(cloud.getMetaData("ALiS.detachedFeatureJobs.v1").toByteArray()).array();
    auto detach=[&](QJsonObject job){job.insert("detached_reason","Scalar fields removed by user; checkpoint files retained. Recover saved blocks before resume.");detached.append(job);};
    const auto state=ProgressiveFeatureStore::state(cloud);if(references(state)){detach(state);cloud.removeMetaData(ProgressiveFeatureStore::metadataKey());}
    auto history=QJsonDocument::fromJson(cloud.getMetaData("ALiS.featureJobHistory.v1").toByteArray()).object();
    const auto historyKeys=history.keys();for(const auto& key:historyKeys)if(references(history.value(key).toObject())){detach(history.value(key).toObject());history.remove(key);}
    cloud.setMetaData("ALiS.featureJobHistory.v1",QJsonDocument(history).toJson(QJsonDocument::Compact));
    cloud.setMetaData("ALiS.detachedFeatureJobs.v1",QJsonDocument(detached).toJson(QJsonDocument::Compact));
    for(const auto& key:QStringList{"ALiS.featureFields","qArchaeoLiDAR.featureFields"}){auto mapping=QJsonDocument::fromJson(cloud.getMetaData(key).toByteArray()).object();for(const auto& feature:mapping.keys())if(names.contains(mapping.value(feature).toString()))mapping.remove(feature);cloud.setMetaData(key,QJsonDocument(mapping).toJson(QJsonDocument::Compact));}
    std::sort(indices.begin(),indices.end(),std::greater<int>());for(int index:indices)cloud.deleteScalarField(index);
    cloud.prepareDisplayForRefresh();QJsonArray removed;for(auto name:selected)removed.append(name);
    return {{"removed_fields",removed},{"released_bytes",double(quint64(cloud.size())*selected.size()*sizeof(ScalarType))},{"checkpoint_files_deleted",false},{"source_geometry_changed",false},{"timestamp_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
}
}
