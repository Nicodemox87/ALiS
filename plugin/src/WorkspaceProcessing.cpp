// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "CloudProfile.h"
#include "PointSelectionDialog.h"
#include "SemanticParametersDialog.h"
#include "BuildingCleaning.h"
#include "DfmSettingsDialog.h"
#include "DfmConflictDialog.h"
#include <ccProgressDialog.h>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QApplication>
#include <ccMainAppInterface.h>
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
QString workingScopeIdentity(const ALiSSession& session){
    if(!session.hasWorkingSelection())return "full";QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray part;part.reserve(65536);for(unsigned i=0;i<session.pointCount();++i){part.append(session.inWorkingSelection(i)?char(1):char(0));if(part.size()==65536){hash.addData(part);part.clear();}}hash.addData(part);return QString::fromLatin1(hash.result().toHex());
}
}
void WorkspaceController::openBuildingCleaning(){
    if(!session()||!m_selectedCloud||m_featureComputationActive)return;
    QDialog dialog(m_dock);dialog.setWindowTitle("ALiS — Cleaning building class / DFM preparation");dialog.resize(620,520);auto* layout=new QVBoxLayout(&dialog);
    auto* note=new QLabel("Only applied Building6 / Roof128 / Wall129 are inspected. Existing Ground and other classes are never changed. Coherent planar roofs + walls are kept. Two-scale dispersed canopy and known nonterminal returns are proposed for removal FROM BUILDING, not deletion of points. Vegetation height classes require current verified HAG; otherwise suspects become Other. Missing measurements are retained. Weak/isolated planes remain review-only by default. No maximum-height cutoff is used: tall buildings are valid.");note->setWordWrap(true);layout->addWidget(note);
    auto* strict=new QCheckBox("Stricter planar and wall/roof support for building cleaning");strict->setChecked(true);layout->addWidget(strict);auto* aggressive=new QCheckBox("Also propose unsupported / disconnected planes as Other (aggressive)");layout->addWidget(aggressive);
    auto* summary=new QLabel("Preview first. Inspect qAL_BuildingCleanStatus / Proposal in Display. Apply is separate and undoable; this is NOT a validated classifier.");summary->setWordWrap(true);layout->addWidget(summary);
    auto* preview=new QPushButton("Preview building cleaning — Working unchanged");layout->addWidget(preview);auto* apply=new QPushButton("Apply reviewed proposals / confirm retained buildings");layout->addWidget(apply);auto* tune=new QPushButton("Explain / tune geometry thresholds…");layout->addWidget(tune);auto* close=new QPushButton("Close and inspect in 3D");layout->addWidget(close);
    QObject::connect(tune,&QPushButton::clicked,&dialog,[&](){tuneSemanticClasses();});
    QObject::connect(preview,&QPushButton::clicked,&dialog,[&](){QString error;if(!previewBuildingCleaning(aggressive->isChecked(),strict->isChecked(),error)){summary->setText(error);return;}const auto report=QJsonDocument::fromJson(m_selectedCloud->getMetaData("ALiS.buildingCleaning").toByteArray()).object();summary->setText(QString("Targets: %1; supported structures: %2; dispersed canopy: %3; nonterminal returns: %4; weak geometry (review): %5; missing measurements (retained): %6. Proposed changes: %7.\nStatus: 0 not targeted, 1 supported, 2 canopy, 3 nonterminal, 4 review-only, 5 missing. Close to inspect in 3D; reopen to Apply. Do NOT accept blindly.").arg(report.value("targets").toDouble()).arg(report.value("supported").toDouble()).arg(report.value("canopy_suspects").toDouble()).arg(report.value("known_nonterminal").toDouble()).arg(report.value("weak_or_disconnected_review").toDouble()).arg(report.value("missing_geometry_retained").toDouble()).arg(report.value("proposed_changes").toDouble()));});
    QObject::connect(apply,&QPushButton::clicked,&dialog,[&](){if(QMessageBox::question(&dialog,"Apply reviewed Building cleaning?","Apply the saved proposals to Working? Ground and other classes are retained. This is undoable; labels are NOT marked Trusted. Unsupported geometry may still require manual review.",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;QString error;if(!applyBuildingCleaning(error))summary->setText(error);else {summary->setText("Reviewed cleaning applied. You can Undo it. Save a new BIN before DFM.");apply->setEnabled(false);}});
    QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);dialog.exec();
}
bool WorkspaceController::previewBuildingCleaning(bool aggressive,bool strict,QString& error){
    if(!session()||!m_selectedCloud||m_featureComputationActive){error="Select an idle cloud";return false;}if(!requireMetricUnits("Building cleaning")){error="Metric coordinates required";return false;}if(!session()->hasAppliedGround()){error="Apply Ground first; cleaning reuses it.";return false;}
    QScopedValueRollback<bool> guard(m_featureComputationActive,true);QProgressDialog progress("Checking Building geometry (no point deletion)…","Cancel",0,0,m_dock);progress.setWindowModality(Qt::ApplicationModal);progress.show();m_dock->setBusy(true);
    try{
        const bool hagCurrent=session()->hagRevision()!=0&&m_selectedCloud->getScalarFieldIndexByName(field::HeightAboveGround)>=0&&m_selectedCloud->getMetaData("ALiS.hagGroundRevision").isValid()&&m_selectedCloud->getMetaData("ALiS.hagGroundRevision").toULongLong()==session()->groundRevision();
        const auto parameters=semanticParametersFromJson(QJsonDocument::fromJson(m_selectedCloud->getMetaData("ALiS.semanticParameters").toByteArray()).object());auto selected=m_pointSelections.find(m_selectedCloud->getUniqueID());const auto* mask=session()->hasWorkingSelection()&&selected!=m_pointSelections.end()?&selected->second.mask:nullptr;
        auto result=cleanBuildingClasses(*m_selectedCloud,session()->appliedGroundMask(),parameters,hagCurrent,aggressive,strict,mask,[&](){QApplication::processEvents();checkpointCheck(!progress.wasCanceled(),"Building cleaning canceled; Working unchanged");});
        if(!result.provenance.value("targets").toDouble()){error="No applied Building6 / Roof128 / Wall129 in working selection. Apply reviewed classification first.";m_dock->setBusy(false);return false;}
        m_selectedCloud->removeMetaData("ALiS.buildingCleaning");
        int status=m_selectedCloud->getScalarFieldIndexByName("qAL_BuildingCleanStatus"),proposal=m_selectedCloud->getScalarFieldIndexByName("qAL_BuildingCleanProposal");if(status<0)status=m_selectedCloud->addScalarField("qAL_BuildingCleanStatus");if(proposal<0)proposal=m_selectedCloud->addScalarField("qAL_BuildingCleanProposal");checkpointCheck(status>=0&&proposal>=0,"Preview field allocation failed; Working unchanged, no valid preview to apply");
        for(unsigned i=0;i<m_selectedCloud->size();++i){m_selectedCloud->getScalarField(status)->setValue(i,result.status[i]);m_selectedCloud->getScalarField(proposal)->setValue(i,result.proposals[i]<0?std::numeric_limits<float>::quiet_NaN():float(result.proposals[i]));}m_selectedCloud->getScalarField(status)->computeMinAndMax();m_selectedCloud->getScalarField(proposal)->computeMinAndMax();
        result.provenance["ground_revision"]=double(session()->groundRevision());result.provenance["scope"]=workingScopeIdentity(*session());m_selectedCloud->setMetaData("ALiS.buildingCleaning",QJsonDocument(result.provenance).toJson(QJsonDocument::Compact));QString ignored;session()->displayScalarField("qAL_BuildingCleanStatus",ignored);updateModelFeatureChoices();syncDisplayControls();m_dock->setBusy(false);refreshHost();updateDockState();showReady("Building cleaning preview ready. Working and Ground unchanged; inspect statuses and proposals before Apply.");return true;
    }catch(const std::exception& e){m_dock->setBusy(false);error=QString::fromUtf8(e.what());return false;}
}
bool WorkspaceController::applyBuildingCleaning(QString& error){
    if(!session()||!m_selectedCloud){error="No cloud";return false;}const auto report=QJsonDocument::fromJson(m_selectedCloud->getMetaData("ALiS.buildingCleaning").toByteArray()).object();const int wi=m_selectedCloud->getScalarFieldIndexByName(field::WorkingAsprs),pi=m_selectedCloud->getScalarFieldIndexByName("qAL_BuildingCleanProposal");if(report.isEmpty()||wi<0||pi<0){error="Run a valid Building cleaning preview first";return false;}
    if(report.value("ground_revision").toDouble()!=double(session()->groundRevision())||report.value("working_sha256").toString()!=scalarPayloadHash(*m_selectedCloud->getScalarField(wi))||report.value("scope").toString()!=workingScopeIdentity(*session())){error="Classification, Ground or working selection changed since Preview. Run Preview again; nothing was applied.";return false;}
    std::vector<unsigned> indices;std::vector<std::int16_t> codes;const auto* working=m_selectedCloud->getScalarField(wi);const auto* proposal=m_selectedCloud->getScalarField(pi);for(unsigned i=0;i<m_selectedCloud->size();++i){const double before=working->getValue(i),after=proposal->getValue(i);if(session()->inWorkingSelection(i)&&(before==6||before==128||before==129)&&std::isfinite(after)&&after!=before){if(after<0||after>255||after==2||std::floor(after)!=after){error="Invalid stored cleaning proposal; Preview again";return false;}indices.push_back(i);codes.push_back(std::int16_t(after));}}
    if(!indices.empty()&&!session()->applyAutomaticClassEdits(indices,codes,error))return false;
    ProcessingRecord record;record.operation="BuildingCleaning.ReviewedApply";record.timestampUtc=QDateTime::currentDateTimeUtc();record.sourceEntityUid=session()->entityUid();record.affectedPoints=indices.size();record.parameters=report;record.parameters["trusted_labels_created"]=false;session()->addProcessingRecord(record);
    m_selectedCloud->setMetaData("ALiS.buildingCleanReviewHash",scalarPayloadHash(*working));m_selectedCloud->setMetaData("ALiS.buildingCleanReviewScope",workingScopeIdentity(*session()));m_selectedCloud->removeMetaData("ALiS.buildingCleaning");QString ignored;session()->displayScalarField(QString::fromUtf8(field::WorkingAsprs),ignored);refreshHost();updateDockState();showReady(QString("Building cleaning reviewed: %1 Working labels changed, zero points deleted. Ground preserved. Undo available for label changes.").arg(indices.size()));return true;
}
bool WorkspaceController::allowFullCloudOperation(const QString& operation){
    if(session()&&session()->hasWorkingSelection()){showError(operation+": this tool has no safe working-selection adapter yet. Nothing was run. Disable 'Work ONLY on these points' explicitly to process the full cloud; hiding alone does not restrict native CloudCompare tools.");return false;}return true;
}
void WorkspaceController::openPointSelection(){
    if(!session()||!m_selectedCloud||m_featureComputationActive)return;
    auto found=m_pointSelections.find(m_selectedCloud->getUniqueID());QJsonArray rules;bool work=false,restore=false;
    if(found!=m_pointSelections.end()){rules=found->second.rules;work=found->second.workOnly;}
    if(!pointSelectionDialog(*m_selectedCloud,rules,work,restore,m_dock))return;
    if(restore){clearPointSelection();return;}QString error;if(!applyPointSelection(rules,work,error))showError(error);
}
bool WorkspaceController::applyPointSelection(const QJsonArray& rules,bool workOnly,QString& error){
    if(!session()||!m_selectedCloud||m_featureComputationActive){error="No idle point cloud selected";return false;}
    QScopedValueRollback<bool> guard(m_featureComputationActive,true);
    QProgressDialog progress("Testing point filters (no geometry or classes changed)…","Cancel",0,0,m_dock);progress.setWindowModality(Qt::ApplicationModal);progress.show();
    try{
        auto mask=pointSelectionMask(*m_selectedCloud,rules,[&](){QApplication::processEvents();checkpointCheck(!progress.wasCanceled(),"Point filter canceled; previous view/selection retained");});
        auto uid=m_selectedCloud->getUniqueID();auto found=m_pointSelections.find(uid);std::vector<unsigned char> previous;
        if(found!=m_pointSelections.end())previous=found->second.previousVisibility;
        else {const auto& old=m_selectedCloud->getTheVisibilityArray();if(old.size()==m_selectedCloud->size())previous.assign(old.begin(),old.end());}
        if(!previous.empty())for(unsigned i=0;i<m_selectedCloud->size();++i)mask[i]=mask[i]&&previous[i]==CCCoreLib::POINT_VISIBLE;
        const auto count=std::count(mask.begin(),mask.end(),1);
        if(workOnly&&count==0){error="No points match. Empty working selection refused; previous selection retained.";return false;}
        if(!m_selectedCloud->resetVisibilityArray()){error="Not enough memory for point visibility";return false;}
        if(workOnly){if(!session()->setWorkingSelection(mask,error))return false;}else session()->clearWorkingSelection();
        auto& state=m_pointSelections[uid];state.rules=rules;state.workOnly=workOnly;state.previousVisibility=std::move(previous);state.mask=std::move(mask);
        enforcePointSelection();refreshHost();updateDockState();showReady(QString("Point filter: %1/%2 visible. %3 No points exported or removed.").arg(count).arg(m_selectedCloud->size()).arg(workOnly?"WORK ONLY ON THIS FROZEN SELECTION.":"Display only; processing scope remains full cloud."));return true;
    }catch(const std::exception& e){error=QString::fromUtf8(e.what());return false;}
}
void WorkspaceController::enforcePointSelection(){
    if(!m_selectedCloud)return;auto found=m_pointSelections.find(m_selectedCloud->getUniqueID());if(found==m_pointSelections.end())return;const auto& mask=found->second.mask;
    if(mask.size()!=m_selectedCloud->size())return;
    if(m_selectedCloud->getTheVisibilityArray().size()!=m_selectedCloud->size()&&!m_selectedCloud->resetVisibilityArray())return;
    auto& visibility=m_selectedCloud->getTheVisibilityArray();for(unsigned i=0;i<m_selectedCloud->size();++i)if(!mask[i])visibility[i]=CCCoreLib::POINT_HIDDEN;
}
void WorkspaceController::clearPointSelection(){
    if(!m_selectedCloud||!session())return;auto found=m_pointSelections.find(m_selectedCloud->getUniqueID());
    if(found!=m_pointSelections.end()){
        if(found->second.previousVisibility.empty())m_selectedCloud->unallocateVisibilityArray();
        else if(m_selectedCloud->resetVisibilityArray()){auto& visibility=m_selectedCloud->getTheVisibilityArray();for(unsigned i=0;i<m_selectedCloud->size();++i)visibility[i]=found->second.previousVisibility[i];}
        else {showError("Cannot restore previous visibility; selection retained.");return;}m_pointSelections.erase(found);
    }
    session()->clearWorkingSelection();refreshHost();updateDockState();showReady("ALiS point filter cleared; previous visibility restored. Working selection disabled.");
}
void WorkspaceController::tuneSemanticClasses(){
    if(!m_selectedCloud||m_featureComputationActive)return;
    auto parameters=semanticParametersFromJson(QJsonDocument::fromJson(m_selectedCloud->getMetaData("ALiS.semanticParameters").toByteArray()).object());
    if(semanticParametersDialog(parameters,m_dock)){m_selectedCloud->setMetaData("ALiS.semanticParameters",QJsonDocument(parameters.toJson()).toJson(QJsonDocument::Compact));showReady("Automatic class parameters saved with this cloud. No classification was run; save BIN to retain them. Ground remains locked.");}
}
void WorkspaceController::computeDfm(double step,bool fill,double edge){
    Q_UNUSED(fill);Q_UNUSED(edge);
    if(!requireMetricUnits("DFM computation")||!session()||!m_selectedCloud)return;
    auto mask=session()->appliedGroundMask();if(mask.empty()){showError("Apply Ground before DFM. Ground is reused, not recalculated.");return;}
    const int index=m_selectedCloud->getScalarFieldIndexByName(field::WorkingAsprs);if(index<0){showError("Apply reviewed building classification to Working first.");return;}auto* sf=m_selectedCloud->getScalarField(index);quint64 buildings=0;
    const auto reviewedScope=m_selectedCloud->getMetaData("ALiS.buildingCleanReviewScope").toString();
    if(m_selectedCloud->getMetaData("ALiS.buildingCleanReviewHash").toString()!=scalarPayloadHash(*sf)||(reviewedScope!="full"&&reviewedScope!=workingScopeIdentity(*session()))){showError("Before DFM, open Prepare > Preprocessing > Cleaning building class, Preview, inspect suspects, then Apply reviewed proposals / confirm retained buildings. Classification or selection changed since the last review. No DFM was created.");return;}
    auto settings=dfmRunSettings(QJsonDocument::fromJson(m_selectedCloud->getMetaData("ALiS.dfmSettings").toByteArray()).object(),step);if(!dfmSettingsDialog(settings,m_dock))return;m_selectedCloud->setMetaData("ALiS.dfmSettings",QJsonDocument(settings.toJson()).toJson(QJsonDocument::Compact));
    std::vector<bool> buildingMask(m_selectedCloud->size(),false);for(unsigned i=0;i<m_selectedCloud->size();++i){const double c=sf->getValue(i);const bool building=std::isfinite(c)&&c>=0&&c<=255&&c==std::floor(c)&&settings.buildingClasses.contains(int(c));mask[i]=session()->inWorkingSelection(i)&&(mask[i]||building);buildingMask[i]=mask[i]&&building;if(mask[i]&&building)++buildings;}
    if(!buildings){showError("No points in the chosen applied Building classes inside working selection. Review/apply classes first; Derived proposals are not silently promoted.");return;}
    TerrainParameters p=settings.parameters;const double scale=m_selectedCloud->getGlobalScale();p.gridStep*=scale;p.maximumInterpolationEdgeLength*=scale;p.dfmFill.radius*=scale;p.dfmFill.maximumLocalRange*=scale;p.dfmFill.maximumMeanDeviation*=scale;p.krigingRange*=scale;p.krigingSill*=scale*scale;p.krigingNugget*=scale*scale;
    QScopedValueRollback<bool> guard(m_featureComputationActive,true);ccProgressDialog progress(true,m_dock);progress.setWindowModality(Qt::ApplicationModal);progress.setMethodTitle("ALiS — staged DFM");progress.setInfo("Raster stages without fill; review conflicts before interpolation");progress.show();TerrainContext context;context.app=m_app;context.progressDialog=&progress;context.surfaceBuildingMask=&buildingMask;context.conflictReview=[&](DfmConflictGrid& grid){progress.hide();const bool accepted=dfmConflictDialog(grid,m_dock);if(accepted){progress.setInfo("Overlap choices accepted; final raster and void interpolation");progress.show();}return accepted;};m_dock->setBusy(true);auto result=m_terrainEngine.run(*m_selectedCloud,mask,p,context);if(!result.succeeded()){showError(result.message);return;}if(!m_app){showError("Host unavailable; surface was not committed.");return;}
    result.provenance.effectiveParameters["buildingClasses"]=settings.toJson().value("classes");result.dtmCloud->setMetaData("ALiS.surfaceSource",QJsonDocument(settings.toJson()).toJson(QJsonDocument::Compact));result.dtmCloud->setMetaData("ALiS.DFM.parameters",QJsonDocument(result.provenance.effectiveParameters).toJson(QJsonDocument::Compact));
    const auto selected=m_pointSelections.find(m_selectedCloud->getUniqueID());if(selected!=m_pointSelections.end()&&selected->second.workOnly)result.dtmCloud->setMetaData("ALiS.selectionRules",QJsonDocument(selected->second.rules).toJson(QJsonDocument::Compact));
    const auto outputName=result.dtmCloud->getName();m_app->addToDB(result.dtmCloud.release());ProcessingRecord record;record.operation="Terrain.ComputeDFM";record.timestampUtc=QDateTime::currentDateTimeUtc();record.sourceEntityUid=session()->entityUid();record.affectedPoints=result.statistics.groundPointCount;record.parameters=result.provenance.effectiveParameters;record.parameters["buildingPoints"]=double(buildings);record.parameters["workingSelection"]=session()->hasWorkingSelection();record.elapsedMilliseconds=result.provenance.elapsedMilliseconds;record.outputFields=QStringList{outputName};session()->addProcessingRecord(record);m_dock->setBusy(false);refreshHost();updateDockState();showReady(QString("DFM ready: %1. Ground wins %2 of %3 overlapping Ground/Building cells. Filled: %4; remaining NODATA: %5. Parameters/source quality saved; DTM/HAG unchanged.").arg(p.dfmMinimumMerge?"MIN Ground + MIN Building → merged MIN":"MIN Ground / MAX Building, Building priority").arg(result.provenance.effectiveParameters.value("groundWinsOverlap").toDouble()).arg(result.provenance.effectiveParameters.value("overlapCells").toDouble()).arg(result.statistics.interpolatedCellCount).arg(result.statistics.nodataCellCount));
}
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
        const QString identity=serializeModelFeatureKey(key);const bool scoped=session()&&session()->hasWorkingSelection();QString name=scoped?QString():index.value(identity).toString();bool owned=!name.isEmpty();
        if(!owned){
            name=QString::fromStdString(it->second->defaultScalarFieldName(key))+QStringLiteral("m")+(scoped?QStringLiteral(" [selection]"):QString());QString base=name;int suffix=2;
            while(m_selectedCloud->getScalarFieldIndexByName(name.toUtf8().constData())>=0)name=base+QStringLiteral("_%1").arg(suffix++);
        }
        std::string message;int sf=it->second->materialize(key,name.toStdString(),owned?MaterializePolicy::OverwriteExisting:MaterializePolicy::FailIfExists,message);
        if(sf<0){error=QStringLiteral("Published %1 fields; failed at %2: %3. Already published fields are retained.").arg(names.size()).arg(name,QString::fromStdString(message));return false;}
        if(!scoped)index.insert(identity,name);else m_selectedCloud->setMetaData("ALiS.partialFeature."+name,"Partial query field; NaN outside frozen selection; NOT a full-cloud feature map entry");names<<name;
        // Persist after each field so even a partial allocation failure remains auditable.
        m_selectedCloud->setMetaData("ALiS.featureFields",QString::fromUtf8(QJsonDocument(index).toJson(QJsonDocument::Compact)));
    }
    return true;
}
}
