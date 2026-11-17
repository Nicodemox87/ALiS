// SPDX-License-Identifier: GPL-2.0-or-later
#include "SpatialBlocksDialog.h"
#include "SpatialBlockGrid.h"
#include "ProgressiveFeatureStore.h"
#include "FeatureRecovery.h"
#include "DiskFeatureSnapshot.h"
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "MemorySafety.h"
#include "AdaptiveFeatureBlocks.h"
#include "BlockPerformance.h"
#include "DurationFormat.h"
#include "FeatureSpeedProfiles.h"
#include "ReleaseVersion.h"
#include "PlatformPaths.h"
#include <ccMainAppInterface.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <ReferenceCloud.h>
#include <FileIOFilter.h>
#include <BinFilter.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QScrollArea>
#include <QThread>
#include <QStorageInfo>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace alis {
namespace {
enum Status { Pending, Processing, Complete, Failed };
struct Tile { bool selected=true; quint64 count=0,contextPoints=0; Status status=Pending; QString file,error,engineVersion; double seconds=0;int subBlocks=0; };
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void jsonFile(const QString& path,const QJsonObject& object){QSaveFile f(path);const auto bytes=QJsonDocument(object).toJson();require(f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size()&&f.commit(),"Cannot commit job manifest");}
QString idText(int id){return QString("B%1").arg(id+1,4,10,QChar('0'));}
const char* statusText(Status s){return s==Processing?"Processing":s==Complete?"Completed":s==Failed?"Failed":"Not processed";}
QColor statusColor(Status s){return s==Processing?QColor(240,195,45):s==Complete?QColor(40,161,93):s==Failed?QColor(212,65,65):QColor(150,150,150);}

class BlockMap final:public QWidget {
public:
    SpatialBlockGrid grid;
    QVector<Tile> tiles;
    QImage rgb,shade;
    bool hillshade=false,locked=false;
    std::function<void()> changed;
    std::function<void(int)> hover;
    explicit BlockMap(QWidget* p=nullptr):QWidget(p){setMinimumSize(340,300);setMouseTracking(true);setAccessibleName("XY spatial blocks; click a core to select or deselect");}
    QRectF plot()const {const QRectF area(65,12,std::max(1,width()-80),std::max(1,height()-55));const double ratio=std::max(.001,grid.x1-grid.x0)/std::max(.001,grid.y1-grid.y0);double w=area.width(),h=w/ratio;if(h>area.height()){h=area.height();w=h*ratio;}return QRectF(area.left()+(area.width()-w)/2,area.top()+(area.height()-h)/2,w,h);}
    QPointF point(double x,double y)const{const auto p=plot();return QPointF(p.left()+(x-grid.x0)/std::max(.001,grid.x1-grid.x0)*p.width(),p.bottom()-(y-grid.y0)/std::max(.001,grid.y1-grid.y0)*p.height());}
    int at(QPoint p)const{const auto a=plot();if(!a.contains(p))return -1;return grid.cell(grid.x0+(p.x()-a.left())/a.width()*(grid.x1-grid.x0),grid.y0+(a.bottom()-p.y())/a.height()*(grid.y1-grid.y0));}
    void paintEvent(QPaintEvent*)override {
        QPainter p(this);p.fillRect(rect(),palette().base());const QRectF area=plot();p.drawImage(area,hillshade?shade:rgb);
        for(int i=0;i<tiles.size();++i){const auto& t=tiles[i];const QPointF tl=point(grid.left(i),grid.top(i)),br=point(grid.right(i),grid.bottom(i));QRectF box(tl,br);QColor c=statusColor(t.status);c.setAlpha(t.status==Pending?(t.selected?65:20):145);p.fillRect(box,c);p.setPen(QPen(palette().mid().color(),.7));p.drawRect(box);if(t.selected){p.setPen(QPen(palette().highlight().color(),2));p.drawRect(box.adjusted(1,1,-1,-1));}if(box.width()>37&&box.height()>20){p.fillRect(QRectF(box.center().x()-20,box.center().y()-8,40,16),QColor(255,255,255,210));p.setPen(Qt::black);p.drawText(box,Qt::AlignCenter,idText(i));}}
        p.setPen(palette().text().color());for(int k=0;k<=2;++k){const double f=k/2.;p.drawText(QRectF(area.left()+f*area.width()-40,area.bottom()+5,80,18),Qt::AlignCenter,QString::number(grid.x0+f*(grid.x1-grid.x0),'f',1));p.drawText(QRectF(0,area.bottom()-f*area.height()-9,area.left()-6,18),Qt::AlignRight|Qt::AlignVCenter,QString::number(grid.y0+f*(grid.y1-grid.y0),'f',1));}p.drawText(QRectF(area.left(),height()-20,area.width(),18),Qt::AlignCenter,"X / Y (m) - N at top - outline = selected");
        const double length=std::min(grid.side,(grid.x1-grid.x0)/4);const double px=area.width()*length/std::max(.001,grid.x1-grid.x0);p.fillRect(QRectF(area.left()+6,area.top()+6,px+12,30),QColor(255,255,255,225));p.setPen(Qt::black);p.drawLine(QPointF(area.left()+12,area.top()+15),QPointF(area.left()+12+px,area.top()+15));p.drawText(QPointF(area.left()+12,area.top()+30),QString::number(length,'g',3)+" m");
    }
    void mousePressEvent(QMouseEvent* e)override{const int i=at(e->pos());if(!locked&&e->button()==Qt::LeftButton&&i>=0&&i<tiles.size()&&tiles[i].count){tiles[i].selected=!tiles[i].selected;update();if(changed)changed();if(hover)hover(i);}}
    void mouseMoveEvent(QMouseEvent* e)override{const int i=at(e->pos());if(i>=0&&hover)hover(i);}
};

class BlocksDialog final:public QDialog {
public:
    ccPointCloud* cloud; QStringList keys; QVector<double> radii; BlockMap* map;
    QStringList originalKeys;QVector<double> originalRadii;QComboBox* speedProfile;QLabel *featureSummary,*usage,*executionInfo;QPushButton* quickEstimate;QCheckBox* autoEstimate;
    BlockTimeEstimator preflightEstimator;QString calibrationNote;bool calibrating=false,preflightMicro=false;QTimer* estimateDebounce=nullptr;
    double calibrationScanSeconds=0,preflightPointSeconds=0,preflightBlockSeconds=0;
    std::unique_ptr<QTemporaryDir> calibrationScratch;quint64 calibrationSupport=0,calibrationCore=0,calibrationQueries=0;double calibrationArea=16.;std::array<double,3> calibrationOrigin{};
    QDoubleSpinBox *side,*memory;QSpinBox* cpuThreads;QComboBox *view,*destination;QLineEdit* output;
    QCheckBox* autoRam;QPushButton* suggestGrid;BlockTimeEstimator timeEstimator,leafTimeEstimator;quint64 activeCoreDone=0;int activeTile=-1;
    QLabel *counts,*details,*estimate,*haloLabel;QProgressBar* progress;QPlainTextEdit* log;
    QPushButton *run,*pilot,*pause,*stop,*retry,*train,*regrid,*all,*none,*browse,*close,*save,*exportLas,*recover;
    bool active=false,stopping=false,pauseRequested=false;QProcess* worker=nullptr;
    double secondsPerPoint=0,lastElapsedMinutes=0;ccPointCloud* target=nullptr;
    std::unique_ptr<ProgressiveFeatureStore> store;
    QString coordinateHash,identity;bool hasCommitted=false;int hovered=-1;
    bool isCopy=false;
    std::function<void(ccPointCloud*)> publish,training;
    ccPointCloud* trainingResult=nullptr;QString jobFolder;QVector<int> currentJob;quint64 completedPoints=0;QElapsedTimer clock;
    double halo()const{return radii.isEmpty()?0:*std::max_element(radii.begin(),radii.end());}
    quint64 selectedPoints()const{quint64 n=0;for(const auto& t:map->tiles)if(t.selected)n+=t.count;return n;}
    double budget()const{return autoRam->isChecked()?double(automaticWorkerBudget(availableRamBytes())):double(boundedManualWorkerBudget(availableRamBytes(),quint64(memory->value()*1073741824.)));}
    bool diskMode()const{return destination->currentIndex()==2;}
    double pointBytes()const{return 240.+keys.size()*8.;}
    void reject()override{if(!active){if(store)store->setSelection(selection());QDialog::reject();}}
    BlocksDialog(ccPointCloud* c,const QStringList& k,const QVector<double>& r,QWidget* parent):QDialog(parent),cloud(c),keys(k),radii(r),originalKeys(k),originalRadii(r){
        setWindowTitle("ALiS - Spatial blocks");setObjectName("ALiS.SpatialBlocks");resize(1060,800);setWindowModality(Qt::ApplicationModal);
        auto* root=new QVBoxLayout(this);auto* heading=new QLabel("<b>Spatial blocks</b> - "+cloud->getName());root->addWidget(heading);
        auto* note=new QLabel("Selected CloudCompare cloud. Only the XY preview is sampled; processing and training subsets keep all source points. Original labels and scalar fields are preserved.");note->setWordWrap(true);root->addWidget(note);
        auto* body=new QHBoxLayout;root->addLayout(body,1);auto* left=new QVBoxLayout;body->addLayout(left,1);auto* tools=new QHBoxLayout;left->addLayout(tools);
        all=new QPushButton("Select all");none=new QPushButton("Deselect all");view=new QComboBox;view->addItems({"RGB preview","Hillshade preview"});tools->addWidget(all);tools->addWidget(none);tools->addStretch();tools->addWidget(view);
        map=new BlockMap;map->setObjectName("ALiS.SpatialBlocks.Map");left->addWidget(map,1);details=new QLabel("Click a block to select it; hover for point count and status.");details->setWordWrap(true);left->addWidget(details);
        auto* legend=new QHBoxLayout;left->addLayout(legend);for(int i=0;i<4;++i){auto* l=new QLabel(QString("<span style='color:%1'>&#9632;</span> %2").arg(statusColor(Status(i)).name(),statusText(Status(i))));legend->addWidget(l);}
        auto* settings=new QGroupBox("Processing settings");auto* settingsScroll=new QScrollArea;settingsScroll->setWidgetResizable(true);settingsScroll->setFrameShape(QFrame::NoFrame);settingsScroll->setMinimumWidth(300);settingsScroll->setMaximumWidth(380);settingsScroll->setWidget(settings);body->addWidget(settingsScroll);auto* form=new QFormLayout(settings);form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto prior=ProgressiveFeatureStore::state(*cloud);if(prior.isEmpty())prior=ProgressiveFeatureStore::state(*cloud,true);
        side=new QDoubleSpinBox;side->setRange(1,10000);side->setValue(prior.value("config").toObject().value("block_m").toDouble(50));side->setSuffix(" m");form->addRow("Block side",side);regrid=new QPushButton("Rebuild grid");form->addRow(regrid);
        suggestGrid=new QPushButton("Suggest grid from this PC / cloud");form->addRow(suggestGrid);suggestGrid->setToolTip("Uses currently available RAM, selected feature count, maximum radius and average point density. Existing job grids stay unchanged. Every processing core is also subdivided automatically using exact context counts.");
        speedProfile=new QComboBox;speedProfile->setObjectName("ALiS.SpatialBlocks.SpeedProfile");speedProfile->addItems({"Complete / current selection","Balanced — fewer fields and scales","Fast — compact geometric descriptors"});form->addRow("Speed profile",speedProfile);
        speedProfile->setToolTip("Complete retains Prepare > Features exactly. Balanced/Fast explicitly change feature names and retain only the smallest selected radii. They create an independent job and do NOT overwrite or mix previous checkpoints. No source point is removed. A model must be trained with the chosen schema; speed is not classification accuracy.");
        featureSummary=new QLabel;featureSummary->setWordWrap(true);form->addRow(featureSummary);
        featureSummary->setToolTip("ALiS values are sphere radii, in metres. 3DMASC SC scales are sphere diameters. Every full-resolution core point is described; halo points remain neighbourhood support only. Presets avoid expensive feature families, not just redundant output columns.");
        estimate=new QLabel("Preparing time estimate...");estimate->setWordWrap(true);estimate->setObjectName("ALiS.SpatialBlocks.TimeEstimate");form->addRow(estimate);
        haloLabel=new QLabel(QString("%1 m (automatic, context only)").arg(halo(),0,'g',8));haloLabel->setWordWrap(true);form->addRow("Halo",haloLabel);
        autoRam=new QCheckBox("Automatic RAM and adaptive sub-blocks");autoRam->setObjectName("ALiS.SpatialBlocks.AutoRAM");autoRam->setChecked(true);form->addRow(autoRam);
        autoRam->setToolTip("Uses up to 24 GiB and at most 50% of currently available RAM, leaving at least 2 GiB and 40% of available RAM for CloudCompare, the OS and other applications. Rechecked before every sub-block. Larger safe contexts can reduce halo repetition. Dense cores still split without discarding points or changing radii/halo. This is a conservative estimate, not an OS hard quota.");
        memory=new QDoubleSpinBox;memory->setRange(.1,64);memory->setValue(std::max(.1,double(automaticWorkerBudget(availableRamBytes()))/1073741824.));memory->setSuffix(" GiB");memory->setEnabled(false);form->addRow("Worker RAM ceiling",memory);
        memory->setToolTip("A ceiling, not RAM forced into use. Manual requests remain capped by free RAM, retaining at least 2 GiB and 25% of currently available RAM. More memory helps only when it avoids subdivision or repeated context processing. Never allocates full-cloud scalar fields in disk-only mode.");
        cpuThreads=new QSpinBox;cpuThreads->setObjectName("ALiS.SpatialBlocks.CPUThreads");cpuThreads->setRange(1,maximumFeatureThreads(QThread::idealThreadCount()));cpuThreads->setValue(QSettings().value("ALiS/SpatialBlocks/CPUThreads",recommendedFeatureThreads(QThread::idealThreadCount())).toInt());form->addRow("CPU threads",cpuThreads);
        cpuThreads->setToolTip("Parallel feature calculation inside one isolated worker. Default: up to 24 threads, leaving at least two logical processors available. More threads require extra neighbourhood scratch RAM and do not guarantee a proportional speed gain. Test a representative block. This is CPU processing; no GPU feature backend is claimed.");
        counts=new QLabel;counts->setWordWrap(true);form->addRow(counts);
        pilot=new QPushButton("Benchmark 1 selected block");form->addRow(pilot);
        pilot->setToolTip("Measures one representative selected, unfinished block near the median point count. Results are retained. Remaining/total time is a wide measured estimate that updates during processing; no fake fixed hardware throughput is assumed.");estimate->setObjectName("ALiS.SpatialBlocks.TimeEstimate");
        autoEstimate=new QCheckBox("Estimate automatically before processing");autoEstimate->setChecked(true);autoEstimate->setObjectName("ALiS.SpatialBlocks.AutoEstimate");form->addRow(autoEstimate);
        quickEstimate=new QPushButton("Refresh quick time estimate");quickEstimate->setObjectName("ALiS.SpatialBlocks.QuickEstimate");form->addRow(quickEstimate);
        quickEstimate->setToolTip("Describes up to 2048 real core queries with COMPLETE neighbourhood support in a small region of this cloud. Keeps no production SFs or checkpoints. Results are cached locally for this cloud/schema/CPU/thread count. Extrapolation is provisional; a full pilot block gives a better estimate. No context truncation; very dense contexts may require a full pilot instead.");
        executionInfo=new QLabel("Execution: sequential blocks; multithreaded CPU worker. GPU / concurrent-block processing is not enabled without a validated benefit.");executionInfo->setWordWrap(true);form->addRow(executionInfo);
        destination=new QComboBox;destination->setObjectName("ALiS.SpatialBlocks.Destination");destination->addItems({"Current cloud (add/update SFs)","One new full-cloud copy","Disk only (bounded RAM; linked features)"});destination->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);destination->setMinimumContentsLength(16);destination->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        if(!ProgressiveFeatureStore::state(*cloud,true).isEmpty())destination->setCurrentIndex(2);
        form->addRow("Feature destination",destination);
        recover=new QPushButton("Recover saved blocks as Scalar Fields...");recover->setObjectName("ALiS.SpatialBlocks.Recover");form->addRow(recover);
        recover->setToolTip("Recover existing blocks.json + ALSF into this exact cloud, without computing features. Validates geometry/order, schema, each checkpoint and unique IDs first. Existing fields are preserved. Save a BIN after recovery.");
        connect(recover,&QPushButton::clicked,this,[this](){const auto folder=QFileDialog::getExistingDirectory(this,"Recover existing feature blocks — no calculation",output->text());if(!folder.isEmpty())recoverFolder(folder);});
        usage=new QLabel;usage->setWordWrap(true);form->addRow(usage);
        save=new QPushButton("Save cloud + progress (BIN)...");form->addRow(save);
        exportLas=new QPushButton("Export unified LAS from disk features...");form->addRow(exportLas);
        train=new QPushButton("Use selected blocks for training...");train->setToolTip("Creates a full-resolution core-only subset with current class fields. Then choose/certify its class field in Classification and train your models. No halo rows are added; labels are not automatically trusted.");form->addRow(train);
        auto* warning=new QLabel("This panel processes a loaded cloud. Model-driven LAS/LAZ disk streaming remains in Classification > Prepare data for this model. Training starts in Classification after confirming labels.");warning->setWordWrap(true);form->addRow(warning);
        auto* path=new QHBoxLayout;root->addLayout(path);path->addWidget(new QLabel("Output folder"));output=new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)+"/ALiS_Block_Tests/"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));output->setObjectName("ALiS.SpatialBlocks.Output");path->addWidget(output,1);browse=new QPushButton("Browse...");path->addWidget(browse);
        log=new QPlainTextEdit;log->setReadOnly(true);log->setMaximumBlockCount(250);log->setMaximumHeight(95);root->addWidget(log);progress=new QProgressBar;progress->setRange(0,100);root->addWidget(progress);
        auto* actions=new QHBoxLayout;root->addLayout(actions);run=new QPushButton("Compute SFs on selected blocks");run->setDefault(true);pause=new QPushButton("Pause after block");stop=new QPushButton("Stop safely");retry=new QPushButton("Retry failed");close=new QPushButton("Close");for(auto* b:{run,pause,stop,retry,close})actions->addWidget(b);
        connect(close,&QPushButton::clicked,this,&BlocksDialog::reject);connect(browse,&QPushButton::clicked,this,[this](){auto p=QFileDialog::getExistingDirectory(this,"Choose output folder",output->text());if(!p.isEmpty())output->setText(p);});
        connect(all,&QPushButton::clicked,this,[this](){for(auto& t:map->tiles)t.selected=t.count>0;if(store)store->setSelection(selection());refresh();});connect(none,&QPushButton::clicked,this,[this](){for(auto& t:map->tiles)t.selected=false;if(store)store->setSelection(selection());refresh();});
        map->changed=[this](){if(store)store->setSelection(selection());refresh();};map->hover=[this](int i){hovered=i;updateDetail();};
        connect(view,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){map->hillshade=i==1;map->update();});connect(regrid,&QPushButton::clicked,this,[this](){rebuild();});
        connect(autoRam,&QCheckBox::toggled,this,[this](){refresh();});
        connect(suggestGrid,&QPushButton::clicked,this,[this](){const auto& g=map->grid;side->setValue(suggestedGridSide(cloud->size(),(g.x1-g.x0)*(g.y1-g.y0),halo(),pointBytes(),quint64(budget())));rebuild();});
        connect(destination,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](){restoreProgress();refresh();});
        connect(save,&QPushButton::clicked,this,[this](){saveCloud();});
        connect(exportLas,&QPushButton::clicked,this,[this](){exportDiskLas();});
        connect(memory,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](){timeEstimator.samples.clear();leafTimeEstimator.samples.clear();refresh();});connect(run,&QPushButton::clicked,this,[this](){start(false,false);});connect(pilot,&QPushButton::clicked,this,[this](){start(true,false);});connect(retry,&QPushButton::clicked,this,[this](){start(false,true);});
        connect(cpuThreads,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int value){QSettings().setValue("ALiS/SpatialBlocks/CPUThreads",value);timeEstimator.samples.clear();leafTimeEstimator.samples.clear();refresh();});
        estimateDebounce=new QTimer(this);estimateDebounce->setSingleShot(true);estimateDebounce->setInterval(500);connect(estimateDebounce,&QTimer::timeout,this,[this](){if(!active&&autoEstimate->isChecked())calibrateTime(false);});
        connect(speedProfile,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index){applySpeedProfile(index);});
        connect(quickEstimate,&QPushButton::clicked,this,[this](){calibrateTime(true);});
        connect(cpuThreads,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](){loadTimeCalibration();scheduleEstimate();});
        connect(autoEstimate,&QCheckBox::toggled,this,[this](bool enabled){if(enabled)scheduleEstimate();else estimateDebounce->stop();});
        connect(pause,&QPushButton::clicked,this,[this](){pauseRequested=true;pause->setEnabled(false);log->appendPlainText("Pause requested: the current block will finish and be saved.");});
        connect(stop,&QPushButton::clicked,this,[this](){stopping=true;if(worker)worker->kill();log->appendPlainText("Stopping only the isolated block worker. Completed files are retained.");});
        connect(train,&QPushButton::clicked,this,[this](){makeTraining();});
        if(prior.isEmpty()){CCVector3d lo,hi;if(cloud->getOwnGlobalBB(lo,hi))side->setValue(suggestedGridSide(cloud->size(),(hi.x-lo.x)*(hi.y-lo.y),halo(),pointBytes(),quint64(budget())));}rebuild();loadTimeCalibration();scheduleEstimate();
        auto* updateClock=new QTimer(this);updateClock->setInterval(2000);connect(updateClock,&QTimer::timeout,this,[this](){refresh();});updateClock->start();
    }
    QString calibrationCacheKey()const {
        QJsonArray fields;for(const auto& k:keys)fields.append(k);
        // Bump this algorithm tag when native feature mathematics/search changes.
        const QJsonObject tag{{"engine","native-core-query-timing-v2"},{"coordinates",coordinateHash},{"fields",fields},{"threads",cpuThreads->value()},{"logical",QThread::idealThreadCount()},{"cpu",qEnvironmentVariable("PROCESSOR_IDENTIFIER")}};
        return "ALiS/BlockTimeCalibration/"+QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(tag).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    }
    void scheduleEstimate(){
        // Headless regression tests drive calibration explicitly, never via timer.
        if(estimateDebounce&&!active&&autoEstimate->isChecked()&&preflightEstimator.samples.empty()&&qEnvironmentVariable("QT_QPA_PLATFORM")!="minimal"&&qEnvironmentVariable("ALIS_DISABLE_AUTO_PREFLIGHT")!="1")estimateDebounce->start();
    }
    void loadTimeCalibration(){
        preflightEstimator.samples.clear();calibrationNote.clear();preflightMicro=false;preflightPointSeconds=preflightBlockSeconds=0;
        const auto cached=QJsonDocument::fromJson(QSettings().value(calibrationCacheKey()).toByteArray()).object();
        const double units=cached.value("units").toDouble(),seconds=cached.value("seconds").toDouble();
        if(std::isfinite(units)&&std::isfinite(seconds)&&units>0&&seconds>0){preflightEstimator.samples.push_back({units,seconds,cached.value("queries").toDouble()});preflightMicro=true;preflightPointSeconds=cached.value("point_seconds").toDouble();preflightBlockSeconds=cached.value("block_seconds").toDouble();calibrationNote="Quick local calibration (cached); provisional extrapolation";}
        // A previous complete block is stronger evidence than a microbenchmark.
        QFile manifestFile(QDir(output->text()).filePath("blocks.json"));
        if(manifestFile.open(QIODevice::ReadOnly)){const auto prior=QJsonDocument::fromJson(manifestFile.readAll()).object();
            if(prior.value("identity").toString()==identity&&prior.value("cpu_threads").toInt()==cpuThreads->value()){
                BlockTimeEstimator measured;for(auto value:prior.value("blocks").toArray()){const auto row=value.toObject();const auto version=row.value("engine_version").toString();if(row.value("status").toString()=="Completed"&&(version=="0.1.0-alpha.5.13"||version==ALIS_RELEASE_VERSION)){
                    bool ok=false;const int id=row.value("id").toString().mid(1).toInt(&ok)-1;if(ok&&id>=0&&id<map->tiles.size())measured.add(row.value("core_points").toDouble(),(map->grid.right(id)-map->grid.left(id))*(map->grid.top(id)-map->grid.bottom(id)),row.value("seconds").toDouble());
                }}if(measured.ready()){preflightEstimator=measured;preflightMicro=false;calibrationNote="Previous full blocks, same cloud/schema/CPU threads";}
            }
        }
        refresh();
    }
    void applySpeedProfile(int index){
        if(active||store)return;estimateDebounce->stop();const auto profile=featureSpeedProfile(index,originalKeys,originalRadii);keys=profile.keys;radii=profile.radii;
        identity=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(configuration()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        for(auto& tile:map->tiles){tile.status=Pending;tile.file.clear();tile.error.clear();tile.seconds=0;tile.subBlocks=0;tile.contextPoints=0;tile.engineVersion.clear();}
        timeEstimator.samples.clear();leafTimeEstimator.samples.clear();lastElapsedMinutes=0;restoreProgress();
        if(QFileInfo::exists(QDir(output->text()).filePath("blocks.json"))){QFile old(QDir(output->text()).filePath("blocks.json"));old.open(QIODevice::ReadOnly);if(QJsonDocument::fromJson(old.readAll()).object().value("identity").toString()!=identity){
            const QFileInfo previous(output->text());output->setText(QDir(previous.absolutePath()).filePath(previous.fileName()+"_"+profile.id+"_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")));
            log->appendPlainText("Changed feature/radius schema: NEW independent output folder selected. Previous checkpoints remain unchanged and cannot be mixed with this profile.");
        }}loadTimeCalibration();scheduleEstimate();
    }
    void prepareCalibrationContext(){
        if(calibrationScratch)return;
        QElapsedTimer scanTimer;scanTimer.start();
        QVector<int> candidates;for(int i=0;i<map->tiles.size();++i)if(map->tiles[i].selected&&map->tiles[i].count)candidates<<i;
        require(!candidates.isEmpty(),"Select at least one nonempty block for a time estimate");std::sort(candidates.begin(),candidates.end(),[this](int a,int b){return map->tiles[a].count<map->tiles[b].count;});const int tile=candidates[candidates.size()/2];
        const auto& g=map->grid;const double centerX=(g.left(tile)+g.right(tile))/2,centerY=(g.bottom(tile)+g.top(tile))/2;
        double best=std::numeric_limits<double>::infinity();CCVector3d center;const unsigned stride=std::max(1u,cloud->size()/20000u);
        for(unsigned i=0;i<cloud->size();i+=stride){const auto p=cloud->toGlobal3d(*cloud->getPoint(i));if(g.cell(p.x,p.y)!=tile)continue;const double distance=(p.x-centerX)*(p.x-centerX)+(p.y-centerY)*(p.y-centerY);if(distance<best){best=distance;center=p;}}
        require(std::isfinite(best),"No representative sample found in selection; use a full pilot block");
        const double supportRadius=originalRadii.isEmpty()?halo():*std::max_element(originalRadii.begin(),originalRadii.end());
        const double half=2.,extent=half+supportRadius;auto scratch=std::unique_ptr<QTemporaryDir>(new QTemporaryDir);require(scratch->isValid(),"Cannot create preflight scratch folder");
        QFile xyz(scratch->filePath("xyz.f64"));require(xyz.open(QIODevice::WriteOnly),"Cannot write preflight support");QByteArray buffer;QVector<quint32> own;quint64 n=0;
        const auto cap=std::min<quint64>(500000,quint64(std::max(0.,budget()-268435456.)/(240.+originalKeys.size()*8.+cpuThreads->value()*96.)));
        require(cap>=1000,"Insufficient RAM for quick calibration; free RAM or use a full pilot block");
        for(unsigned i=0;i<cloud->size();++i){require(!stopping,"Time estimation cancelled; production checkpoints unchanged");const auto p=cloud->toGlobal3d(*cloud->getPoint(i));
            if(std::abs(p.x-center.x)<=extent&&std::abs(p.y-center.y)<=extent){require(n<cap,"Full calibration halo exceeds safe RAM bound; no neighbours discarded. Use a full pilot block instead.");const double record[]={p.x,p.y,p.z};buffer.append(reinterpret_cast<const char*>(record),24);if(std::abs(p.x-center.x)<=half&&std::abs(p.y-center.y)<=half)own<<quint32(n);++n;if(buffer.size()>=1048576){require(xyz.write(buffer)==buffer.size(),"Preflight coordinate write failed");buffer.clear();}}
            if(i%131072==0){progress->setValue(int(100.*i/cloud->size()));progress->setFormat("Preparing exact calibration support: %p%");QApplication::processEvents();}
        }
        require(xyz.write(buffer)==buffer.size(),"Preflight coordinate write failed");xyz.close();require(!own.isEmpty(),"Empty calibration core; use a full pilot block");
        QFile query(scratch->filePath("query_indices.u32"));require(query.open(QIODevice::WriteOnly),"Cannot write calibration queries");const int count=std::min(2048,own.size());for(int q=0;q<count;++q){const auto index=own[int(quint64(q)*own.size()/count)];require(query.write(reinterpret_cast<const char*>(&index),4)==4,"Preflight query write failed");}query.close();
        calibrationSupport=n;calibrationCore=own.size();calibrationQueries=count;calibrationOrigin={{center.x,center.y,center.z}};calibrationScratch=std::move(scratch);calibrationScanSeconds=scanTimer.nsecsElapsed()/1.e9;
    }
    void calibrateTime(bool force){
        if(active||keys.isEmpty()||!selectedPoints())return;if(!force&&preflightEstimator.ready())return;
        active=true;calibrating=true;stopping=false;refresh();QElapsedTimer wall;wall.start();
        try{prepareCalibrationContext();require(QFileInfo::exists(blockWorkerPath()),"Native worker is not installed");const auto directory=calibrationScratch->path();QJsonArray ff;for(auto k:keys)ff.append(k);
            const quint64 allowance=std::min<quint64>(quint64(budget()),quint64(2)*1073741824);require(availableRamBytes()>allowance+536870912,"Available RAM changed; quick estimate postponed");
            const auto request=QDir(directory).filePath("request.json");jsonFile(request,{{"directory",directory},{"point_count",double(calibrationSupport)},{"origin",QJsonArray{calibrationOrigin[0],calibrationOrigin[1],calibrationOrigin[2]}},{"features",ff},{"cache_bytes",calibrationSupport*keys.size()*4.+1048576.},{"memory_bytes",double(allowance)},{"threads",cpuThreads->value()},{"query_indices_file","query_indices.u32"}});
            QProcess process;worker=&process;process.setWorkingDirectory(QCoreApplication::applicationDirPath());process.setProcessChannelMode(QProcess::MergedChannels);QElapsedTimer timer;timer.start();process.start(blockWorkerPath(),{request});require(process.waitForStarted(5000),"Cannot start time calibration");QByteArray diagnostics;
            while(process.state()!=QProcess::NotRunning){process.waitForReadyRead(60);diagnostics+=process.readAllStandardOutput();progress->setFormat("Measuring selected profile (CPU): %p%");QApplication::processEvents();if(stopping||timer.elapsed()>60000||availableRamBytes()<536870912)process.kill();}
            diagnostics+=process.readAllStandardOutput();worker=nullptr;require(!stopping,"Time estimation cancelled; production outputs unchanged");require(process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0,qPrintable("Time calibration failed: "+QString::fromUtf8(diagnostics).right(2048)));
            QJsonObject timing;for(const auto& line:diagnostics.split('\n'))if(line.startsWith("ALIS_TIMING "))timing=QJsonDocument::fromJson(line.mid(12)).object();
            const double wallSeconds=timer.nsecsElapsed()/1.e9,seconds=timing.value("query_seconds").toDouble(),units=double(calibrationQueries)*(1.+calibrationCore/calibrationArea);
            require(seconds>0&&units>0,"Calibration interval too small; use a full pilot block");preflightEstimator.samples={{units,seconds,double(calibrationQueries)}};
            const double load=timing.value("load_seconds").toDouble(),compute=timing.value("compute_seconds").toDouble(),write=timing.value("output_seconds").toDouble();preflightMicro=true;preflightPointSeconds=std::max(0.,load+compute-seconds+write)/calibrationSupport;preflightBlockSeconds=calibrationScanSeconds+std::max(0.,wallSeconds-load-compute-write);
            const QJsonObject entry{{"units",units},{"seconds",seconds},{"queries",double(calibrationQueries)},{"support_points",double(calibrationSupport)},{"point_seconds",preflightPointSeconds},{"block_seconds",preflightBlockSeconds},{"created_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};QSettings().setValue(calibrationCacheKey(),QJsonDocument(entry).toJson(QJsonDocument::Compact));
            calibrationNote=QString("Quick local calibration: %1 queries / %2 complete support points; provisional").arg(calibrationQueries).arg(calibrationSupport);log->appendPlainText(calibrationNote+". No production SFs/checkpoints created.");
        }catch(const std::exception& e){worker=nullptr;calibrationNote="Time estimate unavailable; benchmark a full selected block";log->appendPlainText(QString("Preflight: ")+e.what());if(!calibrationSupport)calibrationScratch.reset();}
        active=false;calibrating=false;stopping=false;progress->setFormat("%p%");refresh();
    }
    QJsonArray selection()const{QJsonArray a;for(int i=0;i<map->tiles.size();++i)if(map->tiles[i].selected)a.append(i);return a;}
    bool recoverFolder(const QString& folder){
        if(active)return false;estimateDebounce->stop();autoEstimate->setChecked(false);stopping=false;active=true;refresh();
        try{
            log->appendPlainText("Recovering saved blocks: validating all files and original point IDs. No feature calculation is launched.");
            auto tick=[this](){QApplication::processEvents();require(!stopping,"Recovery stopped; completed cores and original files retained");};
            const auto audit=inspectFeatureRecovery(*cloud,folder,coordinateHash,tick);const auto manifest=audit.value("manifest").toObject();const auto config=manifest.value("config").toObject();
            QStringList recoveredKeys;QVector<double> recoveredRadii;for(auto k:config.value("features").toArray())recoveredKeys<<k.toString();for(auto r:config.value("radii").toArray())recoveredRadii<<r.toDouble();
            const quint64 bytes=ProgressiveFeatureStore::compatible(*cloud,manifest.value("identity").toString(),recoveredKeys)?0:quint64(cloud->size())*recoveredKeys.size()*sizeof(ScalarType);
            require(availableRamBytes()>bytes+quint64(2)*1073741824,"Insufficient RAM to attach these scalar fields safely; checkpoint files retained");
            store.reset();keys=recoveredKeys;radii=recoveredRadii;originalKeys=keys;originalRadii=radii;
            {const QSignalBlocker a(side),b(destination),c(speedProfile);side->setValue(config.value("block_m").toDouble());destination->setCurrentIndex(0);speedProfile->setCurrentIndex(0);}
            output->setText(QDir(folder).absolutePath());jobFolder=QDir(folder).absolutePath();active=false;rebuild();active=true;
            require(identity==manifest.value("identity").toString(),"Saved grid/feature configuration does not match this cloud");
            // Preserve the original manifest before installing recovered progress.
            const auto original=QDir(jobFolder).filePath("blocks.json"),backup=QDir(jobFolder).filePath("blocks.before_recovery.json");
            if(!QFileInfo::exists(backup))require(QFile::copy(original,backup),"Cannot preserve original recovery manifest");
            target=cloud;store.reset(new ProgressiveFeatureStore(cloud,identity,keys,config));store->setOutputFolder(jobFolder);
            for(auto value:audit.value("validated").toArray()){
                const auto block=value.toObject();const auto id=block.value("id").toString();
                if(!store->completed().contains(id))store->commitFile(id,block.value("file").toString(),quint64(block.value("rows").toDouble()),keys,tick,true);
                progress->setFormat("Recovering "+id+" — no recalculation");tick();
            }
            store->finalizeRanges();store->setSelection(manifest.value("progress").toObject().value("selection").toArray());
            hasCommitted=true;active=false;store.reset();restoreProgress();target=cloud;
            log->appendPlainText(QString("Recovered %1 scalar fields for %2 original rows. No feature worker was launched. Save cloud + progress (BIN) to preserve them; Compute will only process incomplete cores.").arg(keys.size()).arg(audit.value("covered").toDouble(),0,'f',0));
            cloud->prepareDisplayForRefresh();cloud->showSF(true);if(cloud->getNumberOfScalarFields())cloud->setCurrentDisplayedScalarField(int(cloud->getNumberOfScalarFields())-1);refresh();return true;
        }catch(const std::exception& e){active=false;store.reset();log->appendPlainText(QString("RECOVERY ERROR: ")+e.what());restoreProgress();refresh();return false;}
    }
    QJsonObject configuration()const{QJsonArray ff,rr;for(const auto& k:keys)ff.append(k);for(double r:radii)rr.append(r);const auto& g=map->grid;return {{"engine","alis-geometric-native-v1"},{"coordinate_sha256",coordinateHash},{"point_count",double(cloud->size())},{"block_m",g.side},{"x0",g.x0},{"y0",g.y0},{"x1",g.x1},{"y1",g.y1},{"features",ff},{"radii",rr},{"halo_m",halo()}};}
    void updateDetail(){if(hovered<0||hovered>=map->tiles.size())return;const auto& t=map->tiles[hovered];details->setText(QString("%1 | %2 points | %3 | %4").arg(idText(hovered)).arg(t.count).arg(statusText(t.status),t.error.isEmpty()?(t.selected?"Selected":"Excluded"):t.error));}
    void restoreProgress(){
        if(store)return;for(auto& t:map->tiles){t.status=Pending;t.file.clear();t.error.clear();}
        if(destination->currentIndex()==1||!ProgressiveFeatureStore::compatible(*cloud,identity,keys,diskMode()))return;
        const auto state=ProgressiveFeatureStore::state(*cloud,diskMode());const auto done=state.value("blocks").toObject();
        for(int i=0;i<map->tiles.size();++i)if(done.contains(idText(i))&&done.value(idText(i)).toObject().value("points").toDouble()==map->tiles[i].count){map->tiles[i].status=Complete;map->tiles[i].file=done.value(idText(i)).toObject().value("checkpoint").toString();}
        if(state.contains("selection")){const auto chosen=state.value("selection").toArray();for(auto& t:map->tiles)t.selected=false;for(auto v:chosen){int i=v.toInt(-1);if(i>=0&&i<map->tiles.size())map->tiles[i].selected=map->tiles[i].count>0;}}
        const auto folder=state.value("checkpoint_folder").toString();if(!folder.isEmpty())output->setText(folder);
        QFile previous(QDir(folder).filePath("blocks.json"));if(!folder.isEmpty()&&previous.open(QIODevice::ReadOnly)){const auto manifest=QJsonDocument::fromJson(previous.readAll()).object();if(manifest.value("identity").toString()==identity)for(auto value:manifest.value("blocks").toArray()){const auto b=value.toObject();bool ok=false;const int i=b.value("id").toString().mid(1).toInt(&ok)-1;if(!ok||i<0||i>=map->tiles.size())continue;auto& tile=map->tiles[i];if(double(tile.count)!=b.value("core_points").toDouble())continue;tile.seconds=b.value("seconds").toDouble();tile.subBlocks=b.value("adaptive_leaves").toInt();tile.contextPoints=quint64(b.value("context_points_evaluated").toDouble());tile.engineVersion=b.value("engine_version").toString();if(tile.status!=Complete&&b.value("status").toString()=="Failed"){tile.status=Failed;tile.error=b.value("error").toString();}}}
        log->appendPlainText("Saved progressive job recognised: completed cores restored from this cloud's BIN metadata.");
    }
    void ensureTarget(){
        if(store)return;
        if(diskMode()){
            require(budget()>268435456,"Not enough currently available RAM for a bounded feature worker");
            const auto old=ProgressiveFeatureStore::state(*cloud,true).value("checkpoint_folder").toString();
            const bool ledgerExists=QFileInfo::exists(QDir(jobFolder).filePath("disk_features.json"));
            if(ledgerExists){const auto chosen=selection();log->appendPlainText("Verifying existing disk checkpoints before resume (geometry/order, schema and checksums)...");const bool beforeActive=active;active=true;refresh();try{ProgressiveFeatureStore::recoverDiskLedger(*cloud,jobFolder,identity,keys,[this](){QApplication::processEvents();require(!stopping,"Stopped while validating existing checkpoints");});}catch(...){active=beforeActive;throw;}active=beforeActive;restoreProgress();for(auto& tile:map->tiles)tile.selected=false;for(auto v:chosen){int i=v.toInt(-1);if(i>=0&&i<map->tiles.size())map->tiles[i].selected=map->tiles[i].count>0;}}
            const bool fresh=!ledgerExists&&!old.isEmpty()&&QDir(old).absolutePath()!=QDir(jobFolder).absolutePath();target=cloud;store.reset(new ProgressiveFeatureStore(cloud,identity,keys,configuration(),true,fresh));store->setOutputFolder(jobFolder);store->setSelection(selection());if(fresh)for(auto& tile:map->tiles){tile.status=Pending;tile.file.clear();tile.error.clear();}log->appendPlainText("Disk-only adaptive job: no full-cloud SFs or geometry copies allocated. Keep this output folder with your BIN project.");return;
        }
        const quint64 sfBytes=quint64(cloud->size())*keys.size()*4;
        if(!ProgressiveFeatureStore::compatible(*cloud,identity,keys)&&QFileInfo::exists(QDir(jobFolder).filePath("blocks.json"))){
            require(recoverFolder(jobFolder),"Existing checkpoints were not recovered; no feature worker started");
        }
        const bool reuse=ProgressiveFeatureStore::compatible(*cloud,identity,keys);
        const quint64 extraFields=reuse?0:sfBytes;
        const quint64 copyBytes=destination->currentIndex()==1?quint64(cloud->size())*(64+cloud->getNumberOfScalarFields()*4):0;
        require(availableRamBytes()>extraFields+copyBytes+quint64(budget())+536870912,"Full-cloud fields/copy do not fit in RAM. Choose 'Disk only (bounded RAM; linked features)' under Feature destination. Smaller blocks alone cannot reduce full-cloud SF arrays.");
        std::unique_ptr<ccPointCloud> copy;
        if(destination->currentIndex()==1){copy.reset(cloud->cloneThis(nullptr,true));require(copy&&copy->size()==cloud->size()&&copy->getNumberOfScalarFields()==cloud->getNumberOfScalarFields()&&copy->hasColors()==cloud->hasColors(),"Cannot make a complete single-copy destination");copy->setName(cloud->getName()+"_progressive_features");target=copy.get();isCopy=true;}else target=cloud;
        store.reset(new ProgressiveFeatureStore(target,identity,keys,configuration()));store->setOutputFolder(jobFolder);store->setSelection(selection());
        const auto done=store->completed();for(int i=0;i<map->tiles.size();++i)if(done.contains(idText(i)))map->tiles[i].status=Complete;
        if(copy){require(bool(publish),"A copy destination requires a CloudCompare publication callback");auto* p=copy.release();publish(p);}
        log->appendPlainText(isCopy?"One complete destination copy created. Every following block updates this same entity.":"Progressive SFs attached to the current cloud. Existing class fields and geometry are unchanged.");
    }
    void saveCloud(){
        if(active)return;ccPointCloud* c=target?target:(ProgressiveFeatureStore::compatible(*cloud,identity,keys,diskMode())?cloud:nullptr);if(!c)return;
        QString path=QFileDialog::getSaveFileName(this,"Save cloud and progressive state",QDir(output->text()).filePath("cloud_with_progress.bin"),"CloudCompare BIN (*.bin)");if(path.isEmpty())return;if(!path.endsWith(".bin",Qt::CaseInsensitive))path+=".bin";
        try{if(store)store->setSelection(selection());active=true;stopping=false;refresh();QTemporaryDir temp(QFileInfo(path).absolutePath()+"/.alis_save_XXXXXX");require(temp.isValid(),"Cannot create safe export staging folder");const QString staged=temp.filePath("cloud.bin");BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;sp.parentWidget=this;require(bin.saveToFile(c,staged,sp)==CC_FERR_NO_ERROR,"BIN export failed; existing file was not replaced");QFile in(staged);QSaveFile out(path);require(in.open(QIODevice::ReadOnly)&&out.open(QIODevice::WriteOnly),"Cannot open export destination");while(!in.atEnd()){auto bytes=in.read(1048576);require(!bytes.isEmpty()&&out.write(bytes)==bytes.size(),"BIN export copy failed");progress->setValue(int(100.*in.pos()/std::max<qint64>(1,in.size())));QApplication::processEvents();require(!stopping,"Save canceled; previous destination file retained");}require(out.commit(),"Cannot commit saved BIN");log->appendPlainText("Saved full cloud + progress: "+path+". Reopen this BIN and choose Spatial blocks to continue. Original LAS/LAZ was not overwritten.");}catch(const std::exception& e){log->appendPlainText(QString("ERROR: ")+e.what());}active=false;refresh();
    }
    void exportDiskLas(){
        if(active||!diskMode())return;
        if(!store){try{jobFolder=QDir(output->text()).absolutePath();ensureTarget();}catch(const std::exception& e){log->appendPlainText(QString("ERROR: ")+e.what());return;}}
        const QString path=QFileDialog::getSaveFileName(this,"Export full cloud + disk features (NaN for pending points)",QDir(jobFolder).filePath("cloud_with_features.las"),"LAS with extra scalar fields (*.las)");if(path.isEmpty())return;
        try{
            const auto python=defaultPythonPath();const auto script=QDir(QFileInfo(pythonWorkerPath()).absolutePath()).filePath("alis_feature_export.py");require(!python.isEmpty()&&QFileInfo::exists(script),"Unified export needs the installed ALiS Python runtime (NumPy + laspy). Install the complete runtime first.");
            active=true;stopping=false;refresh();log->appendPlainText("Writing a bounded current-cloud snapshot to disk (all current SFs/RGB/normals; no RAM clone)...");
            writeDiskFeatureSnapshot(*cloud,jobFolder,coordinateHash,[this](quint64 done,quint64 n){progress->setValue(int(100.*done/n));progress->setFormat("Export snapshot: %p%");QApplication::processEvents();require(!stopping,"Snapshot canceled; original cloud and checkpoints retained");});
            QProcess process;worker=&process;process.setProcessChannelMode(QProcess::MergedChannels);QStringList args=pythonIsolationArguments(python);args<<script<<"--snapshot"<<QDir(jobFolder).filePath("export_snapshot.json")<<"--ledger"<<QDir(jobFolder).filePath("disk_features.json")<<"--output"<<path;process.start(python,args);require(process.waitForStarted(5000),"Cannot start bounded LAS exporter");QByteArray pending;
            while(process.state()!=QProcess::NotRunning){process.waitForReadyRead(60);pending+=process.readAllStandardOutput();int nl;while((nl=pending.indexOf('\n'))>=0){const auto line=pending.left(nl);pending.remove(0,nl+1);const auto o=QJsonDocument::fromJson(line).object();if(!o.isEmpty()){const auto total=o.value("total").toDouble();if(total>0)progress->setValue(int(100*o.value("done").toDouble()/total));progress->setFormat(o.value("phase").toString()+": %p%");}else if(!line.trimmed().isEmpty())log->appendPlainText(QString::fromUtf8(line));}QApplication::processEvents();if(stopping||availableRamBytes()<536870912){stopping=true;process.kill();}}
            worker=nullptr;log->appendPlainText(QString::fromUtf8(pending+process.readAllStandardOutput()));require(!stopping&&process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0,"Export stopped/failed: prior target and feature checkpoints are retained. See log.");log->appendPlainText("Export complete: "+path+". All source points retained; pending features are NaN. Keep the .features.json report. The large LAS is NOT loaded into CC automatically.");
        }catch(const std::exception& e){worker=nullptr;log->appendPlainText(QString("ERROR: ")+e.what());}
        active=false;progress->setFormat("%p%");refresh();
    }
    void refresh(){int n=0,done=0,failed=0,pending=0;quint64 remaining=0;for(const auto& t:map->tiles)if(t.selected){++n;done+=t.status==Complete;failed+=t.status==Failed;pending+=t.status==Pending;if(t.status!=Complete)remaining+=t.count;}
        QStringList scales;for(double r:radii)scales<<QString::number(r,'g',8);const auto chosenProfile=featureSpeedProfile(speedProfile->currentIndex(),originalKeys,originalRadii);featureSummary->setText(QString("%1 geometric SFs\nRadii: %2 m\nAll points; exact retained-radius neighbours.").arg(keys.size()).arg(scales.join(" / ")));featureSummary->setToolTip(chosenProfile.description+" ALiS scales are radii, not 3DMASC diameters.");
        haloLabel->setText(QString("%1 m (automatic, context only)").arg(halo(),0,'g',8));
        usage->setText(QString("In-cloud SFs: %1 MiB.\nDisk only: NO full-cloud SF arrays. Checkpoints on disk; export later without loading all fields into CC.").arg(double(cloud->size())*keys.size()*4/1048576.,0,'f',1));
        counts->setText(QString("Selected: %1 / %2 blocks\nCore points: %3\nCompleted: %4 | Failed: %5").arg(n).arg(map->tiles.size()).arg(selectedPoints()).arg(done).arg(failed));
        double work=0;const auto& grid=map->grid;for(int i=0;i<map->tiles.size();++i){const auto& t=map->tiles[i];if(t.selected&&t.status!=Complete){double fraction=1.;if(i==activeTile&&t.count)fraction=std::max(0.,1.-double(activeCoreDone)/t.count);work+=fraction*blockWorkUnits(double(t.count),(grid.right(i)-grid.left(i))*(grid.top(i)-grid.bottom(i)));}}
        const auto& estimator=timeEstimator.ready()?timeEstimator:leafTimeEstimator.ready()?leafTimeEstimator:preflightEstimator;double overhead=0;if(!timeEstimator.ready()&&!leafTimeEstimator.ready()&&preflightMicro)for(const auto& t:map->tiles)if(t.selected&&t.status!=Complete){const double supportFactor=std::pow(1.+2*halo()/std::max(1.,map->grid.side),2);overhead+=preflightBlockSeconds+preflightPointSeconds*t.count*supportFactor;}
        const double elapsed=calibrating?0:active&&clock.isValid()?clock.elapsed()/60000.:lastElapsedMinutes,minutes=(estimator.remaining(work)+overhead)/60.;
        const QString evidence=timeEstimator.ready()?QString("Measured on this PC: %1 map block(s)").arg(estimator.samples.size()):leafTimeEstimator.ready()?QString("Measured on this PC: %1 sub-block(s), provisional").arg(estimator.samples.size()):calibrationNote;
        estimate->setText(estimator.ready()?QString("Elapsed: %1\nRemaining: %2 - %3\nEstimated total (this run): %4 - %5\n%6. Density, halo, disk and CPU load can change time.").arg(formatDurationHoursMinutes(elapsed*60.)).arg(formatDurationHoursMinutes(minutes*estimator.lowFactor()*60.)).arg(formatDurationHoursMinutes(minutes*estimator.highFactor()*60.)).arg(formatDurationHoursMinutes((elapsed+minutes*estimator.lowFactor())*60.)).arg(formatDurationHoursMinutes((elapsed+minutes*estimator.highFactor())*60.)).arg(evidence):calibrating?"Estimating selected options on this cloud...":"Time estimate: waiting for automatic calibration, or benchmark a representative block. No unmeasured fixed speed is assumed.");
        if(!active){const auto total=selectedPoints();progress->setValue(total?int(100.*(total-remaining)/total):0);}
        const bool savedDisk=ProgressiveFeatureStore::compatible(*cloud,identity,keys,true)&&!ProgressiveFeatureStore::state(*cloud,true).value("blocks").toObject().isEmpty();
        run->setEnabled(!active&&(pending||failed)&&keys.size());pilot->setEnabled(!active&&(pending||failed)&&keys.size());retry->setEnabled(!active&&failed&&keys.size());train->setEnabled(!active&&selectedPoints()&&!diskMode());train->setToolTip(diskMode()?"Disk features are external, not SFs on the loaded cloud. Export the LAS to reuse them with model-driven disk preparation. In-cloud training subsets remain available in Current cloud/copy modes.":"Create a core-only training subset with current in-memory scalar fields. Labels are not automatically trusted.");pause->setEnabled(active&&!pauseRequested);stop->setEnabled(active);close->setEnabled(!active);exportLas->setEnabled(!active&&diskMode()&&((store&&!store->completed().isEmpty())||savedDisk));
        recover->setEnabled(!active);for(QWidget* w:QList<QWidget*>{autoRam,cpuThreads,all,none,autoEstimate,quickEstimate})w->setEnabled(!active);speedProfile->setEnabled(!active&&!store);if(calibrating)pause->setEnabled(false);memory->setEnabled(!active&&!autoRam->isChecked());if(autoRam->isChecked()&&!worker){const QSignalBlocker quiet(memory);memory->setValue(std::max(.1,budget()/1073741824.));}for(QWidget* w:QList<QWidget*>{side,regrid,suggestGrid,output,browse,destination})w->setEnabled(!active&&!store);save->setEnabled(!active&&(target||ProgressiveFeatureStore::compatible(*cloud,identity,keys,diskMode())));map->locked=active;updateDetail();map->update();
    }
    void rebuild(){if(active)return;try{CCVector3d lo,hi;require(cloud->getOwnGlobalBB(lo,hi),"Cloud has no finite bounds");const SpatialBlockGrid next(lo.x,lo.y,hi.x,hi.y,side->value());map->grid=next;map->tiles=QVector<Tile>(next.size());progress->setRange(0,100);log->appendPlainText("Counting full-resolution cores and building a sampled XY preview...");
        active=true;refresh();QCryptographicHash coordinates(QCryptographicHash::Sha256);QByteArray coordinateBuffer;coordinateBuffer.reserve(65536*24);const int w=600,h=std::max(1,std::min(1200,int(600*std::max(.001,hi.y-lo.y)/std::max(.001,hi.x-lo.x))));QImage rgb(w,h,QImage::Format_RGB32);rgb.fill(palette().base().color());std::vector<double> z(size_t(w)*h,-std::numeric_limits<double>::infinity());const unsigned stride=std::max(1u,cloud->size()/150000u);
        for(unsigned i=0;i<cloud->size();++i){const auto p=cloud->toGlobal3d(*cloud->getPoint(i));const double record[]={p.x,p.y,p.z};coordinateBuffer.append(reinterpret_cast<const char*>(record),24);if(coordinateBuffer.size()>=65536*24){coordinates.addData(coordinateBuffer);coordinateBuffer.clear();}const int cell=next.cell(p.x,p.y);if(cell>=0)map->tiles[cell].count++;if(i%stride==0){int x=std::max(0,std::min(w-1,int((p.x-lo.x)/std::max(.001,hi.x-lo.x)*(w-1)))),y=std::max(0,std::min(h-1,int((hi.y-p.y)/std::max(.001,hi.y-lo.y)*(h-1))));if(p.z>z[size_t(y)*w+x]){z[size_t(y)*w+x]=p.z;if(cloud->hasColors()){const auto c=cloud->getPointColor(i);rgb.setPixelColor(x,y,QColor(c.r,c.g,c.b));}else{const int g=int(40+190*(p.z-lo.z)/std::max(.001,hi.z-lo.z));rgb.setPixelColor(x,y,QColor(g,g,g));}}}if(i%131072==0){progress->setValue(int(100.*i/cloud->size()));QApplication::processEvents();}}coordinates.addData(coordinateBuffer);coordinateHash=QString::fromLatin1(coordinates.result().toHex());identity=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(configuration()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        QImage shade=rgb;const double stepX=std::max(.001,(hi.x-lo.x)/w),stepY=std::max(.001,(hi.y-lo.y)/h);for(int y=1;y<h-1;++y)for(int x=1;x<w-1;++x){auto get=[&](int xx,int yy){return z[size_t(yy)*w+xx];};double a=get(x-1,y),b=get(x+1,y),c=get(x,y-1),d=get(x,y+1);int v=190;if(std::isfinite(a)&&std::isfinite(b)&&std::isfinite(c)&&std::isfinite(d)){const double dx=(b-a)/(2*stepX),dy=(c-d)/(2*stepY);const double lit=(-.5*dx+.5*dy+.70710678)/std::sqrt(1+dx*dx+dy*dy);v=std::max(0,std::min(255,int(255*std::max(.08,lit))));}shade.setPixelColor(x,y,QColor(v,v,v));}
        map->rgb=rgb;map->shade=shade;for(auto& t:map->tiles)t.selected=t.count>0;secondsPerPoint=0;timeEstimator.samples.clear();leafTimeEstimator.samples.clear();progress->setValue(0);active=false;restoreProgress();log->appendPlainText(QString("Ready: %1 points, %2 map blocks. Exact context counts drive automatic sub-blocks; all points/radii preserved.").arg(cloud->size()).arg(next.size()));refresh();
    }catch(const std::exception& e){active=false;log->appendPlainText(QString("ERROR: ")+e.what());refresh();}}
    void manifest(){QJsonArray cells;for(int i=0;i<map->tiles.size();++i){const auto& t=map->tiles[i];cells.append(QJsonObject{{"id",idText(i)},{"selected",t.selected},{"core_points",double(t.count)},{"status",statusText(t.status)},{"file",t.file},{"error",t.error},{"seconds",t.seconds},{"adaptive_leaves",t.subBlocks},{"context_points_evaluated",double(t.contextPoints)},{"engine_version",t.engineVersion}});}QJsonObject record{{"schema","alis-spatial-blocks-2"},{"identity",identity},{"source_name",cloud->getName()},{"source_points",double(cloud->size())},{"config",configuration()},{"adaptive_subdivision",true},{"automatic_ram",autoRam->isChecked()},{"cpu_threads",cpuThreads->value()},{"processing_version",ALIS_RELEASE_VERSION},{"current_worker_budget_bytes",budget()},{"halo_in_output",false},{"geometry_copies_per_block",0},{"destination",diskMode()?"disk_only":isCopy?"single_copy":"current_cloud"},{"full_cloud_sf_bytes",diskMode()?0.:double(cloud->size())*keys.size()*4},{"blocks",cells}};if(store){store->setSelection(selection());record.insert("progress",store->metadata());}jsonFile(QDir(jobFolder).filePath("blocks.json"),record);}
    void runTile(int id){
        Tile& tile=map->tiles[id];tile.status=Processing;tile.error.clear();activeTile=id;activeCoreDone=0;refresh();QElapsedTimer elapsed;elapsed.start();const auto& g=map->grid;
        QTemporaryDir scratch(QDir(jobFolder).filePath(".context_XXXXXX"));require(scratch.isValid(),"Cannot create temporary halo directory");QFile spool(scratch.filePath("context.xyzid"));require(spool.open(QIODevice::WriteOnly),"Cannot spool parent coordinates");quint64 n=0,own=0;QByteArray buffer;buffer.reserve(1048576);
        for(unsigned i=0;i<cloud->size();++i){if(stopping)throw std::runtime_error("Stopped: core remains pending");const auto p=cloud->toGlobal3d(*cloud->getPoint(i));if(g.context(id,p.x,p.y,halo())){const quint32 row=i;const double pos[]={p.x,p.y,p.z};buffer.append(reinterpret_cast<const char*>(&row),4);buffer.append(reinterpret_cast<const char*>(pos),24);own+=g.cell(p.x,p.y)==id;++n;if(buffer.size()>=1048576){require(spool.write(buffer)==buffer.size(),"Disk write failed");buffer.clear();}}if(i%131072==0){progress->setFormat(idText(id)+" - collecting context: "+QString::number(int(100.*i/cloud->size()))+"%");QApplication::processEvents();}}
        require(spool.write(buffer)==buffer.size(),"Disk write failed");spool.close();buffer.clear();require(n>0&&own==tile.count,"Core membership mismatch");
        QStorageInfo storage(jobFolder);require(!storage.isValid()||storage.bytesAvailable()<0||double(storage.bytesAvailable())>tile.count*keys.size()*12.+n*112.+536870912.,"Insufficient disk space for adaptive checkpoints and atomic merging");
        AdaptiveFeatureBlocks adaptive;adaptive.folder=scratch.path();adaptive.keys=keys;adaptive.halo=halo();adaptive.bytesPerPoint=pointBytes();adaptive.workerThreads=cpuThreads->value();adaptive.metadata=checkpointMetadata(identity,idText(id),keys);adaptive.ramBudget=[this](){return quint64(budget());};
        adaptive.tick=[this](){QApplication::processEvents();require(!stopping,"Stopped: unfinished core remains pending");};adaptive.log=[this](QString message){log->appendPlainText(message);};
        adaptive.leafCompleted=[this](quint64 own,quint64,double area,double seconds){leafTimeEstimator.add(double(own),area,seconds);refresh();};
        adaptive.progress=[this,id,&tile](quint64 done,int leaves){activeCoreDone=std::min(tile.count,done);progress->setValue(int(100.*(completedPoints+activeCoreDone)/std::max<quint64>(1,selectedPoints())));progress->setFormat(idText(id)+QString(" - %1 adaptive sub-blocks; %2% of core").arg(leaves).arg(int(100.*activeCoreDone/std::max<quint64>(1,tile.count))));};
        adaptive.compute=[this,&adaptive](const QString& directory,quint64 count,const std::array<double,3>& origin,quint64 allowance,std::function<void(int)> update){
            require(availableRamBytes()>allowance+536870912,"Available RAM changed before worker start");QJsonArray ff;for(auto key:keys)ff.append(key);const QString request=QDir(directory).filePath("request.json");
            jsonFile(request,{{"directory",directory},{"point_count",double(count)},{"origin",QJsonArray{origin[0],origin[1],origin[2]}},{"features",ff},{"cache_bytes",count*keys.size()*4.+1048576.},{"memory_bytes",double(allowance)},{"threads",cpuThreads->value()},{"query_indices_file","query_indices.u32"}});
            log->appendPlainText(QString("CPU worker: %1 threads; RAM ceiling %2 GiB; %3 context points. Full feature schema/radii preserved.").arg(cpuThreads->value()).arg(double(allowance)/1073741824.,0,'f',2).arg(count));
            QProcess process;worker=&process;process.setWorkingDirectory(QCoreApplication::applicationDirPath());process.setProcessChannelMode(QProcess::MergedChannels);process.start(blockWorkerPath(),{request});require(process.waitForStarted(5000),"Cannot launch isolated feature worker");QByteArray pending;QString diagnostics;bool pressure=false;
            while(process.state()!=QProcess::NotRunning){process.waitForReadyRead(60);pending+=process.readAllStandardOutput();int nl;while((nl=pending.indexOf('\n'))>=0){auto line=pending.left(nl).trimmed();pending.remove(0,nl+1);if(line.startsWith("ALIS_PROGRESS "))update(line.mid(14).toInt());else if(!line.isEmpty()){auto text=QString::fromUtf8(line);log->appendPlainText(text);diagnostics=QString(diagnostics+text+'\n').right(8192);}}QApplication::processEvents();if(stopping||availableRamBytes()<536870912){pressure=!stopping;process.kill();}}
            worker=nullptr;if(!pending.isEmpty()){log->appendPlainText(QString::fromUtf8(pending));diagnostics+=QString::fromUtf8(pending);}require(!stopping,"Stopped: core remains pending");require(!pressure,"System RAM pressure stopped this worker");
            if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0)throw std::runtime_error(QString(QString("Feature worker failed: ")+diagnostics).toStdString());
        };
        const QString dest=QDir(jobFolder).filePath(store->metadata().value("job_id").toString()+"_"+idText(id)+".alsf");
        AdaptiveCore core{g.left(id),g.bottom(id),g.right(id),g.top(id),id%g.cols==g.cols-1,id/g.cols==g.rows-1};adaptive.run(spool.fileName(),core,tile.count,dest);
        store->commitFile(idText(id),dest,tile.count,keys,adaptive.tick);hasCommitted=true;target->prepareDisplayForRefresh();tile.file=dest;tile.status=Complete;completedPoints+=tile.count;tile.seconds=elapsed.elapsed()/1000.;tile.subBlocks=adaptive.result.leaves;tile.contextPoints=adaptive.result.contextPoints;tile.engineVersion=ALIS_RELEASE_VERSION;activeTile=-1;activeCoreDone=0;
        timeEstimator.add(double(tile.count),(g.right(id)-g.left(id))*(g.top(id)-g.bottom(id)),tile.seconds);
        log->appendPlainText(QString("%1 completed: %2 core points, %3 adaptive sub-block(s), %4 seconds. Full radii and halo preserved; no halo in checkpoint. %5").arg(idText(id)).arg(tile.count).arg(tile.subBlocks).arg(tile.seconds,0,'f',1).arg(dest));
    }
    void start(bool benchmark,bool failedOnly){if(active)return;estimateDebounce->stop();try{require(QFileInfo::exists(blockWorkerPath()),"Isolated native worker is not installed");require(!keys.isEmpty(),"Select geometric features and radii in Prepare > Features first");jobFolder=QDir(output->text().trimmed()).absolutePath();require(!output->text().trimmed().isEmpty()&&QDir().mkpath(jobFolder),"Choose a writable output folder");
        if(QFileInfo::exists(QDir(jobFolder).filePath("blocks.json"))){QFile f(QDir(jobFolder).filePath("blocks.json"));require(f.open(QIODevice::ReadOnly),"Cannot inspect existing manifest");const auto o=QJsonDocument::fromJson(f.readAll()).object();require(o.value("schema").toString()=="alis-spatial-blocks-2"&&o.value("identity").toString()==identity,"This folder belongs to a different source/order/schema/grid. Choose a new folder");}
        stopping=false;pauseRequested=false;ensureTarget();
        active=true;completedPoints=0;currentJob.clear();for(int i=0;i<map->tiles.size();++i){const auto& t=map->tiles[i];if(t.selected&&t.status==Complete)completedPoints+=t.count;if(t.selected&&t.count&&(failedOnly?t.status==Failed:t.status!=Complete))currentJob.push_back(i);}
        // A tiny boundary core is not a useful speed benchmark. Start near the
        // median full-resolution core count, then retain normal map order.
        if(!currentJob.isEmpty()&&(benchmark||!timeEstimator.ready())){auto sorted=currentJob;std::sort(sorted.begin(),sorted.end(),[this](int a,int b){return map->tiles[a].count<map->tiles[b].count;});int representative=sorted[sorted.size()/2];currentJob.removeOne(representative);currentJob.prepend(representative);}
        clock.start();activeTile=-1;activeCoreDone=0;refresh();manifest();int done=0;for(int i:currentJob){if(stopping||pauseRequested)break;try{runTile(i);}catch(const std::exception& e){map->tiles[i].status=stopping?Pending:Failed;map->tiles[i].error=QString::fromUtf8(e.what());log->appendPlainText(idText(i)+": "+map->tiles[i].error);}worker=nullptr;activeTile=-1;activeCoreDone=0;manifest();refresh();QApplication::processEvents();++done;if(benchmark&&done==1)break;}
        lastElapsedMinutes=clock.elapsed()/60000.;active=false;progress->setFormat("%p%");log->appendPlainText(stopping?"Stopped safely; completed cores retained.":pauseRequested?"Paused after block. Compute resumes pending cores; completed cores are skipped.":benchmark?"Pilot completed; measured time estimate updated.":"Selected adaptive pass finished. Review any red blocks and their exact diagnostics.");manifest();refresh();
    }catch(const std::exception& e){worker=nullptr;active=false;log->appendPlainText(QString("ERROR: ")+e.what());refresh();}}
    void makeTraining(){try{auto* data=target?target:cloud;const quint64 n=selectedPoints();require(n>0,"Select nonempty blocks first");require(n*(64+data->getNumberOfScalarFields()*4.)<budget()&&availableRamBytes()>n*(64+data->getNumberOfScalarFields()*4.)+536870912.,"Training subset exceeds RAM ceiling; select fewer blocks");CCCoreLib::ReferenceCloud core(data);require(core.reserve(unsigned(n)),"Cannot reserve training subset");active=true;refresh();for(unsigned i=0;i<data->size();++i){const auto p=data->toGlobal3d(*data->getPoint(i));const int cell=map->grid.cell(p.x,p.y);if(cell>=0&&map->tiles[cell].selected)core.addPointIndex(i);if(i%131072==0)QApplication::processEvents();}int warnings=0;std::unique_ptr<ccPointCloud> subset(data->partialClone(&core,&warnings,false));require(subset&&warnings==0,"Could not clone all training fields");subset->setName(data->getName()+"_selected_training_cores");QJsonArray ids;for(int i=0;i<map->tiles.size();++i)if(map->tiles[i].selected)ids.append(idText(i));subset->setMetaData("ALiS.trainingBlockSelection",QJsonDocument(QJsonObject{{"source_uid",double(cloud->getUniqueID())},{"grid_m",map->grid.side},{"halo_in_training",false},{"blocks",ids}}).toJson(QJsonDocument::Compact));trainingResult=subset.release();active=false;accept();}catch(const std::exception& e){active=false;log->appendPlainText(QString("ERROR: ")+e.what());refresh();}}
};
}
void showSpatialBlocks(ccPointCloud* cloud,const QStringList& keys,const QVector<double>& radii,QWidget* parent,const std::function<void(ccPointCloud*)>& publish,const std::function<void(ccPointCloud*)>& training){
    QStringList effectiveKeys=keys;QVector<double> effectiveRadii=radii;
    auto savedState=ProgressiveFeatureStore::state(*cloud);if(savedState.isEmpty())savedState=ProgressiveFeatureStore::state(*cloud,true);
    const auto saved=savedState.value("config").toObject();QStringList savedKeys;for(auto k:saved.value("features").toArray())savedKeys<<k.toString();
    if(!savedKeys.isEmpty()&&savedKeys!=keys){QMessageBox question(QMessageBox::Question,"ALiS - Resume feature job","This cloud contains saved progressive features with a different schema. Resume the saved feature/radius settings, or start a new independent job? Existing SFs will not be overwritten.",QMessageBox::NoButton,parent);auto* resume=question.addButton("Resume saved settings",QMessageBox::AcceptRole);question.addButton("Use current settings (new job)",QMessageBox::ActionRole);auto* cancel=question.addButton(QMessageBox::Cancel);question.setDefaultButton(resume);question.exec();if(question.clickedButton()==cancel)return;if(question.clickedButton()==resume){effectiveKeys=savedKeys;effectiveRadii.clear();for(auto r:saved.value("radii").toArray())effectiveRadii<<r.toDouble();}}
    BlocksDialog dialog(cloud,effectiveKeys,effectiveRadii,parent);dialog.publish=publish;dialog.training=training;dialog.exec();if(dialog.trainingResult){if(training)training(dialog.trainingResult);else delete dialog.trainingResult;}
}
void WorkspaceDock::showClassificationWorkspace(){if(m_tabs)m_tabs->setCurrentIndex(3);}
void WorkspaceController::openSpatialBlocks(){
    if(!allowFullCloudOperation("Spatial blocks"))return;
    if(!m_selectedCloud||!m_app)return;if(!requireMetricUnits("Spatial blocks"))return;QStringList keys;const auto radii=m_dock->scaleRadii();for(const auto& id:m_dock->selectedFeatureIds()){bool ok=false;auto feature=featureIdFromUi(id,ok);if(!ok||id=="hag")continue;for(double r:radii){FeatureKey k;k.feature=feature;k.radius=r;keys<<serializeModelFeatureKey(k);}}
    ccPointCloud* copy=nullptr;bool trainingChosen=false;
    showSpatialBlocks(m_selectedCloud,keys,radii,m_dock,[this,&copy](ccPointCloud* c){copy=c;m_app->addToDB(c,false,true,false,false);refreshHost();},[this,&trainingChosen](ccPointCloud* c){trainingChosen=true;m_app->addToDB(c,false,true,false,false);m_app->setSelectedInDB(c,true);selectCloud(c);m_dock->showClassificationWorkspace();showReady("Selected training cores created. Choose this loaded cloud as training set, confirm its class field, then Train. Halo rows excluded; labels are not automatically trusted.");});
    if(copy&&!trainingChosen){m_app->setSelectedInDB(m_selectedCloud,false);m_app->setSelectedInDB(copy,true);selectCloud(copy);}updateModelFeatureChoices();syncDisplayControls();refreshHost();updateDockState();
}
}
