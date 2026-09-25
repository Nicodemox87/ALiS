// SPDX-License-Identifier: GPL-2.0-or-later
// Full real-cloud prediction -> native Session -> BIN -> reopen, not a UI mock.
#include "SessionModel.h"
#include <BinFilter.h>
#include <ccIOPluginInterface.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPluginLoader>
#include <QSaveFile>
#include <iostream>
#include <cmath>
#include <cstring>
#include <stdexcept>
using namespace alis;
void check(bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());}
QJsonObject readJson(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),path);return QJsonDocument::fromJson(f.readAll()).object();}
ccPointCloud* single(ccHObject& root){ccHObject::Container objects;root.filterChildren(objects,true,CC_TYPES::POINT_CLOUD);check(objects.size()==1,"one cloud");return static_cast<ccPointCloud*>(objects.front());}
template<class T> std::vector<T> readArray(const QString& path,unsigned n){QFile f(path);check(f.open(QIODevice::ReadOnly)&&f.size()==qint64(n)*sizeof(T),path);std::vector<T> v(n);check(f.read(reinterpret_cast<char*>(v.data()),f.size())==qint64(n)*sizeof(T),"array read");return v;}
QString digest(const ccPointCloud& cloud,int sf=-1){QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray chunk;
 for(unsigned i=0;i<cloud.size();++i){if(sf<0){const auto*p=cloud.getPoint(i);chunk.append(reinterpret_cast<const char*>(p->u),3*sizeof(PointCoordinateType));}
 else{float v=cloud.getScalarField(sf)->getValue(i);if(std::isnan(v)){quint32 bits=0x7fc00000;chunk.append(reinterpret_cast<const char*>(&bits),4);}else chunk.append(reinterpret_cast<const char*>(&v),4);}
 if(chunk.size()>=1048576){hash.addData(chunk);chunk.clear();}}
 hash.addData(chunk);return QString::fromLatin1(hash.result().toHex());}
QJsonObject snapshot(const ccPointCloud& cloud){QJsonArray fields;
 for(unsigned f=0;f<cloud.getNumberOfScalarFields();++f)fields.append(QJsonObject{{"name",QString::fromUtf8(cloud.getScalarField(f)->getName())},{"sha256",digest(cloud,int(f))}});
 auto shift=cloud.getGlobalShift();return {{"points",double(cloud.size())},{"geometry",digest(cloud)},{"fields",fields},
 {"shift",QJsonArray{shift.x,shift.y,shift.z}},{"scale",cloud.getGlobalScale()},
 {"session",QString::fromUtf8(cloud.getMetaData("ALiS.Session.v1").toByteArray())},
 {"features",cloud.getMetaData("ALiS.featureFields").toString()}};}
