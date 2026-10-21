// SPDX-License-Identifier: GPL-2.0-or-later
#include "SpatialBlocksDialog.h"
#include "SpatialBlockGrid.h"
#include "ProgressiveFeatureStore.h"
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "MemorySafety.h"
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
struct Tile { bool selected=true; quint64 count=0; Status status=Pending; QString file,error; };
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
    QDoubleSpinBox *side,*memory;QComboBox *view,*destination;QLineEdit* output;
    QLabel *counts,*details,*estimate,*haloLabel;QProgressBar* progress;QPlainTextEdit* log;
    QPushButton *run,*pilot,*pause,*stop,*retry,*train,*regrid,*all,*none,*browse,*close,*save;
    bool active=false,stopping=false,pauseRequested=false;QProcess* worker=nullptr;
    double secondsPerPoint=0;ccPointCloud* target=nullptr;
    std::unique_ptr<ProgressiveFeatureStore> store;
    QString coordinateHash,identity;bool hasCommitted=false;int hovered=-1;
    bool isCopy=false;
    std::function<void(ccPointCloud*)> publish,training;
    ccPointCloud* trainingResult=nullptr;QString jobFolder;QVector<int> currentJob;quint64 completedPoints=0;QElapsedTimer clock;
    double halo()const{return radii.isEmpty()?0:*std::max_element(radii.begin(),radii.end());}
    quint64 selectedPoints()const{quint64 n=0;for(const auto& t:map->tiles)if(t.selected)n+=t.count;return n;}
    double budget()const{return memory->value()*1073741824.;}
    double pointBytes()const{return 240.+keys.size()*20.+cloud->getNumberOfScalarFields()*4.;}
    void reject()override{if(!active){if(store)store->setSelection(selection());QDialog::reject();}}
    BlocksDialog(ccPointCloud* c,const QStringList& k,const QVector<double>& r,QWidget* parent):QDialog(parent),cloud(c),keys(k),radii(r){
        setWindowTitle("ALiS - Spatial blocks");setObjectName("ALiS.SpatialBlocks");resize(1060,800);setWindowModality(Qt::ApplicationModal);
        auto* root=new QVBoxLayout(this);auto* heading=new QLabel("<b>Spatial blocks</b> - "+cloud->getName());root->addWidget(heading);
        auto* note=new QLabel("Selected CloudCompare cloud. Only the XY preview is sampled; processing and training subsets keep all source points. Original labels and scalar fields are preserved.");note->setWordWrap(true);root->addWidget(note);
        auto* body=new QHBoxLayout;root->addLayout(body,1);auto* left=new QVBoxLayout;body->addLayout(left,1);auto* tools=new QHBoxLayout;left->addLayout(tools);
        all=new QPushButton("Select all");none=new QPushButton("Deselect all");view=new QComboBox;view->addItems({"RGB preview","Hillshade preview"});tools->addWidget(all);tools->addWidget(none);tools->addStretch();tools->addWidget(view);
        map=new BlockMap;map->setObjectName("ALiS.SpatialBlocks.Map");left->addWidget(map,1);details=new QLabel("Click a block to select it; hover for point count and status.");details->setWordWrap(true);left->addWidget(details);
        auto* legend=new QHBoxLayout;left->addLayout(legend);for(int i=0;i<4;++i){auto* l=new QLabel(QString("<span style='color:%1'>&#9632;</span> %2").arg(statusColor(Status(i)).name(),statusText(Status(i))));legend->addWidget(l);}
        auto* settings=new QGroupBox("Processing settings");settings->setMaximumWidth(300);body->addWidget(settings);auto* form=new QFormLayout(settings);
        side=new QDoubleSpinBox;side->setRange(1,10000);side->setValue(ProgressiveFeatureStore::state(*cloud).value("config").toObject().value("block_m").toDouble(50));side->setSuffix(" m");form->addRow("Block side",side);regrid=new QPushButton("Rebuild grid");form->addRow(regrid);
        QStringList scales;for(double v:radii)scales<<QString::number(v,'g',8);auto* f=new QLabel(QString("%1 geometric SFs\nRadii: %2 m\nFrom Prepare > Features").arg(keys.size()).arg(scales.join(" / ")));f->setWordWrap(true);form->addRow(f);
        haloLabel=new QLabel(QString("%1 m (automatic, context only)").arg(halo(),0,'g',8));haloLabel->setWordWrap(true);form->addRow("Halo",haloLabel);
        memory=new QDoubleSpinBox;memory->setRange(.5,16);memory->setValue(std::min(2.,std::max(.5,double(availableRamBytes())/1073741824./4)));memory->setSuffix(" GiB");form->addRow("Worker RAM ceiling",memory);
        counts=new QLabel;counts->setWordWrap(true);form->addRow(counts);estimate=new QLabel("Time estimate: run one pilot block");estimate->setWordWrap(true);form->addRow(estimate);
        pilot=new QPushButton("Benchmark 1 selected block");form->addRow(pilot);
        destination=new QComboBox;destination->setObjectName("ALiS.SpatialBlocks.Destination");destination->addItems({"Current cloud (add/update SFs)","One new full-cloud copy"});form->addRow("Feature destination",destination);
        auto* usage=new QLabel(QString("Full-cloud SF allocation: %1 MiB.\nA copy also needs XYZ, RGB and existing fields. Unprocessed points = NaN. No per-block geometry copies.").arg(double(cloud->size())*keys.size()*4/1048576.,0,'f',1));usage->setWordWrap(true);form->addRow(usage);
        save=new QPushButton("Save cloud + progress (BIN)...");form->addRow(save);
        train=new QPushButton("Use selected blocks for training...");train->setToolTip("Creates a full-resolution core-only subset with current class fields. Then choose/certify its class field in Classification and train your models. No halo rows are added; labels are not automatically trusted.");form->addRow(train);
        auto* warning=new QLabel("This panel processes a loaded cloud. Model-driven LAS/LAZ disk streaming remains in Classification > Prepare data for this model. Training starts in Classification after confirming labels.");warning->setWordWrap(true);form->addRow(warning);
        auto* path=new QHBoxLayout;root->addLayout(path);path->addWidget(new QLabel("Output folder"));output=new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)+"/ALiS_Block_Tests/"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));output->setObjectName("ALiS.SpatialBlocks.Output");path->addWidget(output,1);browse=new QPushButton("Browse...");path->addWidget(browse);
        log=new QPlainTextEdit;log->setReadOnly(true);log->setMaximumBlockCount(250);log->setMaximumHeight(95);root->addWidget(log);progress=new QProgressBar;progress->setRange(0,100);root->addWidget(progress);
        auto* actions=new QHBoxLayout;root->addLayout(actions);run=new QPushButton("Compute SFs on selected blocks");run->setDefault(true);pause=new QPushButton("Pause after block");stop=new QPushButton("Stop safely");retry=new QPushButton("Retry failed");close=new QPushButton("Close");for(auto* b:{run,pause,stop,retry,close})actions->addWidget(b);
        connect(close,&QPushButton::clicked,this,&BlocksDialog::reject);connect(browse,&QPushButton::clicked,this,[this](){auto p=QFileDialog::getExistingDirectory(this,"Choose output folder",output->text());if(!p.isEmpty())output->setText(p);});
        connect(all,&QPushButton::clicked,this,[this](){for(auto& t:map->tiles)t.selected=t.count>0;if(store)store->setSelection(selection());refresh();});connect(none,&QPushButton::clicked,this,[this](){for(auto& t:map->tiles)t.selected=false;if(store)store->setSelection(selection());refresh();});
        map->changed=[this](){if(store)store->setSelection(selection());refresh();};map->hover=[this](int i){hovered=i;updateDetail();};
        connect(view,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){map->hillshade=i==1;map->update();});connect(regrid,&QPushButton::clicked,this,[this](){rebuild();});
        connect(destination,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](){restoreProgress();refresh();});
        connect(save,&QPushButton::clicked,this,[this](){saveCloud();});
        connect(memory,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](){refresh();});connect(run,&QPushButton::clicked,this,[this](){start(false,false);});connect(pilot,&QPushButton::clicked,this,[this](){start(true,false);});connect(retry,&QPushButton::clicked,this,[this](){start(false,true);});
        connect(pause,&QPushButton::clicked,this,[this](){pauseRequested=true;pause->setEnabled(false);log->appendPlainText("Pause requested: the current block will finish and be saved.");});
        connect(stop,&QPushButton::clicked,this,[this](){stopping=true;if(worker)worker->kill();log->appendPlainText("Stopping only the isolated block worker. Completed files are retained.");});
        connect(train,&QPushButton::clicked,this,[this](){makeTraining();});rebuild();
    }
    QJsonArray selection()const{QJsonArray a;for(int i=0;i<map->tiles.size();++i)if(map->tiles[i].selected)a.append(i);return a;}
    QJsonObject configuration()const{QJsonArray ff,rr;for(const auto& k:keys)ff.append(k);for(double r:radii)rr.append(r);const auto& g=map->grid;return {{"engine","alis-geometric-native-v1"},{"coordinate_sha256",coordinateHash},{"point_count",double(cloud->size())},{"block_m",g.side},{"x0",g.x0},{"y0",g.y0},{"x1",g.x1},{"y1",g.y1},{"features",ff},{"radii",rr},{"halo_m",halo()}};}
    void updateDetail(){if(hovered<0||hovered>=map->tiles.size())return;const auto& t=map->tiles[hovered];details->setText(QString("%1 | %2 points | %3 | %4").arg(idText(hovered)).arg(t.count).arg(statusText(t.status),t.error.isEmpty()?(t.selected?"Selected":"Excluded"):t.error));}
    void restoreProgress(){
        if(store)return;for(auto& t:map->tiles){t.status=Pending;t.file.clear();t.error.clear();}
        if(destination->currentIndex()!=0||!ProgressiveFeatureStore::compatible(*cloud,identity,keys))return;
        const auto state=ProgressiveFeatureStore::state(*cloud);const auto done=state.value("blocks").toObject();
        for(int i=0;i<map->tiles.size();++i)if(done.contains(idText(i))&&done.value(idText(i)).toObject().value("points").toDouble()==map->tiles[i].count)map->tiles[i].status=Complete;
        if(state.contains("selection")){const auto chosen=state.value("selection").toArray();for(auto& t:map->tiles)t.selected=false;for(auto v:chosen){int i=v.toInt(-1);if(i>=0&&i<map->tiles.size())map->tiles[i].selected=map->tiles[i].count>0;}}
        const auto folder=state.value("checkpoint_folder").toString();if(!folder.isEmpty())output->setText(folder);
        log->appendPlainText("Saved progressive job recognised: completed cores restored from this cloud's BIN metadata.");
    }
    void ensureTarget(){
        if(store)return;
        const quint64 sfBytes=quint64(cloud->size())*keys.size()*4;
        const bool reuse=ProgressiveFeatureStore::compatible(*cloud,identity,keys);
        const quint64 extraFields=reuse?0:sfBytes;
        const quint64 copyBytes=destination->currentIndex()==1?quint64(cloud->size())*(64+cloud->getNumberOfScalarFields()*4):0;
        require(availableRamBytes()>extraFields+copyBytes+quint64(budget())+536870912,"Not enough free RAM for the full-cloud SFs/copy plus worker reserve. Select fewer features or use the existing disk-streaming preparation workflow.");
        std::unique_ptr<ccPointCloud> copy;
        if(destination->currentIndex()==1){copy.reset(cloud->cloneThis(nullptr,true));require(copy&&copy->size()==cloud->size()&&copy->getNumberOfScalarFields()==cloud->getNumberOfScalarFields()&&copy->hasColors()==cloud->hasColors(),"Cannot make a complete single-copy destination");copy->setName(cloud->getName()+"_progressive_features");target=copy.get();isCopy=true;}else target=cloud;
        store.reset(new ProgressiveFeatureStore(target,identity,keys,configuration()));store->setOutputFolder(jobFolder);store->setSelection(selection());
        const auto done=store->completed();for(int i=0;i<map->tiles.size();++i)if(done.contains(idText(i)))map->tiles[i].status=Complete;
        if(copy){require(bool(publish),"A copy destination requires a CloudCompare publication callback");auto* p=copy.release();publish(p);}
        log->appendPlainText(isCopy?"One complete destination copy created. Every following block updates this same entity.":"Progressive SFs attached to the current cloud. Existing class fields and geometry are unchanged.");
    }
    void saveCloud(){
        if(active)return;ccPointCloud* c=target?target:(ProgressiveFeatureStore::compatible(*cloud,identity,keys)?cloud:nullptr);if(!c)return;
        QString path=QFileDialog::getSaveFileName(this,"Save cloud and progressive state",QDir(output->text()).filePath("cloud_with_progress.bin"),"CloudCompare BIN (*.bin)");if(path.isEmpty())return;if(!path.endsWith(".bin",Qt::CaseInsensitive))path+=".bin";
        try{if(store)store->setSelection(selection());active=true;stopping=false;refresh();QTemporaryDir temp(QFileInfo(path).absolutePath()+"/.alis_save_XXXXXX");require(temp.isValid(),"Cannot create safe export staging folder");const QString staged=temp.filePath("cloud.bin");BinFilter bin;FileIOFilter::SaveParameters sp;sp.alwaysDisplaySaveDialog=false;sp.parentWidget=this;require(bin.saveToFile(c,staged,sp)==CC_FERR_NO_ERROR,"BIN export failed; existing file was not replaced");QFile in(staged);QSaveFile out(path);require(in.open(QIODevice::ReadOnly)&&out.open(QIODevice::WriteOnly),"Cannot open export destination");while(!in.atEnd()){auto bytes=in.read(1048576);require(!bytes.isEmpty()&&out.write(bytes)==bytes.size(),"BIN export copy failed");progress->setValue(int(100.*in.pos()/std::max<qint64>(1,in.size())));QApplication::processEvents();require(!stopping,"Save canceled; previous destination file retained");}require(out.commit(),"Cannot commit saved BIN");log->appendPlainText("Saved full cloud + progress: "+path+". Reopen this BIN and choose Spatial blocks to continue. Original LAS/LAZ was not overwritten.");}catch(const std::exception& e){log->appendPlainText(QString("ERROR: ")+e.what());}active=false;refresh();
    }
    void refresh(){int n=0,done=0,failed=0,pending=0;quint64 remaining=0;for(const auto& t:map->tiles)if(t.selected){++n;done+=t.status==Complete;failed+=t.status==Failed;pending+=t.status==Pending;if(t.status!=Complete)remaining+=t.count;}
        counts->setText(QString("Selected: %1 / %2 blocks\nCore points: %3\nCompleted: %4 | Failed: %5").arg(n).arg(map->tiles.size()).arg(selectedPoints()).arg(done).arg(failed));
        estimate->setText(secondsPerPoint>0?QString("Remaining estimate: %1-%2 min\nPilot/existing block timing; density and context may vary.").arg(remaining*secondsPerPoint*.7/60,0,'f',1).arg(remaining*secondsPerPoint*1.5/60,0,'f',1):"Time estimate: run one pilot block");
        if(!active){const auto total=selectedPoints();progress->setValue(total?int(100.*(total-remaining)/total):0);}
        run->setEnabled(!active&&pending&&keys.size());pilot->setEnabled(!active&&pending&&keys.size());retry->setEnabled(!active&&failed&&keys.size());train->setEnabled(!active&&selectedPoints());pause->setEnabled(active&&!pauseRequested);stop->setEnabled(active);close->setEnabled(!active);
        for(QWidget* w:QList<QWidget*>{memory,all,none})w->setEnabled(!active);for(QWidget* w:QList<QWidget*>{side,regrid,output,browse,destination})w->setEnabled(!active&&!store);save->setEnabled(!active&&(target||ProgressiveFeatureStore::compatible(*cloud,identity,keys)));map->locked=active;updateDetail();map->update();
    }
    void rebuild(){if(active)return;try{CCVector3d lo,hi;require(cloud->getOwnGlobalBB(lo,hi),"Cloud has no finite bounds");const SpatialBlockGrid next(lo.x,lo.y,hi.x,hi.y,side->value());map->grid=next;map->tiles=QVector<Tile>(next.size());progress->setRange(0,100);log->appendPlainText("Counting full-resolution cores and building a sampled XY preview...");
        active=true;refresh();QCryptographicHash coordinates(QCryptographicHash::Sha256);QByteArray coordinateBuffer;coordinateBuffer.reserve(65536*24);const int w=600,h=std::max(1,std::min(1200,int(600*std::max(.001,hi.y-lo.y)/std::max(.001,hi.x-lo.x))));QImage rgb(w,h,QImage::Format_RGB32);rgb.fill(palette().base().color());std::vector<double> z(size_t(w)*h,-std::numeric_limits<double>::infinity());const unsigned stride=std::max(1u,cloud->size()/150000u);
        for(unsigned i=0;i<cloud->size();++i){const auto p=cloud->toGlobal3d(*cloud->getPoint(i));const double record[]={p.x,p.y,p.z};coordinateBuffer.append(reinterpret_cast<const char*>(record),24);if(coordinateBuffer.size()>=65536*24){coordinates.addData(coordinateBuffer);coordinateBuffer.clear();}const int cell=next.cell(p.x,p.y);if(cell>=0)map->tiles[cell].count++;if(i%stride==0){int x=std::max(0,std::min(w-1,int((p.x-lo.x)/std::max(.001,hi.x-lo.x)*(w-1)))),y=std::max(0,std::min(h-1,int((hi.y-p.y)/std::max(.001,hi.y-lo.y)*(h-1))));if(p.z>z[size_t(y)*w+x]){z[size_t(y)*w+x]=p.z;if(cloud->hasColors()){const auto c=cloud->getPointColor(i);rgb.setPixelColor(x,y,QColor(c.r,c.g,c.b));}else{const int g=int(40+190*(p.z-lo.z)/std::max(.001,hi.z-lo.z));rgb.setPixelColor(x,y,QColor(g,g,g));}}}if(i%131072==0){progress->setValue(int(100.*i/cloud->size()));QApplication::processEvents();}}coordinates.addData(coordinateBuffer);coordinateHash=QString::fromLatin1(coordinates.result().toHex());identity=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(configuration()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        QImage shade=rgb;const double stepX=std::max(.001,(hi.x-lo.x)/w),stepY=std::max(.001,(hi.y-lo.y)/h);for(int y=1;y<h-1;++y)for(int x=1;x<w-1;++x){auto get=[&](int xx,int yy){return z[size_t(yy)*w+xx];};double a=get(x-1,y),b=get(x+1,y),c=get(x,y-1),d=get(x,y+1);int v=190;if(std::isfinite(a)&&std::isfinite(b)&&std::isfinite(c)&&std::isfinite(d)){const double dx=(b-a)/(2*stepX),dy=(c-d)/(2*stepY);const double lit=(-.5*dx+.5*dy+.70710678)/std::sqrt(1+dx*dx+dy*dy);v=std::max(0,std::min(255,int(255*std::max(.08,lit))));}shade.setPixelColor(x,y,QColor(v,v,v));}
        map->rgb=rgb;map->shade=shade;for(auto& t:map->tiles)t.selected=t.count>0;secondsPerPoint=0;progress->setValue(0);active=false;restoreProgress();log->appendPlainText(QString("Ready: %1 points, %2 blocks. Preview sampled, core counts exact.").arg(cloud->size()).arg(next.size()));refresh();
    }catch(const std::exception& e){active=false;log->appendPlainText(QString("ERROR: ")+e.what());refresh();}}
    void manifest(){QJsonArray cells;for(int i=0;i<map->tiles.size();++i){const auto& t=map->tiles[i];cells.append(QJsonObject{{"id",idText(i)},{"selected",t.selected},{"core_points",double(t.count)},{"status",statusText(t.status)},{"file",t.file},{"error",t.error}});}QJsonObject record{{"schema","alis-spatial-blocks-2"},{"identity",identity},{"source_name",cloud->getName()},{"source_points",double(cloud->size())},{"config",configuration()},{"halo_in_output",false},{"geometry_copies_per_block",0},{"destination",isCopy?"single_copy":"current_cloud"},{"blocks",cells}};if(store){store->setSelection(selection());record.insert("progress",store->metadata());}jsonFile(QDir(jobFolder).filePath("blocks.json"),record);}
    void runTile(int id){
        Tile& tile=map->tiles[id];tile.status=Processing;tile.error.clear();refresh();QElapsedTimer elapsed;elapsed.start();const auto& g=map->grid;
        QTemporaryDir scratch(QDir(jobFolder).filePath(".context_XXXXXX"));require(scratch.isValid(),"Cannot create temporary halo directory");QFile xyz(scratch.filePath("xyz.f64"));require(xyz.open(QIODevice::WriteOnly),"Cannot write halo coordinates");QVector<unsigned> original,rows;quint64 n=0;QByteArray buffer;buffer.reserve(65536*24);
        for(unsigned i=0;i<cloud->size();++i){if(stopping)throw std::runtime_error("Stopped: core remains pending");const auto p=cloud->toGlobal3d(*cloud->getPoint(i));if(g.context(id,p.x,p.y,halo())){require((n+1)*pointBytes()+268435456.<budget(),"Block exceeds RAM ceiling; choose smaller blocks or increase the explicit ceiling");const double pos[]={p.x,p.y,p.z};buffer.append(reinterpret_cast<const char*>(pos),24);if(g.cell(p.x,p.y)==id){original.push_back(i);rows.push_back(unsigned(n));}++n;if(buffer.size()>=65536*24){require(xyz.write(buffer)==buffer.size(),"Disk write failed");buffer.clear();}}if(i%131072==0){progress->setFormat(idText(id)+" - collecting context: "+QString::number(int(100.*i/cloud->size()))+"%");QApplication::processEvents();}}
        require(xyz.write(buffer)==buffer.size(),"Disk write failed");xyz.close();buffer.clear();require(n>0&&original.size()==int(tile.count),"Core membership mismatch");require(availableRamBytes()>quint64(std::min(budget(),n*pointBytes())+536870912.),"Not enough free RAM for this block and CloudCompare reserve");
        QJsonArray ff;for(const auto& key:keys)ff.append(key);const auto origin=cloud->toGlobal3d(*cloud->getPoint(original[0]));const QString request=scratch.filePath("request.json");jsonFile(request,{{"directory",scratch.path()},{"point_count",double(n)},{"origin",QJsonArray{origin.x,origin.y,origin.z}},{"features",ff},{"cache_bytes",n*keys.size()*4.+1048576.},{"memory_bytes",budget()},{"threads",4}});
        QProcess process;worker=&process;process.setWorkingDirectory(QCoreApplication::applicationDirPath());process.setProcessChannelMode(QProcess::MergedChannels);process.start(blockWorkerPath(),{request});require(process.waitForStarted(5000),"Cannot launch isolated feature worker");QByteArray pending;while(process.state()!=QProcess::NotRunning){process.waitForReadyRead(60);pending+=process.readAllStandardOutput();int nl;while((nl=pending.indexOf('\n'))>=0){const auto line=pending.left(nl).trimmed();pending.remove(0,nl+1);if(line.startsWith("ALIS_PROGRESS ")){const int pc=line.mid(14).toInt();progress->setValue(int(100.*(completedPoints+tile.count*pc/100.)/std::max<quint64>(1,selectedPoints())));progress->setFormat(idText(id)+" - features: "+QString::number(pc)+"%");}else if(!line.isEmpty())log->appendPlainText(QString::fromUtf8(line));}QApplication::processEvents();if(stopping||availableRamBytes()<536870912){process.kill();if(!stopping){stopping=true;log->appendPlainText("Low system RAM: worker stopped to protect CloudCompare.");}}}worker=nullptr;pending+=process.readAllStandardOutput();if(!pending.isEmpty())log->appendPlainText(QString::fromUtf8(pending));require(!stopping,"Stopped: core remains pending");require(process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0,"Feature worker failed; see diagnostic log");
        const quint64 matrixBytes=quint64(original.size())*keys.size()*4;require(matrixBytes<quint64(budget())&&quint64(original.size())*keys.size()<=quint64(std::numeric_limits<int>::max())&&availableRamBytes()>matrixBytes+536870912,"Not enough RAM for the bounded core matrix");
        QFile values(scratch.filePath("features.f32"));require(values.open(QIODevice::ReadOnly)&&quint64(values.size())==n*keys.size()*4,"Invalid feature worker output dimensions");QVector<float> coreValues;coreValues.resize(int(quint64(rows.size())*keys.size()));
        for(int i=0;i<rows.size();++i){require(values.seek(quint64(rows[i])*keys.size()*4),"Cannot read core feature row");const auto bytes=values.read(keys.size()*4);require(bytes.size()==keys.size()*4,"Truncated feature row");std::memcpy(coreValues.data()+quint64(i)*keys.size(),bytes.constData(),size_t(bytes.size()));if(i%32768==0){QApplication::processEvents();require(!stopping,"Stopped before committing progressive SFs");}}
        // Compact checkpoint: original row IDs + new feature values, never XYZ,
        // RGB, existing SFs or halo rows. The complete geometry is saved only
        // when the user explicitly presses Save cloud + progress.
        const QString dest=QDir(jobFolder).filePath(store->metadata().value("job_id").toString()+"_"+idText(id)+".alsf");QSaveFile patch(dest);require(patch.open(QIODevice::WriteOnly),"Cannot write compact feature checkpoint");
        QDataStream header(&patch);header.setByteOrder(QDataStream::LittleEndian);header.writeRawData("ALISF02",7);const auto meta=QJsonDocument(QJsonObject{{"identity",identity},{"block",idText(id)},{"features",configuration().value("features")}}).toJson(QJsonDocument::Compact);header<<quint32(meta.size());header.writeRawData(meta.constData(),meta.size());header<<quint32(original.size())<<quint32(keys.size());require(header.status()==QDataStream::Ok,"Checkpoint header write failed");QByteArray records;records.reserve(65536*(4+keys.size()*4));for(int i=0;i<original.size();++i){const quint32 row=original[i];records.append(reinterpret_cast<const char*>(&row),4);records.append(reinterpret_cast<const char*>(coreValues.constData()+quint64(i)*keys.size()),keys.size()*4);if(i%65536==65535||i==original.size()-1){require(patch.write(records)==records.size(),"Checkpoint write failed");records.clear();QApplication::processEvents();require(!stopping,"Stopped before committing progressive SFs");}}require(patch.commit(),"Cannot commit compact checkpoint");
        store->commit(idText(id),original,coreValues,dest);hasCommitted=true;target->prepareDisplayForRefresh();tile.file=dest;tile.status=Complete;completedPoints+=tile.count;secondsPerPoint=secondsPerPoint>0?.5*(secondsPerPoint+elapsed.elapsed()/1000./tile.count):elapsed.elapsed()/1000./tile.count;
        log->appendPlainText(QString("%1 completed: %2 core / %3 context points, %4 seconds. SFs updated on %5; halo discarded. Checkpoint: %6").arg(idText(id)).arg(tile.count).arg(n-tile.count).arg(elapsed.elapsed()/1000.,0,'f',1).arg(isCopy?"the single destination copy":"the current cloud",dest));
    }
    void start(bool benchmark,bool failedOnly){if(active)return;try{require(QFileInfo::exists(blockWorkerPath()),"Isolated native worker is not installed");require(!keys.isEmpty(),"Select geometric features and radii in Prepare > Features first");jobFolder=QDir(output->text().trimmed()).absolutePath();require(!output->text().trimmed().isEmpty()&&QDir().mkpath(jobFolder),"Choose a writable output folder");
        if(QFileInfo::exists(QDir(jobFolder).filePath("blocks.json"))){QFile f(QDir(jobFolder).filePath("blocks.json"));require(f.open(QIODevice::ReadOnly),"Cannot inspect existing manifest");const auto o=QJsonDocument::fromJson(f.readAll()).object();require(o.value("schema").toString()=="alis-spatial-blocks-2"&&o.value("identity").toString()==identity,"This folder belongs to a different source/order/schema/grid. Choose a new folder");}
        ensureTarget();
        active=true;stopping=false;pauseRequested=false;completedPoints=0;currentJob.clear();for(int i=0;i<map->tiles.size();++i){const auto& t=map->tiles[i];if(t.selected&&t.status==Complete)completedPoints+=t.count;if(t.selected&&t.count&&(failedOnly?t.status==Failed:t.status==Pending))currentJob.push_back(i);}refresh();int done=0;for(int i:currentJob){if(stopping||pauseRequested)break;try{runTile(i);}catch(const std::exception& e){map->tiles[i].status=stopping?Pending:Failed;map->tiles[i].error=QString::fromUtf8(e.what());log->appendPlainText(idText(i)+": "+map->tiles[i].error);}worker=nullptr;manifest();refresh();QApplication::processEvents();++done;if(benchmark&&done==1)break;}
        active=false;progress->setFormat("%p%");log->appendPlainText(stopping?"Stopped safely; completed cores retained.":pauseRequested?"Paused after block. Compute resumes pending cores; completed cores are skipped.":benchmark?"Pilot completed; timing estimate updated.":"Selected block pass finished. Review red blocks and use Retry failed.");manifest();refresh();
    }catch(const std::exception& e){worker=nullptr;active=false;log->appendPlainText(QString("ERROR: ")+e.what());refresh();}}
    void makeTraining(){try{auto* data=target?target:cloud;const quint64 n=selectedPoints();require(n>0,"Select nonempty blocks first");require(n*(64+data->getNumberOfScalarFields()*4.)<budget()&&availableRamBytes()>n*(64+data->getNumberOfScalarFields()*4.)+536870912.,"Training subset exceeds RAM ceiling; select fewer blocks");CCCoreLib::ReferenceCloud core(data);require(core.reserve(unsigned(n)),"Cannot reserve training subset");active=true;refresh();for(unsigned i=0;i<data->size();++i){const auto p=data->toGlobal3d(*data->getPoint(i));const int cell=map->grid.cell(p.x,p.y);if(cell>=0&&map->tiles[cell].selected)core.addPointIndex(i);if(i%131072==0)QApplication::processEvents();}int warnings=0;std::unique_ptr<ccPointCloud> subset(data->partialClone(&core,&warnings,false));require(subset&&warnings==0,"Could not clone all training fields");subset->setName(data->getName()+"_selected_training_cores");QJsonArray ids;for(int i=0;i<map->tiles.size();++i)if(map->tiles[i].selected)ids.append(idText(i));subset->setMetaData("ALiS.trainingBlockSelection",QJsonDocument(QJsonObject{{"source_uid",double(cloud->getUniqueID())},{"grid_m",map->grid.side},{"halo_in_training",false},{"blocks",ids}}).toJson(QJsonDocument::Compact));trainingResult=subset.release();active=false;accept();}catch(const std::exception& e){active=false;log->appendPlainText(QString("ERROR: ")+e.what());refresh();}}
};
}
void showSpatialBlocks(ccPointCloud* cloud,const QStringList& keys,const QVector<double>& radii,QWidget* parent,const std::function<void(ccPointCloud*)>& publish,const std::function<void(ccPointCloud*)>& training){
    QStringList effectiveKeys=keys;QVector<double> effectiveRadii=radii;
    const auto saved=ProgressiveFeatureStore::state(*cloud).value("config").toObject();QStringList savedKeys;for(auto k:saved.value("features").toArray())savedKeys<<k.toString();
    if(!savedKeys.isEmpty()&&savedKeys!=keys){QMessageBox question(QMessageBox::Question,"ALiS - Resume feature job","This cloud contains saved progressive features with a different schema. Resume the saved feature/radius settings, or start a new independent job? Existing SFs will not be overwritten.",QMessageBox::NoButton,parent);auto* resume=question.addButton("Resume saved settings",QMessageBox::AcceptRole);question.addButton("Use current settings (new job)",QMessageBox::ActionRole);auto* cancel=question.addButton(QMessageBox::Cancel);question.setDefaultButton(resume);question.exec();if(question.clickedButton()==cancel)return;if(question.clickedButton()==resume){effectiveKeys=savedKeys;effectiveRadii.clear();for(auto r:saved.value("radii").toArray())effectiveRadii<<r.toDouble();}}
    BlocksDialog dialog(cloud,effectiveKeys,effectiveRadii,parent);dialog.publish=publish;dialog.training=training;dialog.exec();if(dialog.trainingResult){if(training)training(dialog.trainingResult);else delete dialog.trainingResult;}
}
void WorkspaceDock::showClassificationWorkspace(){if(m_tabs)m_tabs->setCurrentIndex(3);}
void WorkspaceController::openSpatialBlocks(){
    if(!m_selectedCloud||!m_app)return;if(!requireMetricUnits("Spatial blocks"))return;QStringList keys;const auto radii=m_dock->scaleRadii();for(const auto& id:m_dock->selectedFeatureIds()){bool ok=false;auto feature=featureIdFromUi(id,ok);if(!ok||id=="hag")continue;for(double r:radii){FeatureKey k;k.feature=feature;k.radius=r;keys<<serializeModelFeatureKey(k);}}
    ccPointCloud* copy=nullptr;bool trainingChosen=false;
    showSpatialBlocks(m_selectedCloud,keys,radii,m_dock,[this,&copy](ccPointCloud* c){copy=c;m_app->addToDB(c,false,true,false,false);refreshHost();},[this,&trainingChosen](ccPointCloud* c){trainingChosen=true;m_app->addToDB(c,false,true,false,false);m_app->setSelectedInDB(c,true);selectCloud(c);m_dock->showClassificationWorkspace();showReady("Selected training cores created. Choose this loaded cloud as training set, confirm its class field, then Train. Halo rows excluded; labels are not automatically trusted.");});
    if(copy&&!trainingChosen){m_app->setSelectedInDB(m_selectedCloud,false);m_app->setSelectedInDB(copy,true);selectCloud(copy);}updateModelFeatureChoices();syncDisplayControls();refreshHost();updateDockState();
}
}
