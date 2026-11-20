// SPDX-License-Identifier: GPL-2.0-or-later
#include "FeatureRecovery.h"
#include "SemanticGeometry.h"
#include "PointSelection.h"
#include "BuildingCleaning.h"
#include "SemanticParametersDialog.h"
#include "PointSelectionDialog.h"
#include "DfmInterpolation.h"
#include "DfmConflictDialog.h"
#include "DfmSettingsDialog.h"
#include "ScalarFieldManagement.h"
#include "ScalarFieldsDialog.h"
#include <QTimer>
#include <QApplication>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <BinFilter.h>
#include <iostream>
using namespace alis;
int checks=0;
void check(bool ok,const char* message){++checks;checkpointCheck(ok,message);}
template<class F> void refuses(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Unsafe operation must be refused");}
void json(const QString& p,const QJsonObject& value){QFile f(p);check(f.open(QIODevice::WriteOnly),"fixture open");f.write(QJsonDocument(value).toJson());}
int main(int argc,char** argv){QApplication app(argc,argv);
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");app.setFont(QFont("Segoe UI",9));
#endif
try{
    QTemporaryDir dir;ccPointCloud cloud("fixture");check(cloud.reserve(6),"point allocation");for(int i=0;i<6;++i)cloud.addPoint(CCVector3(float(i),0,0));
    int original=cloud.addScalarField("Original");cloud.getScalarField(original)->fill(7);
    const auto originalStats=inspectScalarField(*cloud.getScalarField(original));check(originalStats.finite==6&&originalStats.minimum==7&&originalStats.maximum==7&&originalStats.mean==7,"Scalar statistics: counts/minimum/maximum/mean");
    check(matchesScalarFieldSearch("Original",&originalStats,6,"name","contains","orig"),"Case-insensitive field-name search");
    for(auto operation:QStringList{"=",">=","<=",QString::fromUtf8("≥"),QString::fromUtf8("≤")})check(matchesScalarFieldSearch("Original",&originalStats,6,"minimum",operation,"7"),"Numeric equality and inclusive comparisons");
    check(matchesScalarFieldSearch("Original",&originalStats,6,"maximum",">","6,5"),"Decimal comma accepted");
    check(matchesScalarFieldSearch("Original",&originalStats,6,"mean","<","7.5"),"Mean-value comparison");
    check(matchesScalarFieldSearch("Original",&originalStats,6,"valid","!=","0"),"Valid-count inequality");
    check(!matchesScalarFieldSearch("Original",nullptr,6,"valid","=","0")&&!matchesScalarFieldSearch("Original",nullptr,6,"valid","!=","0"),"Unknown statistics never equal or differ from zero");
    check(!matchesScalarFieldSearch("Original",&originalStats,6,"minimum",">","NaN")&&!matchesScalarFieldSearch("Original",&originalStats,6,"minimum",">","invalid"),"Invalid numeric input does not match");
    check(matchesScalarFieldSearch("Original",nullptr,6,"memory",">","0"),"Memory comparison does not require value analysis");
    check(matchesScalarFieldSearch("qAL_GroundMask",nullptr,6,"status","=","Protected"),"Protected-status search independent of statistics");
    QStringList keys{"planarity@2","sphericity@2"};QJsonObject config{{"point_count",6},{"coordinate_sha256","fixture"},{"features",QJsonArray{"planarity@2","sphericity@2"}}};
    auto identity=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(config).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    auto checkpoint=[&](QString id,unsigned begin,unsigned count){auto path=dir.filePath(id+".alsf");FeatureCheckpointWriter w(path,checkpointMetadata(identity,id,keys),count,2);for(unsigned i=begin;i<begin+count;++i){float v[]={float(i),float(i)*2};w.append(i,reinterpret_cast<const char*>(v));}w.finish();return QJsonObject{{"id",id},{"status","Completed"},{"core_points",int(count)},{"file",path}};};
    auto a=checkpoint("B0001",0,3),b=checkpoint("B0002",3,3);
    QJsonObject manifest{{"schema","alis-spatial-blocks-2"},{"config",config},{"identity",identity},{"blocks",QJsonArray{a,b}}};json(dir.filePath("blocks.json"),manifest);
    refuses([&]{inspectFeatureRecovery(cloud,dir.path(),"wrong");});check(cloud.getNumberOfScalarFields()==1,"Mismatch has no mutation");
    auto audit=inspectFeatureRecovery(cloud,dir.path(),"fixture");check(audit.value("covered").toInt()==6,"Exact ownership");
    ProgressiveFeatureStore store(&cloud,identity,keys,config);store.setOutputFolder(dir.path());store.commitFile("B0001",a.value("file").toString(),3,keys,{},true);
    ProgressiveFeatureStore resume(&cloud,identity,keys,config);check(resume.completed().size()==1,"Resume retains committed core");resume.commitFile("B0002",b.value("file").toString(),3,keys,{},true);resume.finalizeRanges();
    auto names=resume.metadata().value("fields").toObject();for(int j=0;j<2;++j){auto* sf=cloud.getScalarField(cloud.getScalarFieldIndexByName(names.value(keys[j]).toString().toUtf8()));for(unsigned i=0;i<6;++i)check(sf->getValue(i)==i*(j+1),"Exact recovered value");}
    ProgressiveFeatureStore extra(&cloud,"new-radius",{"planarity@8"},QJsonObject{});check(cloud.getNumberOfScalarFields()==4,"New scale appends not replaces");check(cloud.getScalarField(original)->getValue(5)==7,"Original unchanged");check(!cloud.getMetaData("ALiS.featureJobHistory.v1").toByteArray().isEmpty(),"Old job resumable history");
    BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;check(bin.saveToFile(&cloud,dir.filePath("saved.bin"),sp)==CC_FERR_NO_ERROR,"Save native scalar BIN");
    ccHObject root;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;check(bin.loadFile(dir.filePath("saved.bin"),root,lp)==CC_FERR_NO_ERROR,"Reopen BIN");ccHObject::Container objects;root.filterChildren(objects,true,CC_TYPES::POINT_CLOUD);check(objects.size()==1&&static_cast<ccPointCloud*>(objects.front())->getNumberOfScalarFields()==4,"All fields persist");
    manifest["blocks"]=QJsonArray{a,a};json(dir.filePath("blocks.json"),manifest);refuses([&]{inspectFeatureRecovery(cloud,dir.path(),"fixture");});
    auto overlap=checkpoint("B0003",2,2);manifest["blocks"]=QJsonArray{a,overlap};json(dir.filePath("blocks.json"),manifest);refuses([&]{inspectFeatureRecovery(cloud,dir.path(),"fixture");});
    QFile::remove(a.value("file").toString());refuses([&]{inspectFeatureRecovery(cloud,dir.path(),"fixture");});
    refuses([&]{classifySemanticGeometry(cloud,{},QSet<int>{2,6});});
    // Point-level semantic rules: reused Ground, exact requested height bins and invalid returns abstention.
    ccPointCloud semantics;check(semantics.reserve(6),"semantic allocation");for(int i=0;i<6;++i)semantics.addPoint(CCVector3(float(i),0,0));
    QJsonObject mapping;for(const auto& key:QStringList{"planarity@2","sphericity@2","normal_z_absolute@2","planarity@4","sphericity@4"}){int index=semantics.addScalarField(key.toUtf8());semantics.getScalarField(index)->fill(key.startsWith("sphericity")?.3f:.1f);mapping[key]=key;}
    semantics.setMetaData("ALiS.featureFields",QJsonDocument(mapping).toJson());
    for(auto name:{"Return Number","Number Of Returns"})semantics.getScalarField(semantics.addScalarField(name))->fill(1);
    auto* hag=semantics.getScalarField(semantics.addScalarField("qAL_HAG"));float heights[]={0,2,5,6,-1,3};for(unsigned i=0;i<6;++i)hag->setValue(i,heights[i]);
    auto prediction=classifySemanticGeometry(semantics,{true,false,false,false,false,false},QSet<int>{2,3,4,5,6});check(prediction.labels==std::vector<std::int16_t>({2,3,4,5,1,4}),"Ground and vegetation thresholds");
    semantics.getScalarField(semantics.getScalarFieldIndexByName("Return Number"))->setValue(0,0);prediction=classifySemanticGeometry(semantics,{true,false,false,false,false,false},QSet<int>{2,6});check(prediction.labels[0]==2&&prediction.labels[3]==1&&prediction.reasons[0]==1,"Existing Ground survives missing/invalid returns; unchecked other classes abstain");
    prediction=classifySemanticGeometry(semantics,{true,false,false,false,false,false},QSet<int>{3,4,5});check(prediction.labels[0]==2&&prediction.provenance.value("ground_locked").toBool(),"Applied Ground always reused even when not requested");
    SemanticParameters tuned;tuned.lowHeight=1;tuned.mediumHeight=4;prediction=classifySemanticGeometry(semantics,{true,false,false,false,false,false},QSet<int>{3,4,5},{},tuned);check(prediction.labels[1]==4&&prediction.labels[2]==5,"Configurable vegetation HAG bands change real predictions");
    tuned.mediumHeight=tuned.lowHeight;refuses([&]{classifySemanticGeometry(semantics,{true,false,false,false,false,false},QSet<int>{3},{},tuned);});
    semantics.getScalarField(semantics.addScalarField("qAL_ASPRS_Working"))->fill(6);const auto oldFields=semantics.getNumberOfScalarFields();
    auto cleaning=cleanBuildingClasses(semantics,{true,false,false,false,false,false},SemanticParameters(),true,false,false);check(cleaning.proposals==std::vector<std::int16_t>({-1,3,4,5,1,4}),"Cleaning removes dispersed canopy from Building to HAG bands; Ground is not targeted");
    cleaning=cleanBuildingClasses(semantics,{true,false,false,false,false,false},SemanticParameters(),false,false,false);check(cleaning.proposals==std::vector<std::int16_t>({-1,1,1,1,1,1})&&semantics.getNumberOfScalarFields()==oldFields,"Unverified HAG cannot invent vegetation bands; preview does not mutate fields");
    std::vector<unsigned char> selection{1,0,0,1,0,0};prediction=classifySemanticGeometry(semantics,{true,false,false,false,false,false},QSet<int>{3,4,5},{},SemanticParameters(),&selection);check(prediction.labels[0]==2&&prediction.labels[1]==-1&&prediction.labels[3]==5&&prediction.reasons[1]==0,"Semantic selection is index-aligned; outside points are not classified");
    QJsonObject interval,codes;QString selectionError;check(parsePointSelectionRule("qAL_HAG","range","2;5",interval,selectionError),"Parse inclusive HAG range");auto selected=pointSelectionMask(semantics,QJsonArray{interval});check(selected==std::vector<unsigned char>({0,1,1,0,0,1}),"HAG interval matches exact endpoints, no geometry split");
    check(parsePointSelectionRule("qAL_HAG",">=","5",interval,selectionError),"Parse comparison");check(parsePointSelectionRule("qAL_HAG","codes","2,5",codes,selectionError),"Parse class list OR");selected=pointSelectionMask(semantics,QJsonArray{interval,codes});check(selected==std::vector<unsigned char>({0,0,1,0,0,0}),"Multiple filter rows use AND; class codes within a row use OR");
    check(!parsePointSelectionRule("qAL_HAG","range","5;2",interval,selectionError)&&!parsePointSelectionRule("qAL_HAG","codes","2.5",codes,selectionError),"Reject reversed ranges and fractional class codes");
    check(parsePointSelectionRule("qAL_HAG",">=","0,15",interval,selectionError)&&matchesPointSelection(.2,interval)&&!matchesPointSelection(std::numeric_limits<double>::quiet_NaN(),interval),"Decimal comma allowed, missing values never pass numeric predicates");
    refuses([&]{pointSelectionMask(semantics,QJsonArray{QJsonObject{{"field","absent"},{"op","="},{"text","0"}}});});
    ccPointCloud structure;check(structure.reserve(240),"structure fixture");
    // Connected 4x4 roof and wall. Three points per occupied metre voxel.
    for(int x=0;x<4;++x)for(int y=0;y<4;++y)for(int n=0;n<3;++n)structure.addPoint(CCVector3(x+.1f+n*.02f,y+.1f,3.1f));
    for(int x=0;x<4;++x)for(int z=0;z<3;++z)for(int n=0;n<3;++n)structure.addPoint(CCVector3(x+.1f+n*.02f,.1f,z+.1f));
    QJsonObject structureMapping;
    for(auto key:QStringList{"planarity@2","sphericity@2","normal_z_absolute@2","planarity@4","sphericity@4"}){auto* sf=structure.getScalarField(structure.addScalarField(key.toUtf8()));sf->fill(key.startsWith("sphericity")?.05f:.8f);if(key.startsWith("normal"))for(unsigned i=0;i<structure.size();++i)sf->setValue(i,i<48?1.f:0.f);structureMapping[key]=key;}
    structure.setMetaData("ALiS.featureFields",QJsonDocument(structureMapping).toJson());for(auto name:{"Return Number","Number of Returns"})structure.getScalarField(structure.addScalarField(name))->fill(1);
    auto built=classifySemanticGeometry(structure,std::vector<bool>(structure.size(),false),QSet<int>{6});check(std::all_of(built.labels.begin(),built.labels.end(),[](int c){return c==6;}),"Connected walls and roof accepted");
    structure.getScalarField(structure.getScalarFieldIndexByName("normal_z_absolute@2"))->fill(1);
    built=classifySemanticGeometry(structure,std::vector<bool>(structure.size(),false),QSet<int>{6});check(std::all_of(built.labels.begin(),built.labels.end(),[](int c){return c==1;}),"Planarity without wall support does not become building");
    structure.getScalarField(structure.addScalarField("qAL_ASPRS_Working"))->fill(6);
    auto weakBuilding=cleanBuildingClasses(structure,std::vector<bool>(structure.size(),false),SemanticParameters(),false,false,false);check(std::all_of(weakBuilding.proposals.begin(),weakBuilding.proposals.end(),[](int c){return c==-1;}),"Unsupported roof planes retained by conservative cleaning, not mass-demoted");
    weakBuilding=cleanBuildingClasses(structure,std::vector<bool>(structure.size(),false),SemanticParameters(),false,true,false);check(std::all_of(weakBuilding.proposals.begin(),weakBuilding.proposals.end(),[](int c){return c==1;}),"Aggressive unsupported-plane cleaning is explicit");
    structure.getScalarField(structure.getScalarFieldIndexByName("normal_z_absolute@2"))->fill(0);for(unsigned i=0;i<48;++i)structure.getScalarField(structure.getScalarFieldIndexByName("normal_z_absolute@2"))->setValue(i,1);
    built=classifySemanticGeometry(structure,std::vector<bool>(structure.size(),false),QSet<int>{128,129});for(unsigned i=0;i<structure.size();++i)check(built.labels[i]==(i<48?128:129),"Separate roof and wall classes");
    ccPointCloud lines;check(lines.reserve(120),"wire fixture");for(int x=0;x<16;++x)for(int n=0;n<3;++n)lines.addPoint(CCVector3(x+.1f+n*.02f,.1f,5.1f));for(int z=0;z<6;++z)for(int n=0;n<3;++n)lines.addPoint(CCVector3(.1f,.1f,z+.2f+n*.02f));
    QJsonObject lineMap;for(auto key:QStringList{"planarity@2","sphericity@2","normal_z_absolute@2","planarity@4","sphericity@4","linearity@2","linearity@4","z_range@2","z_range@4"}){auto* sf=lines.getScalarField(lines.addScalarField(key.toUtf8()));sf->fill(key.startsWith("linearity")?.95f:.02f);if(key.startsWith("z_range"))for(unsigned i=0;i<lines.size();++i)sf->setValue(i,i<48?.2f:4.f);lineMap[key]=key;}
    lines.setMetaData("ALiS.featureFields",QJsonDocument(lineMap).toJson());auto* lineHag=lines.getScalarField(lines.addScalarField("qAL_HAG"));for(unsigned i=0;i<lines.size();++i)lineHag->setValue(i,lines.getPoint(i)->z);
    auto linear=classifySemanticGeometry(lines,std::vector<bool>(lines.size(),false),QSet<int>{130,131});for(unsigned i=0;i<lines.size();++i)check(linear.labels[i]==(i<48?131:130),"Long wire and cable-supported pole separated");
    auto* linearity=lines.getScalarField(lines.getScalarFieldIndexByName("linearity@4"));for(unsigned i=0;i<48;++i)linearity->setValue(i,.2f);linear=classifySemanticGeometry(lines,std::vector<bool>(lines.size(),false),QSet<int>{130,131});check(std::all_of(linear.labels.begin(),linear.labels.end(),[](int c){return c==1;}),"No coherent cable means no confirmed pole/trunk semantics");
    const auto emptyStats=inspectScalarField(*cloud.getScalarField(cloud.getScalarFieldIndexByName(extra.metadata().value("fields").toObject().value("planarity@8").toString().toUtf8())));check(emptyStats.finite==0&&emptyStats.missing==6,"All-NaN preallocated fields explicitly recognized");
    check(matchesScalarFieldSearch("Empty",&emptyStats,6,"valid","=","0")&&matchesScalarFieldSearch("Empty",&emptyStats,6,"missing_percent",">=","100"),"Empty-column count and percentage filters");
    check(!matchesScalarFieldSearch("Empty",&emptyStats,6,"minimum","!=","0")&&!matchesScalarFieldSearch("Empty",&emptyStats,6,"maximum","=","0"),"Empty columns have no numeric min/max, not zero");
    auto groundIndex=cloud.addScalarField("qAL_GroundMask");cloud.getScalarField(groundIndex)->fill(1);refuses([&]{removeSelectedScalarFields(cloud,{"qAL_GroundMask"});});
    const auto removal=removeSelectedScalarFields(cloud,{extra.metadata().value("fields").toObject().value("planarity@8").toString()});check(!removal.value("checkpoint_files_deleted").toBool()&&QFileInfo::exists(b.value("file").toString()),"Removing columns never deletes checkpoint files");check(ProgressiveFeatureStore::state(cloud).isEmpty(),"Removed job fields detach completed-state claims");check(cloud.getScalarField(cloud.getScalarFieldIndexByName("Original"))->getValue(0)==7,"Scalar removal preserves other columns");
    const int blank=cloud.addScalarField("ALiS empty@7 [fixture]");cloud.getScalarField(blank)->fill(std::numeric_limits<float>::quiet_NaN());const unsigned fieldsBefore=cloud.getNumberOfScalarFields();bool uiChecked=false;
    QTimer::singleShot(0,[&](){auto* dialog=QApplication::activeModalWidget();if(!dialog)return;auto* table=dialog->findChild<QTableWidget*>();if(!table){dialog->close();return;}for(int row=0;row<table->rowCount();++row)if(table->item(row,0)->text()=="ALiS empty@7 [fixture]")table->item(row,0)->setCheckState(Qt::Checked);for(auto* button:dialog->findChildren<QPushButton*>())if(button->text()=="Analyse checked / selected fields")button->click();for(int row=0;row<table->rowCount();++row)if(table->item(row,0)->text()=="ALiS empty@7 [fixture]")uiChecked=table->item(row,2)->text()=="0"&&table->item(row,4)->text().startsWith("EMPTY");dialog->close();});
    const auto uiChanges=manageScalarFieldsDialog(cloud,nullptr);check(uiChecked&&uiChanges.isEmpty()&&cloud.getNumberOfScalarFields()==fieldsBefore,"Scalar manager counts empty values without deleting anything on close");
    bool bulkSafe=false;QTimer::singleShot(0,[&](){auto* dialog=QApplication::activeModalWidget();if(!dialog)return;auto* filter=dialog->findChild<QLineEdit*>();auto* table=dialog->findChild<QTableWidget*>();if(!filter||!table){dialog->close();return;}filter->setText("ALiS empty@7");auto* all=dialog->findChild<QPushButton*>("ALiS.ScalarFields.SelectAll");auto* none=dialog->findChild<QPushButton*>("ALiS.ScalarFields.DeselectAll");if(all&&none){all->click();int checked=0;for(int i=0;i<table->rowCount();++i)checked+=table->item(i,0)->checkState()==Qt::Checked;bulkSafe=checked==1;none->click();for(int i=0;i<table->rowCount();++i)bulkSafe&=table->item(i,0)->checkState()!=Qt::Checked;all->click();filter->setText("No matches");for(int i=0;i<table->rowCount();++i)bulkSafe&=table->item(i,0)->checkState()!=Qt::Checked;}dialog->close();});manageScalarFieldsDialog(cloud,nullptr);check(bulkSafe&&cloud.getNumberOfScalarFields()==fieldsBefore,"Bulk selection obeys filter, protects hidden rows and never adds fields");
    bool numericUiSafe=false;QTimer::singleShot(0,[&](){auto* dialog=QApplication::activeModalWidget();if(!dialog)return;auto* query=dialog->findChild<QLineEdit*>("ALiS.ScalarFields.SearchValue");auto* field=dialog->findChild<QComboBox*>("ALiS.ScalarFields.SearchField");auto* op=dialog->findChild<QComboBox*>("ALiS.ScalarFields.SearchOperator");auto* table=dialog->findChild<QTableWidget*>();if(!query||!field||!op||!table){dialog->close();return;}
        query->setText("ALiS empty@7");dialog->findChild<QPushButton*>("ALiS.ScalarFields.SelectAll")->click();for(auto* button:dialog->findChildren<QPushButton*>())if(button->text()=="Analyse checked / selected fields")button->click();
        field->setCurrentIndex(field->findData("valid"));op->setCurrentIndex(op->findData("="));query->setText("0");int visible=0;for(int row=0;row<table->rowCount();++row)visible+=!table->isRowHidden(row);numericUiSafe=visible==1;
        field->setCurrentIndex(field->findData("minimum"));op->setCurrentIndex(op->findData("!="));for(int row=0;row<table->rowCount();++row)numericUiSafe&=table->isRowHidden(row);
        field->setCurrentIndex(field->findData("memory"));op->setCurrentIndex(op->findData(">="));for(int row=0;row<table->rowCount();++row)numericUiSafe&=!table->isRowHidden(row);dialog->close();
    });manageScalarFieldsDialog(cloud,nullptr);check(numericUiSafe&&cloud.getNumberOfScalarFields()==fieldsBefore,"Numeric search UI uses analysed statistics and never changes scalar fields");
    ccPointCloud recoveredNames;check(recoveredNames.reserve(2),"name recovery fixture");recoveredNames.addPoint(CCVector3(0,0,0));recoveredNames.addPoint(CCVector3(1,0,0));
    for(auto key:QStringList{"planarity@2","sphericity@2","normal_z_absolute@2","planarity@4","sphericity@4"})recoveredNames.getScalarField(recoveredNames.addScalarField(QString("ALiS %1 m [fixture]").arg(key).toUtf8()))->fill(.2f);
    const unsigned nameCount=recoveredNames.getNumberOfScalarFields();classifySemanticGeometry(recoveredNames,{true,false},QSet<int>{2});check(recoveredNames.getNumberOfScalarFields()==nameCount,"Geometry resolves physical SF names without identity map or allocation");
    recoveredNames.getScalarField(0)->fill(std::numeric_limits<float>::quiet_NaN());refuses([&]{classifySemanticGeometry(recoveredNames,{true,false},QSet<int>{2});});check(recoveredNames.getNumberOfScalarFields()==nameCount,"Missing/empty inputs never compute or allocate features");
    {
        const double nan=std::numeric_limits<double>::quiet_NaN();std::vector<double> heights(25,10);std::vector<unsigned char> observed(25,1);heights[12]=nan;observed[12]=0;DfmFillOptions options;options.method=DfmFillMethod::LocalMean;options.radius=2;auto result=fillDfmVoids(5,5,1,heights,observed,options);check(heights[12]==10&&result.filled==1,"Local mean fills one supported void without changing observations");
        heights.assign(25,0);heights[12]=nan;heights[13]=10;options.method=DfmFillMethod::InverseDistance;options.maximumLocalRange=20;options.maximumMeanDeviation=0;result=fillDfmVoids(5,5,1,heights,observed,options);check(!std::isfinite(heights[12])&&result.deviation==1&&heights[13]==10,"IDW estimate exceeding local-mean limit is rejected, never clipped or smoothing observations");
        heights[12]=nan;options.maximumLocalRange=1;options.method=DfmFillMethod::LocalMean;result=fillDfmVoids(5,5,1,heights,observed,options);check(result.discontinuity==1&&!std::isfinite(heights[12]),"Roof/ground discontinuity guards retain NODATA");
        heights.assign(25,10);heights[12]=1000;options.method=DfmFillMethod::Delaunay;options.maximumMeanDeviation=.25;result=fillDfmVoids(5,5,1,heights,observed,options);check(!std::isfinite(heights[12])&&result.deviation==1,"TIN candidate is locally checked before acceptance");
        heights[12]=nan;options.method=DfmFillMethod::Kriging;unsigned calls=0;result=fillDfmVoids(5,5,1,heights,observed,options,{},[&](unsigned x,unsigned y){++calls;check(x==2&&y==2,"Kriging queried only at original void");return 10.;});check(calls==1&&heights[12]==10&&result.filled==1,"Kriging predictor never rewrites observed cells");refuses([&]{fillDfmVoids(5,5,1,heights,observed,options);});
        std::vector<double> line{0,0,0,nan,nan,nan,nan};std::vector<unsigned char> support{1,1,1,0,0,0,0};options.method=DfmFillMethod::LocalMean;options.radius=3;options.minimumSupport=3;options.requireSurroundingSupport=false;result=fillDfmVoids(7,1,1,line,support,options);check(std::isfinite(line[3])&&!std::isfinite(line[4]),"Filled cells do not propagate interpolation into unsupported gaps");
        options.radius=100;refuses([&]{fillDfmVoids(7,1,1,line,support,options);});
        std::vector<double> ground(9,0),building(9,5);DfmConflictGrid review;review.width=review.height=3;review.ground=&ground;review.building=&building;review.initialize();check(review.conflicts==9,"Conflict map counts true cell overlaps");review.addRegion({1,1,2,2,2});check(review.choice(4)==2&&review.choice(0)==0,"Rectangles assign Building only inside chosen region");review.addRegion({2,2,2,2,1});check(review.choice(8)==1,"Last rectangle wins");review.undoRegion();check(review.choice(8)==2,"Undo rectangle restores earlier preference");check(review.toJson().value("regions").toArray().size()==1,"Review regions persist as explicit grid/global rectangles");review.clearRegions();review.fallback=1;check(review.choice(8)==1,"Unmarked overlap preference is explicit");refuses([&]{review.addRegion({0,0,3,3,1});});
        const auto settings=dfmRunSettings({},.25);auto restored=dfmRunSettings(settings.toJson(),1);check(restored.parameters.dfmMinimumMerge&&restored.parameters.dfmFill.method==DfmFillMethod::LocalMean&&restored.parameters.gridStep==.25&&restored.parameters.requireConflictReview,"DFM recipe/settings round trip with explicit overlap review");
    }
    const auto screenshots=qEnvironmentVariable("QAL_TEST_SCREENSHOTS");if(!screenshots.isEmpty()){
        QDir().mkpath(screenshots);auto capture=[&](const QString& file){QTimer::singleShot(0,[&,file](){auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;dialog->resize(540,760);QApplication::processEvents();check(dialog->grab().save(QDir(screenshots).filePath(file)),"render dialog fixture");dialog->reject();});};
        capture("point_filter_dialog.png");QJsonArray rules;bool work=false,restore=false;pointSelectionDialog(semantics,rules,work,restore,nullptr);capture("automatic_class_parameters.png");SemanticParameters parameters;semanticParametersDialog(parameters,nullptr);capture("dfm_recipe_settings.png");auto settings=dfmRunSettings({},.25);dfmSettingsDialog(settings,nullptr);
        std::vector<double> ground(400,0),building(400,std::numeric_limits<double>::quiet_NaN());for(unsigned y=4;y<16;++y)for(unsigned x=5;x<15;++x)building[y*20+x]=5;DfmConflictGrid review;review.width=review.height=20;review.ground=&ground;review.building=&building;review.initialize();review.addRegion({5,4,9,15,2});review.addRegion({10,4,14,8,1});QTimer::singleShot(0,[&](){auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;dialog->resize(950,740);auto* map=dialog->findChild<QGraphicsView*>("ALiS.DFM.ConflictMap");if(map)map->fitInView(map->scene()->sceneRect(),Qt::KeepAspectRatio);QApplication::processEvents();check(dialog->grab().save(QDir(screenshots).filePath("dfm_overlap_review.png")),"render overlap decision map");dialog->reject();});check(!dfmConflictDialog(review,nullptr),"Cancel review never confirms final DFM");
    }
    std::cout<<checks<<" recovery/resume/new-scale/persistence/semantic checks passed"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
