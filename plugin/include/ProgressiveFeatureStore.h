// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QStringList>
#include <QVector>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QCryptographicHash>
#include "FeatureCheckpoint.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace alis {
// SF values and progress live on the SAME entity and therefore survive BIN.
// Only fields owned by this immutable job may be updated; ordinary fields are
// never overwritten. A completed block can legitimately contain NaN features.
class ProgressiveFeatureStore {
    ccPointCloud* m_cloud;
    QJsonObject m_state;
    QVector<int> m_indices;
    bool m_disk=false;
    static void ensure(bool b,const char* s){if(!b)throw std::runtime_error(s);}
public:
    static QString metadataKey(bool disk=false){return disk?"ALiS.diskFeatures.v1":"ALiS.progressiveFeatures.v2";}
    static QJsonObject state(const ccPointCloud& c,bool disk=false){return QJsonDocument::fromJson(c.getMetaData(metadataKey(disk)).toByteArray()).object();}
    static bool compatible(const ccPointCloud& c,const QString& identity,const QStringList& keys,bool disk=false){
        const auto s=state(c,disk);if(s.value("identity").toString()!=identity||s.value("point_count").toDouble()!=c.size())return false;
        if(disk){QStringList saved;for(auto k:s.value("feature_keys").toArray())saved<<k.toString();return saved==keys;}
        const auto names=s.value("fields").toObject();if(names.size()!=keys.size())return false;
        for(const auto& k:keys){const int i=c.getScalarFieldIndexByName(names.value(k).toString().toUtf8().constData());if(i<0||c.getScalarField(i)->currentSize()!=c.size())return false;}return true;
    }
    ProgressiveFeatureStore(ccPointCloud* c,const QString& identity,const QStringList& keys,const QJsonObject& config,bool disk=false,bool freshDisk=false):m_cloud(c),m_disk(disk){
        ensure(c&&!keys.isEmpty(),"Progressive features need a cloud and nonempty schema");
        if(disk){
            if(!freshDisk&&compatible(*c,identity,keys,true)){m_state=state(*c,true);return;}
            QJsonArray list;for(const auto& k:keys)list.append(k);
            m_state={{"schema",3},{"disk_only",true},{"job_id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"identity",identity},{"point_count",double(c->size())},{"config",config},{"feature_keys",list},{"blocks",QJsonObject()},{"selection",QJsonArray()}};persist();return;
        }
        if(compatible(*c,identity,keys)){m_state=state(*c);const auto names=m_state.value("fields").toObject();for(const auto& k:keys)m_indices<<c->getScalarFieldIndexByName(names.value(k).toString().toUtf8().constData());return;}
        const QString token=QUuid::createUuid().toString(QUuid::WithoutBraces);QJsonObject names;
        const auto previous=state(*c);if(!previous.isEmpty()){
            auto history=QJsonDocument::fromJson(c->getMetaData("ALiS.featureJobHistory.v1").toByteArray()).object();
            history.insert(previous.value("job_id").toString(),previous);
            c->setMetaData("ALiS.featureJobHistory.v1",QJsonDocument(history).toJson(QJsonDocument::Compact));
        }
        try{for(const auto& k:keys){QString name=QString("ALiS %1 m [%2]").arg(k,token.left(8));int suffix=1;while(c->getScalarFieldIndexByName(name.toUtf8().constData())>=0)name+=QString::number(suffix++);int index=c->addScalarField(name.toUtf8().constData());ensure(index>=0,"Cannot allocate full-cloud feature fields: use fewer features or a smaller cloud");m_indices<<index;c->getScalarField(index)->fill(std::numeric_limits<ScalarType>::quiet_NaN());names.insert(k,name);}}
        catch(...){for(int i=m_indices.size()-1;i>=0;--i)c->deleteScalarField(m_indices[i]);throw;}
        m_state={{"schema",2},{"job_id",token},{"identity",identity},{"point_count",double(c->size())},{"config",config},{"fields",names},{"blocks",QJsonObject()},{"selection",QJsonArray()}};
        persist();
    }
    ccPointCloud* cloud()const{return m_cloud;}
    QJsonObject metadata()const{return m_state;}
    QJsonObject completed()const{return m_state.value("blocks").toObject();}
    void persist(){
        if(m_disk){
            const auto folder=m_state.value("checkpoint_folder").toString();
            if(!folder.isEmpty()){QSaveFile file(QDir(folder).filePath("disk_features.json"));const auto bytes=QJsonDocument(m_state).toJson();ensure(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot commit disk feature ledger; existing ledger retained");}
            m_cloud->setMetaData(metadataKey(true),QJsonDocument(m_state).toJson(QJsonDocument::Compact));return;
        }
        m_cloud->setMetaData(metadataKey(),QJsonDocument(m_state).toJson(QJsonDocument::Compact));
        auto mapping=QJsonDocument::fromJson(m_cloud->getMetaData("ALiS.featureFields").toByteArray()).object();
        const auto names=m_state.value("fields").toObject();for(auto it=names.begin();it!=names.end();++it)mapping.insert(it.key(),it.value());
        m_cloud->setMetaData("ALiS.featureFields",QJsonDocument(mapping).toJson(QJsonDocument::Compact));
    }
    void setSelection(const QJsonArray& cells){m_state.insert("selection",cells);persist();}
    void setOutputFolder(const QString& path){m_state.insert("checkpoint_folder",path);persist();}
    // Resume a disk ledger even when the last BIN predates the linked job.
    // Geometry/order, schema, all committed files and checksums must match first.
    static bool recoverDiskLedger(ccPointCloud& cloud,const QString& folder,const QString& identity,const QStringList& keys,const std::function<void()>& tick={}){
        QFile f(QDir(folder).filePath("disk_features.json"));if(!f.exists())return false;
        ensure(f.open(QIODevice::ReadOnly)&&f.size()<64*1024*1024,"Cannot inspect disk feature ledger");auto saved=QJsonDocument::fromJson(f.readAll()).object();
        QJsonArray schema;for(auto key:keys)schema.append(key);
        ensure(saved.value("schema").toInt()==3&&saved.value("disk_only").toBool()&&saved.value("identity").toString()==identity&&saved.value("point_count").toDouble()==cloud.size()&&saved.value("feature_keys").toArray()==schema&&!saved.value("job_id").toString().isEmpty(),"Disk ledger belongs to a different cloud/order/grid/schema");
        auto blocks=saved.value("blocks").toObject();for(auto it=blocks.begin();it!=blocks.end();++it){const auto entry=it.value().toObject();auto path=entry.value("checkpoint").toString();
            ensure(QFileInfo(path).absolutePath()==QDir(folder).absolutePath(),"Checkpoint must remain in this job folder");
            const auto hash=validateFeatureCheckpoint(path,checkpointMetadata(identity,it.key(),keys),quint64(entry.value("points").toDouble()),cloud.size(),tick);
            ensure(hash==entry.value("sha256").toString(),"Disk checkpoint checksum mismatch: existing outputs retained");}
        saved.insert("checkpoint_folder",QDir(folder).absolutePath());cloud.setMetaData(metadataKey(true),QJsonDocument(saved).toJson(QJsonDocument::Compact));return true;
    }
    void finalizeRanges(){if(!m_disk)for(int i:m_indices)m_cloud->getScalarField(i)->computeMinAndMax();}
    void commitFile(const QString& block,const QString& checkpoint,quint64 rows,const QStringList& keys,const std::function<void()>& tick={},bool deferRanges=false){
        ensure(!completed().contains(block),"Completed blocks are not recomputed implicitly");
        const int columns=m_disk?m_state.value("feature_keys").toArray().size():m_indices.size();ensure(rows>0&&columns==keys.size(),"Invalid streamed feature commit dimensions");
        auto hash=validateFeatureCheckpoint(checkpoint,checkpointMetadata(m_state.value("identity").toString(),block,keys),rows,m_cloud->size(),tick);
        // Validate before scattering. Cancellation leaves this core pending;
        // only fields owned by this job have been touched and can be resumed.
        auto blocks=completed();QJsonObject entry{{"points",double(rows)},{"checkpoint",checkpoint}};if(m_disk)entry.insert("sha256",hash);blocks.insert(block,entry);
        if(!m_disk){FeatureCheckpointReader reader(checkpoint);quint64 n=0;while(reader.valid){for(int col=0;col<columns;++col){float value;std::memcpy(&value,reader.record.constData()+4+col*4,4);m_cloud->getScalarField(m_indices[col])->setValue(reader.row,value);}reader.advance();if(++n%32768==0&&tick)tick();}if(!deferRanges)finalizeRanges();}
        const auto before=m_state;m_state.insert("blocks",blocks);try{persist();}catch(...){m_state=before;throw;}
    }
    void commit(const QString& block,const QVector<unsigned>& rows,const QVector<float>& values,const QString& checkpoint){
        ensure(!completed().contains(block),"Completed blocks are not recomputed implicitly");
        const int columns=m_disk?m_state.value("feature_keys").toArray().size():m_indices.size();
        ensure(rows.size()>0&&quint64(values.size())==quint64(rows.size())*columns,"Invalid progressive core matrix");
        for(int i=0;i<rows.size();++i){ensure(rows[i]<m_cloud->size()&&(i==0||rows[i]>rows[i-1]),"Invalid or duplicated original point indices");}
        for(float v:values)ensure(!std::isinf(v),"Infinite feature value is not a valid output");
        // Validation/allocations precede the scatter. Do not pump GUI events or
        // accept cancellation inside this short, non-allocating commit section.
        auto blocks=completed();QJsonObject entry{{"points",double(rows.size())},{"checkpoint",checkpoint}};
        if(m_disk){QFile file(checkpoint);QCryptographicHash hash(QCryptographicHash::Sha256);ensure(file.open(QIODevice::ReadOnly)&&hash.addData(&file),"Cannot verify disk checkpoint");entry.insert("sha256",QString::fromLatin1(hash.result().toHex()));}
        blocks.insert(block,entry);
        for(int f=0;f<m_indices.size();++f){auto* sf=m_cloud->getScalarField(m_indices[f]);for(int i=0;i<rows.size();++i)sf->setValue(rows[i],values[i*m_indices.size()+f]);sf->computeMinAndMax();}
        const auto before=m_state;m_state.insert("blocks",blocks);
        try{persist();}catch(...){m_state=before;throw;}
    }
};
}
