// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "CloudProfile.h"
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QDateTime>
#include <algorithm>
namespace alis {
namespace {
QString library(){return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("ALiS/presets");}
QByteArray featureFieldMetadata(const ccPointCloud& cloud){
    QVariant value=cloud.getMetaData("ALiS.featureFields");
    if(value.toByteArray().isEmpty())value=cloud.getMetaData("qArchaeoLiDAR.featureFields");
    return value.toByteArray();
}
bool writePreset(const QString& path,const ProcessingPreset& p,QString& error){
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)){error=file.errorString();return false;}
    auto bytes=QJsonDocument(processingPresetToJson(p)).toJson();
    if(file.write(bytes)!=bytes.size()||!file.commit()){error=file.errorString();return false;}return true;
}
bool readPreset(const QString& path,ProcessingPreset& p,QString& error){
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    if(file.size()>1024*1024){error="Preset exceeds 1 MiB";return false;}
    QJsonParseError jsonError;auto doc=QJsonDocument::fromJson(file.readAll(),&jsonError);
    if(jsonError.error!=QJsonParseError::NoError||!doc.isObject()){error="Invalid preset JSON";return false;}
    return processingPresetFromJson(doc.object(),p,error);
}
QString list(const QVector<double>& values){QStringList out;for(double v:values)out<<QString::number(v,'g',17);return out.join(", ");}
void mergeSpacing(QJsonObject& profile,const SpacingSummary& spacing){
    if(!spacing.valid)return;
    profile.insert("nnSamples",double(spacing.sampledPoints));profile.insert("nnMean",spacing.mean);profile.insert("nnMedian",spacing.median);
    profile.insert("nnMinimum",spacing.minimum);profile.insert("nnP10",spacing.percentile10);profile.insert("nnP90",spacing.percentile90);
    profile.insert("nnDuplicates",double(spacing.duplicateSamples));profile.insert("nnDensity2D",spacing.estimatedDensity2D);
}
}
void WorkspaceController::refreshCloudProfile(bool measureSpacing,bool forceScan){
    if(!m_selectedCloud)return;
    m_dock->setBusy(true);m_dock->setProgress(QStringLiteral("Point cloud information"),0,0,false);
    try {
        SpacingSummary spacing;std::string error;bool haveSpacing=false;
		const bool needSpacing=measureSpacing&&(forceScan||!m_cloudProfile.contains("nnMedian"));
        if(needSpacing){auto it=m_featureEngines.find(m_selectedCloud->getUniqueID());if(it!=m_featureEngines.end())haveSpacing=it->second->estimateSpacing(spacing,error,2048);}
		if(forceScan||m_cloudProfile.isEmpty()){
			const QJsonObject previous=m_cloudProfile;
			m_cloudProfile=cloudProfile(*m_selectedCloud,m_metricUnitsConfirmed,haveSpacing?&spacing:nullptr);
			if(!haveSpacing)for(auto it=previous.begin();it!=previous.end();++it)if(it.key().startsWith("nn"))m_cloudProfile.insert(it.key(),it.value());
		}else{
			m_cloudProfile.insert("name",m_selectedCloud->getName());m_cloudProfile.insert("metricConfirmed",m_metricUnitsConfirmed);
			if(haveSpacing)mergeSpacing(m_cloudProfile,spacing);
		}
		m_cloudProfile.remove("statisticsStale");m_cloudProfile.remove("staleReason");
		m_cloudProfileCache[static_cast<quint64>(m_selectedCloud->getUniqueID())]=m_cloudProfile;
		m_dock->setCatalogCloudProfile(m_cloudProfile);
		m_estimatedSpacingMetres=m_cloudProfile.value("nnMedian").toDouble();m_estimatedDensityPerSquareMetre=m_cloudProfile.value("nnDensity2D").toDouble();
        m_dock->setCloudProfileText(cloudProfileText(m_cloudProfile)+(error.empty()?QString():QStringLiteral("\nSpacing unavailable: ")+QString::fromStdString(error)));
	}catch(const std::bad_alloc&){m_dock->clearProgress();showError(QStringLiteral("Not enough memory for cloud profile."));return;}
    m_dock->clearProgress();m_dock->setBusy(false);
}
void WorkspaceController::markCloudProfileStale(const QString& reason){
	if(!m_selectedCloud||m_cloudProfile.isEmpty())return;
	m_cloudProfile.insert("statisticsStale",true);m_cloudProfile.insert("staleReason",reason);
	m_cloudProfileCache[static_cast<quint64>(m_selectedCloud->getUniqueID())]=m_cloudProfile;
	m_dock->setCatalogCloudProfile(m_cloudProfile);m_dock->setCloudProfileText(cloudProfileText(m_cloudProfile));
}
ProcessingPreset WorkspaceController::captureProcessingPreset() const {
    auto p=m_dock->processingPreset();p.base=m_groundBaseParameters;p.name=m_processingPresetName;p.sourceProfile=m_cloudProfile;return p;
}
void WorkspaceController::applyProcessingPreset(const ProcessingPreset& p){
    m_processingPresetName=p.name;m_groundBaseParameters=p.base;
    m_groundParameters=applySimpleGroundControls(p.base,static_cast<SimpleLevel>(p.complexity),static_cast<SimpleLevel>(p.microrelief),static_cast<SimpleLevel>(p.vegetation),0.);
    m_groundAlgorithmId=p.algorithm;m_groundFilter.reset(p.algorithm.startsWith("pmf.")?static_cast<GroundFilter*>(new PMFGroundFilter):static_cast<GroundFilter*>(new CSFGroundFilter));
    m_pmfWindowSizes=list(p.windows);m_pmfThresholds=list(p.thresholds);m_pmfCellSizeMetres=p.cellSize;
    m_dock->setProcessingPreset(p);
}
void WorkspaceController::reloadProcessingLibrary(){
    QStringList paths,names;QDir dir(library());for(auto file:dir.entryInfoList({"*.json"},QDir::Files,QDir::Name)){ProcessingPreset p;QString e;if(readPreset(file.absoluteFilePath(),p,e)){paths<<file.absoluteFilePath();names<<p.name;}}
    m_dock->setPresetLibrary(paths,names);
}
void WorkspaceController::saveProcessingPreset(){
    auto p=captureProcessingPreset();bool ok=false;
    p.name=QInputDialog::getText(m_dock,QStringLiteral("New multiscale preset"),QStringLiteral("Name"),QLineEdit::Normal,p.name,&ok).trimmed();if(!ok)return;
    ProcessingPreset validated;QString error;if(!processingPresetFromJson(processingPresetToJson(p),validated,error)){showError(error);return;}
    QString path=QFileDialog::getSaveFileName(m_dock,QStringLiteral("Export new preset JSON"),p.name+".json",QStringLiteral("Preset JSON (*.json)"));if(path.isEmpty())return;
    if(!path.endsWith(".json",Qt::CaseInsensitive))path+=".json";
    if(!QDir().mkpath(library())){showError("Cannot create preset library");return;}
    if(!writePreset(path,p,error)){showError(error);return;}
    QString stored=QDir(library()).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)+".json");
    if(!writePreset(stored,p,error)){showError(QStringLiteral("Export saved, but library copy failed: ")+error);return;}
    m_processingPresetName=p.name;reloadProcessingLibrary();m_dock->setProcessingSuggestion(QStringLiteral("Saved %1. Exact ground/context/feature radii and source profile stored. Export: %2").arg(p.name,path));showReady("New preset saved; existing presets unchanged.");
}
void WorkspaceController::importProcessingPreset(){
    QString path=QFileDialog::getOpenFileName(m_dock,QStringLiteral("Import multiscale preset"),QString(),QStringLiteral("Preset JSON (*.json)"));if(!path.isEmpty())loadProcessingPreset(path,true);
}
void WorkspaceController::loadProcessingPreset(const QString& path,bool addToLibrary){
    ProcessingPreset p;QString error;if(!readPreset(path,p,error)){showError(error);return;}
    for(auto id:p.features){bool valid=false;featureIdFromUi(id,valid);if(id!="hag"&&!valid){showError(QStringLiteral("Unknown feature ID: ")+id);return;}}
    if(addToLibrary){if(!QDir().mkpath(library())||!writePreset(QDir(library()).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)+".json"),p,error)){showError("Cannot add imported preset: "+error);return;}reloadProcessingLibrary();}
    applyProcessingPreset(p);m_dock->setProcessingSuggestion(QStringLiteral("Loaded exact settings: %1. The saved source profile is provenance, not measurements of this cloud. Use Adapt explicitly for new scale suggestions.").arg(p.name));showReady("Preset loaded; no filtering or classification started.");
}
void WorkspaceController::adaptProcessingPreset(){
    if(!requireMetricUnits("Adaptive preset"))return;
    if(!m_cloudProfile.contains("nnMedian"))refreshCloudProfile(true,false);
    auto p=captureProcessingPreset();ProcessingPreset validated;QString explanation;
    if(!processingPresetFromJson(processingPresetToJson(p),validated,explanation)){showError(explanation);return;}
    if(!suggestProcessingPreset(p,m_cloudProfile,explanation)){showError(explanation);return;}
    const QString suffix=QStringLiteral(" — adapted");if(!p.name.endsWith(suffix))p.name=p.name.left(120-suffix.size())+suffix;
    applyProcessingPreset(p);m_dock->setProcessingSuggestion(explanation);m_dock->showScaleSuggestion(p.radii,explanation);
    showReady(QStringLiteral("Suggested %1 feature radii. Ground tolerance unchanged; inspect before Preview.").arg(p.radii.size()));
}
bool WorkspaceController::publishComputedFeatures(const std::vector<FeatureRequest>& requests,QStringList& names,QString& error){
    auto it=m_featureEngines.find(m_selectedCloud->getUniqueID());if(it==m_featureEngines.end()){error="No feature engine";return false;}
    QJsonObject index=QJsonDocument::fromJson(featureFieldMetadata(*m_selectedCloud)).object();
    for(const auto& request:requests){
        FeatureKey key;key.feature=request.feature;key.radius=request.radius;key.sourceScalarField=request.sourceScalarField;
        const QString identity=serializeModelFeatureKey(key);QString name=index.value(identity).toString();bool owned=!name.isEmpty();
        if(!owned){
            name=QString::fromStdString(it->second->defaultScalarFieldName(key))+QStringLiteral("m");QString base=name;int suffix=2;
            while(m_selectedCloud->getScalarFieldIndexByName(name.toUtf8().constData())>=0)name=base+QStringLiteral("_%1").arg(suffix++);
        }
        std::string message;int sf=it->second->materialize(key,name.toStdString(),owned?MaterializePolicy::OverwriteExisting:MaterializePolicy::FailIfExists,message);
        if(sf<0){error=QStringLiteral("Published %1 fields; failed at %2: %3. Already published fields are retained.").arg(names.size()).arg(name,QString::fromStdString(message));return false;}
        index.insert(identity,name);names<<name;
        // Persist after each field so even a partial allocation failure remains auditable.
        m_selectedCloud->setMetaData("ALiS.featureFields",QString::fromUtf8(QJsonDocument(index).toJson(QJsonDocument::Compact)));
    }
    return true;
}
}
