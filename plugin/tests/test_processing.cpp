// SPDX-License-Identifier: GPL-2.0-or-later
#include "ProcessingPreset.h"
#include "CloudProfile.h"
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "ModelCatalog.h"
#include "MlInputFields.h"
#include "VegetationRules.h"
#include "FeatureRangeRules.h"
#include "VegetationScoreIdentity.h"
#include <QLineEdit>
#include <QProcess>
#include <QEventLoop>
#include <QTimer>
#include <QProgressDialog>
#include <ccIOPluginInterface.h>
#include <QElapsedTimer>
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <ccStdPluginInterface.h>
#include <BinFilter.h>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QFile>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QDir>
#include <QFontDatabase>
#include <QPluginLoader>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <cstring>
using namespace alis;
int benchmarkGround(const QString&,const QString&);
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void vegetationRulesTests(){
    using namespace alis::vegetation;
    Observation plane;plane.count=30;plane.planarity=.85;plane.scattering=.01;plane.variation=.005;plane.roughnessRatio=.005;
    Observation volume;volume.count=30;volume.planarity=.1;volume.scattering=.5;volume.variation=.15;volume.roughnessRatio=.2;
    Thresholds t;std::array<Observation,3> leaves{{volume,volume,volume}},wall{{plane,plane,volume}};
    check(decide(leaves,t,true,2,3,true).category==Ground,"ground protected regardless of canopy geometry");
    check(decide(leaves,t,false,1,3,false).category==Vegetation,"multiscale volume vegetation proposal");
    check(decide(wall,t,false,1,1,true).category==OtherSurface,"wall under canopy retained even with multiple returns");
    wall[1]=volume;check(decide(wall,t,false,1,1,true).category==Uncertain,"fine planar surface vetoes coarse canopy");
    check(decide(leaves,t,false,1,.1,true).category==Uncertain,"near-ground archaeological protection");
    check(decide(leaves,t,false,1,NAN,true).category==Vegetation,"binary geometry separation does not require HAG");
    check(decide(leaves,t,false,1,-1,true).category==Uncertain,"negative HAG retained as terrain inconsistency");
    check(decide(leaves,t,false,7,3,true).category==Excluded,"existing noise not reclassified as vegetation");
    for(auto& o:leaves)o.count=5;
    check(decide(leaves,t,false,1,3,true).category==Uncertain,"sparse unsupported points retained");
    auto a=radii(.02,1000,1),b=radii(.2,10,1);
    check(a[0]<b[0]&&a[0]<a[1]&&a[1]<a[2]&&b[2]<=1,"radii adapt to density and cap");
    auto fallback=adapt({},0,12);check(std::isfinite(fallback.planar),"empty calibration bounded fallback");
    auto highSupport=radii(.02,1000,2,64,.5);check(highSupport[0]>a[0]&&highSupport[2]<=2,"explicit support target affects bounded scales");
    check(withinFeatureRange(.5,.5,1)&&withinFeatureRange(1,.5,1),"range boundaries inclusive");
    check(!withinFeatureRange(std::numeric_limits<double>::quiet_NaN(),0,1),"missing feature goes To validate");
    check(!withinFeatureRange(.5,1,0),"inverted range rejected");
}
int vegetationModelRegression(const QString& input,const QString& output,const QString& pluginPath,const QString& model,const QString& python,const QString& worker){
    QPluginLoader loader(pluginPath);auto* plugin=qobject_cast<ccIOPluginInterface*>(loader.instance());check(plugin!=nullptr,qPrintable(loader.errorString()));
    ccHObject root;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    check(plugin->getFilters().front()->loadFile(input,root,lp)==CC_FERR_NO_ERROR,"load full certified LAS for model pipeline");
    ccHObject::Container clouds;root.filterChildren(clouds,true,CC_TYPES::POINT_CLOUD);check(clouds.size()==1,"one certified cloud");auto* cloud=static_cast<ccPointCloud*>(clouds.front());
    WorkspaceDock dock;WorkspaceController controller(nullptr,&dock);controller.selectCloud(cloud);dock.metricUnitsConfirmationChanged(true);auto* s=controller.session();
    auto* working=cloud->getScalarField(cloud->getScalarFieldIndexByName(field::WorkingAsprs));std::vector<float> original(cloud->size());std::vector<bool> ground(cloud->size());
    for(unsigned i=0;i<cloud->size();++i){original[i]=working->getValue(i);ground[i]=original[i]==2;}
    QString error;check(s->setGroundPreview(ground,error)&&s->applyGroundPreview(error),"preserve certified Ground");
    int ticks=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&](){++ticks;});heartbeat.start(20);
    dock.vegetationModelRequested(model,output,python,worker,true);
    auto* process=controller.findChild<QProcess*>();check(process!=nullptr,"model process exists");
    if(process->state()!=QProcess::NotRunning){QEventLoop loop;QObject::connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),&loop,&QEventLoop::quit);loop.exec();}
    const int scoreIndex=cloud->getScalarFieldIndexByName("qAL_VegetationScore");const int reviewIndex=cloud->getScalarFieldIndexByName("qAL_VegetationReview");
    check(scoreIndex>=0&&reviewIndex>=0,"model score and review published through native pipeline");check(ticks>2,"GUI heartbeat during model preparation/inference");
    for(unsigned i=0;i<cloud->size();++i){check(working->getValue(i)==original[i],"model preserved Working");if(ground[i])check(cloud->getScalarField(reviewIndex)->getValue(i)==3,"model protected Ground");}
    dock.vegetationThresholdRequested(1.000001);const int updated=cloud->getScalarFieldIndexByName("qAL_VegetationReview");
    for(unsigned i=0;i<cloud->size();++i)check(cloud->getScalarField(updated)->getValue(i)!=1,"threshold >1 fully abstains without new worker");
    check(process->state()==QProcess::NotRunning,"threshold filter does not launch inference");
    dock.vegetationThresholdRequested(.9864730834960938);
    BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;
    check(bin.saveToFile(cloud,QDir(output).filePath("native_vegetation_model.bin"),sp)==CC_FERR_NO_ERROR,"save native model result");
    std::cout<<"Native vegetation model regression passed, points="<<cloud->size()<<", heartbeat="<<ticks<<std::endl;return 0;
}
int vegetationRegression(const QString& input,const QString& output,const QString& pluginPath){
    QPluginLoader loader(pluginPath);auto* plugin=qobject_cast<ccIOPluginInterface*>(loader.instance());
    check(plugin!=nullptr,qPrintable(loader.errorString()));auto filters=plugin->getFilters();
    ccHObject root;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    check(filters.front()->loadFile(input,root,lp)==CC_FERR_NO_ERROR,"load complete certified LAS");
    ccHObject::Container clouds;root.filterChildren(clouds,true,CC_TYPES::POINT_CLOUD);check(clouds.size()==1,"one certified cloud");auto* cloud=static_cast<ccPointCloud*>(clouds.front());
    std::cerr<<"[vegetation] Loaded "<<cloud->size()<<" points"<<std::endl;
    WorkspaceDock dock;WorkspaceController controller(nullptr,&dock);controller.selectCloud(cloud);dock.metricUnitsConfirmationChanged(true);
    auto* s=controller.session();check(s!=nullptr,"certified session initialized");
    auto* working=cloud->getScalarField(cloud->getScalarFieldIndexByName(field::WorkingAsprs));
    auto* trusted=cloud->getScalarField(cloud->getScalarFieldIndexByName(field::AsprsTrainingClass));
    // Only this explicit certified-data regression marks imported labels trusted.
    // Production preview never trusts LAS labels automatically.
    std::vector<float> original(cloud->size());for(unsigned i=0;i<cloud->size();++i){original[i]=working->getValue(i);trusted->setValue(i,original[i]);}
    std::vector<bool> groundMask(cloud->size());unsigned groundCount=0;
    for(unsigned i=0;i<cloud->size();++i){groundMask[i]=original[i]==2;groundCount+=groundMask[i];}
    check(groundCount==1969683,"certified Ground label histogram matches certification manifest");
    QString groundError;check(s->setGroundPreview(groundMask,groundError)&&s->applyGroundPreview(groundError),"accept existing certified Ground without running another filter");
    std::cerr<<"[vegetation] DTM / HAG using existing Ground"<<std::endl;
    dock.computeDtmRequested(.25,true,2.0);dock.computeHagRequested();
    int ticks=0;QTimer timer;QObject::connect(&timer,&QTimer::timeout,[&](){++ticks;});timer.start(10);
    check(QDir().mkpath(output),"vegetation output directory");
    for(int preset=0;preset<3;++preset){std::cerr<<"[vegetation] Preset "<<preset<<std::endl;
        dock.vegetationPreviewRequested(preset,1.,12,.5,2.,output);
        int sf=cloud->getScalarFieldIndexByName("qAL_VegetationReview");check(sf>=0,"vegetation output published");
        const auto report=QJsonDocument::fromJson(cloud->getMetaData("ALiS.vegetationPreview").toByteArray()).object();
        check(report["preset"].toInt(-1)==preset,"fresh preset report");
        for(unsigned i=0;i<cloud->size();++i){check(working->getValue(i)==original[i],"Working labels preserved");if(original[i]==2)check(cloud->getScalarField(sf)->getValue(i)==3,"all certified Ground protected");}
        std::cout<<QJsonDocument(report).toJson(QJsonDocument::Compact).constData()<<std::endl;
        if(preset==0){BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;check(bin.saveToFile(cloud,QDir(output).filePath("vegetation_preserve_structures.bin"),sp)==CC_FERR_NO_ERROR,"save full certified cloud with preview");}
    }
    timer.stop();check(ticks>0,"vegetation processing leaves GUI responsive");return 0;
}
int featureRegression(const QString& input,const QString& output,const QString& pluginPath){
    QPluginLoader loader(pluginPath);auto* plugin=qobject_cast<ccIOPluginInterface*>(loader.instance());
    check(plugin!=nullptr,qPrintable(loader.errorString()));auto filters=plugin->getFilters();check(!filters.empty(),"LAS filter available");
    ccHObject root;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    check(filters.front()->loadFile(input,root,lp)==CC_FERR_NO_ERROR,"load complete LAS");
    ccHObject::Container clouds;root.filterChildren(clouds,true,CC_TYPES::POINT_CLOUD);check(clouds.size()==1,"one cloud");auto* cloud=static_cast<ccPointCloud*>(clouds.front());
    WorkspaceDock dock;WorkspaceController controller(nullptr,&dock);controller.selectCloud(cloud);dock.metricUnitsConfirmationChanged(true);
    int ticks=0;QTimer timer;QObject::connect(&timer,&QTimer::timeout,[&](){++ticks;});timer.start(10);
    const unsigned before=cloud->getNumberOfScalarFields();QElapsedTimer elapsed;elapsed.start();
    dock.computeFeaturesRequested({"planarity","roughness","verticality"},{.1,.25,.5});
    const qint64 computeMs=elapsed.elapsed();timer.stop();
    check(cloud->getNumberOfScalarFields()==before+9,"all nine feature SF published for every LAS point");check(ticks>0,"GUI heartbeat during real-cloud calculation");
    elapsed.restart();dock.computeFeaturesRequested({"planarity","roughness","verticality"},{.1,.25,.5});const qint64 reusedMs=elapsed.elapsed();
    check(cloud->getNumberOfScalarFields()==before+9,"repeat computation reuses fields");
    check(QDir().mkpath(output),"output folder");BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;
    check(bin.saveToFile(cloud,QDir(output).filePath("feature_regression.bin"),sp)==CC_FERR_NO_ERROR,"save full cloud and computed SF");
    QJsonObject report{{"source",input},{"points",double(cloud->size())},{"subsampled",false},{"computed_fields",9},{"compute_ms",double(computeMs)},{"cached_ms",double(reusedMs)},{"gui_heartbeats",ticks}};
    QFile file(QDir(output).filePath("feature_regression.json"));check(file.open(QIODevice::WriteOnly),"report");file.write(QJsonDocument(report).toJson());std::cout<<QJsonDocument(report).toJson().constData()<<std::endl;return 0;
}
void featureRangeIntegrationTests(){
    ccPointCloud cloud("range test");check(cloud.reserve(5),"range points allocation");for(int i=0;i<5;++i)cloud.addPoint(CCVector3(float(i),0,0));
    int cls=cloud.addScalarField("Classification"),feature=cloud.addScalarField("test geometry");
    const float values[]={NAN,.25f,.5f,.75f,NAN};for(int i=0;i<5;++i){cloud.getScalarField(cls)->setValue(i,i==0?2.f:6.f);cloud.getScalarField(feature)->setValue(i,values[i]);}
    WorkspaceDock dock;WorkspaceController controller(nullptr,&dock);controller.selectCloud(&cloud);QString error;auto* s=controller.session();
    check(s->setGroundPreview({true,false,false,false,false},error)&&s->applyGroundPreview(error),"range ground setup");
    QTemporaryDir out;const QJsonArray rules{QJsonObject{{"field","test geometry"},{"minimum",.25},{"maximum",.5},{"enabled",true}}};
    check(controller.applyFeatureRanges(rules,out.path(),error),qPrintable(error));
    auto* review=cloud.getScalarField(cloud.getScalarFieldIndexByName("qAL_ReviewStatus"));
    check(review&&review->getValue(0)==0&&review->getValue(1)==0&&review->getValue(2)==0&&review->getValue(3)==1&&review->getValue(4)==1,"inclusive ranges, missing values and Ground protection on every point");
    check(QDir(out.path()).entryList({"*.json"},QDir::Files).size()==1,"range report saved");
    check(!controller.applyFeatureRanges({QJsonObject{{"field","missing"},{"minimum",0},{"maximum",1}}},out.path(),error)&&review->getValue(3)==1,"invalid range preserves previous review");
    int veg=cloud.addScalarField("qAL_VegetationReview");for(int i=0;i<5;++i)cloud.getScalarField(veg)->setValue(i,i==0?3.f:1.f);
    cloud.setMetaData("ALiS.vegetationPreview",QJsonDocument(QJsonObject{{"source_uid",QString::number(cloud.getUniqueID())},{"source_revision",QString::number(s->sourceRevision())}}).toJson());
    check(controller.refineVegetationWithRanges(out.path(),error),qPrintable(error));
    auto* refined=cloud.getScalarField(cloud.getScalarFieldIndexByName("qAL_VegetationReview"));
    check(refined->getValue(0)==3&&refined->getValue(1)==1&&refined->getValue(2)==1&&refined->getValue(3)==0&&refined->getValue(4)==0,"existing SF ranges refine vegetation while protecting Ground");
    cloud.getScalarField(feature)->setValue(2,.9f);
    check(controller.refineVegetationWithRanges(out.path(),error)&&refined->getValue(2)==0,"refinement reads edited SF values rather than cached mask");
    check(controller.applyFeatureRanges({},out.path(),error),"reset review");review=cloud.getScalarField(cloud.getScalarFieldIndexByName("qAL_ReviewStatus"));
    check(!controller.refineVegetationWithRanges(out.path(),error),"no silent refinement without enabled ranges");
    auto* working=cloud.getScalarField(cloud.getScalarFieldIndexByName(field::WorkingAsprs));
    for(int i=0;i<5;++i)check(review->getValue(i)==0&&working->getValue(i)==(i==0?2.f:6.f),"reset preserves ASPRS");
}
void portableScoreTests(){
    ccPointCloud cloud("portable scores");check(cloud.reserve(5),"score fixture points");
    for(int i=0;i<5;++i)cloud.addPoint(CCVector3(float(i),0,0));
    const int cls=cloud.addScalarField("Classification");for(int i=0;i<5;++i)cloud.getScalarField(cls)->setValue(i,i==0?2.f:6.f);
    WorkspaceDock dock;WorkspaceController controller(nullptr,&dock);controller.selectCloud(&cloud);QString error;
    auto* session=controller.session();check(session->setGroundPreview({true,false,false,false,false},error)&&session->applyGroundPreview(error),"score Ground setup");
    const int score=cloud.addScalarField("qAL_VegetationScore");
    const float values[]{NAN,.9f,.2f,.8f,NAN};for(int i=0;i<5;++i)cloud.getScalarField(score)->setValue(i,values[i]);
    QStringList fields{QString::fromUtf8(field::WorkingAsprs),"qAL_VegetationScore"};
    for(int k=0;k<26;++k){QString name=QStringLiteral("model_feature_%1").arg(k);fields<<name;
        int sf=cloud.addScalarField(name.toUtf8().constData());for(int i=0;i<5;++i)cloud.getScalarField(sf)->setValue(i,float(i+k));}
    const auto digest=vegetationScoreFingerprint(cloud,fields,error);check(digest.size()==64,"full score fingerprint");
    check(vegetationScoreFingerprint(cloud,fields,error,[](int){return false;}).isEmpty(),"fingerprint cancellation");
    QTemporaryDir directory;
    cloud.setMetaData("ALiS.vegetationScores",QJsonDocument(QJsonObject{
        {"source_uid",QString::number(cloud.getUniqueID())},{"source_revision",QString::number(session->sourceRevision())},
        {"output_directory","Z:/missing-original-computer"},{"model_threshold",.5},
        {"content_identity",QJsonObject{{"schema","alis-vegetation-score-content/1"},{"sha256",digest},{"fields",QJsonArray::fromStringList(fields)}}}
        }).toJson());
    BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;
    const auto path=directory.filePath("scores.bin");check(bin.saveToFile(&cloud,path,sp)==CC_FERR_NO_ERROR,"save portable score BIN");
    ccHObject root;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    check(bin.loadFile(path,root,lp)==CC_FERR_NO_ERROR,"reopen portable score BIN");ccHObject::Container objects;root.filterChildren(objects,true,CC_TYPES::POINT_CLOUD);
    check(objects.size()==1,"one score cloud");auto* loaded=static_cast<ccPointCloud*>(objects.front());
    check(loaded->getUniqueID()!=cloud.getUniqueID(),"reopen changes UID");
    check(vegetationScoreFingerprint(*loaded,fields,error)==digest,"BIN preserves exact content identity including NaNs");
    WorkspaceDock reopened;WorkspaceController other(nullptr,&reopened);other.selectCloud(loaded);
    reopened.findChild<QLineEdit*>("ALiS.Workspace.Terrain.Vegetation.Output")->setText(directory.path());
    reopened.vegetationThresholdRequested(.5);
    int reviewIndex=loaded->getScalarFieldIndexByName("qAL_VegetationReview");check(reviewIndex>=0,"threshold reuses scores after reopen without inference");
    auto* review=loaded->getScalarField(reviewIndex);check(review->getValue(0)==3&&review->getValue(1)==1&&review->getValue(2)==0&&review->getValue(4)==0,"Ground protected and unsupported retained");
    const auto before=loaded->getMetaData("ALiS.vegetationPreview").toByteArray();
    auto* input=loaded->getScalarField(loaded->getScalarFieldIndexByName("model_feature_0"));input->setValue(1,123.f);
    reopened.vegetationThresholdRequested(.99);check(before==loaded->getMetaData("ALiS.vegetationPreview").toByteArray(),"edited feature vetoes reuse and preserves review");input->setValue(1,1.f);
    auto* scores=loaded->getScalarField(loaded->getScalarFieldIndexByName("qAL_VegetationScore"));scores->setValue(1,.3f);
    check(vegetationScoreFingerprint(*loaded,fields,error)!=digest,"score edit invalidates identity");scores->setValue(1,.9f);
    auto* labels=loaded->getScalarField(loaded->getScalarFieldIndexByName(field::WorkingAsprs));const auto label=labels->getValue(1);labels->setValue(1,2.f);
    check(vegetationScoreFingerprint(*loaded,fields,error)!=digest,"label edit invalidates identity");labels->setValue(1,label);
    loaded->translate(CCVector3(1,0,0));check(vegetationScoreFingerprint(*loaded,fields,error)!=digest,"geometry edit invalidates identity");
    loaded->translate(CCVector3(-1,0,0));loaded->deleteScalarField(loaded->getScalarFieldIndexByName("model_feature_0"));
    check(vegetationScoreFingerprint(*loaded,fields,error).isEmpty(),"missing feature rejects identity");
}
void tests(){
    portableScoreTests();
	vegetationRulesTests();
	featureRangeIntegrationTests();
	std::cerr<<"[processing-tests] logic and first workspace"<<std::endl;
    ProcessingPreset p;p.name="Test recipe";p.base=groundPresetParameters(GroundPreset::CloudCompareReference,.01);p.features=QStringList{"planarity","roughness"};p.radii={1.5,2.5};
    ProcessingPreset restored;QString error;
    check(processingPresetFromJson(processingPresetToJson(p),restored,error),"preset roundtrip");
    check(restored.radii==p.radii&&restored.features==p.features&&restored.base.classificationThreshold==.5,"preset values retained");
    const QString profileDirectory=qEnvironmentVariable("QAL_TEST_PROFILE_DIRECTORY");
    if(!profileDirectory.isEmpty()){QDir dir(profileDirectory);auto files=dir.entryList({"ground_*.json"},QDir::Files);check(files.size()==8,"eight importable shipped presets");for(const auto& name:files){QFile json(dir.filePath(name));check(json.open(QIODevice::ReadOnly),"open shipped preset");ProcessingPreset shipped;check(processingPresetFromJson(QJsonDocument::fromJson(json.readAll()).object(),shipped,error),"validate shipped recipe");}}
    const QString pluginPath=qEnvironmentVariable("QAL_TEST_PLUGIN");if(!pluginPath.isEmpty()){QPluginLoader plugin(pluginPath);QObject* instance=plugin.instance();check(instance!=nullptr,qPrintable(plugin.errorString()));auto* standard=qobject_cast<ccStdPluginInterface*>(instance);auto actions=standard?standard->getActions():QList<QAction*>();check(actions.size()==3,"workspace, standalone Annotator and Settings actions exposed");check(!actions[0]->icon().isNull()&&!actions[1]->icon().isNull()&&!actions[2]->icon().isNull()&&actions[0]->icon().cacheKey()!=actions[1]->icon().cacheKey()&&actions[1]->icon().cacheKey()!=actions[2]->icon().cacheKey(),"coordinated but distinct action icons");check(plugin.unload(),"unload compiled plugin");}
    auto bad=processingPresetToJson(p);bad.insert("radiiMetres",QJsonArray{1,1});check(!processingPresetFromJson(bad,restored,error),"reject duplicate radii");
    bad=processingPresetToJson(p);bad.insert("schema","future");check(!processingPresetFromJson(bad,restored,error),"reject unknown schema");
    bad=processingPresetToJson(p);bad.insert("controls",QJsonArray{1.5,1,1});check(!processingPresetFromJson(bad,restored,error),"reject fractional levels");
    auto neutral=applySimpleGroundControls(p.base,SimpleLevel::Medium,SimpleLevel::Medium,SimpleLevel::Medium,.01);
    check(neutral.clothResolution==2&&neutral.classificationThreshold==.5,"neutral controls preserve native reference");
    auto vegetated=applySimpleGroundControls(p.base,SimpleLevel::High,SimpleLevel::High,SimpleLevel::High,.01);
    check(vegetated.slopeProcessing&&vegetated.rigidness==1&&vegetated.classificationThreshold==.5,"vegetation must not disable slope recovery or tighten threshold");
    check(groundPresetParameters(GroundPreset::Forest,.01).classificationThreshold==groundPresetParameters(GroundPreset::Forest,1.).classificationThreshold,"vertical tolerance independent of density");
    QJsonObject profile{{"metricConfirmed",true},{"nnMedian",.1},{"nnDensity2D",50.},{"validReturnFraction",.5},{"lastReturnFraction",.1},{"extentX",100.},{"extentY",100.}};
    auto a=p,b=p;check(suggestProcessingPreset(a,profile,error),"suggest from cloud");profile.insert("lastReturnFraction",.9);check(suggestProcessingPreset(b,profile,error)&&a.radii==b.radii,"ignore unreliable returns");
    profile.insert("metricConfirmed",false);check(!suggestProcessingPreset(b,profile,error),"refuse metric guesses");
    ccPointCloud cloud("UI test ground cloud");check(cloud.reserve(225),"cloud allocation");for(int y=0;y<15;++y)for(int x=0;x<15;++x)cloud.addPoint(CCVector3(float(x),float(y),float(.1*x)));
    int cls=cloud.addScalarField("Classification"),rn=cloud.addScalarField("Return Number"),nr=cloud.addScalarField("Number Of Returns");
    check(cls>=0&&rn>=0&&nr>=0,"scalar allocation");for(unsigned i=0;i<cloud.size();++i){cloud.getScalarField(cls)->setValue(i,i<100?2.f:4.f);cloud.getScalarField(rn)->setValue(i,1);cloud.getScalarField(nr)->setValue(i,i%2?2.f:0.f);}
    auto info=cloudProfile(cloud,true);check(info["originalClasses"].toObject()["2"].toInt()==100,"class histogram scans full cloud");check(info["validReturnFraction"].toDouble()<.51,"invalid echo counts excluded");
    check(cloudProfileText(info).contains("points/m²")&&!cloudProfileText(info).contains("%4"),"density units render correctly");
    QTemporaryDir temporary;check(temporary.isValid(),"temporary directory");QString saved=temporary.filePath("preset.json");QFile file(saved);check(file.open(QIODevice::WriteOnly),"preset test write");file.write(QJsonDocument(processingPresetToJson(p)).toJson());file.close();
    {
        WorkspaceDock dock;WorkspaceController controller(nullptr,&dock);controller.selectCloud(&cloud);
        dock.metricUnitsConfirmationChanged(true);
        check(!dock.findChild<QComboBox*>("ALiS.Workspace.Features.InspectorFeature"),"inspector removed");
		auto* displayGroup=dock.findChild<QGroupBox*>("ALiS.Workspace.Visualization.Group");
		auto* legend=dock.findChild<QCheckBox*>("ALiS.Display.ShowLegend");
		check(displayGroup&&displayGroup->title()=="Display"&&legend,"Display has a concise title and legend control");
        auto* threshold=dock.findChild<QDoubleSpinBox*>("ALiS.Workspace.Terrain.ClassificationThreshold");
        auto* resolution=dock.findChild<QDoubleSpinBox*>("ALiS.Workspace.Terrain.ClothResolution");
        auto* vegetation=dock.findChild<QComboBox*>("ALiS.Workspace.Terrain.VegetationDensity");
        auto* preset=dock.findChild<QComboBox*>("ALiS.Workspace.Terrain.Preset");
        check(threshold&&resolution&&vegetation&&preset,"ground controls present");
        dock.setBusy(true);check(!preset->isEnabled()&&!vegetation->isEnabled(),"parameters locked during processing");dock.setBusy(false);
        preset->setCurrentIndex(1);preset->setCurrentIndex(7);
        vegetation->setCurrentIndex(2);double first=resolution->value();vegetation->setCurrentIndex(0);vegetation->setCurrentIndex(2);
        check(resolution->value()==first&&threshold->value()==.5,"context changes are non-cumulative");
        vegetation->setCurrentIndex(1);check(resolution->value()==2,"neutral returns to same base");
        dock.loadProcessingPresetRequested(saved);check(dock.scaleRadii()==p.radii,"import exact multiscale radii");
        auto precise=p;precise.cellSize=.1234567;precise.dtmStep=.2345678;precise.base.classificationThreshold=.3456789;precise.minimumRadius=.1234567;
        dock.setProcessingPreset(precise);auto ui=dock.processingPreset();check(std::abs(ui.cellSize-precise.cellSize)<1e-12&&std::abs(ui.dtmStep-precise.dtmStep)<1e-12&&std::abs(ui.base.classificationThreshold-precise.base.classificationThreshold)<1e-12,"UI retains imported sub-millimetre precision");
        dock.loadProcessingPresetRequested(saved);
        check(dock.selectedFeatureIds().size()==2,"import selected features");
        int heartbeats=0; QTimer heartbeat; QObject::connect(&heartbeat,&QTimer::timeout,[&](){++heartbeats;controller.selectCloud(nullptr);}); heartbeat.start(5);
        unsigned before=cloud.getNumberOfScalarFields();dock.computeFeaturesRequested(p.features,p.radii);heartbeat.stop();
        check(heartbeats>0&&controller.selectedCloud()==&cloud,"feature computation pumps GUI while guarding selection reentrancy");
        check(cloud.getNumberOfScalarFields()==before+4,"automatic publication of every feature/radius");
        auto mapping=QJsonDocument::fromJson(cloud.getMetaData("ALiS.featureFields").toString().toUtf8()).object();check(mapping.size()==4,"persistent feature identity map");
        dock.computeFeaturesRequested(p.features,p.radii);check(cloud.getNumberOfScalarFields()==before+4,"recompute reuses owned fields without duplicates");
        QTimer::singleShot(0,[&](){auto dialogs=dock.findChildren<QProgressDialog*>();for(auto* dialog:dialogs){dialog->cancel();QMetaObject::invokeMethod(dialog,"canceled",Qt::DirectConnection);}});
        dock.computeFeaturesRequested({"planarity"},{3.5});check(cloud.getNumberOfScalarFields()==before+4,"canceled computation never publishes partial scalar fields");
        check(cloud.reserveTheRGBTable(),"RGB allocation");for(unsigned i=0;i<cloud.size();++i)cloud.addColor(ccColor::Rgb(10,20,30));
        dock.displayRefreshRequested();
        auto* inputs=dock.findChild<QListWidget*>("ALiS.Workspace.Models.Bootstrap.FeatureList");
        bool hasRed=false,hasReturn=false,classUnchecked=false;
        if(inputs)for(int i=0;i<inputs->count();++i){auto* item=inputs->item(i);auto key=item->data(Qt::UserRole).toString();hasRed|=key=="rgb:red";hasReturn|=key==scalarInputKey("Return Number");classUnchecked|=key==scalarInputKey("Classification")&&item->checkState()==Qt::Unchecked;}
        check(hasRed&&hasReturn&&classUnchecked,"clustering exposes RGB and existing SF; prior classes are opt-in");
        CloudAttributeInput input;check(resolveCloudAttribute(cloud,scalarInputKey("Return Number"),input,error)&&input.value(cloud,0)==1,"exact SF values resolve without geometry");
        check(resolveCloudAttribute(cloud,"rgb:green",input,error)&&input.value(cloud,0)==20,"exact RGB channel resolves without geometry");
        auto* display=dock.findChild<QComboBox*>("ALiS.Workspace.Visualization.Mode");check(display&&display->findData(QString("sf:%1").arg(mapping.begin().value().toString()))>=0,"published features in Display");
		legend->setChecked(true);check(cloud.sfColorScaleShown(),"Display legend control shows CloudCompare scale");
		legend->setChecked(false);check(!cloud.sfColorScaleShown(),"Display legend control hides CloudCompare scale");
		auto* profileText=dock.findChild<QLabel*>("ALiS.Workspace.CloudInfo.Text");check(profileText,"cloud profile text present");
		const QString cachedText=profileText->text();
		controller.selectCloud(nullptr);
		auto* original=cloud.getScalarField(cloud.getScalarFieldIndexByName("qAL_ASPRS_Original"));check(original,"session original class field present");
		original->setValue(0,7.f);
		controller.selectCloud(&cloud);check(profileText->text()==cachedText,"reselection reuses cached cloud statistics");
		dock.cloudProfileRequested();check(profileText->text()!=cachedText&&profileText->text().contains("7 Low point (noise)"),"explicit refresh rescans changed classes");
		original->setValue(0,2.f);dock.cloudProfileRequested();
		int prediction=cloud.addScalarField(field::AsprsPrediction),confidence=cloud.addScalarField(field::AsprsConfidence);
		check(prediction>=0&&confidence>=0,"prediction fixture fields");
		for(unsigned i=0;i<cloud.size();++i){cloud.getScalarField(prediction)->setValue(i,2.f);cloud.getScalarField(confidence)->setValue(i,i%2?.8f:.4f);}
		dock.predictionConfidenceFilterRequested(true,.5);const auto& filtered=cloud.getTheVisibilityArray();quint64 visible=0;for(unsigned char value:filtered)if(value==CCCoreLib::POINT_VISIBLE)++visible;
		check(filtered.size()==cloud.size()&&visible==cloud.size()/2,"downstream confidence threshold filters view without prediction");
		dock.predictionConfidenceFilterRequested(false,0.);check(cloud.getTheVisibilityArray().empty(),"confidence filter can be disabled without predicting again");
    }
    cloud.removeMetaData("ALiS.metricUnitsConfirmed");
	std::cerr<<"[processing-tests] persistence"<<std::endl;
    {WorkspaceDock remembered;WorkspaceController controller(nullptr,&remembered);controller.selectCloud(&cloud);auto* metric=remembered.findChild<QCheckBox*>("ALiS.Workspace.Session.MetricUnitsConfirmed");auto* units=remembered.findChild<QLabel*>("ALiS.Workspace.Session.Units");check(metric&&metric->isChecked()&&units&&units->text().contains("remembered"),"metric confirmation remembered by stable cloud identity");}
    cloud.setMetaData("ALiS.metricUnitsConfirmed",true);
    BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;
    QString binPath=temporary.filePath("features.bin");check(bin.saveToFile(&cloud,binPath,sp)==CC_FERR_NO_ERROR,"save published features to BIN");
    ccHObject root;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    check(bin.loadFile(binPath,root,lp)==CC_FERR_NO_ERROR,"reopen feature BIN");
    ccHObject::Container objects;root.filterChildren(objects,true,CC_TYPES::POINT_CLOUD);check(objects.size()==1,"one reopened cloud");
    auto* loaded=static_cast<ccPointCloud*>(objects.front());
    check(loaded->size()==cloud.size()&&loaded->getNumberOfScalarFields()==cloud.getNumberOfScalarFields(),"BIN preserves point and SF counts");
    for(unsigned f=0;f<cloud.getNumberOfScalarFields();++f)for(unsigned i=0;i<cloud.size();++i){double a=cloud.getScalarField(f)->getValue(i),b=loaded->getScalarField(f)->getValue(i);if(!(a==b||(std::isnan(a)&&std::isnan(b))))throw std::runtime_error("BIN scalar value mismatch");}
    {
		std::cerr<<"[processing-tests] reopened workspace and catalogs"<<std::endl;
        WorkspaceDock reopened;WorkspaceController controller(nullptr,&reopened);controller.selectCloud(loaded);auto* persistedMetric=reopened.findChild<QCheckBox*>("ALiS.Workspace.Session.MetricUnitsConfirmed");check(persistedMetric&&persistedMetric->isChecked(),"BIN preserves metric confirmation metadata");
        auto* mainTabs=reopened.findChild<QTabWidget*>("ALiS.Workspace.Tabs");auto* prepareTabs=reopened.findChild<QTabWidget*>("ALiS.Workspace.Prepare.Tabs");auto* annotationTabs=reopened.findChild<QTabWidget*>("ALiS.Workspace.Annotate.Tabs");
		auto* modelTabs=reopened.findChild<QTabWidget*>("ALiS.Workspace.Models.Tabs");
        check(mainTabs&&mainTabs->count()==5&&mainTabs->tabText(0)=="Session"&&mainTabs->tabText(1)=="Prepare"&&mainTabs->tabText(2)=="Annotate"&&mainTabs->tabText(3)=="Classification"&&mainTabs->tabText(4)=="History","five conceptual workspace areas with Classification");
		check(prepareTabs&&prepareTabs->count()==3&&prepareTabs->tabText(0)=="Pre-processing"&&prepareTabs->tabText(1)=="Terrain"&&prepareTabs->tabText(2)=="Features","Prepare contains only preparation workflows");
		check(modelTabs&&modelTabs->count()==3&&modelTabs->tabText(0)=="Vegetation / structures"&&modelTabs->currentIndex()==0,"separation is the first and default Classification panel");
		auto* separation=modelTabs->widget(0);
		check(separation->findChild<QComboBox*>("ALiS.Workspace.Terrain.Vegetation.Mode")
			&&separation->findChild<QPushButton*>("ALiS.Workspace.Terrain.Vegetation.RunModel")
			&&separation->findChild<QPushButton*>("ALiS.Workspace.Terrain.Vegetation.FilterScore")
			&&separation->findChild<QPushButton*>("ALiS.Workspace.Terrain.Vegetation.Extract")
			&&separation->findChild<QPushButton*>("ALiS.Workspace.Vegetation.RefineFeatures")
			&&separation->findChild<QPushButton*>("ALiS.Workspace.Features.RangeReview"),"all separation and interval controls share dedicated panel");
		check(!prepareTabs->widget(1)->findChild<QGroupBox*>("ALiS.Workspace.Terrain.Vegetation")
			&&!prepareTabs->widget(2)->findChild<QPushButton*>("ALiS.Workspace.Features.RangeReview")
			&&prepareTabs->widget(1)->findChild<QGroupBox*>("ALiS.Workspace.Presets"),"no duplicate controls; terrain recipes stay in Terrain");
		check(reopened.findChild<QPushButton*>("ALiS.Workspace.Preprocessing.SOR")
			&& reopened.findChild<QPushButton*>("ALiS.Workspace.Preprocessing.Noise")
			&& reopened.findChild<QPushButton*>("ALiS.Workspace.Preprocessing.Subsample"),"native CloudCompare preprocessing launchers present");
		check(reopened.findChild<QCheckBox*>("ALiS.Workspace.Terrain.DtmFillEmpty")
			&& reopened.findChild<QDoubleSpinBox*>("ALiS.Workspace.Terrain.DtmMaxEdge")
			&& reopened.findChild<QPushButton*>("ALiS.Workspace.Terrain.OpenDem"),"DTM void fill and native DEM/DSM controls present");
		check(annotationTabs&&annotationTabs->count()==2&&annotationTabs->tabText(0)=="Studio"&&annotationTabs->tabText(1)=="Quick labels","Annotate groups studio and labels");
		check(modelTabs&&modelTabs->count()==3&&modelTabs->tabText(1)=="Supervised workflow"&&modelTabs->tabText(2)=="Unsupervised clustering","supervised and clustering workflows retained after separation panel");
		check(!reopened.findChild<QComboBox*>("ALiS.Workspace.Models.Unsupervised.Mode"),"deferred pre-trained selector removed");
		reopened.showPrepareFeatures();check(mainTabs->currentIndex()==1&&prepareTabs->currentIndex()==2,"Features workflow navigation");
        reopened.showAnnotationWorkspace();check(mainTabs->currentIndex()==2&&annotationTabs->currentIndex()==0,"Annotator workflow navigation");
        check(reopened.findChild<QLabel*>("ALiS.Workspace.Models.Tutorial.Text")!=nullptr,"Models tutorial present");
		check(reopened.findChild<QGroupBox*>("ALiS.Workspace.Models.WorkflowStatus")!=nullptr,"guided Models workflow present");
		check(reopened.findChild<QPushButton*>("ALiS.Workspace.Models.ViewResults")!=nullptr,"model report action present");
		check(reopened.findChild<QPushButton*>("ALiS.Workspace.Models.SelectClassificationCloud")!=nullptr,"explicit classification target action present");
        auto* list=reopened.findChild<QListWidget*>("ALiS.Workspace.Models.FeatureList");check(list&&list->count()==4,"new controller reuses persisted SF schema without cache");
		auto* rfProfile=reopened.findChild<QComboBox*>("ALiS.Workspace.Models.Card.random_forest.Profile");
		auto* rfDepth=reopened.findChild<QSpinBox*>("ALiS.Workspace.Models.Card.random_forest.MaxDepth");
		auto* rfImpute=reopened.findChild<QCheckBox*>("ALiS.Workspace.Models.Card.random_forest.ImputeMissing");
		auto* confidenceFilter=reopened.findChild<QCheckBox*>("ALiS.Workspace.Models.ConfidenceViewFilter");
		auto* confidenceThreshold=reopened.findChild<QDoubleSpinBox*>("ALiS.Workspace.Models.ConfidenceThreshold");
		check(rfProfile&&rfProfile->count()==5&&rfProfile->itemText(0).contains("Ultra low")&&rfProfile->itemText(4)=="Ultra high"&&rfDepth&&rfImpute&&rfImpute->isChecked(),"five classifier tuning levels, advanced RF controls and robust missing-value handling present");
		check(confidenceFilter&&confidenceThreshold&&confidenceThreshold->value()==0.0,"prediction is unthresholded by default with downstream confidence filter");
		auto* pointnetEnabled = reopened.findChild<QCheckBox*>("ALiS.Workspace.Models.Card.pointnet.Enabled");
		auto* pointnetRadius = reopened.findChild<QDoubleSpinBox*>("ALiS.Workspace.Models.PointNet.Radius");
		auto* pointnetNeighbors = reopened.findChild<QSpinBox*>("ALiS.Workspace.Models.PointNet.Neighbors");
		check(pointnetEnabled && pointnetRadius && pointnetNeighbors && pointnetRadius->value()==0.5 && pointnetNeighbors->value()==64,"PointNet raw-XYZ card and patch controls present");
		pointnetEnabled->setChecked(true);
		const auto pointnetSpec = reopened.selectedModelSpecifications().last().split('|');
		check(pointnetSpec.size()==15 && pointnetSpec[0]=="pointnet" && pointnetSpec[2]=="30" && pointnetSpec[3]=="128" && pointnetSpec[12]=="0", "PointNet uses epochs, bounded patch batches and no feature imputation");
		pointnetEnabled->setChecked(false);
		check(reopened.findChild<QPushButton*>("ALiS.Workspace.Models.ComparePredictions")&&reopened.findChild<QPushButton*>("ALiS.Workspace.Models.ViewPredictionStatistics"),"target-cloud model comparison and prediction statistics actions present");
		reopened.trainModelRequested({},"","","","",{"random_forest|cpu|100|32|0.001|0|1|sqrt|balanced_subsample|gini|0.9|0.9|1"},20,.2,42,"internal_spatial");
        auto* status=reopened.findChild<QLabel*>("ALiS.Workspace.Status");check(status&&status->text().contains("Manual/Trusted"),"Train signal reaches validation");
        reopened.predictModelRequested({},"","",temporary.filePath("missing.joblib"),"",1000);check(status->text().contains("does not exist"),"Predict signal reaches validation");
        QStringList keys;for(int i=0;i<list->count();++i)keys<<list->item(i)->data(Qt::UserRole).toString();
        QString repository=temporary.filePath("repository");reopened.exportModelDatasetRequested(keys,repository);
        auto datasets=QDir(repository+"/datasets").entryList(QDir::Dirs|QDir::NoDotAndDotDot);check(datasets.size()==1,"export dataset from reopened SF without cache");
        QDir dataset(repository+"/datasets/"+datasets.front());QFile matrix(dataset.filePath("features.f32"));check(matrix.open(QIODevice::ReadOnly)&&matrix.size()==qint64(loaded->size())*keys.size()*4,"full aligned exported feature matrix");
        const auto bytes=matrix.readAll();auto mapping=QJsonDocument::fromJson(loaded->getMetaData("ALiS.featureFields").toString().toUtf8()).object();
        for(int k=0;k<keys.size();++k){auto* sf=loaded->getScalarField(loaded->getScalarFieldIndexByName(mapping[keys[k]].toString().toUtf8().constData()));for(unsigned i=0;i<loaded->size();++i){float value;std::memcpy(&value,bytes.constData()+4*(i*keys.size()+k),4);double expected=sf->getValue(i);if(!(value==expected||(std::isnan(value)&&std::isnan(expected))))throw std::runtime_error("Exported saved SF value mismatch");}}
        QFile manifest(dataset.filePath("manifest.json"));check(manifest.open(QIODevice::ReadOnly),"reopened export manifest");auto data=QJsonDocument::fromJson(manifest.readAll()).object();check(data["source"].toObject()["cloud_profile"].toObject()["points"].toInt()==225,"cloud info accompanies ML dataset");
		QString modelDir=repository+"/models/random_forest/test";check(QDir().mkpath(modelDir),"catalog model folder");QFile modelFile(modelDir+"/model.joblib");check(modelFile.open(QIODevice::WriteOnly),"catalog model file");modelFile.write("fixture");modelFile.close();
		QJsonObject modelManifest{{"schema","qal-model-entry/1.0"},{"artifact_id","fixture-model"},{"classifier_id","random_forest"},{"model",QDir::toNativeSeparators(modelDir+"/model.joblib")},{"target_domain","asprs_working"},{"feature_names",QJsonArray::fromStringList(keys)},{"balanced_accuracy",.91},{"validation_strategy","external_cloud"},{"training_profile",data["source"].toObject()["cloud_profile"]}};
		QFile modelManifestFile(modelDir+"/manifest.json");check(modelManifestFile.open(QIODevice::WriteOnly),"catalog manifest file");modelManifestFile.write(QJsonDocument(modelManifest).toJson());modelManifestFile.close();
		auto catalog=ModelCatalog::refresh(repository,data["source"].toObject()["cloud_profile"].toObject(),keys);check(catalog.modelCount==1&&catalog.datasetCount==1&&catalog.recommendations.front().compatible&&catalog.recommendations.front().status=="Ready","intelligent catalog indexes and ranks compatible model");
		reopened.setModelRepositoryPath(repository);auto* catalogTree=reopened.findChild<QTreeWidget*>("ALiS.Workspace.Models.SmartCatalog");check(catalogTree!=nullptr,"smart catalog visible in Models");reopened.refreshModelCatalog();check(catalogTree->topLevelItemCount()==1,"smart catalog recommendation rendered");check(reopened.selectedCatalogModelPaths().size()==1,"compatible catalog selection available for target-cloud comparison");
		check(!reopened.findChild<QTreeWidget*>("ALiS.Workspace.Models.Pretrained.Catalog"),"pretrained library removed from current release");
		check(ModelCatalog::pretrainedModels(repository).isEmpty(),"no shipped pretrained model catalog");
		QString rgbRepository=temporary.filePath("rgb_repository");reopened.exportModelDatasetRequested({"rgb:red",scalarInputKey("Return Number")},rgbRepository);
		auto rgbDatasets=QDir(rgbRepository+"/datasets").entryList(QDir::Dirs|QDir::NoDotAndDotDot);check(rgbDatasets.size()==1,"export existing RGB and SF only");
		QFile rgbMatrix(rgbRepository+"/datasets/"+rgbDatasets.front()+"/features.f32");check(rgbMatrix.open(QIODevice::ReadOnly)&&rgbMatrix.size()==qint64(loaded->size())*8,"RGB and SF aligned export size");
		auto rgbBytes=rgbMatrix.readAll();for(unsigned i=0;i<loaded->size();++i){float values[2];std::memcpy(values,rgbBytes.constData()+i*8,8);check(values[0]==10&&values[1]==1,"RGB/SF exported without geometry calculation");}
		std::cerr<<"[processing-tests] catalogs complete"<<std::endl;
		QString xyzRepository=temporary.filePath("xyz_repository"); reopened.exportModelDatasetRequested({"xyz:x","xyz:y","xyz:z"},xyzRepository);
		auto xyzDatasets=QDir(xyzRepository+"/datasets").entryList(QDir::Dirs|QDir::NoDotAndDotDot);
		check(xyzDatasets.size()==1,"PointNet XYZ-only dataset exports without feature computation");
		QFile xyzMatrix(xyzRepository+"/datasets/"+xyzDatasets.front()+"/features.f32");
		check(xyzMatrix.open(QIODevice::ReadOnly) && xyzMatrix.size()==qint64(loaded->size())*12,"PointNet XYZ export preserves all rows");
		auto xyzBytes=xyzMatrix.readAll();
		for(unsigned i=0;i<loaded->size();++i){float xyz[3];std::memcpy(xyz,xyzBytes.constData()+i*12,12);for(int d=0;d<3;++d)if(xyz[d]!=loaded->getPoint(i)->u[d])throw std::runtime_error("XYZ export mismatch");}
		const QString screenshots=qEnvironmentVariable("QAL_TEST_SCREENSHOTS");
		if(!screenshots.isEmpty()){QDir().mkpath(screenshots);reopened.resize(700,1100);reopened.show();auto save=[&](const QString& name){QApplication::processEvents();check(reopened.grab().save(QDir(screenshots).filePath(name)),"save UI screenshot");};mainTabs->setCurrentIndex(0);save("session.png");mainTabs->setCurrentIndex(1);prepareTabs->setCurrentIndex(0);save("prepare_preprocessing.png");prepareTabs->setCurrentIndex(1);save("prepare_terrain.png");prepareTabs->setCurrentIndex(2);save("prepare_features.png");mainTabs->setCurrentIndex(2);annotationTabs->setCurrentIndex(0);save("annotate_studio.png");mainTabs->setCurrentIndex(3);save("models.png");}
    }
    std::cout<<"Processing tests passed: "<<checks<<" checks\n";
}
}
int main(int argc,char** argv){QApplication app(argc,argv);QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");app.setFont(QFont("Segoe UI",9));QStandardPaths::setTestModeEnabled(true);app.setOrganizationName("ALiSTests");app.setApplicationName("ProcessingTests");try{if(argc==8&&QString::fromLocal8Bit(argv[1])=="--vegetation-model-regression")return vegetationModelRegression(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]),QString::fromLocal8Bit(argv[4]),QString::fromLocal8Bit(argv[5]),QString::fromLocal8Bit(argv[6]),QString::fromLocal8Bit(argv[7]));if(argc==5&&QString::fromLocal8Bit(argv[1])=="--vegetation-regression")return vegetationRegression(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]),QString::fromLocal8Bit(argv[4]));if(argc==5&&QString::fromLocal8Bit(argv[1])=="--feature-regression")return featureRegression(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]),QString::fromLocal8Bit(argv[4]));if(argc==4&&QString::fromLocal8Bit(argv[1])=="--benchmark")return benchmarkGround(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]));tests();return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<std::endl;return 1;}}
