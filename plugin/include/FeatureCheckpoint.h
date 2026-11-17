// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFile>
#include <QSaveFile>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace alis {
inline void checkpointCheck(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline QJsonObject checkpointMetadata(const QString& identity,const QString& block,const QStringList& keys){
    QJsonArray a;for(const auto& key:keys)a.append(key);return {{"identity",identity},{"block",block},{"features",a}};
}
// The ALISF02 format is unchanged. Readers/writers never allocate a whole core.
class FeatureCheckpointReader {
    QFile file;QByteArray buffer;int cursor=0;quint32 readRows=0,last=0;
public:
    QJsonObject metadata;quint32 rows=0,columns=0;quint32 row=0;QByteArray record;bool valid=false;
    explicit FeatureCheckpointReader(const QString& path):file(path){
        checkpointCheck(file.open(QIODevice::ReadOnly),"Cannot open feature checkpoint");
        QDataStream in(&file);in.setByteOrder(QDataStream::LittleEndian);char magic[7];quint32 length=0;
        checkpointCheck(in.readRawData(magic,7)==7&&std::memcmp(magic,"ALISF02",7)==0,"Invalid checkpoint signature");in>>length;
        checkpointCheck(length>0&&length<=16*1024*1024,"Invalid checkpoint metadata size");
        QByteArray bytes(int(length),Qt::Uninitialized);checkpointCheck(in.readRawData(bytes.data(),int(length))==int(length),"Truncated checkpoint header");
        metadata=QJsonDocument::fromJson(bytes).object();in>>rows>>columns;
        checkpointCheck(in.status()==QDataStream::Ok&&columns>0&&columns<=4096&&metadata.value("features").toArray().size()==int(columns),"Invalid checkpoint dimensions");
        checkpointCheck(file.size()-file.pos()==qint64(rows)*(4+qint64(columns)*4),"Invalid checkpoint record size");advance();
    }
    void advance(){
        if(readRows==rows){valid=false;return;}
        const int stride=4+int(columns)*4;
        if(cursor+stride>buffer.size()){buffer=file.read(qint64(std::max(1,1048576/stride))*stride);cursor=0;}
        checkpointCheck(cursor+stride<=buffer.size(),"Truncated checkpoint record");record=buffer.mid(cursor,stride);cursor+=stride;
        std::memcpy(&row,record.constData(),4);checkpointCheck(readRows==0||row>last,"Duplicate/unsorted checkpoint point ID");last=row;++readRows;
        for(quint32 f=0;f<columns;++f){float v;std::memcpy(&v,record.constData()+4+f*4,4);checkpointCheck(!std::isinf(v),"Infinite checkpoint feature");}valid=true;
    }
};
class FeatureCheckpointWriter {
    QSaveFile file;QByteArray buffer;quint32 expected,written=0,last=0;int stride;
public:
    FeatureCheckpointWriter(const QString& path,const QJsonObject& metadata,quint32 rows,quint32 columns):file(path),expected(rows),stride(4+int(columns)*4){
        checkpointCheck(columns>0&&columns<=4096&&file.open(QIODevice::WriteOnly),"Cannot create feature checkpoint");
        QDataStream out(&file);out.setByteOrder(QDataStream::LittleEndian);out.writeRawData("ALISF02",7);auto bytes=QJsonDocument(metadata).toJson(QJsonDocument::Compact);out<<quint32(bytes.size());out.writeRawData(bytes.constData(),bytes.size());out<<rows<<columns;
        checkpointCheck(out.status()==QDataStream::Ok,"Cannot write checkpoint header");buffer.reserve(1048576+stride);
    }
    void append(quint32 row,const char* values){
        checkpointCheck(written<expected&&(written==0||row>last),"Invalid merged checkpoint point ownership");last=row;++written;
        buffer.append(reinterpret_cast<const char*>(&row),4);buffer.append(values,stride-4);if(buffer.size()>=1048576)flush();
    }
    void flush(){checkpointCheck(file.write(buffer)==buffer.size(),"Cannot write feature checkpoint records");buffer.clear();}
    void finish(){checkpointCheck(written==expected,"Merged checkpoint core count mismatch");flush();checkpointCheck(file.commit(),"Cannot atomically commit feature checkpoint");}
};
inline QString validateFeatureCheckpoint(const QString& path,const QJsonObject& metadata,quint64 count,quint64 sourcePoints,const std::function<void()>& tick={}){
    {FeatureCheckpointReader r(path);checkpointCheck(r.metadata==metadata&&r.rows==count,"Checkpoint schema/source/count mismatch");quint64 n=0;
     while(r.valid){checkpointCheck(r.row<sourcePoints,"Checkpoint ID outside source cloud");r.advance();if(++n%32768==0&&tick)tick();}}
    QFile f(path);QCryptographicHash hash(QCryptographicHash::Sha256);checkpointCheck(f.open(QIODevice::ReadOnly),"Cannot hash checkpoint");
    while(!f.atEnd()){hash.addData(f.read(1048576));checkpointCheck(f.error()==QFile::NoError,"Cannot read checkpoint for hashing");if(tick)tick();}
    return QString::fromLatin1(hash.result().toHex());
}
// At most four sorted inputs are merged at a time by the adaptive quadtree.
inline void mergeFeatureCheckpoints(const QStringList& inputs,const QString& output,const QJsonObject& metadata,quint32 count,quint32 columns,const std::function<void()>& tick={}){
    checkpointCheck(inputs.size()>0&&inputs.size()<=4,"Invalid adaptive merge fan-in");
    std::vector<std::unique_ptr<FeatureCheckpointReader>> readers;
    for(const auto& path:inputs){readers.emplace_back(new FeatureCheckpointReader(path));checkpointCheck(readers.back()->metadata==metadata&&readers.back()->columns==columns,"Incompatible adaptive fragment");}
    FeatureCheckpointWriter out(output,metadata,count,columns);quint64 n=0;
    for(;;){int chosen=-1;for(int i=0;i<int(readers.size());++i)if(readers[i]->valid&&(chosen<0||readers[i]->row<readers[chosen]->row))chosen=i;if(chosen<0)break;
        auto& r=*readers[chosen];out.append(r.row,r.record.constData()+4);r.advance();if(++n%32768==0&&tick)tick();}
    out.finish();
}
}
