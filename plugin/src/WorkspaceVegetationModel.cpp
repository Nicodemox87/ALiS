// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "VegetationScoreIdentity.h"
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <ccColorScale.h>
#include <ccProgressDialog.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QSaveFile>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QCoreApplication>
#include <QScopedValueRollback>
#include <cmath>
#include <cstring>
#include <limits>

namespace alis {
void WorkspaceController::predictVegetationModel(const QString& model,const QString& directory,
                                                const QString& python,const QString& worker,bool trusted)
{
    if(m_featureComputationActive || m_mlProcess->state()!=QProcess::NotRunning || !m_selectedCloud || !session())return;
    if(!requireMetricUnits(QStringLiteral("Vegetation model")))return;
    if(!session()->hasAppliedGround()){showError(QStringLiteral("Apply Ground before running the vegetation model."));return;}
    if(!trusted){showError(QStringLiteral("Confirm that you trust this local model. Joblib can execute code; use only your own or verified model files."));return;}
    QFile file(model+QStringLiteral(".json"));
    if(!file.open(QIODevice::ReadOnly)){showError(QStringLiteral("Missing model manifest: select the .joblib together with its .joblib.json file."));return;}
    const auto metadata=QJsonDocument::fromJson(file.readAll()).object();
    if(metadata.value("schema").toString()!=QStringLiteral("alis-vegetation-local-model/1") ||
       metadata.value("transform").toString()!=QStringLiteral("alis-vegetation-normalized-geometry/1")){
        showError(QStringLiteral("Unsupported vegetation model schema."));return;
    }
    QVector<double> scales;for(const auto& value:metadata.value("radii_m").toArray())scales<<value.toDouble(-1);
    if(scales.size()!=2 || !(scales[0]>0 && scales[0]<scales[1] && scales[1]<=3)){
        showError(QStringLiteral("Invalid model radii; compatible fixed physical scales are required."));return;
    }
    const QStringList features{"neighbor_count","planarity","sphericity","surface_variation","linearity","normal_z_absolute",
        "barycenter_offset_ratio","roughness","z_range","z_standard_deviation","eigenvalues_sum","pca_1","pca_2"};
    auto* cloud=m_selectedCloud;auto* current=session();const auto revision=current->sourceRevision();
    computeFeatures(features,scales);
    if(m_selectedCloud!=cloud || session()!=current || current->sourceRevision()!=revision){showError(QStringLiteral("Cloud changed during feature preparation."));return;}
    if(directory.trimmed().isEmpty()){showError(QStringLiteral("Choose an output folder."));return;}
    const QString job=QDir(directory).filePath(QStringLiteral("vegetation_model_%1").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")));
    if(!QDir().mkpath(job)){showError(QStringLiteral("Cannot create vegetation model output folder."));return;}
    QStringList keys;for(const auto& name:features)for(double radius:scales){bool valid=false;const auto key=featureKey(name,radius,valid);if(!valid){showError(QStringLiteral("Unknown model feature."));return;}keys<<serializeModelFeatureKey(key);}
    QString error;const QString dataset=QDir(job).filePath(QStringLiteral("input"));
    if(!writeMlDataset(keys,dataset,error)){if(!error.isEmpty())showError(error);return;}
    if(m_selectedCloud!=cloud || current->sourceRevision()!=revision){showError(QStringLiteral("Cloud changed during export."));return;}
    m_vegetationSourceRevision=revision;m_vegetationOutputPath=QDir(job).filePath(QStringLiteral("result"));
    const QString script=QDir(QFileInfo(worker).absolutePath()).filePath(QStringLiteral("vegetation_model_worker.py"));
    launchMlWorker(QStringLiteral("vegetation-predict"),python,script,
        {"--dataset",dataset,"--model",model,"--output",m_vegetationOutputPath,"--trust-local-model"});
}

bool WorkspaceController::loadVegetationScores(QString& error)
{
    if(!m_selectedCloud || !session() || session()->sourceRevision()!=m_vegetationSourceRevision){error="Cloud changed; scores not imported.";return false;}
    QFile json(QDir(m_vegetationOutputPath).filePath("report.json"));
    if(!json.open(QIODevice::ReadOnly)){error="Vegetation report missing.";return false;}
    auto report=QJsonDocument::fromJson(json.readAll()).object();
    if(report.value("schema").toString()!=QStringLiteral("alis-vegetation-score/1") || report.value("point_count").toDouble()!=m_selectedCloud->size()){
        error="Vegetation output schema or point alignment mismatch.";return false;
    }
    QFile scores(QDir(m_vegetationOutputPath).filePath("score.f32"));
    if(!scores.open(QIODevice::ReadOnly) || scores.size()!=qint64(m_selectedCloud->size())*4){error="Truncated vegetation score array.";return false;}
    auto* field=new ccScalarField("qAL_VegetationScore");field->link();
    if(!field->resizeSafe(m_selectedCloud->size())){field->release();error="Not enough RAM for vegetation scores.";return false;}
    unsigned row=0;
    while(!scores.atEnd()){
        const QByteArray bytes=scores.read(1024*1024);
        if(bytes.isEmpty() || bytes.size()%4){field->release();error="Score read failure.";return false;}
        for(int offset=0;offset<bytes.size();offset+=4){float value;std::memcpy(&value,bytes.constData()+offset,4);
            if(row>=m_selectedCloud->size() || (!std::isnan(value)&&(!std::isfinite(value)||value<0||value>1))){field->release();error="Invalid vegetation score.";return false;}
            field->setValue(row++,value);
        }
    }
    if(row!=m_selectedCloud->size()){field->release();error="Incomplete vegetation scores.";return false;}
    field->computeMinAndMax();int previous=m_selectedCloud->getScalarFieldIndexByName("qAL_VegetationScore");
    if(previous>=0)m_selectedCloud->deleteScalarField(previous);
    if(m_selectedCloud->addScalarField(field)<0){field->release();error="Could not publish vegetation score field.";return false;}field->release();
    report.insert("source_uid",QString::number(m_selectedCloud->getUniqueID()));report.insert("source_revision",QString::number(session()->sourceRevision()));
    const auto featureIndex=QJsonDocument::fromJson(m_selectedCloud->getMetaData("ALiS.featureFields").toByteArray()).object();
    QStringList identityFields{QString::fromUtf8(field::WorkingAsprs),QStringLiteral("qAL_VegetationScore")};
    for(const auto& key:report.value("feature_keys").toArray()){
        const QString name=featureIndex.value(key.toString()).toString();
        if(name.isEmpty()){error="Model feature identity missing; recompute compatible scores.";return false;}
        identityFields<<name;
    }
    if(identityFields.size()!=28){error="Incomplete model feature identity.";return false;}
    ccProgressDialog identityProgress(true,m_dock);identityProgress.setWindowTitle(QStringLiteral("Save portable score identity"));
    identityProgress.setLabelText(QStringLiteral("Checking all geometry, feature fields and scores"));identityProgress.setRange(0,100);identityProgress.show();
    QString digest;
    { QScopedValueRollback<bool> checking(m_featureComputationActive,true);
      digest=vegetationScoreFingerprint(*m_selectedCloud,identityFields,error,[&](int percent){identityProgress.setValue(percent);return !identityProgress.wasCanceled();}); }
    identityProgress.hide();
    if(digest.isEmpty())return false;
    report.insert("content_identity",QJsonObject{{"schema","alis-vegetation-score-content/1"},
        {"sha256",digest},{"fields",QJsonArray::fromStringList(identityFields)}});
    report.insert("output_directory",m_vegetationOutputPath);report.insert("cloud_profile",m_cloudProfile);
    m_selectedCloud->setMetaData("ALiS.vegetationScores",QJsonDocument(report).toJson(QJsonDocument::Compact));
    if(auto* spin=m_dock->findChild<QDoubleSpinBox*>("ALiS.Workspace.Terrain.Vegetation.ScoreThreshold"))spin->setValue(report.value("model_threshold").toDouble());
    const auto previousReview=m_selectedCloud->getMetaData("ALiS.vegetationPreview").toByteArray();
    filterVegetationScore(report.value("model_threshold").toDouble());
    if(m_selectedCloud->getMetaData("ALiS.vegetationPreview").toByteArray()==previousReview){error="Scores imported, but review publication failed. Check report-folder permissions and apply the threshold again.";return false;}
    return true;
}

void WorkspaceController::filterVegetationScore(double threshold)
{
    if(m_featureComputationActive || m_mlProcess->state()!=QProcess::NotRunning || !m_selectedCloud || !session())return;
    if(!std::isfinite(threshold)||threshold<0||threshold>1.000001){showError(QStringLiteral("Invalid score threshold."));return;}
    auto* cloud=m_selectedCloud;auto* current=session();
    auto report=QJsonDocument::fromJson(cloud->getMetaData("ALiS.vegetationScores").toByteArray()).object();
    const int scoreIndex=cloud->getScalarFieldIndexByName("qAL_VegetationScore");
    const auto identity=report.value("content_identity").toObject();
    if(scoreIndex<0 || identity.value("schema").toString()!=QStringLiteral("alis-vegetation-score-content/1") ||
        identity.value("sha256").toString().size()!=64){
        showError(QStringLiteral("These scores have no portable content identity. Recompute once, then save as BIN to reuse them after reopening."));return;
    }
    QStringList identityFields;for(const auto& name:identity.value("fields").toArray())identityFields<<name.toString();
    if(identityFields.size()!=28 || !identityFields.contains(QString::fromUtf8(field::WorkingAsprs))){showError(QStringLiteral("Incomplete saved score identity."));return;}
    QString identityError;
    ccProgressDialog progress(true,m_dock);progress.setWindowTitle(QStringLiteral("Verify saved vegetation scores"));
    progress.setLabelText(QStringLiteral("Checking all points, model feature fields and scores — no inference"));progress.setRange(0,100);progress.show();
    QString digest;
    { QScopedValueRollback<bool> checking(m_featureComputationActive,true);
    digest=vegetationScoreFingerprint(*cloud,identityFields,identityError,[&](int percent){
        progress.setValue(percent);QCoreApplication::processEvents();
        return !progress.wasCanceled() && m_selectedCloud==cloud && session()==current;
    }); } progress.hide();
    if(digest.isEmpty() || digest!=identity.value("sha256").toString()){
        showError(identityError.isEmpty()?QStringLiteral("Geometry, labels, features or scores changed. Recompute model scores; previous review preserved."):identityError);return;
    }
    report.insert("source_uid",QString::number(cloud->getUniqueID()));report.insert("source_revision",QString::number(current->sourceRevision()));
    auto* score=cloud->getScalarField(scoreIndex);
    const int workingIndex=cloud->getScalarFieldIndexByName(field::WorkingAsprs);
    if(workingIndex<0 || score->currentSize()!=cloud->size()){showError(QStringLiteral("Incomplete session or score field."));return;}
    auto* working=cloud->getScalarField(workingIndex);const auto ground=current->appliedGroundMask();
    if(ground.size()!=cloud->size()){showError(QStringLiteral("Ground mask is not aligned."));return;}
    auto* review=new ccScalarField("qAL_VegetationReview");review->link();
    if(!review->resizeSafe(cloud->size())){review->release();showError(QStringLiteral("Not enough RAM for preview."));return;}
    quint64 proposed=0,retained=0,protectedGround=0,excluded=0,missing=0;
    for(unsigned i=0;i<cloud->size();++i){const auto raw=working->getValue(i);const int code=std::isfinite(raw)?int(raw):1;
        int category=0;
        if(ground[i]){category=3;++protectedGround;}
        else if(code==7||code==9||code==18||code==22){category=4;++excluded;}
        else if(std::isfinite(score->getValue(i))&&score->getValue(i)>=threshold){category=1;++proposed;}
        else{++retained;missing+=!std::isfinite(score->getValue(i));}
        review->setValue(i,ScalarType(category));
    }
    review->computeMinAndMax();
    report.insert("threshold",threshold);report.insert("proposed_vegetation",double(proposed));report.insert("retained_uncertain",double(retained));
    report.insert("protected_ground",double(protectedGround));report.insert("excluded_points",double(excluded));report.insert("missing_score_non_ground",double(missing));
    report.insert("created_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    // A moved BIN must not depend on the original workstation's report folder.
    const auto* directoryEdit=m_dock->findChild<QLineEdit*>("ALiS.Workspace.Terrain.Vegetation.Output");
    const QString selectedDirectory=directoryEdit?directoryEdit->text().trimmed():QString();
    const QString directory=selectedDirectory.isEmpty()?report.value("output_directory").toString():selectedDirectory;
    if(directory.isEmpty() || !QDir().mkpath(directory)){review->release();showError(QStringLiteral("Choose a writable vegetation output folder."));return;}
    report.insert("output_directory",directory);
    const QString path=QDir(directory).filePath(QStringLiteral("review_%1.json").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")));
    QSaveFile output(path);auto bytes=QJsonDocument(report).toJson();
    if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit()){review->release();showError(QStringLiteral("Could not save review report; previous review preserved."));return;}
    const auto validation=report.value("validation").toObject();const auto test=validation.value("test").toObject();
    const QString summary=QStringLiteral("Vegetation proposal: %1\nRetained / uncertain: %2\nProtected Ground: %3\nMissing non-ground scores: %4\nThreshold: %5 (not calibrated probability).\nTraining-site test AT THE MODEL'S ORIGINAL REFERENCE THRESHOLD: precision %6%, recall %7%, worst block Building error %8%. These are NOT current-cloud accuracy or re-evaluated metrics for a changed threshold.\nReport: %9\nNo Working classes changed. Review before extraction.")
        .arg(QString::number(proposed),QString::number(retained),QString::number(protectedGround),QString::number(missing),QString::number(threshold,'g',9))
        .arg(100*test.value("vegetation_precision").toDouble(),0,'f',2).arg(100*test.value("vegetation_recall").toDouble(),0,'f',2)
        .arg(100*validation.value("worst_block_building_error").toDouble(),0,'f',2).arg(path);
    QSaveFile human(path+".html");const auto html=(QStringLiteral("<!doctype html><meta charset='utf-8'><title>ALiS vegetation model review</title><style>body{font:16px system-ui;max-width:1000px;margin:35px auto;color:#183653}pre{white-space:pre-wrap}</style><h1>Vegetation model - EXPERIMENTAL</h1><pre>%1</pre><h2>Parameters and provenance</h2><pre>%2</pre>").arg(summary.toHtmlEscaped(),QString::fromUtf8(bytes).toHtmlEscaped())).toUtf8();
    if(!human.open(QIODevice::WriteOnly)||human.write(html)!=html.size()||!human.commit()){review->release();showError(QStringLiteral("Could not save readable report; previous review preserved."));return;}
    const int old=cloud->getScalarFieldIndexByName("qAL_VegetationReview");if(old>=0)cloud->deleteScalarField(old);
    const int newIndex=cloud->addScalarField(review);review->release();if(newIndex<0){showError(QStringLiteral("Could not publish review."));return;}
    // Old heuristic height proposals must not appear to belong to new model scores.
    for(const char* stale:{"qAL_VegetationASPRS","qAL_VegetationEvidence"}){int index=cloud->getScalarFieldIndexByName(stale);if(index>=0)cloud->deleteScalarField(index);}
    auto palette=ccColorScale::Create(QStringLiteral("ALiS vegetation review"));
    palette->insert(ccColorScaleElement(0,QColor(245,190,45)),false);palette->insert(ccColorScaleElement(.25,QColor(40,170,70)),false);
    palette->insert(ccColorScaleElement(.5,QColor(70,145,230)),false);palette->insert(ccColorScaleElement(.75,QColor(160,110,65)),false);
    palette->insert(ccColorScaleElement(1,QColor(120,120,120)),false);palette->update();palette->setAbsolute(0,4);
    static_cast<ccScalarField*>(cloud->getScalarField(cloud->getScalarFieldIndexByName("qAL_VegetationReview")))->setColorScale(palette);
    cloud->setMetaData("ALiS.vegetationPreview",QJsonDocument(report).toJson(QJsonDocument::Compact));
ProcessingRecord record;record.operation="Vegetation.ModelScoreReview";record.timestampUtc=QDateTime::currentDateTimeUtc();record.sourceEntityUid=current->entityUid();record.affectedPoints=cloud->size();record.parameters=report;record.parameters.insert("report_path",path);record.outputFields=QStringList{"qAL_VegetationScore","qAL_VegetationReview"};current->addProcessingRecord(record);
    QString error;current->displayScalarField("qAL_VegetationReview",error);refreshHost();updateModelFeatureChoices();syncDisplayControls();updateDockState();showReady(summary);
    if(m_app)QMessageBox::information(m_dock,QStringLiteral("Vegetation model review"),summary);
}
}
