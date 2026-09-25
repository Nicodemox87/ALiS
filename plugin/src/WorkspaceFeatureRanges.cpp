// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "FeatureRangeRules.h"
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <ccColorScale.h>
#include <ccProgressDialog.h>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QPainter>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileDialog>
#include <QSaveFile>
#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QStandardPaths>
#include <QScopedValueRollback>
#include <QCoreApplication>
#include <QProcess>
#include <QScrollArea>
#include <functional>
#include <map>
#include <array>
#include <algorithm>

namespace {
struct Box {int code=0;unsigned count=0;std::array<double,5> q;};
class RangeChart final:public QWidget {
public:
    std::vector<Box> boxes;QString title;double low=0,high=1,rangeLow=0,rangeHigh=1;bool enabled=false,dragging=false;int anchor=0;
    std::function<void(double,double)> selected;
    RangeChart(){setMinimumSize(560,220);setMouseTracking(true);}
    double at(int x)const{return low+(std::max(160,std::min(width()-25,x))-160)*(high-low)/std::max(1,width()-185);}
    int position(double x)const{return 160+int((std::max(low,std::min(high,x))-low)/std::max(1e-15,high-low)*(width()-185));}
protected:
    void mousePressEvent(QMouseEvent* e)override{if(e->button()==Qt::LeftButton&&e->x()>=160){anchor=e->x();dragging=true;}}
    void mouseMoveEvent(QMouseEvent* e)override{if(dragging){rangeLow=at(std::min(anchor,e->x()));rangeHigh=at(std::max(anchor,e->x()));enabled=true;update();}}
    void mouseReleaseEvent(QMouseEvent* e)override{if(dragging){dragging=false;if(selected)selected(at(std::min(anchor,e->x())),at(std::max(anchor,e->x())));}}
    void paintEvent(QPaintEvent*)override{
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#f4f7fb"));
        p.setPen(QColor("#193454"));p.drawText(8,16,title);
        if(enabled){int a=position(std::max(low,rangeLow)),b=position(std::min(high,rangeHigh));if(b>=a)p.fillRect(QRect(a,15,b-a,height()-50),QColor(70,170,200,45));}
        const QColor colors[]={QColor("#5686c4"),QColor("#439663"),QColor("#ac7547"),QColor("#9366b5")};
        const int step=std::max(24,(height()-65)/std::max(1,int(boxes.size())));
        for(int i=0;i<int(boxes.size());++i){const auto& b=boxes[i];int y=35+i*step;p.setPen(QColor("#193454"));p.drawText(8,y+4,QStringLiteral("Class %1  n=%2").arg(b.code).arg(b.count));
            p.setPen(QPen(colors[i%4],3));p.drawLine(position(b.q[0]),y,position(b.q[4]),y);p.fillRect(QRect(position(b.q[1]),y-6,std::max(1,position(b.q[3])-position(b.q[1])),12),colors[i%4]);p.setBrush(Qt::black);p.setPen(Qt::NoPen);p.drawEllipse(QPoint(position(b.q[2]),y),3,3);}
        p.setPen(QColor("#193454"));p.drawText(160,height()-12,QString::number(low,'g',6));p.drawText(width()-110,height()-12,QString::number(high,'g',6));
    }
};
}

