// SPDX-License-Identifier: GPL-2.0-or-later
#include "FeatureRecovery.h"
#include <QApplication>
#include <QPluginLoader>
#include <BinFilter.h>
#include <iostream>
using namespace alis;
ccPointCloud* load(BinFilter& bin,const QString& path,ccHObject& root){
    FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    checkpointCheck(bin.loadFile(path,root,lp)==CC_FERR_NO_ERROR,"Load scene failed");ccHObject::Container clouds;root.filterChildren(clouds,true,CC_TYPES::POINT_CLOUD);checkpointCheck(clouds.size()==1,"Exactly one cloud expected");return static_cast<ccPointCloud*>(clouds.front());
}
bool same(float a,float b){return (std::isnan(a)&&std::isnan(b))||std::memcmp(&a,&b,4)==0;}
int main(int argc,char** argv){QApplication app(argc,argv);try{
    auto args=app.arguments();checkpointCheck(args.size()==4,"Usage: audit original.bin recovered.bin checkpoint_folder");
    QPluginLoader las("C:/Program Files/CloudCompare/plugins/QLAS_IO_PLUGIN.dll");checkpointCheck(las.instance()!=nullptr,"LAS metadata registration failed");BinFilter bin;ccHObject beforeRoot,afterRoot;
    std::cout<<"Reopening restored scene..."<<std::endl;auto* after=load(bin,args[2],afterRoot);
    std::cout<<"Reopening preserved original scene..."<<std::endl;auto* before=load(bin,args[1],beforeRoot);
    checkpointCheck(before->size()==after->size()&&before->getGlobalShift().x==after->getGlobalShift().x&&before->getGlobalShift().y==after->getGlobalShift().y&&before->getGlobalShift().z==after->getGlobalShift().z&&before->getGlobalScale()==after->getGlobalScale(),"Count/shift/scale changed");
    checkpointCheck(before->hasColors()==after->hasColors(),"Color presence changed");
    for(unsigned i=0;i<before->size();++i){const auto& a=*before->getPoint(i);const auto& b=*after->getPoint(i);checkpointCheck(a.x==b.x&&a.y==b.y&&a.z==b.z,"Coordinates/order changed");if(before->hasColors()){auto a=before->getPointColor(i),b=after->getPointColor(i);checkpointCheck(a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a,"Original colors changed");}}
    for(unsigned f=0;f<before->getNumberOfScalarFields();++f){auto* a=before->getScalarField(f);int index=after->getScalarFieldIndexByName(a->getName());checkpointCheck(index>=0,"Original field missing");auto* b=after->getScalarField(index);for(unsigned i=0;i<before->size();++i)checkpointCheck(same(a->getValue(i),b->getValue(i)),"Original scalar values changed");}
    checkpointCheck(before->getMetaData("ALiS.Session.v1")==after->getMetaData("ALiS.Session.v1"),"Original session / Ground / Working history changed");
    const auto state=ProgressiveFeatureStore::state(*after);const auto names=state.value("fields").toObject();const auto config=state.value("config").toObject();QStringList keys;QVector<int> indices;
    for(auto key:config.value("features").toArray()){keys<<key.toString();int index=after->getScalarFieldIndexByName(names.value(key.toString()).toString().toUtf8());checkpointCheck(index>=0,"Recovered SF missing after BIN reopen");indices<<index;}
    auto audit=inspectFeatureRecovery(*after,args[3],config.value("coordinate_sha256").toString());checkpointCheck(audit.value("covered").toDouble()==after->size(),"Missing recovered point ownership");
    for(auto entry:audit.value("validated").toArray()){
        FeatureCheckpointReader reader(entry.toObject().value("file").toString());while(reader.valid){for(int f=0;f<indices.size();++f){float expected;std::memcpy(&expected,reader.record.constData()+4+4*f,4);checkpointCheck(same(expected,after->getScalarField(indices[f])->getValue(reader.row)),"Scalar checkpoint mismatch after BIN reopen");}reader.advance();}
        std::cout<<"Verified "<<entry.toObject().value("id").toString().toStdString()<<std::endl;
    }
    QJsonObject result{{"passed",true},{"points",double(after->size())},{"new_scalar_fields",keys.size()},{"original_scalar_fields",int(before->getNumberOfScalarFields())},{"geometry_order_rgb_original_fields_session_preserved",true},{"all_checkpoint_values_compared_after_bin_reopen",true},{"scene",args[2]},{"field_names",names}};
    QSaveFile report(args[2]+".verified.json");auto data=QJsonDocument(result).toJson();checkpointCheck(report.open(QIODevice::WriteOnly)&&report.write(data)==data.size()&&report.commit(),"Cannot save verification report");
    std::cout<<"REOPEN AUDIT PASSED"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