int main(int argc,char**argv){QApplication app(argc,argv);try{
 auto args=app.arguments();check(args.size()==5,"Usage: roundtrip dataset prediction-bundle output-directory LAS-plugin");
 QDir dataset(args[1]),bundle(args[2]),output(args[3]);check(QDir().mkpath(output.path()),"output directory");
 QString binPath=output.filePath("classified_with_features.bin");check(!QFileInfo::exists(binPath),"Refusing to overwrite BIN");
 auto manifest=readJson(dataset.filePath("manifest.json")),input=readJson(dataset.filePath("input.json"));
 auto predictionMeta=readJson(bundle.filePath("result.json"));unsigned n=unsigned(manifest.value("rows").toDouble());
 check(predictionMeta.value("point_count").toDouble()==n,"prediction alignment");
 QPluginLoader loader(args[4]);auto* plugin=qobject_cast<ccIOPluginInterface*>(loader.instance());check(plugin,loader.errorString());
 FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
 auto shift=input.value("global_shift").toArray();CCVector3d globalShift(shift[0].toDouble(),shift[1].toDouble(),shift[2].toDouble());bool enabled=true;
 lp._coordinatesShift=&globalShift;lp._coordinatesShiftEnabled=&enabled;
 ccHObject root;check(plugin->getFilters().front()->loadFile(dataset.filePath("source_training.las"),root,lp)==CC_FERR_NO_ERROR,"load certified source");
 auto* cloud=single(root);check(cloud->size()==n,"all original points loaded");QString error;ALiSSession session(*cloud);check(session.initialize(error),error);
 const auto originalIndex=cloud->getScalarFieldIndexByName(field::OriginalClassification);const QString originalDigest=digest(*cloud,originalIndex);
 auto names=manifest.value("feature_names").toArray();QVector<ccScalarField*> fields;QJsonObject index;
 for(const auto& key:names){QString name=key.toString()=="hag@0"?QString::fromUtf8(field::HeightAboveGround):QStringLiteral("qAL_%1").arg(key.toString());
 int sf=cloud->getScalarFieldIndexByName(name.toUtf8().constData());if(sf<0)sf=cloud->addScalarField(name.toUtf8().constData());check(sf>=0,"allocate feature SF");
 fields<<static_cast<ccScalarField*>(cloud->getScalarField(sf));index.insert(key.toString(),name);}
 QFile featureFile(dataset.filePath("features.f32"));const qint64 width=names.size()*4;check(featureFile.open(QIODevice::ReadOnly)&&featureFile.size()==n*width,"feature matrix shape");
 unsigned row=0;while(!featureFile.atEnd()){auto bytes=featureFile.read(width*8192);check(bytes.size()%width==0,"aligned feature rows");
 for(qint64 at=0;at<bytes.size();at+=width,++row)for(int f=0;f<fields.size();++f){float value;std::memcpy(&value,bytes.constData()+at+4*f,4);fields[f]->setValue(row,value);}}
 for(auto* f:fields)f->computeMinAndMax();cloud->setMetaData("ALiS.featureFields",QString::fromUtf8(QJsonDocument(index).toJson(QJsonDocument::Compact)));
 cloud->setMetaData("ALiS.Validation.FeatureManifest",QJsonDocument(manifest).toJson(QJsonDocument::Compact));
 auto prediction=readArray<std::int16_t>(bundle.filePath("predictions.i16"),n);auto confidence=readArray<float>(bundle.filePath("confidence.f32"),n);
 check(session.setAsprsPredictions(prediction,confidence,predictionMeta.value("provenance").toObject(),0,error),error);
 check(session.applyAsprsPredictions(0,error),error);check(digest(*cloud,originalIndex)==originalDigest,"original labels preserved after Apply");
 auto working=cloud->getScalarField(cloud->getScalarFieldIndexByName(field::WorkingAsprs));for(unsigned i=0;i<n;++i)check(working->getValue(i)==prediction[i],"Working equals full prediction");
 check(session.displayScalarField(QString::fromUtf8(field::WorkingAsprs),error),error);
 cloud->setName("ALiS validated model result - full cloud (Original labels retained)");
 const auto before=snapshot(*cloud);BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;
 check(bin.saveToFile(cloud,binPath,sp)==CC_FERR_NO_ERROR,"save native BIN");std::cout<<"Saved full BIN; reopening"<<std::endl;
 ccHObject reopenedRoot;check(bin.loadFile(binPath,reopenedRoot,lp)==CC_FERR_NO_ERROR,"reopen native BIN");auto* reopened=single(reopenedRoot);
 check(snapshot(*reopened)==before,"BIN preserves geometry, all SFs, features and session metadata");ALiSSession restored(*reopened);check(restored.initialize(error),error);
 check(digest(*reopened,reopened->getScalarFieldIndexByName(field::WorkingAsprs))==digest(*cloud,cloud->getScalarFieldIndexByName(field::WorkingAsprs)),"Session reopens classified Working");
 auto report=QJsonObject{{"schema","alis-classifier-native-roundtrip/1"},{"points",double(n)},{"feature_fields",fields.size()},
 {"bin",binPath},{"original_labels_preserved",true},{"working_equals_prediction",true},{"all_point_geometry_and_scalar_fields_verified",true},
 {"session_reopened",true},{"snapshot",before}};
 QSaveFile json(output.filePath("native_roundtrip.json"));auto bytes=QJsonDocument(report).toJson();check(json.open(QIODevice::WriteOnly)&&json.write(bytes)==bytes.size()&&json.commit(),"roundtrip report");
 std::cout<<"PASS: "<<n<<" points, "<<fields.size()<<" features, prediction, confidence, Apply, save and reopen"<<std::endl;return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<std::endl;return 1;}}