namespace alis {
bool WorkspaceController::refineVegetationWithRanges(const QString& directory,QString& error){
    if(!m_selectedCloud||!session()||m_featureComputationActive||m_mlProcess->state()!=QProcess::NotRunning){error="Select a cloud and wait for active processing.";return false;}
    auto* cloud=m_selectedCloud;const auto revision=session()->sourceRevision();
    auto previous=QJsonDocument::fromJson(cloud->getMetaData("ALiS.vegetationPreview").toByteArray()).object();
    int index=cloud->getScalarFieldIndexByName("qAL_VegetationReview");
    if(index<0||previous.value("source_uid").toString()!=QString::number(cloud->getUniqueID())||previous.value("source_revision").toString()!=QString::number(revision)){error="Run a fresh vegetation preview before feature refinement.";return false;}
    const auto ranges=QJsonDocument::fromJson(cloud->getMetaData("ALiS.featureRangeReview").toByteArray()).object().value("rules").toArray();
    if(ranges.isEmpty()){error="First choose vegetation intervals in Feature distributions / range review and apply them.";return false;}
    if(!applyFeatureRanges(ranges,directory,error))return false;
    if(m_selectedCloud!=cloud||!session()||session()->sourceRevision()!=revision){error="Cloud changed; vegetation refinement canceled.";return false;}
    // Read the freshly checked mask, never a stale cached decision from edited SFs.
    auto* mask=cloud->getScalarField(cloud->getScalarFieldIndexByName("qAL_ReviewStatus"));
    auto* proposal=cloud->getScalarField(cloud->getScalarFieldIndexByName("qAL_VegetationReview"));
    if(!mask||!proposal||proposal->currentSize()!=cloud->size()){error="Misaligned vegetation proposal.";return false;}
    quint64 vetoed=0,remaining=0;for(unsigned i=0;i<cloud->size();++i)if(proposal->getValue(i)==1){if(mask->getValue(i)==1)++vetoed;else ++remaining;}
    QJsonObject report{{"schema","alis-vegetation-feature-refinement/1"},{"source_uid",QString::number(cloud->getUniqueID())},{"source_revision",QString::number(revision)},
        {"point_count",double(cloud->size())},{"base_preview",previous},{"feature_ranges",ranges},{"proposed_vegetation",double(remaining)},{"moved_to_uncertain",double(vetoed)},
        {"classes_modified",false},{"certified",false},{"rule","Vegetation proposal AND all inclusive feature ranges; rejected proposals retained as uncertain / To validate"}};
    const QString path=QDir(directory).filePath("vegetation_feature_refinement_"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")+".json");
    QSaveFile file(path);const auto bytes=QJsonDocument(report).toJson();if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){error="Could not save refinement report; vegetation proposal preserved.";return false;}
    const int asprsIndex=cloud->getScalarFieldIndexByName("qAL_VegetationASPRS");auto* proposedAsprs=asprsIndex<0?nullptr:cloud->getScalarField(asprsIndex);
    for(unsigned i=0;i<cloud->size();++i)if(proposal->getValue(i)==1&&mask->getValue(i)==1){proposal->setValue(i,0);if(proposedAsprs)proposedAsprs->setValue(i,1);}
    proposal->computeMinAndMax();if(proposedAsprs)proposedAsprs->computeMinAndMax();
    cloud->setMetaData("ALiS.vegetationPreview",QJsonDocument(report).toJson(QJsonDocument::Compact));
    ProcessingRecord record;record.operation="Vegetation.FeatureRangeRefinement";record.timestampUtc=QDateTime::currentDateTimeUtc();record.sourceEntityUid=session()->entityUid();record.affectedPoints=vetoed;record.parameters=report;record.parameters.insert("report_path",path);record.outputFields=QStringList{"qAL_VegetationReview","qAL_ReviewStatus"};if(proposedAsprs)record.outputFields<<"qAL_VegetationASPRS";session()->addProcessingRecord(record);
    session()->displayScalarField("qAL_VegetationReview",error);refreshHost();syncDisplayControls();showReady(QStringLiteral("Existing feature ranges: %1 vegetation proposals retained as To validate; %2 vegetation proposals remain. ASPRS unchanged. %3").arg(vetoed).arg(remaining).arg(path));return true;
}
void WorkspaceController::openFeatureRangeReview(){
    if(!m_selectedCloud||!session()||m_featureComputationActive||m_mlProcess->state()!=QProcess::NotRunning)return;
    auto* cloud=m_selectedCloud;const auto revision=session()->sourceRevision();
    QDialog dialog(m_dock);dialog.setWindowTitle(QStringLiteral("ALiS — feature distributions and range review"));dialog.resize(1080,650);
    auto* layout=new QVBoxLayout(&dialog);auto* note=new QLabel(QStringLiteral("Choose a feature; drag across the chart or enter an inclusive range. Rectangle = P25–P75, dot = median, line = P10–P90.\nClasses are descriptive, not automatically certified. All enabled ranges use AND; missing/out-of-range points become To validate. Ground and ASPRS remain unchanged."));note->setWordWrap(true);layout->addWidget(note);
    auto* body=new QHBoxLayout;auto* list=new QListWidget;list->setObjectName("ALiS.FeatureRanges.Fields");list->setMaximumWidth(330);body->addWidget(list);auto* right=new QVBoxLayout;auto* chart=new RangeChart;auto* chartScroll=new QScrollArea;chartScroll->setWidget(chart);chartScroll->setWidgetResizable(true);right->addWidget(chartScroll,1);
    auto* fast=new QCheckBox(QStringLiteral("Fast chart — at most 50,000 points; no feature recomputation"));fast->setChecked(true);right->addWidget(fast);
    auto* counts=new QLabel;counts->setWordWrap(true);right->addWidget(counts);
    auto* active=new QCheckBox(QStringLiteral("Use range for this feature"));auto* low=new QDoubleSpinBox;auto* high=new QDoubleSpinBox;
    for(auto* spin:{low,high}){spin->setDecimals(9);spin->setRange(-1e30,1e30);}
    auto* form=new QFormLayout;form->addRow(active);form->addRow(QStringLiteral("Minimum (inclusive)"),low);form->addRow(QStringLiteral("Maximum (inclusive)"),high);right->addLayout(form);body->addLayout(right,1);layout->addLayout(body);
    auto* output=new QLineEdit(QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("ALiS/feature-range-reviews"));
    auto* browse=new QPushButton(QStringLiteral("Output folder…"));auto* paths=new QHBoxLayout;paths->addWidget(output);paths->addWidget(browse);layout->addLayout(paths);
    auto* apply=new QPushButton(QStringLiteral("Apply all enabled ranges → To validate"));apply->setObjectName("ALiS.FeatureRanges.Apply");auto* reset=new QPushButton(QStringLiteral("Reset review"));auto* savePlot=new QPushButton(QStringLiteral("Save chart PNG"));auto* close=new QPushButton(QStringLiteral("Close"));auto* buttons=new QHBoxLayout;buttons->addWidget(savePlot);buttons->addWidget(apply);buttons->addWidget(reset);buttons->addWidget(close);layout->addLayout(buttons);
    std::map<QString,QJsonObject> rules;bool loading=false;
    const auto previous=QJsonDocument::fromJson(cloud->getMetaData("ALiS.featureRangeReview").toByteArray()).object();
    for(const auto& value:previous.value("rules").toArray()){auto rule=value.toObject();rules[rule.value("field").toString()]=rule;}
    for(unsigned i=0;i<cloud->getNumberOfScalarFields();++i){const QString name=QString::fromUtf8(cloud->getScalarFieldName(i));
        if(name==QString::fromUtf8(field::WorkingAsprs)||name.contains("TrainingClass")||name.contains("Original")||name=="qAL_ReviewStatus")continue;new QListWidgetItem(name,list);}
    auto remember=[&](){if(loading||!list->currentItem())return;const auto name=list->currentItem()->text();rules[name]=QJsonObject{{"field",name},{"minimum",low->value()},{"maximum",high->value()},{"enabled",active->isChecked()}};chart->rangeLow=low->value();chart->rangeHigh=high->value();chart->enabled=active->isChecked();chart->update();};
    auto refresh=[&](){if(!list->currentItem())return;loading=true;const auto name=list->currentItem()->text();int index=cloud->getScalarFieldIndexByName(name.toUtf8().constData());if(index<0){loading=false;return;}auto* sf=cloud->getScalarField(index);
        const int classIndex=cloud->getScalarFieldIndexByName(field::WorkingAsprs);auto* classes=classIndex<0?nullptr:cloud->getScalarField(classIndex);
        const unsigned limit=fast->isChecked()?std::min(50000u,cloud->size()):cloud->size();std::map<int,std::vector<double>> values;unsigned valid=0;
        ccProgressDialog progress(true,&dialog);progress.setWindowModality(Qt::ApplicationModal);progress.setRange(0,100);progress.setMethodTitle("Reading feature distribution");if(!fast->isChecked())progress.show();
        for(unsigned j=0;j<limit;++j){if(!fast->isChecked()&&(j&65535)==0){progress.setValue(int(80.0*j/std::max(1u,limit)));QCoreApplication::processEvents();if(progress.wasCanceled()){loading=false;return;}}unsigned i=unsigned(quint64(j)*cloud->size()/std::max(1u,limit));double value=sf->getValue(i);if(!std::isfinite(value))continue;double code=classes?classes->getValue(i):0;values[std::isfinite(code)?int(code):0].push_back(value);++valid;}
        progress.setInfo("Sorting per-class values");chart->title=name;
        chart->boxes.clear();chart->low=std::numeric_limits<double>::infinity();chart->high=-chart->low;
        for(auto& pair:values){auto& v=pair.second;std::sort(v.begin(),v.end());Box box;box.code=pair.first;box.count=unsigned(v.size());int k=0;for(double q:{.1,.25,.5,.75,.9}){const double pos=(v.size()-1)*q;const auto a=std::size_t(pos);box.q[k++]=v[a]+(pos-a)*(v[std::min(a+1,v.size()-1)]-v[a]);}chart->low=std::min(chart->low,box.q[0]);chart->high=std::max(chart->high,box.q[4]);chart->boxes.push_back(box);}
        if(!std::isfinite(chart->low)){chart->low=0;chart->high=1;}if(chart->high<=chart->low)chart->high=chart->low+1;
        chart->setMinimumHeight(std::max(220,65+int(chart->boxes.size())*30));
        counts->setText(QStringLiteral("%1 / %2 sampled values finite. Chart shows P10–P90, not the full range; rare classes may be absent in fast mode. Rules check ALL %3 points.").arg(valid).arg(limit).arg(cloud->size()));
        const auto found=rules.find(name);active->setChecked(found!=rules.end()&&found->second.value("enabled").toBool());low->setValue(found!=rules.end()?found->second.value("minimum").toDouble():chart->low);high->setValue(found!=rules.end()?found->second.value("maximum").toDouble():chart->high);
        chart->rangeLow=low->value();chart->rangeHigh=high->value();chart->enabled=active->isChecked();chart->update();progress.hide();loading=false;};
    chart->selected=[&](double a,double b){loading=true;low->setValue(a);high->setValue(b);active->setChecked(true);loading=false;remember();};
    connect(list,&QListWidget::currentRowChanged,&dialog,[&](int){refresh();});connect(fast,&QCheckBox::toggled,&dialog,[&](bool){refresh();});connect(active,&QCheckBox::toggled,&dialog,[&](bool){remember();});connect(low,QOverload<double>::of(&QDoubleSpinBox::valueChanged),&dialog,[&](double){remember();});connect(high,QOverload<double>::of(&QDoubleSpinBox::valueChanged),&dialog,[&](double){remember();});
    connect(browse,&QPushButton::clicked,&dialog,[&](){auto path=QFileDialog::getExistingDirectory(&dialog,"Range review output",output->text());if(!path.isEmpty())output->setText(path);});
    connect(savePlot,&QPushButton::clicked,&dialog,[&](){if(!list->currentItem())return;auto path=QFileDialog::getSaveFileName(&dialog,"Save descriptive chart",QDir(output->text()).filePath("feature_distribution.png"),"PNG (*.png)");if(!path.isEmpty()&&!chart->grab().save(path))QMessageBox::warning(&dialog,"Chart","Could not save chart.");});
    connect(apply,&QPushButton::clicked,&dialog,[&](){if(m_selectedCloud!=cloud||session()->sourceRevision()!=revision){QMessageBox::warning(&dialog,"Cloud changed","Reopen this panel after cloud edits.");return;}remember();QJsonArray selected;for(const auto& pair:rules)if(pair.second.value("enabled").toBool())selected.append(pair.second);QString error;if(!applyFeatureRanges(selected,output->text(),error))QMessageBox::warning(&dialog,"Ranges",error);else QMessageBox::information(&dialog,"Range review","To validate status applied to all points. Original classes unchanged. This is a review filter; no automatic training or certified class assignment.");});
    connect(reset,&QPushButton::clicked,&dialog,[&](){if(m_selectedCloud!=cloud||!session()||session()->sourceRevision()!=revision)return;QString error;if(!applyFeatureRanges({},output->text(),error)){QMessageBox::warning(&dialog,"Ranges",error);return;}rules.clear();refresh();});
    connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);if(list->count())list->setCurrentRow(0);dialog.exec();
}

