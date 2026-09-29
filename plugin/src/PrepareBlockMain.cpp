// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated native worker. The GUI never allocates a full-cloud feature matrix.
#include "FeatureEngine.h"
#include "GroundFilter.h"
#include "TerrainEngine.h"
#include <ccPointCloud.h>
#include <GenericProgressCallback.h>
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <limits>
#include <QSaveFile>
#include <iostream>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <atomic>
using namespace alis;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct BlockProgress : CCCoreLib::GenericProgressCallback {
 std::atomic<int> bucket{-1};
 void update(float percent) override {int next=int(percent)/5,old=bucket.load();if(next>old&&bucket.compare_exchange_strong(old,next))std::cout<<"ALIS_PROGRESS "<<next*5<<std::endl;}
 void start() override {bucket=-1;} void stop() override {}
 void setMethodTitle(const char*) override {} void setInfo(const char*) override {}
 bool isCancelRequested() override {return false;} // The parent stops only this isolated child.
};
static const char* names[]={"neighbor_count","density_2d","density_3d","eigenvalue_1","eigenvalue_2","eigenvalue_3","eigenvalues_sum","pca_1","pca_2","linearity","planarity","sphericity","anisotropy","omnivariance","eigenentropy","surface_variation","verticality","normal_x","normal_y","normal_z","normal_z_absolute","dip","dip_direction","roughness","signed_roughness","mean_curvature","gaussian_curvature","normal_change_rate","moment_order_1","barycenter_offset_ratio","z_minimum","z_maximum","z_range","z_mean","z_standard_deviation","z_above_minimum","z_below_maximum","z_relative_to_mean","z_percentile_10","z_percentile_25","z_median","z_percentile_75","z_percentile_90"};
int main(int argc,char**argv){
 // qCSF creates a QProgressDialog internally; use Qt's non-interactive minimal
 // backend in this isolated process, never CloudCompare's GUI or a popup.
 qputenv("QT_QPA_PLATFORM", "minimal");
 QApplication app(argc,argv);
 try{
  check(argc==2,"Usage: ALiS_prepare_block <request.json>"); QFile jf(QString::fromLocal8Bit(argv[1]));check(jf.open(QIODevice::ReadOnly),"Cannot read request");
  const auto j=QJsonDocument::fromJson(jf.readAll()).object(); QDir dir(j.value("directory").toString());
  const auto n=j.value("point_count").toVariant().toULongLong();check(n>0&&n<=std::numeric_limits<unsigned>::max(),"Invalid point count");
  QFile xyz(dir.filePath("xyz.f64"));check(xyz.open(QIODevice::ReadOnly)&&quint64(xyz.size())==n*24,"Invalid XYZ file");
  ccPointCloud cloud("ALiS bounded block");check(cloud.reserve(unsigned(n)),"XYZ allocation failed");
  auto origin=j.value("origin").toArray();check(origin.size()==3,"Invalid origin");
  cloud.setGlobalShift(CCVector3d(-origin[0].toDouble(),-origin[1].toDouble(),-origin[2].toDouble()));
  while(!xyz.atEnd()){auto bytes=xyz.read(65536*24);check(bytes.size()%24==0,"Invalid coordinate record");for(int k=0;k<bytes.size();k+=24){double p[3];std::memcpy(p,bytes.constData()+k,24);for(int a=0;a<3;++a)check(std::isfinite(p[a]),"Nonfinite coordinate");cloud.addPoint(CCVector3(p[0]-origin[0].toDouble(),p[1]-origin[1].toDouble(),p[2]-origin[2].toDouble()));}}
  const auto keys=j.value("features").toArray(); const auto supplied=j.value("supplied").toObject();
  FeatureEngine engine(&cloud); engine.setCacheByteLimit(size_t(j.value("cache_bytes").toDouble()));
  std::vector<FeatureRequest> req;std::vector<FeatureKey> parsed; bool needsHag=false;
  for(const auto& v:keys){QString key=v.toString();FeatureKey fk;
   if(!supplied.contains(key)&&key!="hag@0") {auto parts=key.split('@');check(parts.size()==2,"Unsupported feature key");int id=-1;for(int i=0;i<43;++i)if(parts[0]==names[i]){id=i;break;}check(id>=0,"Unknown geometric feature");bool ok=false;double r=parts[1].toDouble(&ok);check(ok&&std::isfinite(r)&&r>0,"Invalid radius");fk.feature=FeatureId(id);fk.radius=r;req.push_back({fk.feature,r,{}});}
   needsHag|=key=="hag@0"&&!supplied.contains(key);parsed.push_back(fk);
  }
  ComputeReport report;std::string error;ComputeOptions opts;opts.maxThreadCount=j.value("threads").toInt(4);
  BlockProgress progress;
  if(!req.empty())check(engine.compute(req,report,error,&progress,opts),error.c_str());
  TerrainResult terrain;
  if(needsHag){
   check(j.value("allow_tiled_hag").toBool(),"HAG requires explicit tiled-terrain approval");
   auto g=j.value("ground_recipe").toObject();auto t=j.value("terrain_recipe").toObject();
   check(g.contains("clothResolution")&&g.contains("classificationThreshold")&&t.contains("gridStep"),"Model lacks a complete terrain recipe");
   GroundFilterParameters gp;gp.clothResolution=g.value("clothResolution").toDouble();gp.classificationThreshold=g.value("classificationThreshold").toDouble();gp.rigidness=g.value("rigidness").toInt();gp.iterations=g.value("iterations").toInt();gp.timeStep=g.value("timeStep").toDouble();gp.smoothSlope=g.value("smoothSlope").toBool();
   CSFGroundFilter filter;auto ground=filter.run(cloud,gp,{});check(ground.succeeded(),ground.message.toUtf8().constData());
   TerrainParameters tp;tp.gridStep=t.value("gridStep").toDouble();tp.interpolateEmptyCells=t.value("interpolateEmptyCells").toBool();tp.maximumInterpolationEdgeLength=t.value("maximumInterpolationEdgeLength").toDouble();
   TerrainEngine te;terrain=te.run(cloud,ground.isGround,tp);check(terrain.succeeded(),terrain.message.toUtf8().constData());
  }
  // Supplied columns are bounded by the same block memory estimate.
  std::vector<QByteArray> raw;raw.resize(keys.size());std::vector<const std::vector<ScalarType>*> columns(keys.size(),nullptr);
  for(int f=0;f<keys.size();++f){auto key=keys[f].toString();if(supplied.contains(key)){QFile file(dir.filePath(supplied.value(key).toString()));check(file.open(QIODevice::ReadOnly)&&quint64(file.size())==n*4,"Invalid supplied feature size");raw[f]=file.readAll();}else if(key!="hag@0")columns[f]=engine.cachedValues(parsed[f]);}
  QSaveFile out(dir.filePath("features.f32"));check(out.open(QIODevice::WriteOnly),"Cannot write features");std::vector<float> buffer;buffer.reserve(8192*keys.size());
  for(unsigned i=0;i<n;++i){for(int f=0;f<keys.size();++f){float v;if(!raw[f].isEmpty())std::memcpy(&v,raw[f].constData()+size_t(i)*4,4);else if(keys[f].toString()=="hag@0")v=float(terrain.heightAboveGround[i]);else {check(columns[f]!=nullptr,"Missing feature column");v=(*columns[f])[i];}buffer.push_back(v);}if((i+1)%8192==0||i+1==n){auto bytes=qint64(buffer.size()*4);check(out.write(reinterpret_cast<char*>(buffer.data()),bytes)==bytes,"Feature write failed");buffer.clear();}}
  check(out.commit(),"Cannot commit features");std::cout<<"READY "<<n<<" rows x "<<keys.size()<<" features"<<std::endl;return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}
}
