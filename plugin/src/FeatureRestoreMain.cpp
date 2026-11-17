// SPDX-License-Identifier: GPL-2.0-or-later
#include "FeatureRecovery.h"
#include <QApplication>
#include <BinFilter.h>
#include <QTemporaryDir>
#include <QPluginLoader>
#include <ccLog.h>
#include <iostream>

class RecoveryLog : public ccLog { public: void logMessage(const QString& message,int) override { std::cerr<<message.toStdString()<<std::endl; } };

QString coordinateFingerprint(const ccPointCloud& cloud){
    QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray buffer;buffer.reserve(65536*24);
    for(unsigned i=0;i<cloud.size();++i){const auto p=cloud.toGlobal3d(*cloud.getPoint(i));const double xyz[]={p.x,p.y,p.z};buffer.append(reinterpret_cast<const char*>(xyz),24);if(buffer.size()>=65536*24){hash.addData(buffer);buffer.clear();}}
    hash.addData(buffer);return QString::fromLatin1(hash.result().toHex());
}
int main(int argc,char** argv){
    QApplication app(argc,argv);
    RecoveryLog logger;ccLog::RegisterInstance(&logger);
    try{
        // LAS VLR metadata in a BIN requires the LAS plugin's QVariant stream registrations.
        QPluginLoader las(qEnvironmentVariable("ALIS_LAS_PLUGIN","C:/Program Files/CloudCompare/plugins/QLAS_IO_PLUGIN.dll"));
        alis::checkpointCheck(las.instance()!=nullptr,las.errorString().toUtf8().constData());
        const auto args=app.arguments();if(args.size()!=4&&args.size()!=5)throw std::runtime_error("Usage: restore source.bin checkpoints output.bin [--verify-only]");
        ccHObject loaded;BinFilter bin;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
        std::cout<<"Loading saved scene..."<<std::endl;
        alis::checkpointCheck(bin.loadFile(args[1],loaded,lp)==CC_FERR_NO_ERROR,"Cannot load saved BIN scene");
        ccHObject::Container clouds;loaded.filterChildren(clouds,true,CC_TYPES::POINT_CLOUD);alis::checkpointCheck(clouds.size()==1,"Saved scene must contain exactly one point cloud");auto* cloud=static_cast<ccPointCloud*>(clouds.front());
        const auto coordinates=coordinateFingerprint(*cloud);std::cout<<"points="<<cloud->size()<<" coordinate_sha256="<<coordinates.toStdString()<<std::endl;
        const auto audit=alis::inspectFeatureRecovery(*cloud,args[2],coordinates);const auto manifest=audit.value("manifest").toObject();const auto config=manifest.value("config").toObject();QStringList keys;for(auto key:config.value("features").toArray())keys<<key.toString();
        std::cout<<"Validated "<<audit.value("validated").toArray().size()<<" cores; "<<keys.size()<<" feature columns"<<std::endl;
        if(args.size()==5){alis::checkpointCheck(args[4]=="--verify-only","Unknown option");return 0;}
        alis::checkpointCheck(!QFileInfo::exists(args[3]),"Output already exists; no overwrite");
        const quint64 bytes=quint64(cloud->size())*keys.size()*sizeof(ScalarType);alis::checkpointCheck(alis::availableRamBytes()>bytes+quint64(3)*1073741824,"Insufficient safe RAM to recover scalar fields");
        const auto originalFields=cloud->getNumberOfScalarFields();std::vector<CCCoreLib::ScalarField*> originalPointers;for(unsigned i=0;i<originalFields;++i)originalPointers.push_back(cloud->getScalarField(i));
        alis::ProgressiveFeatureStore store(cloud,manifest.value("identity").toString(),keys,config);store.setOutputFolder(args[2]);
        for(auto value:audit.value("validated").toArray()){const auto block=value.toObject();const auto id=block.value("id").toString();if(!store.completed().contains(id))store.commitFile(id,block.value("file").toString(),quint64(block.value("rows").toDouble()),keys,{},true);std::cout<<"Recovered "<<id.toStdString()<<std::endl;}
        store.finalizeRanges();store.setSelection(manifest.value("progress").toObject().value("selection").toArray());
        const auto names=store.metadata().value("fields").toObject();QVector<int> indices;for(auto key:keys)indices<<cloud->getScalarFieldIndexByName(names.value(key).toString().toUtf8().constData());
        for(auto value:audit.value("validated").toArray()){alis::FeatureCheckpointReader reader(value.toObject().value("file").toString());while(reader.valid){for(int j=0;j<indices.size();++j){float expected;std::memcpy(&expected,reader.record.constData()+4+j*4,4);const float actual=cloud->getScalarField(indices[j])->getValue(reader.row);alis::checkpointCheck((std::isnan(expected)&&std::isnan(actual))||std::memcmp(&expected,&actual,4)==0,"Recovered scalar payload differs from checkpoint");}reader.advance();}}
        for(unsigned i=0;i<originalFields;++i)alis::checkpointCheck(originalPointers[i]==cloud->getScalarField(i),"Original scalar fields changed");
        alis::checkpointCheck(coordinateFingerprint(*cloud)==coordinates,"Geometry/order changed during recovery");
        cloud->setCurrentDisplayedScalarField(indices[0]);cloud->showSF(true);
        QTemporaryDir stage(QFileInfo(args[3]).absolutePath()+"/.alis_recover_XXXXXX");alis::checkpointCheck(stage.isValid(),"Cannot create save staging");FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;
        const auto staged=stage.filePath("recovered.bin");std::cout<<"Saving verified scalar fields into BIN..."<<std::endl;
        alis::checkpointCheck(bin.saveToFile(&loaded,staged,sp)==CC_FERR_NO_ERROR,"Failed to save recovered BIN");alis::checkpointCheck(QFile::rename(staged,args[3]),"Cannot commit recovered BIN");
        QJsonObject result{{"complete",true},{"points",double(cloud->size())},{"features",keys.size()},{"cores",audit.value("validated").toArray().size()},{"covered",audit.value("covered")},{"coordinate_sha256",coordinates},{"original_fields_preserved",true},{"all_checkpoint_values_compared",true},{"no_feature_recalculation",true},{"output",args[3]},{"source",args[1]},{"field_names",names}};
        QSaveFile report(args[3]+".recovery.json");const auto json=QJsonDocument(result).toJson();alis::checkpointCheck(report.open(QIODevice::WriteOnly)&&report.write(json)==json.size()&&report.commit(),"Cannot save recovery report");
        std::cout<<"RECOVERY PASSED"<<std::endl;return 0;
    }catch(const std::exception& error){std::cerr<<"RECOVERY FAILED: "<<error.what()<<std::endl;return 1;}
}
