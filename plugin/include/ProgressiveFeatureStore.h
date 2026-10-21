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
    static void ensure(bool b,const char* s){if(!b)throw std::runtime_error(s);}
public:
    static QString metadataKey(){return "ALiS.progressiveFeatures.v2";}
    static QJsonObject state(const ccPointCloud& c){return QJsonDocument::fromJson(c.getMetaData(metadataKey()).toByteArray()).object();}
    static bool compatible(const ccPointCloud& c,const QString& identity,const QStringList& keys){
        const auto s=state(c);if(s.value("identity").toString()!=identity||s.value("point_count").toDouble()!=c.size())return false;
        const auto names=s.value("fields").toObject();if(names.size()!=keys.size())return false;
        for(const auto& k:keys){const int i=c.getScalarFieldIndexByName(names.value(k).toString().toUtf8().constData());if(i<0||c.getScalarField(i)->currentSize()!=c.size())return false;}return true;
    }
    ProgressiveFeatureStore(ccPointCloud* c,const QString& identity,const QStringList& keys,const QJsonObject& config):m_cloud(c){
        ensure(c&&!keys.isEmpty(),"Progressive features need a cloud and nonempty schema");
        if(compatible(*c,identity,keys)){m_state=state(*c);const auto names=m_state.value("fields").toObject();for(const auto& k:keys)m_indices<<c->getScalarFieldIndexByName(names.value(k).toString().toUtf8().constData());return;}
        const QString token=QUuid::createUuid().toString(QUuid::WithoutBraces);QJsonObject names;
        try{for(const auto& k:keys){QString name=QString("ALiS %1 m [%2]").arg(k,token.left(8));int suffix=1;while(c->getScalarFieldIndexByName(name.toUtf8().constData())>=0)name+=QString::number(suffix++);int index=c->addScalarField(name.toUtf8().constData());ensure(index>=0,"Cannot allocate full-cloud feature fields: use fewer features or a smaller cloud");m_indices<<index;c->getScalarField(index)->fill(std::numeric_limits<ScalarType>::quiet_NaN());names.insert(k,name);}}
        catch(...){for(int i=m_indices.size()-1;i>=0;--i)c->deleteScalarField(m_indices[i]);throw;}
        m_state={{"schema",2},{"job_id",token},{"identity",identity},{"point_count",double(c->size())},{"config",config},{"fields",names},{"blocks",QJsonObject()},{"selection",QJsonArray()}};
        persist();
    }
    ccPointCloud* cloud()const{return m_cloud;}
    QJsonObject metadata()const{return m_state;}
    QJsonObject completed()const{return m_state.value("blocks").toObject();}
    void persist(){
        m_cloud->setMetaData(metadataKey(),QJsonDocument(m_state).toJson(QJsonDocument::Compact));
        auto mapping=QJsonDocument::fromJson(m_cloud->getMetaData("ALiS.featureFields").toByteArray()).object();
        const auto names=m_state.value("fields").toObject();for(auto it=names.begin();it!=names.end();++it)mapping.insert(it.key(),it.value());
        m_cloud->setMetaData("ALiS.featureFields",QJsonDocument(mapping).toJson(QJsonDocument::Compact));
    }
    void setSelection(const QJsonArray& cells){m_state.insert("selection",cells);persist();}
    void setOutputFolder(const QString& path){m_state.insert("checkpoint_folder",path);persist();}
    void commit(const QString& block,const QVector<unsigned>& rows,const QVector<float>& values,const QString& checkpoint){
        ensure(!completed().contains(block),"Completed blocks are not recomputed implicitly");
        ensure(rows.size()>0&&quint64(values.size())==quint64(rows.size())*m_indices.size(),"Invalid progressive core matrix");
        for(int i=0;i<rows.size();++i){ensure(rows[i]<m_cloud->size()&&(i==0||rows[i]>rows[i-1]),"Invalid or duplicated original point indices");}
        for(float v:values)ensure(!std::isinf(v),"Infinite feature value is not a valid output");
        // Validation/allocations precede the scatter. Do not pump GUI events or
        // accept cancellation inside this short, non-allocating commit section.
        auto blocks=completed();blocks.insert(block,QJsonObject{{"points",double(rows.size())},{"checkpoint",checkpoint}});
        for(int f=0;f<m_indices.size();++f){auto* sf=m_cloud->getScalarField(m_indices[f]);for(int i=0;i<rows.size();++i)sf->setValue(rows[i],values[i*m_indices.size()+f]);sf->computeMinAndMax();}
        m_state.insert("blocks",blocks);persist();
    }
};
}