bool WorkspaceController::applyFeatureRanges(const QJsonArray& rules,const QString& directory,QString& error){
    if(!m_selectedCloud||!session()||m_featureComputationActive||m_mlProcess->state()!=QProcess::NotRunning){error="Select a cloud and wait for active processing.";return false;}
    auto* cloud=m_selectedCloud;const auto revision=session()->sourceRevision();
    struct Rule{CCCoreLib::ScalarField* sf;double low,high;};std::vector<Rule> prepared;
    for(const auto& value:rules){const auto rule=value.toObject();const auto name=rule.value("field").toString();const int index=m_selectedCloud->getScalarFieldIndexByName(name.toUtf8().constData());double low=rule.value("minimum").toDouble(std::numeric_limits<double>::quiet_NaN()),high=rule.value("maximum").toDouble(std::numeric_limits<double>::quiet_NaN());
        if(index<0||name=="qAL_ReviewStatus"||!withinFeatureRange(low,low,high)){error="Invalid/missing field or inverted range: "+name;return false;}auto* sf=m_selectedCloud->getScalarField(index);if(sf->currentSize()!=m_selectedCloud->size()){error="Misaligned feature field.";return false;}prepared.push_back({sf,low,high});}
    if(directory.trimmed().isEmpty()||!QDir().mkpath(directory)){error="Choose a writable report folder.";return false;}
    auto* review=new ccScalarField("qAL_ReviewStatus");review->link();if(!review->resizeSafe(m_selectedCloud->size())){review->release();error="Not enough memory.";return false;}
    const auto ground=session()->appliedGroundMask();quint64 flagged=0,protectedGround=0;
    QScopedValueRollback<bool> guard(m_featureComputationActive,true);ccProgressDialog progress(true,m_dock);progress.setWindowModality(Qt::ApplicationModal);progress.setMinimumDuration(0);progress.setRange(0,100);progress.setMethodTitle("Checking feature ranges on all points");progress.show();
    for(unsigned i=0;i<m_selectedCloud->size();++i){if((i&65535)==0){progress.setValue(int(100.0*i/m_selectedCloud->size()));QCoreApplication::processEvents();if(progress.wasCanceled()){review->release();error="Canceled; previous review preserved.";return false;}}bool accepted=true;if(ground.size()==m_selectedCloud->size()&&ground[i])++protectedGround;else for(const auto& rule:prepared)if(!withinFeatureRange(rule.sf->getValue(i),rule.low,rule.high)){accepted=false;break;}review->setValue(i,accepted?0.f:1.f);flagged+=!accepted;}
    progress.hide();if(m_selectedCloud!=cloud||!session()||session()->sourceRevision()!=revision){review->release();error="Cloud changed; review not published.";return false;}review->computeMinAndMax();const QJsonObject report{{"schema","alis-feature-range-review/1"},{"rules",rules},{"combination","AND; inclusive; NaN => To validate; no rules => reset"},{"point_count",double(m_selectedCloud->size())},{"to_validate",double(flagged)},{"protected_ground",double(protectedGround)},{"status_0","Within ranges / Ground protected"},{"status_1","To validate"},{"classes_modified",false},{"labels_certified",false},{"source_uid",QString::number(m_selectedCloud->getUniqueID())},{"source_revision",QString::number(session()->sourceRevision())}};
    const QString path=QDir(directory).filePath("feature_ranges_"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")+".json");QSaveFile file(path);auto bytes=QJsonDocument(report).toJson();if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){review->release();error="Could not save range report.";return false;}
    int old=m_selectedCloud->getScalarFieldIndexByName("qAL_ReviewStatus");if(old>=0)m_selectedCloud->deleteScalarField(old);if(m_selectedCloud->addScalarField(review)<0){review->release();error="Could not publish review field.";return false;}review->release();
    auto palette=ccColorScale::Create("ALiS - To validate");palette->insert(ccColorScaleElement(0,QColor("#5686c4")),false);palette->insert(ccColorScaleElement(1,QColor("#f3b52c")),false);palette->update();palette->setAbsolute(0,1);static_cast<ccScalarField*>(m_selectedCloud->getScalarField(m_selectedCloud->getScalarFieldIndexByName("qAL_ReviewStatus")))->setColorScale(palette);
    m_selectedCloud->setMetaData("ALiS.featureRangeReview",QJsonDocument(report).toJson(QJsonDocument::Compact));ProcessingRecord record;record.operation="Features.RangeReview";record.timestampUtc=QDateTime::currentDateTimeUtc();record.sourceEntityUid=session()->entityUid();record.affectedPoints=m_selectedCloud->size();record.parameters=report;record.parameters.insert("report_path",path);record.outputFields=QStringList{"qAL_ReviewStatus"};session()->addProcessingRecord(record);
    session()->displayScalarField("qAL_ReviewStatus",error);refreshHost();updateModelFeatureChoices();syncDisplayControls();showReady(QStringLiteral("To validate: %1 points. ASPRS unchanged. %2").arg(flagged).arg(path));return true;
}
}
