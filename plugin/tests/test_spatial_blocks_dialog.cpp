// SPDX-License-Identifier: GPL-2.0-or-later
#include "SpatialBlocksDialog.h"
#include "ProgressiveFeatureStore.h"
#include "DiskFeatureSnapshot.h"
#include "AdaptiveFeatureBlocks.h"
#include "BlockPerformance.h"
#include "PlatformPaths.h"
#include <ccPointCloud.h>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QDir>
#include <QTimer>
#include <QProcess>
#include <BinFilter.h>
#include <iostream>
#include <memory>
#include <stdexcept>
int spatialGridRegression();
int adaptiveBlockRegression(){
    int checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};QTemporaryDir folder;check(folder.isValid(),"adaptive test folder");
    ccPointCloud cloud("Adaptive edge fixture");cloud.reserve(1681);for(int y=0;y<=40;++y)for(int x=0;x<=40;++x)cloud.addPoint(CCVector3(x*.1f,y*.1f,.015f*std::sin(x*.4f)*std::cos(y*.3f)));
    const QStringList keys{"neighbor_count@0.37","planarity@0.37","z_minimum@0.37","roughness@0.37"};auto metadata=alis::checkpointMetadata("adaptive-test-identity","B0001",keys);
    QFile spool(folder.filePath("source.xyzid"));check(spool.open(QIODevice::WriteOnly),"adaptive source spool");
    for(unsigned i=0;i<cloud.size();++i){const auto p=cloud.getPoint(i);quint32 row=i;const double v[]={p->x,p->y,p->z};spool.write(reinterpret_cast<const char*>(&row),4);spool.write(reinterpret_cast<const char*>(v),24);}spool.close();
    auto native=[&](const QString& directory,quint64 count,const std::array<double,3>& origin,quint64 budget,std::function<void(int)> update){
        QJsonArray features;for(auto k:keys)features.append(k);QFile request(QDir(directory).filePath("request.json"));check(request.open(QIODevice::WriteOnly),"native adaptive request");request.write(QJsonDocument(QJsonObject{{"directory",directory},{"point_count",double(count)},{"origin",QJsonArray{origin[0],origin[1],origin[2]}},{"features",features},{"cache_bytes",count*keys.size()*4.+1048576.},{"memory_bytes",double(budget)},{"threads",2},{"query_indices_file","query_indices.u32"}}).toJson());request.close();
        QProcess p;p.start(alis::blockWorkerPath(),{request.fileName()});check(p.waitForFinished(60000)&&p.exitCode()==0,qPrintable(QString::fromUtf8(p.readAllStandardError())));update(100);
    };
    alis::AdaptiveCore core{0,0,4,4,true,true};alis::AdaptiveFeatureBlocks runner;runner.folder=folder.path();runner.keys=keys;runner.metadata=metadata;runner.halo=.37;runner.bytesPerPoint=500;runner.ramBudget=[](){return quint64(1073741824);};runner.compute=native;
    const auto baseline=folder.filePath("baseline.alsf"),split=folder.filePath("split.alsf");runner.run(spool.fileName(),core,cloud.size(),baseline);check(runner.result.leaves==1,"baseline unsplit");
    runner.maxContextPoints=800;quint64 previous=0;runner.progress=[&](quint64 n,int){check(n>=previous&&n<=cloud.size(),"adaptive progress monotonic");previous=n;};runner.run(spool.fileName(),core,cloud.size(),split);
    check(runner.result.leaves>1&&runner.result.corePoints==cloud.size(),"adaptive split preserves all core points");check(runner.result.contextPoints>cloud.size(),"halo read per leaf but not stored in output");
    alis::FeatureCheckpointReader a(baseline),b(split);while(a.valid){check(b.valid&&a.row==b.row,"adaptive IDs match monolithic source order");for(int f=0;f<keys.size();++f){float av,bv;std::memcpy(&av,a.record.constData()+4+f*4,4);std::memcpy(&bv,b.record.constData()+4+f*4,4);check((std::isnan(av)&&std::isnan(bv))||std::abs(av-bv)<.001f*std::max(1.f,std::abs(av)),"adaptive features match monolithic worker, including edges/halo");}a.advance();b.advance();}check(!b.valid,"no duplicated halo records");
    auto token=alis::validateFeatureCheckpoint(split,metadata,cloud.size(),cloud.size());check(!token.isEmpty(),"merged checkpoint verifies");
    int ticks=0;runner.tick=[&](){if(++ticks>2)throw std::runtime_error("Stopped by adaptive test");};bool rejected=false;try{runner.run(spool.fileName(),core,cloud.size(),split);}catch(const std::exception&){rejected=true;}check(rejected&&alis::validateFeatureCheckpoint(split,metadata,cloud.size(),cloud.size())==token,"cancel preserves previous atomic output");runner.tick={};runner.progress={};
    bool first=true;runner.maxContextPoints=0;runner.compute=[&](const QString& d,quint64 n,const std::array<double,3>& o,quint64 ram,std::function<void(int)> update){if(first){first=false;throw std::runtime_error("test worker memory pressure");}native(d,n,o,ram,update);};runner.run(spool.fileName(),core,cloud.size(),folder.filePath("retry.alsf"));check(runner.result.leaves>1,"worker memory pressure triggers bounded subdivision");runner.compute=native;
    runner.maxContextPoints=800;runner.halo=10;rejected=false;try{runner.run(spool.fileName(),core,cloud.size(),folder.filePath("impossible.alsf"));}catch(const std::exception& e){rejected=QString::fromUtf8(e.what()).contains("halo");}check(rejected,"pathological fixed halo terminates with explicit memory diagnostic");
    // Stream commit and recover an older BIN's external ledger: no N x K SFs.
    QJsonObject config{{"features",metadata.value("features")}};alis::ProgressiveFeatureStore store(&cloud,"adaptive-test-identity",keys,config,true);store.setOutputFolder(folder.path());store.commitFile("B0001",split,cloud.size(),keys);check(cloud.getNumberOfScalarFields()==0,"adaptive disk commit has zero full-cloud SF allocation");
    cloud.setMetaData(alis::ProgressiveFeatureStore::metadataKey(true),QByteArray());check(alis::ProgressiveFeatureStore::recoverDiskLedger(cloud,folder.path(),"adaptive-test-identity",keys),"disk resume without recent BIN metadata");check(alis::ProgressiveFeatureStore::state(cloud,true).value("blocks").toObject().size()==1,"verified parent recovered without recomputation");
    rejected=false;try{alis::ProgressiveFeatureStore::recoverDiskLedger(cloud,folder.path(),"changed-source",keys);}catch(...){rejected=true;}check(rejected,"resume rejects different source/order/schema");
    QFile corrupt(split);check(corrupt.open(QIODevice::ReadWrite),"open corruption fixture");corrupt.seek(corrupt.size()-1);char last;corrupt.read(&last,1);last^=1;corrupt.seek(corrupt.size()-1);corrupt.write(&last,1);corrupt.close();rejected=false;try{alis::ProgressiveFeatureStore::recoverDiskLedger(cloud,folder.path(),"adaptive-test-identity",keys);}catch(...){rejected=true;}check(rejected,"resume detects altered checkpoint hash");
    for(quint64 available:{quint64(512)*1024*1024,quint64(4)*1073741824,quint64(16)*1073741824,quint64(128)*1073741824}){auto budget=alis::automaticWorkerBudget(available);check(budget<available&&budget<=quint64(24)*1073741824&&budget<=available/2,"auto RAM reserves system/other-app headroom");auto manual=alis::boundedManualWorkerBudget(available,quint64(64)*1073741824);check(manual<available&&manual<=available*3/4,"manual RAM still reserves headroom");}
    for(int logical:{0,1,2,4,8,16,32,64,128}){auto threads=alis::recommendedFeatureThreads(logical);check(threads>=1&&threads<=24&&threads<=alis::maximumFeatureThreads(logical),"hardware-aware CPU default bounded");check(alis::maximumFeatureThreads(logical)<=32,"CPU maximum bounded");}
    check(alis::recommendedFeatureThreads(32)==24,"32-thread workstation defaults to 24 feature threads");check(alis::automaticWorkerBudget(quint64(64)*1073741824)==quint64(24)*1073741824,"large workstation may use 24 GiB worker ceiling");
    {alis::AdaptiveFeatureBlocks model;model.keys=QStringList{"fixture"};model.bytesPerPoint=1616;model.workerThreads=24;for(quint64 ram:{quint64(512)*1024*1024,quint64(1)*1073741824,quint64(10)*1073741824}){auto n=model.capacity(ram);const long double estimate=268435456.L+static_cast<long double>(n)*1616+24.L*std::min<quint64>(n,200000)*96;check(estimate<=ram,"adaptive capacity accounts separately for bounded per-thread scratch");}check(model.capacity(quint64(10)*1073741824)>4607419,"10 GiB safely supports representative context without artificial per-point scratch multiplication");}
    check(alis::suggestedGridSide(1e8,1e6,4,4000,quint64(4)*1073741824)<=100,"auto grid bounded");
    alis::BlockTimeEstimator time;time.add(353,10000,5.7);check(!time.ready(),"tiny boundary pilot cannot bias ETA");time.add(1e6,10000,120);check(time.ready()&&std::abs(time.remaining(alis::blockWorkUnits(2e6,20000))-240)<1e-6,"density-scaled measured remaining time");check(time.lowFactor()<1&&time.highFactor()>1,"ETA displays uncertainty");
    std::cout<<checks<<" adaptive subdivision / native equivalence / bounded merge / recovery / ETA checks passed\n";return checks;
}
int adaptiveAllFeaturesRegression(){
    int checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    const QStringList names=QString("neighbor_count density_2d density_3d eigenvalue_1 eigenvalue_2 eigenvalue_3 eigenvalues_sum pca_1 pca_2 linearity planarity sphericity anisotropy omnivariance eigenentropy surface_variation verticality normal_x normal_y normal_z normal_z_absolute dip dip_direction roughness signed_roughness mean_curvature gaussian_curvature normal_change_rate moment_order_1 barycenter_offset_ratio z_minimum z_maximum z_range z_mean z_standard_deviation z_above_minimum z_below_maximum z_relative_to_mean z_percentile_10 z_percentile_25 z_median z_percentile_75 z_percentile_90").split(' ');
    QStringList keys;for(auto name:names)for(double radius:{.5,1.,2.,4.})keys<<QString("%1@%2").arg(name).arg(radius);check(keys.size()==172,"full 43 x 4 feature schema");
    QTemporaryDir folder;QFile spool(folder.filePath("source.xyzid"));check(spool.open(QIODevice::WriteOnly),"full feature spool");quint32 index=0;
    for(int y=0;y<=80;++y)for(int x=0;x<=80;++x){const double p[]={double(float(x*.47f)),double(float(y*.47f)),double(float(.15*std::sin(x*.12)*std::cos(y*.17)))};spool.write(reinterpret_cast<const char*>(&index),4);spool.write(reinterpret_cast<const char*>(p),24);++index;}spool.close();
    alis::AdaptiveFeatureBlocks runner;runner.folder=folder.path();runner.keys=keys;runner.metadata=alis::checkpointMetadata("all-feature-test","B0001",keys);runner.halo=4;runner.bytesPerPoint=4000;runner.ramBudget=[](){return quint64(1073741824);};
    runner.compute=[&](const QString& directory,quint64 n,const std::array<double,3>& origin,quint64 ram,std::function<void(int)> update){QJsonArray a;for(auto key:keys)a.append(key);QFile request(QDir(directory).filePath("request.json"));check(request.open(QIODevice::WriteOnly),"full feature request");request.write(QJsonDocument(QJsonObject{{"directory",directory},{"point_count",double(n)},{"origin",QJsonArray{origin[0],origin[1],origin[2]}},{"features",a},{"cache_bytes",n*keys.size()*4.+1048576.},{"memory_bytes",double(ram)},{"threads",2},{"query_indices_file","query_indices.u32"}}).toJson());request.close();QProcess p;p.start(alis::blockWorkerPath(),{request.fileName()});check(p.waitForFinished(60000)&&p.exitCode()==0,qPrintable(QString::fromUtf8(p.readAllStandardError())));update(100);};
    const alis::AdaptiveCore core{0,0,double(float(80*.47f)),double(float(80*.47f)),true,true};runner.run(spool.fileName(),core,index,folder.filePath("whole.alsf"));runner.maxContextPoints=4000;runner.run(spool.fileName(),core,index,folder.filePath("split.alsf"));check(runner.result.leaves>1,"172-field forced adaptive subdivision");
    alis::FeatureCheckpointReader a(folder.filePath("whole.alsf")),b(folder.filePath("split.alsf"));while(a.valid){check(b.valid&&a.row==b.row,"172-field ownership/order preserved");for(int f=0;f<keys.size();++f){float av,bv;std::memcpy(&av,a.record.constData()+4+f*4,4);std::memcpy(&bv,b.record.constData()+4+f*4,4);if(!((std::isnan(av)&&std::isnan(bv))||std::abs(av-bv)<.001f*std::max(1.f,std::abs(av))))throw std::runtime_error(QString("Full-schema mismatch: %1 point %2 (%3 vs %4)").arg(keys[f]).arg(a.row).arg(av).arg(bv).toStdString());++checks;}a.advance();b.advance();}check(!b.valid,"172-field halo excluded");
    std::cout<<checks<<" all 172 geometric fields at 0.5 / 1 / 2 / 4 m: monolithic-adaptive checks passed\n";return checks;
}
int spatialDialogRegression(){
    spatialGridRegression();adaptiveBlockRegression();adaptiveAllFeaturesRegression();int checks=0;
    auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    auto fixture=[](){auto c=std::make_unique<ccPointCloud>("Progressive test");c->reserve(1681);for(int y=0;y<=40;++y)for(int x=0;x<=40;++x)c->addPoint(CCVector3(x*.1f,y*.1f,0));c->setGlobalShift(CCVector3d(-440000,-4135000,0));int k=c->addScalarField("Classification");c->getScalarField(k)->fill(2);c->getScalarField(k)->computeMinAndMax();return c;};
    const QStringList keys{"neighbor_count@0.37","planarity@0.37"};const QVector<double> radii{.37};
    QTemporaryDir folder;check(folder.isValid(),"test output folder");auto source=fixture();QString failure;int publications=0;std::unique_ptr<ccPointCloud> copied,training;
    auto open=[&](ccPointCloud* data,const std::function<void(QDialog*,std::function<QPushButton*(QString)>)>& body){
        QTimer driver;driver.setInterval(30);QObject::connect(&driver,&QTimer::timeout,[&](){auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!d||d->objectName()!="ALiS.SpatialBlocks")return;driver.stop();
            auto button=[&](QString name){for(auto* b:d->findChildren<QPushButton*>())if(b->text()==name)return b;throw std::runtime_error(QString("Missing button %1").arg(name).toStdString());};
            try{body(d,button);}catch(const std::exception& e){failure=e.what();d->reject();}});driver.start();
        std::cerr<<"[spatial-tests] opening progressive dialog"<<std::endl;
        alis::showSpatialBlocks(data,keys,radii,nullptr,[&](ccPointCloud* c){++publications;copied.reset(c);},[&](ccPointCloud* c){training.reset(c);});check(failure.isEmpty(),qPrintable(failure));
    };
    auto selectOne=[&](QDialog* d,const std::function<QPushButton*(QString)>& b){auto spins=d->findChildren<QDoubleSpinBox*>();check(spins.size()==2,"grid and worker controls");spins[0]->setValue(2);b("Rebuild grid")->click();b("Deselect all")->click();check(!b("Compute SFs on selected blocks")->isEnabled(),"empty selection guarded");auto* map=d->findChild<QWidget*>("ALiS.SpatialBlocks.Map");check(map!=nullptr,"native map");const double aw=map->width()-80,ah=map->height()-55,a=std::min(aw,ah);QPointF p(65+(aw-a)/2+a*.25,12+(ah-a)/2+a*.25);QMouseEvent click(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(map,&click);check(b("Compute SFs on selected blocks")->isEnabled(),"map click selects core");};
    const QString partial=folder.filePath("partial.bin");
    open(source.get(),[&](QDialog* d,std::function<QPushButton*(QString)> b){
        auto* profiles=d->findChild<QComboBox*>("ALiS.SpatialBlocks.SpeedProfile");check(profiles&&profiles->count()==3,"three explicit speed profiles");
        profiles->setCurrentIndex(2);check(d->findChild<QPushButton*>("ALiS.SpatialBlocks.QuickEstimate")!=nullptr,"quick preflight available");
        b("Refresh quick time estimate")->click();check(d->findChild<QLabel*>("ALiS.SpatialBlocks.TimeEstimate")->text().contains(" h "),"preflight displays hh/mm before production");
        check(source->getNumberOfScalarFields()==1&&alis::ProgressiveFeatureStore::state(*source).isEmpty(),"calibration creates no production SF/state");profiles->setCurrentIndex(0);
        d->findChild<QLineEdit*>("ALiS.SpatialBlocks.Output")->setText(folder.filePath("current"));selectOne(d,b);bool tick=false;QTimer::singleShot(0,[&](){tick=true;});b("Benchmark 1 selected block")->click();
        check(tick,"GUI heartbeat during isolated worker");check(publications==0,"current-cloud mode never publishes another geometry");check(source->size()==1681&&source->getNumberOfScalarFields()==3,"same full-size source gains exactly two SFs");
        auto state=alis::ProgressiveFeatureStore::state(*source);check(state.value("blocks").toObject().size()==1,"one completed core");const auto fields=state.value("fields").toObject();check(fields.size()==2,"owned feature schema");const int sf=source->getScalarFieldIndexByName(fields.value(keys[0]).toString().toUtf8().constData());unsigned nan=0;for(unsigned i=0;i<source->size();++i){check(source->getScalarField(0)->getValue(i)==2,"classes untouched");nan+=std::isnan(source->getScalarField(sf)->getValue(i));}check(nan>0&&nan<source->size(),"unprocessed source rows remain NaN");
        check(QDir(folder.filePath("current")).entryList({"*.bin"},QDir::Files).isEmpty(),"no per-block BIN geometry");check(QDir(folder.filePath("current")).entryList({"*.alsf"},QDir::Files).size()==1,"one compact checkpoint");
        BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;check(bin.saveToFile(source.get(),partial,sp)==CC_FERR_NO_ERROR,"save partial cloud and metadata as BIN");
        check(!alis::ProgressiveFeatureStore::compatible(*source,"wrong row fingerprint",keys),"wrong geometry/order identity not reused");check(!alis::ProgressiveFeatureStore::compatible(*source,state.value("identity").toString(),{"planarity@1"}),"changed schema not reused");
        b("Select all")->click();QTimer::singleShot(0,[&](){b("Pause after block")->click();});b("Compute SFs on selected blocks")->click();check(alis::ProgressiveFeatureStore::state(*source).value("blocks").toObject().size()==2,"pause commits exactly current core");
        QTimer::singleShot(0,[&](){b("Stop safely")->click();});b("Compute SFs on selected blocks")->click();check(alis::ProgressiveFeatureStore::state(*source).value("blocks").toObject().size()==2,"stop never marks interrupted core complete");b("Close")->click();
    });
    ccHObject loaded;BinFilter bin;FileIOFilter::LoadParameters lp;lp.alwaysDisplayLoadDialog=false;lp.shiftHandlingMode=ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;check(bin.loadFile(partial,loaded,lp)==CC_FERR_NO_ERROR,"reopen partial BIN");ccHObject::Container clouds;loaded.filterChildren(clouds,true,CC_TYPES::POINT_CLOUD);check(clouds.size()==1,"single reopened full cloud");auto* reopened=static_cast<ccPointCloud*>(clouds.front());
    open(reopened,[&](QDialog*,std::function<QPushButton*(QString)> b){check(!b("Benchmark 1 selected block")->isEnabled(),"saved selected completed core skipped after reopening");b("Select all")->click();b("Compute SFs on selected blocks")->click();const auto state=alis::ProgressiveFeatureStore::state(*reopened);check(state.value("blocks").toObject().size()==4,"remaining cores completed on same reopened entity");check(reopened->getNumberOfScalarFields()==3,"resume reuses fields, no duplicate SFs");const auto names=state.value("fields").toObject();for(auto it=names.begin();it!=names.end();++it){auto* f=reopened->getScalarField(reopened->getScalarFieldIndexByName(it.value().toString().toUtf8().constData()));for(unsigned i=0;i<reopened->size();++i)check(std::isfinite(f->getValue(i)),"all four completed plane cores have features");}b("Use selected blocks for training...")->click();});
    check(training&&training->size()==reopened->size(),"explicit training subset remains available");check(publications==0,"no extra geometry for current-cloud processing/resume");
    auto clean=fixture();
    open(clean.get(),[&](QDialog* d,std::function<QPushButton*(QString)> b){d->findChild<QLineEdit*>("ALiS.SpatialBlocks.Output")->setText(folder.filePath("copy"));d->findChild<QComboBox*>("ALiS.SpatialBlocks.Destination")->setCurrentIndex(1);selectOne(d,b);b("Benchmark 1 selected block")->click();check(publications==1&&copied&&copied->size()==clean->size(),"one complete destination copy, not a block clone");check(clean->getNumberOfScalarFields()==1&&!alis::ProgressiveFeatureStore::state(*clean).contains("identity"),"copy mode leaves original SFs and metadata untouched");b("Select all")->click();b("Compute SFs on selected blocks")->click();check(publications==1,"following blocks reuse the single copy");check(alis::ProgressiveFeatureStore::state(*copied).value("blocks").toObject().size()==4,"single copy accumulates every core");b("Close")->click();});
    auto disk=fixture();const QString diskFolder=folder.filePath("disk_only"),diskBin=folder.filePath("disk_partial.bin");
    open(disk.get(),[&](QDialog* d,std::function<QPushButton*(QString)> b){
        d->findChild<QLineEdit*>("ALiS.SpatialBlocks.Output")->setText(diskFolder);d->findChild<QComboBox*>("ALiS.SpatialBlocks.Destination")->setCurrentIndex(2);selectOne(d,b);b("Benchmark 1 selected block")->click();
        check(disk->getNumberOfScalarFields()==1,"disk-only adds NO full-cloud fields");check(publications==1,"disk-only creates no geometry copy");
        const auto s=alis::ProgressiveFeatureStore::state(*disk,true);check(s.value("blocks").toObject().size()==1,"disk-only partial progress persisted");check(QFileInfo::exists(QDir(diskFolder).filePath("disk_features.json")),"disk-only external ledger written");
        BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;check(bin.saveToFile(disk.get(),diskBin,sp)==CC_FERR_NO_ERROR,"BIN saves linked disk job");b("Close")->click();
    });
    ccHObject diskLoaded;check(bin.loadFile(diskBin,diskLoaded,lp)==CC_FERR_NO_ERROR,"linked disk BIN reopens");ccHObject::Container diskClouds;diskLoaded.filterChildren(diskClouds,true,CC_TYPES::POINT_CLOUD);check(diskClouds.size()==1,"linked disk single cloud");auto* diskReopened=static_cast<ccPointCloud*>(diskClouds.front());
    open(diskReopened,[&](QDialog* d,std::function<QPushButton*(QString)> b){
        check(d->findChild<QComboBox*>("ALiS.SpatialBlocks.Destination")->currentIndex()==2,"disk destination restored after BIN reopen");check(!b("Benchmark 1 selected block")->isEnabled(),"disk completed core skipped");check(b("Export unified LAS from disk features...")->isEnabled(),"disk export available on reopen without allocating fields");
        b("Select all")->click();b("Compute SFs on selected blocks")->click();const auto s=alis::ProgressiveFeatureStore::state(*diskReopened,true);check(s.value("blocks").toObject().size()==4,"disk resume completes remaining cores");check(diskReopened->getNumberOfScalarFields()==1,"all disk cores still allocate no new full-cloud SFs");
        alis::writeDiskFeatureSnapshot(*diskReopened,diskFolder,s.value("config").toObject().value("coordinate_sha256").toString());check(QFileInfo(QDir(diskFolder).filePath("export_snapshot.bin")).size()==qint64(diskReopened->size())*28,"snapshot writes bounded XYZ and original class field only");
        const auto python=qEnvironmentVariable("ALIS_TEST_EXPORT_PYTHON"),script=qEnvironmentVariable("ALIS_TEST_EXPORT_SCRIPT");
        if(!python.isEmpty()&&!script.isEmpty()){QProcess p;p.start(python,{script,"--snapshot",QDir(diskFolder).filePath("export_snapshot.json"),"--ledger",QDir(diskFolder).filePath("disk_features.json"),"--output",QDir(diskFolder).filePath("unified.las")});check(p.waitForFinished(60000)&&p.exitCode()==0,qPrintable(QString::fromUtf8(p.readAllStandardError())));check(QFileInfo(QDir(diskFolder).filePath("unified.las")).size()>0,"native checkpoint to Python LAS export integration");}
        b("Close")->click();
    });
    QFile oldLedger(QDir(diskFolder).filePath("disk_features.json"));check(oldLedger.open(QIODevice::ReadOnly),"old schema ledger readable");const auto oldBytes=oldLedger.readAll();oldLedger.close();
    open(diskReopened,[&](QDialog* d,std::function<QPushButton*(QString)> b){
        auto* profile=d->findChild<QComboBox*>("ALiS.SpatialBlocks.SpeedProfile");profile->setCurrentIndex(2);const QString fresh=d->findChild<QLineEdit*>("ALiS.SpatialBlocks.Output")->text();check(QDir(fresh).absolutePath()!=QDir(diskFolder).absolutePath(),"changed speed profile selects independent folder");
        check(b("Compute SFs on selected blocks")->isEnabled(),"different schema does not reuse completed old cores");b("Benchmark 1 selected block")->click();const auto state=alis::ProgressiveFeatureStore::state(*diskReopened,true);check(state.value("config").toObject().value("features").toArray().size()==8,"fast schema records eight exact descriptors at single fixture radius");check(!profile->isEnabled(),"schema locked once job created");check(diskReopened->getNumberOfScalarFields()==1,"fast disk job still adds no full-cloud SFs");
        check(oldLedger.open(QIODevice::ReadOnly)&&oldLedger.readAll()==oldBytes,"old full-profile ledger is unchanged");oldLedger.close();b("Close")->click();
    });
    std::cout<<checks<<" progressive SF / BIN resume / pause-stop / single-copy / disk-only checks passed\n";return 0;
}
