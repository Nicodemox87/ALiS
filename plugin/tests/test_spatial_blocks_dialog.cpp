// SPDX-License-Identifier: GPL-2.0-or-later
#include "SpatialBlocksDialog.h"
#include "ProgressiveFeatureStore.h"
#include <ccPointCloud.h>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QDir>
#include <QTimer>
#include <BinFilter.h>
#include <iostream>
#include <memory>
#include <stdexcept>
int spatialGridRegression();
int spatialDialogRegression(){
    spatialGridRegression();int checks=0;
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
    std::cout<<checks<<" progressive SF / BIN resume / pause-stop / single-copy checks passed\n";return 0;
}
