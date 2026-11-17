// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QStorageInfo>
#include <functional>
#include <stdexcept>
namespace alis {
inline void writeDiskFeatureSnapshot(ccPointCloud& cloud,const QString& folder,const QString& coordinateHash,
                                    const std::function<void(quint64,quint64)>& progress={}) {
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    QJsonArray columns;
    for(unsigned f=0;f<cloud.getNumberOfScalarFields();++f)columns.append(QJsonObject{{"name",QString::fromUtf8(cloud.getScalarField(f)->getName())},{"kind","sf"}});
    if(cloud.hasColors())for(const auto* name:{"red","green","blue"})columns.append(QJsonObject{{"name",name},{"kind","rgb"}});
    if(cloud.hasNormals())for(const auto* name:{"normal_x","normal_y","normal_z"})columns.append(QJsonObject{{"name",name},{"kind","normal"}});
    const quint64 rowBytes=24+4*columns.size(),bytes=quint64(cloud.size())*rowBytes;
    require(quint64(QStorageInfo(folder).bytesAvailable())>bytes+536870912,"Insufficient disk space for the bounded export snapshot");
    QSaveFile file(QDir(folder).filePath("export_snapshot.bin"));require(file.open(QIODevice::WriteOnly),"Cannot open export snapshot");
    QCryptographicHash hash(QCryptographicHash::Sha256);CCVector3d lo,hi;require(cloud.getOwnGlobalBB(lo,hi),"Missing cloud bounds");
    for(unsigned first=0;first<cloud.size();) {
        const unsigned n=std::min(16384u,cloud.size()-first);QByteArray buffer;buffer.reserve(int(n*rowBytes));
        for(unsigned i=first;i<first+n;++i){const auto p=cloud.toGlobal3d(*cloud.getPoint(i));const double xyz[]={p.x,p.y,p.z};buffer.append(reinterpret_cast<const char*>(xyz),24);hash.addData(reinterpret_cast<const char*>(xyz),24);
            for(unsigned f=0;f<cloud.getNumberOfScalarFields();++f){const float v=cloud.getScalarField(f)->getValue(i);buffer.append(reinterpret_cast<const char*>(&v),4);}
            if(cloud.hasColors()){const auto c=cloud.getPointColor(i);for(float v:{float(c.r),float(c.g),float(c.b)})buffer.append(reinterpret_cast<const char*>(&v),4);}
            if(cloud.hasNormals()){const auto normal=cloud.getPointNormal(i);for(float v:{normal.x,normal.y,normal.z})buffer.append(reinterpret_cast<const char*>(&v),4);}
        }
        require(file.write(buffer)==buffer.size(),"Export snapshot write failed; prior snapshot retained");first+=n;if(progress)progress(first,cloud.size());
    }
    require(QString::fromLatin1(hash.result().toHex())==coordinateHash,"Cloud geometry/order changed: rebuild the grid before exporting");
    require(file.commit(),"Cannot commit export snapshot");
    QJsonObject meta{{"schema","alis-disk-feature-snapshot/1"},{"point_count",double(cloud.size())},{"coordinate_sha256",coordinateHash},{"columns",columns},{"mins",QJsonArray{lo.x,lo.y,lo.z}},{"maxs",QJsonArray{hi.x,hi.y,hi.z}},{"cloud_name",cloud.getName()}};
    // Keep CC metadata separately; do not invent a CRS or restore absent LAS fields.
    meta.insert("cc_metadata",QJsonObject::fromVariantMap(cloud.metaData()));
    QSaveFile json(QDir(folder).filePath("export_snapshot.json"));const auto data=QJsonDocument(meta).toJson();require(json.open(QIODevice::WriteOnly)&&json.write(data)==data.size()&&json.commit(),"Cannot commit snapshot metadata");
}
}
