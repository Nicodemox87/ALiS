// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "MlInputFields.h"
#include "MemorySafety.h"
#include <ccPointCloud.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <cstring>
#include <stdexcept>

namespace alis {
namespace {
class PreparationDialog : public QDialog {
public:
    using QDialog::QDialog;
    bool active=false;
    void reject() override { if(!active) QDialog::reject(); }
};
void writeJson(const QString& path,const QJsonObject& object) {
    QSaveFile f(path);const auto data=QJsonDocument(object).toJson();
    if(!f.open(QIODevice::WriteOnly)||f.write(data)!=data.size()||!f.commit())throw std::runtime_error("Cannot save job configuration");
}
QJsonObject readJson(const QString& path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read job manifest");
    QJsonParseError error;auto doc=QJsonDocument::fromJson(f.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())throw std::runtime_error("Invalid job manifest");return doc.object();
}
}

void WorkspaceController::prepareModelData(const QString& python,const QString& worker,const QString& selectedModel) {
    PreparationDialog dialog(m_dock);dialog.setWindowTitle("ALiS - Prepare data for this model");dialog.resize(850,720);dialog.setWindowModality(Qt::ApplicationModal);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("1. Inspect a trusted model  >  2. Prepare bounded blocks  >  3. Classify  >  4. Review Derived predictions\nThe source is unchanged. Halo points provide context only; final outputs contain every original point once.");intro->setWordWrap(true);layout->addWidget(intro);
    auto* form=new QFormLayout;layout->addLayout(form);
    auto pathRow=[&](const QString& label,const QString& initial,bool directory,const QString& filter) {
        auto* row=new QWidget;auto* h=new QHBoxLayout(row);h->setContentsMargins(0,0,0,0);auto* edit=new QLineEdit(initial);auto* browse=new QPushButton("Browse...");h->addWidget(edit);h->addWidget(browse);form->addRow(label,row);
        connect(browse,&QPushButton::clicked,&dialog,[&,edit,directory,filter](){auto p=directory?QFileDialog::getExistingDirectory(&dialog,"Choose folder",edit->text()):QFileDialog::getOpenFileName(&dialog,"Choose file",edit->text(),filter);if(!p.isEmpty())edit->setText(p);});return edit;
    };
    auto* model=pathRow("Saved model",selectedModel,false,"ALiS model (*.joblib)");
    auto* trust=new QCheckBox("I trust this model's author (joblib/pickle can execute code)");form->addRow(trust);
    auto* source=new QComboBox;source->addItem("Selected cloud in CloudCompare (current fields / edits)");source->addItem("LAS / LAZ directly from disk (no full-cloud loading)");form->addRow("Source",source);
    auto* cloud=m_selectedCloud;const quint64 uid=cloud?cloud->getUniqueID():0;const unsigned count=cloud?cloud->size():0;
    auto* selected=new QLabel(cloud?QString("%1 - %2 points").arg(cloud->getName()).arg(count):"No selected CC cloud: choose disk mode");form->addRow("CC snapshot",selected);
    if(!cloud)source->setCurrentIndex(1);
    auto* input=pathRow("Disk LAS / LAZ",QString(),false,"Point clouds (*.las *.laz)");
    auto* output=pathRow("Job / output folder",QDir::homePath()+"/ALiS_jobs/prepare_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"),true,QString());
    auto* size=new QDoubleSpinBox;size->setRange(1,10000);size->setValue(50);size->setSuffix(" m");form->addRow("Maximum block side",size);
    auto* budget=new QDoubleSpinBox;budget->setRange(.5,64);budget->setValue(2);budget->setSuffix(" GiB");form->addRow("Additional worker RAM budget",budget);
    auto* metric=new QCheckBox("Coordinates / radii are in metres (verify CRS units)");form->addRow(metric);
    auto* reuse=new QCheckBox("Reuse named geometric SFs: I verified their radii, units and provenance");form->addRow(reuse);
    auto* hag=new QCheckBox("Allow missing HAG from the model's recorded CSF / terrain recipe (validate tile edges)");form->addRow(hag);
    QJsonObject recipeOverride;
    auto* recipe=new QPushButton("Set explicit CSF / HAG recipe for a legacy model...");form->addRow(recipe);
    connect(recipe,&QPushButton::clicked,&dialog,[&](){
        const auto g=currentGroundParameters();
        QJsonObject proposed=recipeOverride.isEmpty()?QJsonObject{{"ground_recipe",QJsonObject{{"clothResolution",g.clothResolution},{"classificationThreshold",g.classificationThreshold},{"rigidness",g.rigidness},{"iterations",g.iterations},{"timeStep",g.timeStep},{"smoothSlope",g.smoothSlope}}},{"terrain_recipe",QJsonObject{{"gridStep",.5},{"interpolateEmptyCells",true},{"maximumInterpolationEdgeLength",10.}}}}:recipeOverride;
        bool ok=false;const auto text=QInputDialog::getMultiLineText(&dialog,"Explicit metric terrain recipe","Use only if this matches your training process. Defaults below are NOT inferred from the model.\nEdit values in metres. Empty text restores the model recipe.",QString::fromUtf8(QJsonDocument(proposed).toJson()),&ok);
        if(ok){if(text.trimmed().isEmpty()){recipeOverride={};recipe->setText("Set explicit CSF / HAG recipe for a legacy model...");return;}QJsonParseError err;auto doc=QJsonDocument::fromJson(text.toUtf8(),&err);if(err.error!=QJsonParseError::NoError||!doc.isObject()){QMessageBox::warning(&dialog,"Recipe","Invalid JSON; recipe unchanged.");return;}recipeOverride=doc.object();recipe->setText("Explicit CSF / HAG recipe selected - edit...");}
    });
    auto* info=new QPlainTextEdit;info->setReadOnly(true);info->setMaximumBlockCount(600);layout->addWidget(info,1);
    auto* bar=new QProgressBar;bar->setRange(0,100);layout->addWidget(bar);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* inspect=new QPushButton("1. Inspect model");auto* prepare=new QPushButton("2. Prepare / resume");auto* predict=new QPushButton("3. Classify prepared");auto* import=new QPushButton("4. Show in CC");auto* stop=new QPushButton("Stop safely");stop->setEnabled(false);
    for(auto* b:{inspect,prepare,predict,import,stop})actions->addWidget(b);
    auto* bottom=new QHBoxLayout;layout->addLayout(bottom);auto* open=new QPushButton("Open outputs / reports");auto* close=new QPushButton("Close");bottom->addWidget(open);bottom->addWidget(close);
    connect(close,&QPushButton::clicked,&dialog,&QDialog::reject);
    connect(open,&QPushButton::clicked,&dialog,[&](){QDesktopServices::openUrl(QUrl::fromLocalFile(output->text()));});
    QString job;bool cancelRequested=false;quint64 snapshotRevision=0;bool snapshotMade=false;
    connect(stop,&QPushButton::clicked,&dialog,[&](){cancelRequested=true;if(!job.isEmpty()){QFile f(QDir(job).filePath("CANCEL"));f.open(QIODevice::WriteOnly);}info->appendPlainText("Stopping safely; waiting for the current operation. Completed blocks are retained.");});
    const QString script=QDir(QFileInfo(worker).absolutePath()).filePath("alis_prepare.py");
    const QString native=QDir(QCoreApplication::applicationDirPath()).filePath("ALiS_prepare_block.exe");
    auto run=[&](const QStringList& args) {
        QProcess process;process.setProcessChannelMode(QProcess::MergedChannels);QByteArray pending;
        QEventLoop loop;
        connect(&process,&QProcess::readyReadStandardOutput,&dialog,[&](){pending+=process.readAllStandardOutput();int nl;while((nl=pending.indexOf('\n'))>=0){auto line=pending.left(nl);pending.remove(0,nl+1);auto o=QJsonDocument::fromJson(line).object();if(!o.isEmpty()){const double total=o.value("total").toDouble();bar->setRange(0,total>0?100:0);if(total>0)bar->setValue(int(100*o.value("done").toDouble()/total));info->appendPlainText(o.value("phase").toString()+": "+o.value("message").toString());}else info->appendPlainText(QString::fromUtf8(line));}});
        connect(&process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),&loop,&QEventLoop::quit);
        process.start(python,QStringList()<<script<<args);
        if(!process.waitForStarted(5000))throw std::runtime_error(process.errorString().toStdString());
        bar->setRange(0,0);loop.exec();
        if(!pending.isEmpty())info->appendPlainText(QString::fromUtf8(pending));
        if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0)throw std::runtime_error("Operation did not complete. See the message above and status.json; source unchanged.");
        bar->setRange(0,100);bar->setValue(100);
    };
    auto execute=[&](int action) {
        if(dialog.active)return;
        QScopedValueRollback<bool> active(dialog.active,true);m_dock->setBusy(true);cancelRequested=false;
        for(auto* b:{inspect,prepare,predict,import,close})b->setEnabled(false);stop->setEnabled(true);
        // Freeze all input controls so the immutable job identity cannot change while running.
        for(int r=0;r<form->rowCount();++r)for(auto role:{QFormLayout::FieldRole,QFormLayout::SpanningRole})if(auto* item=form->itemAt(r,role))if(item->widget())item->widget()->setEnabled(false);
        try {
            if(!trust->isChecked())throw std::runtime_error("Confirm that you trust the model before opening executable model artifacts.");
            job=QDir(output->text().trimmed()).absolutePath();if(!QDir().mkpath(job))throw std::runtime_error("Cannot create output directory");
            // Removing only our cancellation marker is the explicit resume action.
            if(action==1||action==2)QFile::remove(QDir(job).filePath("CANCEL"));
            if(action==0||action==1){
                run({"inspect","--model",model->text(),"--output",QDir(job).filePath("requirements.json")});
                const auto requirements=readJson(QDir(job).filePath("requirements.json"));
                info->appendPlainText(QString::fromUtf8(QJsonDocument(requirements).toJson()));
                if(action==1){
                    if(!metric->isChecked())throw std::runtime_error("Confirm metric units before preparation.");
                    QString sourcePath=input->text();
                    if(source->currentIndex()==0){
                        if(!cloud||m_selectedCloud!=cloud||cloud->getUniqueID()!=uid||cloud->size()!=count)throw std::runtime_error("Selected CC cloud changed. Close and reopen preparation.");
                        auto* s=session();snapshotRevision=s?s->sourceRevision():0;
                        sourcePath=QDir(job).filePath("cc_snapshot");QDir().mkpath(sourcePath);
                        std::vector<CloudAttributeInput> attrs;QJsonArray columns;
                        auto mapping=QJsonDocument::fromJson(cloud->getMetaData("ALiS.featureFields").toByteArray()).object();
                        if(mapping.isEmpty())mapping=QJsonDocument::fromJson(cloud->getMetaData("qArchaeoLiDAR.featureFields").toByteArray()).object();
                        for(const auto& value:requirements.value("features").toArray()){
                            const QString key=value.toString();CloudAttributeInput attr;QString error;
                            bool resolved=false;
                            if(key.startsWith("sf:")||key.startsWith("rgb:"))resolved=resolveCloudAttribute(*cloud,key,attr,error);
                            else if(key=="hag@0"|| (reuse->isChecked()&&mapping.contains(key))){const QString name=key=="hag@0"?QString::fromUtf8(field::HeightAboveGround):mapping.value(key).toString();resolved=resolveCloudAttribute(*cloud,scalarInputKey(name),attr,error);}
                            if(resolved){attrs.push_back(attr);columns.append(key);}
                        }
                        const qint64 bytes=qint64(count)*(24+4*attrs.size());QStorageInfo storage(job);
                        if(storage.bytesAvailable()<bytes+qint64(512)*1024*1024)throw std::runtime_error("Insufficient disk space for the immutable CC snapshot.");
                        const QJsonObject meta{{"point_count",double(count)},{"origin",QJsonArray{cloud->toGlobal3d(*cloud->getPoint(0)).x,cloud->toGlobal3d(*cloud->getPoint(0)).y,cloud->toGlobal3d(*cloud->getPoint(0)).z}},{"columns",columns},{"entity_uid",double(uid)},{"source_revision",double(snapshotRevision)}};
                        const QString file=QDir(sourcePath).filePath("points.bin");QFile existing(file);const bool verify=existing.exists();QSaveFile saved(file);
                        if(verify){if(readJson(QDir(sourcePath).filePath("snapshot.json"))!=meta||!existing.open(QIODevice::ReadOnly)||existing.size()!=bytes)throw std::runtime_error("Snapshot identity changed. Choose a NEW output folder.");}
                        else if(!saved.open(QIODevice::WriteOnly))throw std::runtime_error("Cannot write snapshot");
                        info->appendPlainText("Exporting / verifying current CC points and fields, without a cloud clone...");bar->setRange(0,100);
                        for(unsigned first=0;first<count;){
                            const unsigned n=std::min(65536u,count-first);QByteArray buffer;buffer.reserve(int(n*(24+4*attrs.size())));
                            for(unsigned i=first;i<first+n;++i){const auto p=cloud->toGlobal3d(*cloud->getPoint(i));const double xyz[]={p.x,p.y,p.z};buffer.append(reinterpret_cast<const char*>(xyz),24);for(const auto& a:attrs){float v=a.value(*cloud,i);buffer.append(reinterpret_cast<const char*>(&v),4);}}
                            if(verify){if(existing.read(buffer.size())!=buffer)throw std::runtime_error("Cloud coordinates / fields changed. Choose a NEW output folder.");}
                            else if(saved.write(buffer)!=buffer.size())throw std::runtime_error("Snapshot write failed (disk full?)");
                            first+=n;bar->setValue(int(100.*first/count));QApplication::processEvents();if(cancelRequested)throw std::runtime_error("Snapshot canceled safely.");
                        }
                        if(!verify){if(!saved.commit())throw std::runtime_error("Cannot commit snapshot");writeJson(QDir(sourcePath).filePath("snapshot.json"),meta);}snapshotMade=true;
                    }
                    QJsonObject config{{"source",sourcePath},{"model",model->text()},{"native",native},{"metric_units",true},{"block_m",size->value()},{"memory_gib",budget->value()},{"reuse_features",reuse->isChecked()},{"allow_tiled_hag",hag->isChecked()}};
                    for(auto it=recipeOverride.begin();it!=recipeOverride.end();++it)if(it.key()=="ground_recipe"||it.key()=="terrain_recipe")config.insert(it.key(),it.value());
                    writeJson(QDir(job).filePath("config.json"),config);run({"prepare","--config",QDir(job).filePath("config.json"),"--job",job});
                }
            } else if(action==2){
                const auto plan=readJson(QDir(job).filePath("plan.json"));
                if(QFileInfo(plan.value("model").toString()).absoluteFilePath()!=QFileInfo(model->text()).absoluteFilePath())throw std::runtime_error("This job belongs to another model. Select its recorded model, or prepare a NEW job folder.");
                run({"classify","--job",job});info->appendPlainText("Results: result/REPORT.html, class counts, confidence and provenance. Disk input: classified.las. No ground-truth accuracy is inferred.");
            }
            else {
                if(readJson(QDir(job).filePath("status.json")).value("stage").toString()!="complete")throw std::runtime_error("Classification is not complete; partial outputs cannot be imported.");
                const auto plan=readJson(QDir(job).filePath("plan.json"));
                if(plan.value("source_kind").toString()=="las"){
                    info->appendPlainText("Use CloudCompare File > Open: "+QDir(job).filePath("classified.las")+". Loading the entire output is optional and uses RAM.");
                    QDesktopServices::openUrl(QUrl::fromLocalFile(job));
                }else{
                    if(!snapshotMade||!cloud||m_selectedCloud!=cloud||cloud->size()!=count||!session()||session()->sourceRevision()!=snapshotRevision)throw std::runtime_error("For safe CC alignment, re-run Prepare / resume on the unchanged selected cloud in this dialog before import.");
                    const auto result=readJson(QDir(job).filePath("result/result.json"));if(result.value("point_count").toDouble()!=count||result.value("output_checksums").toObject().size()!=2)throw std::runtime_error("Point count / checksum manifest mismatch");
                    QString error;if(!session()->importBlockedPredictions(QDir(job).filePath("result"),result,error))throw std::runtime_error(error.toStdString());snapshotRevision=session()->sourceRevision();
                    m_mlPredictionStatistics=QString::fromUtf8(QJsonDocument(result).toJson());m_predictionStatisticsCache[uid]=m_mlPredictionStatistics;
                    refreshHost();syncDisplayControls();info->appendPlainText("Derived Classification and Confidence displayed. Working / Original / Trusted labels unchanged. Review before Apply.");
                }
            }
        }catch(const std::exception& e){info->appendPlainText(QString("ERROR: ")+QString::fromUtf8(e.what()));bar->setRange(0,100);}
        for(int r=0;r<form->rowCount();++r)for(auto role:{QFormLayout::FieldRole,QFormLayout::SpanningRole})if(auto* item=form->itemAt(r,role))if(item->widget())item->widget()->setEnabled(true);
        for(auto* b:{inspect,prepare,predict,import,close})b->setEnabled(true);stop->setEnabled(false);m_dock->setBusy(false);updateDockState();
    };
    connect(inspect,&QPushButton::clicked,&dialog,[&](){execute(0);});connect(prepare,&QPushButton::clicked,&dialog,[&](){execute(1);});connect(predict,&QPushButton::clicked,&dialog,[&](){execute(2);});connect(import,&QPushButton::clicked,&dialog,[&](){execute(3);});
    dialog.exec();
}
}
