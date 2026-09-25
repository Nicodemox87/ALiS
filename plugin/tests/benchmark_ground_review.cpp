// SPDX-License-Identifier: GPL-2.0-or-later
// Full-resolution comparison. Reference labels are used ONLY for evaluation.
#include "GroundFilter.h"
#include <CSF.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace alis;
namespace {
void require(bool ok,const QString& why){if(!ok)throw std::runtime_error(why.toStdString());}
void save(const QString& path,const QByteArray& bytes){QSaveFile file(path);require(file.open(QIODevice::WriteOnly),path);require(file.write(bytes)==bytes.size()&&file.commit(),path);}
struct Cell { double groundMin=1e99,vegetationMax=-1e99; };
}
int benchmarkGround(const QString& dataset,const QString& output)
{
    QDir src(dataset),dst(output);
    require(!QFileInfo::exists(dst.filePath("report.json")),"Refusing to overwrite an existing benchmark");
    require(QDir().mkpath(output),"Create benchmark directory");
    QFile meta(src.filePath("input.json"));require(meta.open(QIODevice::ReadOnly),"Read input.json");
    auto input=QJsonDocument::fromJson(meta.readAll()).object();
    require(input["schema"].toString()=="qal-native-feature-input/1.0","Input schema");
    unsigned count=input["point_count"].toInt();require(count>0&&count<(1u<<24),"Row IDs must fit exactly in scalar float");
    require(input["global_scale"].toDouble()==1.,"Benchmark uses metric, unscaled coordinates");
    ccPointCloud cloud("Full resolution ground benchmark");require(cloud.reserve(count),"Reserve points");
    auto shift=input["global_shift"].toArray();require(shift.size()==3,"Global shift");
    cloud.setGlobalShift(CCVector3d(shift[0].toDouble(),shift[1].toDouble(),shift[2].toDouble()));
    QFile xyz(src.filePath("xyz_local.f32"));require(xyz.open(QIODevice::ReadOnly)&&xyz.size()==qint64(count)*12,"Full XYZ input");
    while(!xyz.atEnd()){auto bytes=xyz.read(12*65536);require(bytes.size()%12==0,"Aligned XYZ");for(int o=0;o<bytes.size();o+=12){float p[3];std::memcpy(p,bytes.constData()+o,12);require(std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2]),"Finite XYZ");cloud.addPoint(CCVector3(p[0],p[1],p[2]));}}
    require(cloud.size()==count,"No subsampling");
    QFile labels(src.filePath("labels.u8"));require(labels.open(QIODevice::ReadOnly)&&labels.size()==count,"Reference labels");const auto truth=labels.readAll();
    auto cellKey=[](const CCVector3& p){return std::make_pair(int(std::floor(p.x/2.)),int(std::floor(p.y/2.)));};
    std::map<std::pair<int,int>,Cell> cells;
    for(unsigned i=0;i<count;++i){auto p=cloud.getPoint(i);auto& cell=cells[cellKey(*p)];if(truth[i]==2)cell.groundMin=std::min(cell.groundMin,double(p->z));if(truth[i]==4)cell.vegetationMax=std::max(cell.vegetationMax,double(p->z));}
    std::vector<bool> covered(count,false);for(unsigned i=0;i<count;++i)if(truth[i]==2){const auto& c=cells.at(cellKey(*cloud.getPoint(i)));covered[i]=c.vegetationMax-c.groundMin>1.;}
    QJsonArray runs;QByteArray csv("run,ground_points,precision,recall,f1,building_false_ground,vegetation_false_ground,covered_ground_recall,elapsed_ms\n");
    auto record=[&](const QString& name,const GroundFilterResult& r){
        require(r.succeeded()&&r.isGround.size()==count,r.message);
        quint64 tp=0,fp=0,fn=0,building=0,vegetation=0,coveredTotal=0,coveredFound=0;
        QByteArray mask(int(count),char(0));
        for(unsigned i=0;i<count;++i){bool g=r.isGround[i],t=truth[i]==2;mask[i]=g?1:0;if(g&&t)++tp;if(g&&!t)++fp;if(!g&&t)++fn;if(g&&truth[i]==6)++building;if(g&&truth[i]==4)++vegetation;if(covered[i]){++coveredTotal;if(g)++coveredFound;}}
        double precision=double(tp)/std::max<quint64>(1,tp+fp),recall=double(tp)/std::max<quint64>(1,tp+fn),f1=2*precision*recall/std::max(1e-20,precision+recall),cover=double(coveredFound)/std::max<quint64>(1,coveredTotal);
        runs.append(QJsonObject{{"name",name},{"parameters",r.provenance.effectiveParameters},{"ground_points",double(r.groundPointCount)},{"precision",precision},{"recall",recall},{"f1",f1},{"building_false_ground",double(building)},{"vegetation_false_ground",double(vegetation)},{"covered_ground_count",double(coveredTotal)},{"covered_ground_recall",cover},{"elapsed_ms",double(r.provenance.elapsedMilliseconds)}});
        csv+=QString("%1,%2,%3,%4,%5,%6,%7,%8,%9\n").arg(name).arg(r.groundPointCount).arg(precision,0,'g',12).arg(recall,0,'g',12).arg(f1,0,'g',12).arg(building).arg(vegetation).arg(cover,0,'g',12).arg(r.provenance.elapsedMilliseconds).toUtf8();
        save(dst.filePath(name+".ground.u8"),mask);save(dst.filePath("metrics.csv"),csv);
        std::cout<<name.toStdString()<<" precision="<<precision<<" recall="<<recall<<" f1="<<f1<<std::endl;
    };
    CSFGroundFilter csf;GroundFilterParameters params;params.clothResolution=2.;params.classificationThreshold=.5;params.rigidness=2;params.smoothSlope=false;
    auto reference=csf.run(cloud,params,{});record("csf_cc_reference",reference);
    int row=cloud.addScalarField("benchmark_row_id");require(row>=0,"Allocate exact row IDs");for(unsigned i=0;i<count;++i)cloud.getScalarField(row)->setValue(i,float(i));
    CSF::Parameters native;native.cloth_resolution=2.;native.class_threshold=.5;native.rigidness=2;native.smoothSlope=false;native.iterations=500;native.time_step=.65;
    ccPointCloud *ground=nullptr,*off=nullptr;ccMesh* mesh=nullptr;
    require(CSF::Apply(&cloud,native,ground,off,false,mesh,nullptr),"Native CloudCompare CSF overload");
    require(ground&&off&&ground->size()+off->size()==count,"Native output partition");
    std::vector<bool> nativeMask(count,false);auto* ids=ground->getScalarField(ground->getScalarFieldIndexByName("benchmark_row_id"));require(ids,"Native IDs retained");
    for(unsigned i=0;i<ground->size();++i){unsigned id=unsigned(ids->getValue(i));require(id<count,"Native row ID range");nativeMask[id]=true;}
    quint64 mismatches=0;for(unsigned i=0;i<count;++i)if(reference.isGround[i]!=nativeMask[i])++mismatches;
    delete ground;delete off;cloud.deleteScalarField(row);
    std::cout<<"Native CloudCompare mask mismatches: "<<mismatches<<std::endl;
    require(mismatches==0,"Adapter must match native CloudCompare point for point");
    params.clothResolution=.72594;params.classificationThreshold=.151237;record("csf_strict_073m_015m",csf.run(cloud,params,{}));
    params.classificationThreshold=.5;record("csf_same_grid_tolerance_050",csf.run(cloud,params,{}));
    params.clothResolution=1.;record("csf_new_archaeology",csf.run(cloud,params,{}));
    params.rigidness=1;params.smoothSlope=true;record("csf_new_hilly",csf.run(cloud,params,{}));
    PMFGroundFilter pmf;params.pmfWindowSizes={1,2,4,8};params.pmfThresholds={.08,.15,.3,.6};params.pmfCellSize=0;
    record("pmf_previous_strict",pmf.run(cloud,params,{}));
    params.pmfWindowSizes={3,6,12,20};params.pmfThresholds={.5,.8,1.4,2.};params.pmfCellSize=.5;
    record("pmf_new_balanced",pmf.run(cloud,params,{}));
    QJsonObject report{{"schema","qal-ground-review/1.0"},{"input",input},{"point_count",double(count)},{"native_cc_mask_mismatches",double(mismatches)},
        {"notes","Full resolution; reference labels used only for evaluation. Covered-ground stratum = class-2 points in 2m XY cells with class-4 points >1m above minimum ground; not proof of actual occlusion. Single site, descriptive comparison; not independent validation or preset optimization."},{"runs",runs}};
    save(dst.filePath("report.json"),QJsonDocument(report).toJson());return 0;
}
