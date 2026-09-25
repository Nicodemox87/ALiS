// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceController.h"

#include "WorkspaceDock.h"
#include "CloudProfile.h"
#include "AnnotationStudio.h"
#include "ModelResultsDialog.h"
#include "MlInputFields.h"
#include "VegetationRules.h"
#include <ReferenceCloud.h>
#include <ccColorScale.h>

#include <ccMainAppInterface.h>
#include <ccGLWindowInterface.h>
#include <ccPointCloud.h>
#include <ccProgressDialog.h>
#include <ccScalarField.h>
#include <ccColorScalesManager.h>
#include <ccColorScaleEditorDlg.h>

#include <QDateTime>
#include <QAction>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QEventLoop>
#include <QScopedValueRollback>
#include <GenericProgressCallback.h>
#include <atomic>
#include <future>
#include <mutex>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <map>

namespace
{
	// Workers never touch widgets or pump GUI events. The main thread polls this
	// bridge while the octree's parallel computation runs on a dedicated thread.
	class FeatureProgress final : public CCCoreLib::GenericProgressCallback
	{
	public:
		std::atomic<float> percent{0};
		std::atomic<bool> canceled{false};
		std::mutex mutex;
		QString title = QStringLiteral("ALiS — multiscale features");
		QString info = QStringLiteral("Building/reusing octree…");
		void update(float value) override { percent.store(value); }
		void setMethodTitle(const char* value) override { std::lock_guard<std::mutex> lock(mutex); title = QString::fromUtf8(value); }
		void setInfo(const char* value) override { std::lock_guard<std::mutex> lock(mutex); info = QString::fromUtf8(value); }
		void start() override { percent.store(0); }
		void stop() override {}
		bool isCancelRequested() override { return canceled.load(); }
	};

	QString countText(quint64 count)
	{
		return QLocale().toString(static_cast<qulonglong>(count));
	}

	double metresToLocal(double metres, const ccPointCloud& cloud)
	{
		return metres * cloud.getGlobalScale();
	}

	bool wktDeclaresMetres(const QString& wkt)
	{
		static const QRegularExpression metreUnit(
			QStringLiteral("(?:LENGTHUNIT|UNIT)\\s*\\[\\s*[\"'](?:metre|meter|metres|meters)[\"']\\s*,\\s*1(?:\\.0*)?(?:\\s*,|\\s*\\])"),
			QRegularExpression::CaseInsensitiveOption);
		return metreUnit.match(wkt).hasMatch();
	}

	QString metricConfirmationSettingsKey(ccPointCloud& cloud)
	{
		CCVector3d minimum;
		CCVector3d maximum;
		const bool validBounds = cloud.getOwnGlobalBB(minimum, maximum);
		const CCVector3d shift = cloud.getGlobalShift();
		QStringList identity = {
			QString::number(static_cast<qulonglong>(cloud.size())),
			cloud.getName(),
			QString::number(cloud.getGlobalScale(), 'g', 17),
			QString::number(shift.x, 'g', 17),
			QString::number(shift.y, 'g', 17),
			QString::number(shift.z, 'g', 17)
		};
		if (validBounds)
		{
			for (double value : {minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z})
			{
				identity.push_back(QString::number(value, 'g', 17));
			}
		}
		for (const char* key : {"LAS.scale.x", "LAS.scale.y", "LAS.scale.z", "LAS.offset.x", "LAS.offset.y", "LAS.offset.z"})
		{
			identity.push_back(cloud.getMetaData(QString::fromLatin1(key)).toString());
		}
		const QByteArray digest = QCryptographicHash::hash(identity.join(QLatin1Char('|')).toUtf8(), QCryptographicHash::Sha256).toHex();
		return QStringLiteral("ALiS/metric-confirmations/%1").arg(QString::fromLatin1(digest));
	}

	QString legacyMetricConfirmationSettingsKey(ccPointCloud& cloud)
	{
		QString key = metricConfirmationSettingsKey(cloud);
		key.replace(QStringLiteral("ALiS/"), QStringLiteral("qArchaeoLiDAR/"));
		return key;
	}

	QByteArray featureFieldMetadata(const ccPointCloud& cloud)
	{
		QVariant value = cloud.getMetaData(QStringLiteral("ALiS.featureFields"));
		if (value.toByteArray().isEmpty())
		{
			value = cloud.getMetaData(QStringLiteral("qArchaeoLiDAR.featureFields"));
		}
		return value.toByteArray();
	}

	QString radiusName(double radiusMetres)
	{
		return QString::number(radiusMetres, 'f', radiusMetres < 1.0 ? 3 : 2);
	}

	QVector<double> cleanPositiveRadii(const QVector<double>& input)
	{
		QVector<double> output;
		for (double radius : input)
		{
			if (std::isfinite(radius) && radius > 0.0)
			{
				output.push_back(radius);
			}
		}
		std::sort(output.begin(), output.end());
		output.erase(std::unique(output.begin(), output.end(), [](double a, double b)
		{
			return std::abs(a - b) <= 1.0e-9 * std::max(1.0, std::max(a, b));
		}), output.end());
		return output;
	}

	bool parsePositiveSequence(const QString& text, std::vector<double>& values)
	{
		values.clear();
		const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
		for (const QString& token : tokens)
		{
			bool ok = false;
			const double value = QLocale::c().toDouble(token, &ok);
			if (!ok || !std::isfinite(value) || value <= 0.0)
			{
				values.clear();
				return false;
			}
			values.push_back(value);
		}
		return !values.empty();
	}

	struct FeatureUiMapping
	{
		alis::FeatureId feature;
		const char* stableId;
	};

	const FeatureUiMapping FeatureUiMappings[] = {
		{ alis::FeatureId::NeighborCount, "neighbor_count" },
		{ alis::FeatureId::Density2D, "density_2d" },
		{ alis::FeatureId::Density3D, "density_3d" },
		{ alis::FeatureId::Eigenvalue1, "eigenvalue_1" },
		{ alis::FeatureId::Eigenvalue2, "eigenvalue_2" },
		{ alis::FeatureId::Eigenvalue3, "eigenvalue_3" },
		{ alis::FeatureId::EigenvaluesSum, "eigenvalues_sum" },
		{ alis::FeatureId::PCA1, "pca_1" },
		{ alis::FeatureId::PCA2, "pca_2" },
		{ alis::FeatureId::Linearity, "linearity" },
		{ alis::FeatureId::Planarity, "planarity" },
		{ alis::FeatureId::Sphericity, "sphericity" },
		{ alis::FeatureId::Anisotropy, "anisotropy" },
		{ alis::FeatureId::Omnivariance, "omnivariance" },
		{ alis::FeatureId::Eigenentropy, "eigenentropy" },
		{ alis::FeatureId::SurfaceVariation, "surface_variation" },
		{ alis::FeatureId::Verticality, "verticality" },
		{ alis::FeatureId::NormalX, "normal_x" },
		{ alis::FeatureId::NormalY, "normal_y" },
		{ alis::FeatureId::NormalZ, "normal_z" },
		{ alis::FeatureId::NormalZAbsolute, "normal_z_absolute" },
		{ alis::FeatureId::Dip, "dip" },
		{ alis::FeatureId::DipDirection, "dip_direction" },
		{ alis::FeatureId::Roughness, "roughness" },
		{ alis::FeatureId::SignedRoughness, "signed_roughness" },
		{ alis::FeatureId::MeanCurvature, "mean_curvature" },
		{ alis::FeatureId::GaussianCurvature, "gaussian_curvature" },
		{ alis::FeatureId::NormalChangeRate, "normal_change_rate" },
		{ alis::FeatureId::MomentOrder1, "moment_order_1" },
		{ alis::FeatureId::BarycenterOffsetRatio, "barycenter_offset_ratio" },
		{ alis::FeatureId::ZMinimum, "z_minimum" },
		{ alis::FeatureId::ZMaximum, "z_maximum" },
		{ alis::FeatureId::ZRange, "z_range" },
		{ alis::FeatureId::ZMean, "z_mean" },
		{ alis::FeatureId::ZStandardDeviation, "z_standard_deviation" },
		{ alis::FeatureId::ZAboveMinimum, "z_above_minimum" },
		{ alis::FeatureId::ZBelowMaximum, "z_below_maximum" },
		{ alis::FeatureId::ZRelativeToMean, "z_relative_to_mean" },
		{ alis::FeatureId::ZPercentile10, "z_percentile_10" },
		{ alis::FeatureId::ZPercentile25, "z_percentile_25" },
		{ alis::FeatureId::ZMedian, "z_median" },
		{ alis::FeatureId::ZPercentile75, "z_percentile_75" },
		{ alis::FeatureId::ZPercentile90, "z_percentile_90" },
	};
}

namespace alis
{
	WorkspaceController::WorkspaceController(ccMainAppInterface* app, WorkspaceDock* dock, QObject* parent)
		: QObject(parent)
		, m_app(app)
		, m_dock(dock)
		, m_groundFilter(new CSFGroundFilter)
	{
		// Match the values initially shown by CloudCompare v2.13.2 qCSF.
		m_groundParameters.clothResolution = 2.0;
		m_groundParameters.classificationThreshold = 0.5;
		m_groundParameters.timeStep = 0.65;
		m_groundParameters.rigidness = 2;
		m_groundParameters.iterations = 500;
		m_groundParameters.slopeProcessing = false;
		m_groundBaseParameters = m_groundParameters;
		m_mlProcess = new QProcess(this);
		m_mlProcess->setProcessChannelMode(QProcess::SeparateChannels);
		connect(m_mlProcess, &QProcess::readyReadStandardOutput, this, &WorkspaceController::readMlStandardOutput);
		connect(m_mlProcess, &QProcess::readyReadStandardError, this, [this]()
		{
			m_mlStderrBuffer.append(m_mlProcess->readAllStandardError());
			if (m_mlStderrBuffer.size() > 65536) m_mlStderrBuffer = m_mlStderrBuffer.right(65536);
		});
		connect(m_mlProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
			[this](int code, QProcess::ExitStatus status) { finishMlWorker(code, static_cast<int>(status)); });
		connect(m_mlProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
		{
			if (error == QProcess::FailedToStart)
			{
				m_mlOperation.clear();
				m_dock->clearProgress();
				showError(QStringLiteral("ML worker failed to start: %1").arg(m_mlProcess->errorString()));
			}
		});

		QString python = qEnvironmentVariable("ALIS_PYTHON");
		if (python.isEmpty()) python = qEnvironmentVariable("QARCHAEOLIDAR_PYTHON");
		if (python.isEmpty()) {
			const QString managed = QDir(QDir::homePath()).filePath(QStringLiteral(".alis/runtime-cu128/Scripts/python.exe"));
			const QString verified = QDir(QDir::homePath()).filePath(QStringLiteral(".alis/runtime-cu128/alis-runtime-ready.json"));
			if (QFileInfo::exists(managed) && QFileInfo::exists(verified)) python = managed;
		}
		if (python.isEmpty()) python = QStandardPaths::findExecutable(QStringLiteral("python.exe"));
		if (python.isEmpty()) python = QStandardPaths::findExecutable(QStringLiteral("python"));
		if (python.isEmpty())
		{
			showError(QStringLiteral("Python was not found. Install Python 3.10+ and the ALiS worker requirements, "
			                         "or set the ALIS_PYTHON environment variable to a trusted Python executable."));
			return;
		}
		QString worker = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("worker/ALiS/qal_ml_worker.py"));
		if (!QFileInfo::exists(worker))
		{
			const QString legacyWorker = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("worker/qArchaeoLiDAR/qal_ml_worker.py"));
			if (QFileInfo::exists(legacyWorker)) worker = legacyWorker;
		}
		const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
		QString defaultRepository = QDir(appData).filePath(QStringLiteral("ALiS/repository"));
		const QString legacyRepository = QDir(appData).filePath(QStringLiteral("qArchaeoLiDAR/repository"));
		if (!QFileInfo::exists(defaultRepository) && QFileInfo::exists(legacyRepository)) defaultRepository = legacyRepository;
		QSettings settings;
		QString repository = settings.value(QStringLiteral("ALiS/modelRepository")).toString();
		if (repository.isEmpty()) repository = settings.value(QStringLiteral("qArchaeoLiDAR/modelRepository"), defaultRepository).toString();
		m_mlModelPath.clear();
		m_dock->setModelWorkerPaths(QDir::toNativeSeparators(python), QDir::toNativeSeparators(worker),
			QString(), QDir::toNativeSeparators(repository));
		connectDock();
		syncGroundParametersToDock();
		reloadProcessingLibrary();
	}

	void WorkspaceController::connectDock()
	{
		connect(m_dock,&WorkspaceDock::cloudProfileRequested,this,[this](){refreshCloudProfile(true,true);});
		connect(m_dock,&WorkspaceDock::saveProcessingPresetRequested,this,&WorkspaceController::saveProcessingPreset);
		connect(m_dock,&WorkspaceDock::importProcessingPresetRequested,this,&WorkspaceController::importProcessingPreset);
		connect(m_dock,&WorkspaceDock::loadProcessingPresetRequested,this,[this](const QString& path){loadProcessingPreset(path);});
		connect(m_dock,&WorkspaceDock::adaptProcessingPresetRequested,this,&WorkspaceController::adaptProcessingPreset);
		connect(m_dock, &WorkspaceDock::groundAlgorithmChanged, this, [this](const QString& algorithmId)
		{
			if (algorithmId.startsWith(QStringLiteral("pmf.")))
			{
				m_groundAlgorithmId = QStringLiteral("pmf.lidr_zhang.fast_raster.v1");
				m_groundFilter.reset(new PMFGroundFilter);
				showReady(QStringLiteral("Progressive Morphological Filter selected; review ws/th before Preview."));
			}
			else
			{
				m_groundAlgorithmId = QStringLiteral("csf.cloudcompare.v2.13.2");
				m_groundFilter.reset(new CSFGroundFilter);
				showReady(QStringLiteral("Native CloudCompare qCSF selected."));
			}
		});
		connect(m_dock, &WorkspaceDock::groundPresetChanged, this, [this](int index)
		{
			if (index < 0 || index > 7) return;
			m_groundParameters = groundPresetParameters(static_cast<GroundPreset>(index),
				m_estimatedSpacingMetres > 0.0 ? m_estimatedSpacingMetres : 0.025);
			m_groundBaseParameters = m_groundParameters;
			m_dock->resetSimpleGroundControls();
			m_processingPresetName=QStringLiteral("Ground preset %1").arg(index);
			syncGroundParametersToDock();
			showReady(QStringLiteral("Ground preset loaded; all effective parameters are visible and editable."));
		});
		connect(m_dock, &WorkspaceDock::simpleGroundControlsChanged, this, [this](int complexity, int microrelief, int vegetation)
		{
			m_groundParameters = applySimpleGroundControls(m_groundBaseParameters, static_cast<SimpleLevel>(complexity),
				static_cast<SimpleLevel>(microrelief), static_cast<SimpleLevel>(vegetation),
				m_estimatedSpacingMetres > 0.0 ? m_estimatedSpacingMetres : 0.025);
			syncGroundParametersToDock();
		});
		connect(m_dock, &WorkspaceDock::advancedGroundParametersChanged, this,
			[this](double resolution, double threshold, double timeStep, int rigidness, int iterations, bool slope)
		{
			m_groundParameters.clothResolution = resolution;
			m_groundParameters.classificationThreshold = threshold;
			m_groundParameters.timeStep = timeStep;
			m_groundParameters.rigidness = rigidness;
			m_groundParameters.iterations = iterations;
			m_groundParameters.slopeProcessing = slope;
			m_groundBaseParameters = m_groundParameters;
			m_dock->resetSimpleGroundControls();
		});
		connect(m_dock, &WorkspaceDock::pmfParametersChanged, this,
		        [this](const QString& windows, const QString& thresholds, double cellSize)
		        {
			        m_pmfWindowSizes = windows;
			        m_pmfThresholds = thresholds;
			        m_pmfCellSizeMetres = cellSize;
		        });
		connect(m_dock, &WorkspaceDock::metricUnitsConfirmationChanged, this, [this](bool confirmed)
		{
			if (!m_selectedCloud)
			{
				return;
			}
			if (m_metricUnitsLockedByMetadata)
			{
				m_metricUnitsConfirmed = true;
				m_dock->setMetricUnitConfirmation(true, true);
				return;
			}
			m_metricUnitsConfirmed = confirmed;
			m_metricUnitsRemembered = confirmed;
			m_explicitMetricConfirmations[static_cast<quint64>(m_selectedCloud->getUniqueID())] = confirmed;
			QSettings settings;
			const QString settingsKey = metricConfirmationSettingsKey(*m_selectedCloud);
			if (confirmed)
			{
				settings.setValue(settingsKey, true);
				m_selectedCloud->setMetaData(QStringLiteral("ALiS.metricUnitsConfirmed"), true);
			}
			else
			{
				settings.remove(settingsKey);
				m_selectedCloud->removeMetaData(QStringLiteral("ALiS.metricUnitsConfirmed"));
			}
			m_cloudProfile.insert(QStringLiteral("metricConfirmed"),confirmed);
			refreshCloudProfile(confirmed,false);
			if (confirmed)
			{
				updateModelFeatureChoices();
			}
			else
			{
				m_dock->setModelFeatureChoices(QStringList(), QStringList());
			}
			updateDockState(confirmed
				? QStringLiteral("Metric coordinates confirmed and remembered for this cloud.")
				: QStringLiteral("Metric confirmation removed; operations expressed in metres are locked."));
		});
		connect(m_dock, &WorkspaceDock::groundPreviewRequested, this, &WorkspaceController::runGroundPreview);
		connect(m_dock, &WorkspaceDock::groundApplyRequested, this, &WorkspaceController::applyGround);
		connect(m_dock, &WorkspaceDock::groundDiscardRequested, this, &WorkspaceController::discardGround);
		connect(m_dock, &WorkspaceDock::groundRestoreRequested, this, &WorkspaceController::restoreGround);
		connect(m_dock, &WorkspaceDock::assignGroundToAsprsRequested, this, &WorkspaceController::assignGroundToAsprs);
		connect(m_dock, &WorkspaceDock::preprocessingActionRequested, this, [this](const QString& actionId)
		{
			if (!m_app || !m_app->getMainWindow())
			{
				showError(QStringLiteral("CloudCompare host is unavailable."));
				return;
			}
			QString objectName;
			if (actionId == QStringLiteral("host.sor")) objectName = QStringLiteral("actionSORFilter");
			else if (actionId == QStringLiteral("host.noise")) objectName = QStringLiteral("actionNoiseFilter");
			else if (actionId == QStringLiteral("host.subsample")) objectName = QStringLiteral("actionSubsample");
			else if (actionId == QStringLiteral("host.rasterize")) objectName = QStringLiteral("actionRasterize");
			QAction* action = objectName.isEmpty() ? nullptr : m_app->getMainWindow()->findChild<QAction*>(objectName);
			if (!action)
			{
				showError(QStringLiteral("CloudCompare action '%1' was not found.").arg(objectName));
				return;
			}
			if (!action->isEnabled())
			{
				showError(QStringLiteral("Select a compatible point cloud in the CloudCompare DB Tree first."));
				return;
			}
			action->trigger();
		});
		connect(m_dock, &WorkspaceDock::computeDtmRequested, this, &WorkspaceController::computeDtm);
		connect(m_dock, &WorkspaceDock::computeHagRequested, this, &WorkspaceController::computeHag);
		connect(m_dock, &WorkspaceDock::annotationActionRequested, this, [this](const QString& actionId)
		{
			if (actionId == QStringLiteral("studio")) { openAnnotationStudio(); return; }
			if (!m_app)
			{
				showError(QStringLiteral("CloudCompare host is unavailable."));
				return;
			}
			if (actionId.startsWith(QStringLiteral("view.")))
			{
				ccGLWindowInterface* window = m_app->getActiveGLWindow();
				if (!window) { showError(QStringLiteral("No active 3D view.")); return; }
				CC_VIEW_ORIENTATION orientation = CC_ISO_VIEW_1;
				if (actionId == QStringLiteral("view.top")) orientation = CC_TOP_VIEW;
				else if (actionId == QStringLiteral("view.front")) orientation = CC_FRONT_VIEW;
				else if (actionId == QStringLiteral("view.right")) orientation = CC_RIGHT_VIEW;
				window->setView(orientation);
				showReady(QStringLiteral("CloudCompare view changed."));
				return;
			}

			QString objectName;
			if (actionId == QStringLiteral("host.segment")) objectName = QStringLiteral("actionSegment");
			else if (actionId == QStringLiteral("host.cross_section")) objectName = QStringLiteral("actionCrossSection");
			else if (actionId == QStringLiteral("host.new_view")) objectName = QStringLiteral("actionNew3DView");
			else if (actionId == QStringLiteral("host.tile_views")) objectName = QStringLiteral("actionTile3DViews");
			QAction* action = objectName.isEmpty() || !m_app->getMainWindow()
				? nullptr : m_app->getMainWindow()->findChild<QAction*>(objectName);
			if (!action) { showError(QStringLiteral("CloudCompare action '%1' was not found.").arg(objectName)); return; }
			if (!action->isEnabled())
			{
				showError(actionId == QStringLiteral("host.tile_views")
					? QStringLiteral("Open at least two 3D views before tiling them.")
					: QStringLiteral("Select a cloud in CloudCompare before launching this tool."));
				return;
			}
			action->trigger();
		});
		connect(m_dock, &WorkspaceDock::suggestScalesRequested, this, &WorkspaceController::suggestScales);
		connect(m_dock, &WorkspaceDock::vegetationPreviewRequested, this, &WorkspaceController::previewVegetation);
		connect(m_dock, &WorkspaceDock::vegetationModelRequested, this, &WorkspaceController::predictVegetationModel);
		connect(m_dock, &WorkspaceDock::vegetationThresholdRequested, this, &WorkspaceController::filterVegetationScore);
		connect(m_dock, &WorkspaceDock::featureRangeReviewRequested, this, &WorkspaceController::openFeatureRangeReview);
		connect(m_dock, &WorkspaceDock::vegetationExtractRequested, this, &WorkspaceController::extractVegetation);
		connect(m_dock, &WorkspaceDock::vegetationFeatureRefinementRequested, this, [this](const QString& directory){QString error;if(!refineVegetationWithRanges(directory,error))showError(error);});
		connect(m_dock, &WorkspaceDock::computeFeaturesRequested, this, &WorkspaceController::computeFeatures);
		connect(m_dock, &WorkspaceDock::selectTrainingSetRequested, this, &WorkspaceController::selectTrainingSet);
		connect(m_dock, &WorkspaceDock::selectClassificationCloudRequested, this, &WorkspaceController::selectClassificationCloud);
		connect(m_dock, &WorkspaceDock::exportModelDatasetRequested, this, &WorkspaceController::exportModelDataset);
		connect(m_dock, &WorkspaceDock::trainModelRequested, this, &WorkspaceController::trainModels);
		connect(m_dock, &WorkspaceDock::predictModelRequested, this, &WorkspaceController::predictModel);
		connect(m_dock, &WorkspaceDock::compareModelPredictionsRequested, this, &WorkspaceController::predictModels);
		connect(m_dock, &WorkspaceDock::applyModelPredictionsRequested, this, &WorkspaceController::applyModelPredictions);
		connect(m_dock, &WorkspaceDock::predictionConfidenceFilterRequested, this, &WorkspaceController::filterPredictionByConfidence);
		connect(m_dock, &WorkspaceDock::showPredictionStatisticsRequested, this, &WorkspaceController::showPredictionStatistics);
		connect(m_dock, &WorkspaceDock::showModelResultsRequested, this, &WorkspaceController::showModelResults);
		connect(m_dock, &WorkspaceDock::bootstrapCapabilitiesRequested, this, &WorkspaceController::inspectBootstrapCapabilities);
		connect(m_dock, &WorkspaceDock::bootstrapLabelsRequested, this, &WorkspaceController::bootstrapLabels);
		connect(m_dock, &WorkspaceDock::pretrainedInstallRequested, this, &WorkspaceController::installPretrainedModel);
		connect(m_dock, &WorkspaceDock::pretrainedRemoveRequested, this, &WorkspaceController::removePretrainedModel);
		connect(m_dock, &WorkspaceDock::pretrainedMaintenanceRequested, this, &WorkspaceController::maintainPretrainedModels);
		connect(m_dock, &WorkspaceDock::cancelProcessingRequested, this, &WorkspaceController::cancelMlWorker);
		connect(m_dock, &WorkspaceDock::applyLabelsRequested, this, &WorkspaceController::applyLabels);
		connect(m_dock, &WorkspaceDock::restoreOriginalLabelsRequested, this, &WorkspaceController::restoreLabels);
		connect(m_dock, &WorkspaceDock::clearArchaeologyRequested, this, &WorkspaceController::clearArchaeology);
		connect(m_dock, &WorkspaceDock::undoRequested, this, [this]()
		{
			performUndoRedo(false);
		});
		connect(m_dock, &WorkspaceDock::redoRequested, this, [this]()
		{
			performUndoRedo(true);
		});
		connect(m_dock, &WorkspaceDock::visualizationRequested, this, &WorkspaceController::visualize);
		connect(m_dock, &WorkspaceDock::displayRefreshRequested, this, [this]() { syncDisplayControls(); updateModelFeatureChoices(); });
		connect(m_dock, &WorkspaceDock::displayLegendVisibilityRequested, this, [this](bool visible)
		{
			if (!m_selectedCloud || !m_selectedCloud->sfShown()) return;
			m_selectedCloud->showSFColorsScale(visible);
			refreshHost();
			syncDisplayControls();
		});
		connect(m_dock, &WorkspaceDock::displayPaletteRequested, this, [this](const QString& uuid)
		{
			auto* sf = m_selectedCloud ? m_selectedCloud->getCurrentDisplayedScalarField() : nullptr;
			auto scale = ccColorScalesManager::GetUniqueInstance()->getScale(uuid);
			if (sf && scale) { sf->setColorScale(scale); refreshHost(); syncDisplayControls(); }
		});
		connect(m_dock, &WorkspaceDock::displayRangeRequested, this, [this](double start, double stop)
		{
			auto* sf = m_selectedCloud ? m_selectedCloud->getCurrentDisplayedScalarField() : nullptr;
			if (!sf || sf->logScale() || !sf->getColorScale() || !sf->getColorScale()->isRelative()) return;
			if (start > stop) { showError(QStringLiteral("Colour minimum must not exceed maximum.")); syncDisplayControls(); return; }
			sf->setSaturationStart(start); sf->setSaturationStop(stop); refreshHost(); syncDisplayControls();
		});
		connect(m_dock, &WorkspaceDock::displayRangeResetRequested, this, [this]()
		{
			auto* sf = m_selectedCloud ? m_selectedCloud->getCurrentDisplayedScalarField() : nullptr;
			if (!sf || sf->logScale() || !sf->getColorScale() || !sf->getColorScale()->isRelative()) return;
			sf->setSaturationStart(sf->saturationRange().min()); sf->setSaturationStop(sf->saturationRange().max());
			refreshHost(); syncDisplayControls();
		});
		connect(m_dock, &WorkspaceDock::displayPaletteEditorRequested, this, [this]()
		{
			auto* sf = m_selectedCloud ? m_selectedCloud->getCurrentDisplayedScalarField() : nullptr;
			if (!sf || !m_app) return;
			ccColorScaleEditorDialog editor(ccColorScalesManager::GetUniqueInstance(), m_app, sf->getColorScale(), m_app->getMainWindow());
			editor.setAssociatedScalarField(sf);
			if (editor.exec() == QDialog::Accepted && editor.getActiveScale()) sf->setColorScale(editor.getActiveScale());
			refreshHost(); syncDisplayControls();
		});
	}

	void WorkspaceController::selectCloud(ccPointCloud* cloud)
	{
		if (m_featureComputationActive) return;
		if (cloud && cloud == m_selectedCloud)
		{
			updateDockState();
			return;
		}
		if (m_annotationStudio) { delete m_annotationStudio.data(); m_annotationStudio = nullptr; }
		m_selectedCloud = cloud;
		m_cloudProfile=QJsonObject();
		m_terrainResult.reset();
		m_dtmAvailable = false;
		m_metricUnitsConfirmed = false;
		m_metricUnitsLockedByMetadata = false;
		m_metricUnitsRemembered = false;
		m_estimatedSpacingMetres = 0.0;
		m_estimatedDensityPerSquareMetre = 0.0;
		m_mlDatasetPath.clear();
		m_mlDatasetFeatureKeys.clear();
		m_mlDatasetRevision = std::numeric_limits<quint64>::max();
		m_mlPredictionPath.clear();
		m_mlSummary.clear();
		m_mlPredictionStatistics.clear();
		if (!cloud)
		{
			m_dock->setCloudProfileText(QStringLiteral("Select a point cloud"));
			m_dock->clearSession();
			return;
		}
		QString error;
		ALiSSession* current = ensureSession(error);
		if (!current)
		{
			showError(error);
			return;
		}
		// A metric lock is granted only by an explicit WKT linear-unit declaration.
		// A user decision is remembered by a stable geometry/header fingerprint and,
		// when the cloud is saved as BIN, by native CloudCompare metadata.
		const QString projection = cloud->hasMetaData(QStringLiteral("LAS.projection"))
			? cloud->getMetaData(QStringLiteral("LAS.projection")).toString() : QString();
		m_metricUnitsLockedByMetadata = wktDeclaresMetres(projection);
		const quint64 uid = static_cast<quint64>(cloud->getUniqueID());
		const auto cachedPredictionStatistics = m_predictionStatisticsCache.find(uid);
		if (cachedPredictionStatistics != m_predictionStatisticsCache.end()) m_mlPredictionStatistics = cachedPredictionStatistics->second;
		const auto explicitConfirmation = m_explicitMetricConfirmations.find(uid);
		QSettings settings;
		const bool embeddedConfirmation = cloud->getMetaData(QStringLiteral("ALiS.metricUnitsConfirmed")).toBool()
			|| cloud->getMetaData(QStringLiteral("qArchaeoLiDAR.metricUnitsConfirmed")).toBool();
		const bool savedConfirmation = settings.value(metricConfirmationSettingsKey(*cloud),
			settings.value(legacyMetricConfirmationSettingsKey(*cloud), false)).toBool();
		m_metricUnitsRemembered = !m_metricUnitsLockedByMetadata && (embeddedConfirmation || savedConfirmation);
		m_metricUnitsConfirmed = m_metricUnitsLockedByMetadata
			|| m_metricUnitsRemembered
			|| (explicitConfirmation != m_explicitMetricConfirmations.end() && explicitConfirmation->second);
		const auto cachedProfile = m_cloudProfileCache.find(uid);
		if (cachedProfile != m_cloudProfileCache.end())
		{
			m_cloudProfile = cachedProfile->second;
			m_cloudProfile.insert(QStringLiteral("name"), cloud->getName());
			m_cloudProfile.insert(QStringLiteral("metricConfirmed"), m_metricUnitsConfirmed);
			cachedProfile->second = m_cloudProfile;
			m_estimatedSpacingMetres = m_cloudProfile.value(QStringLiteral("nnMedian")).toDouble();
			m_estimatedDensityPerSquareMetre = m_cloudProfile.value(QStringLiteral("nnDensity2D")).toDouble();
			m_dock->setCatalogCloudProfile(m_cloudProfile);
			m_dock->setCloudProfileText(cloudProfileText(m_cloudProfile));
			if (m_metricUnitsConfirmed && !m_cloudProfile.contains(QStringLiteral("nnMedian"))) refreshCloudProfile(true, false);
		}
		else
		{
			refreshCloudProfile(m_metricUnitsConfirmed, true);
		}
		syncGroundParametersToDock();
		updateDockState();
		if (m_metricUnitsConfirmed)
		{
			updateModelFeatureChoices();
		}
		else
		{
			m_dock->setModelFeatureChoices(QStringList(), QStringList());
		}
	}

	ccPointCloud* WorkspaceController::selectedCloud() const { return m_selectedCloud; }

	ALiSSession* WorkspaceController::session() const
	{
		if (!m_selectedCloud) return nullptr;
		const auto found = m_sessions.find(static_cast<quint64>(m_selectedCloud->getUniqueID()));
		return found == m_sessions.end() ? nullptr : found->second.get();
	}

	ALiSSession* WorkspaceController::ensureSession(QString& error)
	{
		if (!m_selectedCloud) { error = QStringLiteral("Select a single point cloud."); return nullptr; }
		const quint64 uid = static_cast<quint64>(m_selectedCloud->getUniqueID());
		auto found = m_sessions.find(uid);
		if (found == m_sessions.end())
		{
			std::unique_ptr<ALiSSession> created(new ALiSSession(*m_selectedCloud));
			if (!created->initialize(error)) return nullptr;
			found = m_sessions.emplace(uid, std::move(created)).first;
			m_featureEngines.emplace(uid, std::unique_ptr<FeatureEngine>(new FeatureEngine(m_selectedCloud)));
		}
		if (!found->second->isCompatibleWith(*m_selectedCloud))
		{
			error = QStringLiteral("The cloud geometry/point count changed; start a new session."); return nullptr;
		}
		return found->second.get();
	}

	void WorkspaceController::updateDockState(const QString& status)
	{
		syncDisplayControls();
		ALiSSession* s = session();
		if (!s || !m_selectedCloud) { m_dock->clearSession(); return; }
		const QString units = m_metricUnitsLockedByMetadata
			? QStringLiteral("metres (CRS metadata confirmed)")
			: (m_metricUnitsRemembered ? QStringLiteral("metres (remembered for this cloud)")
				: (m_metricUnitsConfirmed ? QStringLiteral("metres (explicit user confirmation)")
				: QStringLiteral("unknown; confirm that coordinates are metres")));
		m_dock->setSessionInfo(m_selectedCloud->getName(), s->entityUid(), s->pointCount(), units, s->dirty());
		m_dock->setMetricUnitConfirmation(m_metricUnitsConfirmed, m_metricUnitsLockedByMetadata);
		const QString groundSummary = s->hasGroundPreview()
			? QStringLiteral("Preview available - review before Apply")
			: (s->hasAppliedGround() ? QStringLiteral("Ground applied and synchronized with ASPRS class 2") : QString());
		m_dock->setGroundState(s->hasGroundPreview(), s->hasAppliedGround(), m_dtmAvailable,
			m_selectedCloud->getScalarFieldIndexByName(field::HeightAboveGround) >= 0,
			groundSummary);
		m_dock->setUndoRedoAvailable(s->canUndo(), s->canRedo());
		const bool predictionsAvailable = m_selectedCloud->getScalarFieldIndexByName(field::AsprsPrediction) >= 0
			&& m_selectedCloud->getScalarFieldIndexByName(field::AsprsConfidence) >= 0;
		m_dock->setModelState(s->trustedTrainingCount(), QDir::toNativeSeparators(m_mlDatasetPath),
			QFileInfo::exists(m_mlModelPath), predictionsAvailable, m_mlSummary);
		m_dock->setPredictionStatistics(m_mlPredictionStatistics);
		QString trainingName;
		QString testName;
		const auto trainingFound = m_sessions.find(m_trainingCloudUid);
		if (trainingFound != m_sessions.end()) trainingName = trainingFound->second->cloud().getName();
		const auto testFound = m_sessions.find(m_testCloudUid);
		if (testFound != m_sessions.end()) testName = testFound->second->cloud().getName();
		m_dock->setTrainingSetState(trainingName, testName);
		QString predictionName;
		const auto predictionFound = m_sessions.find(m_predictionCloudUid);
		if (predictionFound != m_sessions.end()) predictionName = predictionFound->second->cloud().getName();
		m_dock->setClassificationTargetState(predictionName);
		const auto& visibility = m_selectedCloud->getTheVisibilityArray();
		quint64 visibleCount = 0;
		if (visibility.size() == m_selectedCloud->size())
		{
			for (unsigned index = 0; index < visibility.size(); ++index)
			{
				if (visibility[index] == CCCoreLib::POINT_VISIBLE) ++visibleCount;
			}
		}
		const bool nonTrivialSubset = visibility.size() == m_selectedCloud->size()
			&& visibleCount > 0 && visibleCount < m_selectedCloud->size();
		m_dock->setVisibleSubset(nonTrivialSubset
			? QStringLiteral("CloudCompare visible subset")
			: QStringLiteral("No non-trivial visible subset"),
			nonTrivialSubset ? visibleCount : 0);
		m_dock->clearHistory();
		for (const ProcessingRecord& record : s->history())
		{
			m_dock->appendHistoryEntry(record.timestampUtc.toLocalTime().toString(Qt::ISODate), record.operation,
				QStringLiteral("%1 points; %2 ms").arg(record.affectedPoints).arg(record.elapsedMilliseconds));
		}
		if (!status.isEmpty()) m_dock->setStatus(status, WorkspaceDock::StatusTone::Ready);
	}

	void WorkspaceController::refreshHost()
	{
		if (m_selectedCloud) m_selectedCloud->prepareDisplayForRefresh();
		if (m_annotationStudio) m_annotationStudio->refreshColours();
		if (m_app) { m_app->updateUI(); m_app->refreshAll(); }
	}

	WorkspaceController::~WorkspaceController() { delete m_annotationStudio.data(); }

	void WorkspaceController::openAnnotationStudio()
	{
		if (!m_app || !m_selectedCloud || !requireMetricUnits(QStringLiteral("Annotation section"))) return;
		if (m_mlProcess && m_mlProcess->state() != QProcess::NotRunning) { showError(QStringLiteral("Wait for the active model job before annotating.")); return; }
		if (m_annotationStudio) { m_annotationStudio->show(); m_annotationStudio->raise(); m_annotationStudio->activateWindow(); return; }
		QString error;
		auto* current = ensureSession(error);
		if (!current) { showError(error); return; }
		try { m_annotationStudio = new AnnotationStudio(m_selectedCloud, m_app->getMainWindow()); }
		catch (const std::bad_alloc&) { showError(QStringLiteral("Not enough memory to open the annotation studio.")); return; }
		m_annotationStudio->setHistoryAvailable(current->canUndo(), current->canRedo());
		connect(m_annotationStudio, &AnnotationStudio::labelsRequested, this, [this](const std::vector<unsigned>& ids, int asprs, int)
		{
			QString error; auto* s = ensureSession(error); if (!s) { showError(error); return; }
			const quint64 previousGroundRevision = s->groundRevision();
			bool ok = asprs >= 0 && s->applyManualAsprs(ids, static_cast<std::uint8_t>(asprs), error);
			if (!ok) { showError(error); return; }
			if (s->groundRevision() != previousGroundRevision) resetGroundDerivedState();
			markCloudProfileStale(QStringLiteral("manual ASPRS labels changed"));
			s->displayScalarField(QString::fromUtf8(field::WorkingAsprs), error);
			refreshHost(); updateDockState();
			showReady(QStringLiteral("Assigned trusted labels to %1 explicitly selected original points. Save the cloud to keep labels on disk.").arg(countText(ids.size())));
		});
		connect(m_annotationStudio, &AnnotationStudio::historyRequested, this, &WorkspaceController::performUndoRedo);
		m_annotationStudio->show(); m_annotationStudio->raise();
	}

	void WorkspaceController::syncDisplayControls()
	{
		if (m_annotationStudio && session()) m_annotationStudio->setHistoryAvailable(session()->canUndo(), session()->canRedo());
		QStringList fields, ids, names;
		if (m_selectedCloud)
			for (unsigned i = 0; i < m_selectedCloud->getNumberOfScalarFields(); ++i)
				fields << QString::fromUtf8(m_selectedCloud->getScalarField(i)->getName());
		auto* active = m_selectedCloud && m_selectedCloud->sfShown() ? m_selectedCloud->getCurrentDisplayedScalarField() : nullptr;
		m_dock->setDisplayFields(fields, active ? QString::fromUtf8(active->getName()) : QString(),
			m_selectedCloud && m_selectedCloud->getTheVisibilityArray().size() == m_selectedCloud->size());
		const auto& scales = ccColorScalesManager::GetUniqueInstance()->map();
		for (auto it = scales.cbegin(); it != scales.cend(); ++it) { ids << it.key(); names << it.value()->getName(); }
		auto* sf = m_selectedCloud && m_selectedCloud->sfShown() ? m_selectedCloud->getCurrentDisplayedScalarField() : nullptr;
		m_dock->setDisplayPalettes(ids, names, sf && sf->getColorScale() ? sf->getColorScale()->getUuid() : QString());
		const bool enabled = sf && !sf->logScale() && sf->getColorScale() && sf->getColorScale()->isRelative();
		if (enabled) m_dock->setDisplayRange(true, sf->saturationRange().min(), sf->saturationRange().max(), sf->saturationRange().start(), sf->saturationRange().stop());
		else m_dock->setDisplayRange(false, 0, 1, 0, 1);
		m_dock->setDisplayLegendState(sf != nullptr, sf && m_selectedCloud->sfColorScaleShown());
	}

	void WorkspaceController::showError(const QString& message)
	{
		m_dock->setBusy(false); m_dock->setStatus(message.isEmpty() ? QStringLiteral("Operation failed") : message, WorkspaceDock::StatusTone::Error);
		if (m_app) m_app->dispToConsole(QStringLiteral("[ALiS] %1").arg(message), ccMainAppInterface::ERR_CONSOLE_MESSAGE);
	}

	void WorkspaceController::showReady(const QString& message)
	{
		m_dock->setBusy(false); m_dock->setStatus(message, WorkspaceDock::StatusTone::Ready);
		if (m_app) m_app->dispToConsole(QStringLiteral("[ALiS] %1").arg(message));
	}

	bool WorkspaceController::requireMetricUnits(const QString& operation)
	{
		if (m_metricUnitsConfirmed)
		{
			return true;
		}
		showError(QStringLiteral("%1 requires metric coordinates. Confirm 'Coordinates are metres' in Session, or load CRS metadata that declares metre units.")
			.arg(operation));
		return false;
	}

	void WorkspaceController::syncGroundParametersToDock()
	{
		m_dock->setAdvancedGroundParameters(m_groundParameters.clothResolution,
			m_groundParameters.classificationThreshold,
			m_groundParameters.timeStep,
			m_groundParameters.rigidness,
			m_groundParameters.iterations,
			m_groundParameters.slopeProcessing);
		m_dock->setPmfParameters(m_pmfWindowSizes, m_pmfThresholds, m_pmfCellSizeMetres);
	}

	void WorkspaceController::resetGroundDerivedState()
	{
		m_terrainResult.reset();
		m_dtmAvailable = false;
		if (m_selectedCloud)
		{
			const quint64 uid = static_cast<quint64>(m_selectedCloud->getUniqueID());
			auto engine = m_featureEngines.find(uid);
			if (engine != m_featureEngines.end())
			{
				engine->second->clearCache();
			}
		}
		m_dock->setModelFeatureChoices(QStringList(), QStringList());
		m_mlDatasetPath.clear();
		m_mlDatasetFeatureKeys.clear();
		m_mlDatasetRevision = std::numeric_limits<quint64>::max();
	}

	void WorkspaceController::performUndoRedo(bool redo)
	{
		ALiSSession* s = session();
		if (!s)
		{
			showError(QStringLiteral("No active session."));
			return;
		}
		const quint64 previousGroundRevision = s->groundRevision();
		const quint64 previousSourceRevision = s->sourceRevision();
		QString message;
		const bool succeeded = redo ? s->redo(message) : s->undo(message);
		if (!succeeded)
		{
			showError(message);
			return;
		}
		if (s->groundRevision() != previousGroundRevision)
		{
			resetGroundDerivedState();
		}
		if (s->sourceRevision() != previousSourceRevision) markCloudProfileStale(QStringLiteral("Undo/Redo changed classification attributes"));
		refreshHost();
		updateDockState(message);
	}

	GroundFilterParameters WorkspaceController::currentGroundParameters() const
	{
		GroundFilterParameters p;
		if (!m_selectedCloud) return p;
		p.clothResolution = metresToLocal(m_groundParameters.clothResolution, *m_selectedCloud);
		p.classificationThreshold = metresToLocal(m_groundParameters.classificationThreshold, *m_selectedCloud);
		p.timeStep = m_groundParameters.timeStep; p.rigidness = m_groundParameters.rigidness;
		p.iterations = m_groundParameters.iterations; p.smoothSlope = m_groundParameters.slopeProcessing;
		p.pmfCellSize = metresToLocal(m_pmfCellSizeMetres, *m_selectedCloud);
		std::vector<double> windowsMetres;
		std::vector<double> thresholdsMetres;
		if (parsePositiveSequence(m_pmfWindowSizes, windowsMetres)
		    && parsePositiveSequence(m_pmfThresholds, thresholdsMetres))
		{
			p.pmfWindowSizes.reserve(windowsMetres.size());
			p.pmfThresholds.reserve(thresholdsMetres.size());
			for (double value : windowsMetres) p.pmfWindowSizes.push_back(metresToLocal(value, *m_selectedCloud));
			for (double value : thresholdsMetres) p.pmfThresholds.push_back(metresToLocal(value, *m_selectedCloud));
		}
		return p;
	}

	void WorkspaceController::recordGroundRun(const GroundFilterResult& result)
	{
		ALiSSession* s = session(); if (!s) return;
		ProcessingRecord record; record.operation = QStringLiteral("GroundFilter.%1.Preview").arg(result.provenance.algorithmId);
		record.timestampUtc = QDateTime::currentDateTimeUtc(); record.parameters = result.provenance.effectiveParameters;
		record.parameters.insert(QStringLiteral("processingPreset"), processingPresetToJson(captureProcessingPreset()));
		record.sourceEntityUid = s->entityUid(); record.affectedPoints = s->pointCount();
		record.outputFields = QStringList() << QString::fromUtf8(field::GroundPreview);
		record.elapsedMilliseconds = result.provenance.elapsedMilliseconds; s->addProcessingRecord(record);
	}

	void WorkspaceController::runGroundPreview()
	{
		if (!requireMetricUnits(QStringLiteral("Ground Preview"))) return;
		QString error; ALiSSession* s = ensureSession(error); if (!s) { showError(error); return; }
		m_dock->setBusy(true);
		m_dock->setProgress(m_groundAlgorithmId.startsWith(QStringLiteral("pmf."))
		                        ? QStringLiteral("PMF Ground Preview")
		                        : QStringLiteral("CSF Ground Preview"),
		                    0, 0, false);
		GroundFilterContext context; context.app = m_app; context.parentWidget = m_dock;
		const GroundFilterResult result = m_groundFilter->run(*m_selectedCloud, currentGroundParameters(), context);
		m_dock->clearProgress();
		if (!result.succeeded() || !s->setGroundPreview(result.isGround, error)) { showError(!error.isEmpty() ? error : result.message); return; }
		recordGroundRun(result); refreshHost(); updateDockState();
		showReady(QStringLiteral("Ground Preview: %1 Ground, %2 Non-ground. Review before Apply.")
			.arg(countText(result.groundPointCount), countText(result.offGroundPointCount)));
	}

	void WorkspaceController::applyGround()
	{
		QString error; ALiSSession* s = session(); if (!s || !s->applyGroundPreview(error)) { showError(error); return; }
		resetGroundDerivedState();
		markCloudProfileStale(QStringLiteral("Ground classification changed"));
		refreshHost(); updateDockState(); showReady(QStringLiteral("Ground applied: ASPRS class 2 and the DTM mask are synchronized. Rejected former class-2 points became Unclassified (1)."));
	}

	void WorkspaceController::discardGround()
	{
		QString error; ALiSSession* s = session(); if (!s || !s->discardGroundPreview(error)) { showError(error); return; }
		refreshHost(); updateDockState(); showReady(QStringLiteral("Ground preview discarded."));
	}

	void WorkspaceController::restoreGround()
	{
		ALiSSession* s = session();
		if (!s) { showError(QStringLiteral("No active session.")); return; }
		QString message;
		if (!s->restoreGround(message)) { showError(message); return; }
		resetGroundDerivedState();
		markCloudProfileStale(QStringLiteral("Ground classification was restored"));
		refreshHost();
		updateDockState(message);
	}

	void WorkspaceController::assignGroundToAsprs()
	{
		QString error; ALiSSession* s = session(); if (!s || !s->assignGroundToWorkingAsprs(error)) { showError(error); return; }
		markCloudProfileStale(QStringLiteral("ASPRS Working classes changed"));
		refreshHost(); updateDockState(); showReady(QStringLiteral("Only Ground points were assigned ASPRS class 2 in the Working field."));
	}

	void WorkspaceController::computeDtm(double gridStepMetres,
	                                    bool fillEmptyCells,
	                                    double maximumInterpolationEdgeMetres)
	{
		if (!requireMetricUnits(QStringLiteral("DTM computation"))) return;
		ALiSSession* s = session(); if (!s) { showError(QStringLiteral("No active session.")); return; }
		const std::vector<bool> mask = s->appliedGroundMask(); if (mask.empty()) { showError(QStringLiteral("Apply Ground before computing DTM.")); return; }
		TerrainParameters p;
		p.gridStep = metresToLocal(gridStepMetres, *m_selectedCloud);
		p.interpolateEmptyCells = fillEmptyCells;
		p.maximumInterpolationEdgeLength = fillEmptyCells
			? metresToLocal(maximumInterpolationEdgeMetres, *m_selectedCloud)
			: 0.0;
		ccProgressDialog progress(true, m_dock);
		progress.setWindowModality(Qt::WindowModal);
		progress.setMethodTitle(QStringLiteral("ALiS Terrain"));
		progress.setInfo(QStringLiteral("Computing qAL DTM and HAG..."));
		TerrainContext context; context.app = m_app; context.progressDialog = &progress;
		m_dock->setBusy(true);
		TerrainResult result = m_terrainEngine.run(*m_selectedCloud, mask, p, context);
		if (!result.succeeded()) { showError(result.message); return; }
		m_terrainResult.reset(new TerrainResult(std::move(result)));
		QJsonObject hagParams = m_terrainResult->provenance.effectiveParameters;
		QString error;
		if (!s->setHagValues(m_terrainResult->heightAboveGround, hagParams, m_terrainResult->provenance.elapsedMilliseconds, error)) { showError(error); return; }
		if (!m_app || !m_terrainResult->dtmCloud)
		{
			showError(QStringLiteral("DTM was computed, but its CloudCompare entity could not be committed."));
			return;
		}
		ccPointCloud* dtm = m_terrainResult->dtmCloud.release();
		m_app->addToDB(dtm); // DB Tree now owns the committed DTM entity.
		m_dtmAvailable = true;
		ProcessingRecord record; record.operation = QStringLiteral("Terrain.ComputeDTM"); record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.parameters = hagParams; record.sourceEntityUid = s->entityUid(); record.affectedPoints = m_terrainResult->statistics.groundPointCount;
		record.outputFields = QStringList() << QStringLiteral("qAL DTM"); record.elapsedMilliseconds = m_terrainResult->provenance.elapsedMilliseconds; s->addProcessingRecord(record);
		refreshHost(); updateDockState();
		showReady(QStringLiteral("DTM %1 m (%2); HAG valid %3/%4, NODATA %5.")
			.arg(gridStepMetres, 0, 'g', 6)
			.arg(fillEmptyCells ? QStringLiteral("Delaunay void fill") : QStringLiteral("voids left NODATA"))
			.arg(countText(m_terrainResult->statistics.heightAboveGround.validCount), countText(s->pointCount()),
				countText(m_terrainResult->statistics.heightAboveGround.nodataCount)));
	}

	void WorkspaceController::computeHag()
	{
		if (!requireMetricUnits(QStringLiteral("HAG computation"))) return;
		if (!m_terrainResult) { showError(QStringLiteral("Compute DTM first; HAG is derived from its persistent session grid.")); return; }
		QString error; ALiSSession* s = session();
		if (!s || !s->setHagValues(m_terrainResult->heightAboveGround, m_terrainResult->provenance.effectiveParameters,
			m_terrainResult->provenance.elapsedMilliseconds, error)) { showError(error); return; }
		refreshHost(); updateDockState(); showReady(QStringLiteral("qAL_HAG materialized and displayed."));
	}

	void WorkspaceController::previewVegetation(int preset, double maximumRadius, int minimumNeighbors,
	                                          double lowHeight, double mediumHeight, const QString& outputDirectory,
	                                          int targetNeighbors, double fineRadiusLimit)
	{
		if(m_featureComputationActive || !m_selectedCloud || !session()) return;
		if(!requireMetricUnits(QStringLiteral("Vegetation separation"))) return;
		if(preset<0 || preset>2 || minimumNeighbors<8 || minimumNeighbors>64 || targetNeighbors<8 || targetNeighbors>128
		   || !std::isfinite(fineRadiusLimit) || fineRadiusLimit<.05 || fineRadiusLimit>.5 || !std::isfinite(maximumRadius)
		   || maximumRadius<.2 || maximumRadius>3 || !std::isfinite(lowHeight) || !std::isfinite(mediumHeight)
		   || lowHeight<=0 || mediumHeight<=lowHeight){showError(QStringLiteral("Check preset, radius and height limits (medium must exceed low)."));return;}
		auto* cloud=m_selectedCloud; auto* s=session();
		auto scalar=[cloud](const char* name)->ccScalarField*{int i=cloud->getScalarFieldIndexByName(name);return i<0?nullptr:static_cast<ccScalarField*>(cloud->getScalarField(i));};
		auto* hag=scalar(field::HeightAboveGround);auto* working=scalar(field::WorkingAsprs);
		if(!s->hasAppliedGround() || !working || (hag && hag->currentSize()!=cloud->size())){showError(QStringLiteral("Apply Ground first. DTM/HAG is optional for binary separation and required only for height subclasses."));return;}
		if(outputDirectory.trimmed().isEmpty() || !QDir().mkpath(outputDirectory)){showError(QStringLiteral("Choose a writable report folder."));return;}
		const quint64 sourceRevision=s->sourceRevision();
		const qint64 started=QDateTime::currentMSecsSinceEpoch();
		if(!m_cloudProfile.contains(QStringLiteral("nnMedian")))refreshCloudProfile(true,false);
		const auto scales=vegetation::radii(m_cloudProfile.value("nnMedian").toDouble(.1),m_cloudProfile.value("nnDensity2D").toDouble(),maximumRadius,static_cast<unsigned>(targetNeighbors),fineRadiusLimit);
		const QVector<double> radii{scales[0],scales[1],scales[2]};
		computeFeatures({"neighbor_count","planarity","sphericity","surface_variation","roughness"},radii);
		if(m_selectedCloud!=cloud || session()!=s || s->sourceRevision()!=sourceRevision){showError(QStringLiteral("Cloud changed; run vegetation preview again."));return;}
		auto engine=m_featureEngines.find(cloud->getUniqueID());if(engine==m_featureEngines.end())return;
		const FeatureId ids[]={FeatureId::NeighborCount,FeatureId::Planarity,FeatureId::Sphericity,FeatureId::SurfaceVariation,FeatureId::Roughness};
		const std::vector<ScalarType>* values[3][5]={};
		for(int r=0;r<3;++r)for(int f=0;f<5;++f){FeatureKey k;k.feature=ids[f];k.radius=scales[r];values[r][f]=engine->second->cachedValues(k);
			if(!values[r][f] || values[r][f]->size()!=cloud->size()){showError(QStringLiteral("Feature computation incomplete/canceled. Previous vegetation result was not replaced."));return;}}
		auto alias=[cloud](const QStringList& names)->ccScalarField*{for(unsigned i=0;i<cloud->getNumberOfScalarFields();++i){QString name=QString::fromUtf8(cloud->getScalarFieldName(i)).toLower().remove(QRegularExpression("[^a-z0-9]"));if(names.contains(name))return static_cast<ccScalarField*>(cloud->getScalarField(i));}return nullptr;};
		const auto* rn=alias({"returnnumber","returnindex"});const auto* nr=alias({"numberofreturns","numberofreturn","numberofechoes","numberofechos"});
		const auto* trusted=scalar(field::AsprsTrainingClass);
		const auto ground=s->appliedGroundMask();
		QScopedValueRollback<bool> guard(m_featureComputationActive,true);
		m_dock->setBusy(true);
		ccProgressDialog progress(true,m_dock);progress.setWindowModality(Qt::ApplicationModal);progress.setMinimumDuration(0);
		progress.setAutoClose(false);progress.setAutoReset(false);progress.setMethodTitle(QStringLiteral("ALiS — adaptive vegetation preview [CPU]"));
		progress.setInfo(QStringLiteral("Adapting thresholds, checking each point and protecting ground…"));progress.show();
		std::atomic<bool> canceled{false};std::atomic<int> percent{0};connect(&progress,&QProgressDialog::canceled,&progress,[&](){canceled.store(true);});
		std::vector<float> category,evidence,asprs;QJsonObject report;std::array<quint64,5> histogram{};QString failure;
		try{
			auto future=std::async(std::launch::async,[&](){
				auto observation=[&](unsigned i,int r){vegetation::Observation o;o.count=(*values[r][0])[i];o.planarity=(*values[r][1])[i];o.scattering=(*values[r][2])[i];o.variation=(*values[r][3])[i];o.roughnessRatio=std::abs((*values[r][4])[i])/scales[r];return o;};
				std::vector<vegetation::Observation> sample;sample.reserve(30000);
				// Deterministic bounded sample, excluding Ground and existing semantic
				// noise/water. Labels are NOT used to learn decision thresholds.
				const unsigned stride=std::max(1u,cloud->size()/10000u);
				for(unsigned i=0;i<cloud->size();i+=stride){if(canceled.load())return false;int code=std::isfinite(working->getValue(i))?int(working->getValue(i)):1;
					if(ground[i] || code==7 || code==18 || code==9 || code==22)continue;for(int r=0;r<3;++r)sample.push_back(observation(i,r));}
				auto thresholds=vegetation::adapt(sample,preset,static_cast<unsigned>(minimumNeighbors));
				std::array<quint64,10> reasons{};quint64 tp=0,fp=0,fn=0,tn=0,auditUncertain=0,validReturns=0,missingHag=0,vegetationMissingHag=0;
				category.resize(cloud->size());evidence.resize(cloud->size());asprs.resize(cloud->size(),1);
				for(unsigned i=0;i<cloud->size();++i){
					if((i&4095)==0){if(canceled.load())return false;percent.store(int(100.0*i/cloud->size()));}
					const float raw=working->getValue(i);const int code=std::isfinite(raw)?int(raw):1;
					const double h=hag?hag->getValue(i):std::numeric_limits<double>::quiet_NaN();bool multiple=false;missingHag+=!std::isfinite(h);
					if(rn&&nr){double r=rn->getValue(i),n=nr->getValue(i);if(std::isfinite(r)&&std::isfinite(n)&&r>=1&&r<=n&&n<=15&&r==std::floor(r)&&n==std::floor(n)){++validReturns;multiple=n>1;}}
					std::array<vegetation::Observation,3> local{{observation(i,0),observation(i,1),observation(i,2)}};
					const auto d=vegetation::decide(local,thresholds,ground[i],code,h,multiple);
					category[i]=float(d.category);evidence[i]=float(d.evidence);++histogram[d.category];++reasons[d.reason];
					if(d.category==vegetation::Ground)asprs[i]=2;
					else if(d.category==vegetation::Vegetation){if(std::isfinite(h))asprs[i]=h<=lowHeight?3.f:(h<=mediumHeight?4.f:5.f);else ++vegetationMissingHag;}
					if(!ground[i]&&trusted&&isTrustedAsprsLabel(raw,trusted->getValue(i))&&code>=2&&code!=7&&code!=18&&code!=22){
						bool truth=code>=3&&code<=5,pred=d.category==vegetation::Vegetation;
						if(truth&&pred)++tp;else if(!truth&&pred)++fp;else if(truth)++fn;else ++tn;
						auditUncertain+=(d.category==vegetation::Uncertain);
					}
				}
				QJsonArray counts,why;for(auto n:histogram)counts.append(double(n));for(auto n:reasons)why.append(double(n));
				report=QJsonObject{{"schema","alis-vegetation-preview/1.0"},{"preset",preset},{"point_count",double(cloud->size())},
					{"radii_metres",QJsonArray{scales[0],scales[1],scales[2]}},{"cloud_profile",m_cloudProfile},
					{"thresholds",QJsonObject{{"planarity_protection",thresholds.planar},{"scattering",thresholds.scattering},{"surface_variation",thresholds.variation},{"abs_roughness_over_radius",thresholds.roughnessRatio},{"near_ground_protection_m",thresholds.nearGround},{"minimum_neighbors",minimumNeighbors}}},
					{"adaptation_observations",double(sample.size())},{"counts_0_uncertain_1_vegetation_2_surface_3_ground_4_excluded",counts},{"reason_counts",why},
					{"reason_codes","1 ground; 2 existing excluded class; 3 negative HAG; 4 insufficient supported scales; 5 fine planar protection; 6 near-ground protection; 7 multiscale volume agreement; 8 planar agreement; 9 ambiguous"},
					{"missing_hag_points",double(missingHag)},{"vegetation_without_height_subclass",double(vegetationMissingHag)},
					{"height_breaks_m",QJsonArray{lowHeight,mediumHeight}},{"valid_return_points",double(validReturns)},
					{"subsampled",false},{"device","cpu"},{"certified",false},{"working_classes_modified",false},
					{"limitations","Unvalidated adaptive heuristic, not a universal classifier. Eigenvalue descriptors are correlated. Evidence is NOT calibrated confidence. Retained points include uncertain vegetation, rocks, trunks and structures. Occluded unsampled surfaces cannot be recovered. Ground is protected and not audited by this stage."},
					{"references",QJsonArray{"https://doi.org/10.5194/isprs-archives-XLVIII-M-9-2025-821-2025","https://arxiv.org/abs/1107.0550","https://lidar.univ-rennes.fr/en/3dmasc"}},
					{"threshold_origin","ALiS guarded empirical quantiles; not thresholds claimed by cited papers"},
					{"manual_audit",QJsonObject{{"scope","Current cloud, only matching manually confirmed non-ground labels; not independent site validation"},{"tp",double(tp)},{"fp_vegetation_removed_from_other",double(fp)},{"fn_vegetation_retained",double(fn)},{"tn",double(tn)},{"uncertain_in_audit",double(auditUncertain)},{"precision",tp+fp?QJsonValue(double(tp)/(tp+fp)):QJsonValue()},{"recall",tp+fn?QJsonValue(double(tp)/(tp+fn)):QJsonValue()}}}};
				return true;
			});
			QEventLoop loop;QTimer timer;connect(&timer,&QTimer::timeout,&loop,[&](){progress.setValue(percent.load());if(future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready)loop.quit();});timer.start(50);loop.exec();
			if(!future.get()||canceled.load())failure=QStringLiteral("Vegetation preview canceled. Previous preview and original points preserved.");
		}catch(const std::exception& e){failure=QString::fromUtf8(e.what());}
		progress.hide();m_dock->setBusy(false);if(!failure.isEmpty()){showError(failure);return;}
		const qint64 elapsed=QDateTime::currentMSecsSinceEpoch()-started;
		report.insert("scale_policy",QJsonObject{{"schema","alis-vegetation-scale-policy/2"},{"target_neighbors_initial_estimate",targetNeighbors},{"fine_radius_limit_m",fineRadiusLimit},{"maximum_radius_m",maximumRadius},{"rule","sqrt(k/(pi*rho)); clamp fine scale, then r/2r/4r. Actual per-point support checked. No universal density invariance."}});
		report.insert("elapsed_ms",double(elapsed));report.insert("source_revision",QString::number(s->sourceRevision()));report.insert("source_uid",QString::number(cloud->getUniqueID()));
		report.insert("created_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
		const QString path=QDir(outputDirectory).filePath(QStringLiteral("vegetation_%1.json").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")));
		QSaveFile file(path);const QByteArray bytes=QJsonDocument(report).toJson();
		if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){showError(QStringLiteral("Could not save report. Preview not published: %1").arg(file.errorString()));return;}
		const auto audit=report.value("manual_audit").toObject();
		const double fp=audit.value("fp_vegetation_removed_from_other").toDouble(),tn=audit.value("tn").toDouble();
		const QString auditText=audit.value("precision").isDouble()
			? QStringLiteral("Manual-label audit (not independent validation): vegetation precision %1%; recall %2%; other points incorrectly sent to vegetation %3 / %4 (%5%).")
				.arg(100*audit.value("precision").toDouble(),0,'f',2).arg(100*audit.value("recall").toDouble(),0,'f',2).arg(fp,0,'f',0).arg(fp+tn,0,'f',0).arg(fp+tn>0?100*fp/(fp+tn):0,0,'f',2)
			: QStringLiteral("No eligible manually confirmed reference labels: reliability has NOT been measured on this cloud.");
		QString html=QStringLiteral("<!doctype html><html lang='en'><meta charset='utf-8'><title>ALiS vegetation preview</title>"
			"<style>body{font:16px system-ui;max-width:1050px;margin:40px auto;padding:20px;color:#193454}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ccd;padding:8px;text-align:left}pre{white-space:pre-wrap;background:#f2f5f8;padding:14px}.warn{background:#fff3cd;padding:16px}</style>"
			"<h1>ALiS — vegetation / retained points</h1><p class='warn'><b>EXPERIMENTAL HEURISTIC — NOT CERTIFIED.</b> Review before separation. Other surface does not mean Building. Original points and Working labels are unchanged.</p><p>%1</p><p>%2</p>"
			"<table><tr><th>Disposition</th><th>Points</th></tr><tr><td>Uncertain (retained)</td><td>%3</td></tr><tr><td>Vegetation proposal</td><td>%4</td></tr><tr><td>Other surface (retained)</td><td>%5</td></tr><tr><td>Protected Ground</td><td>%6</td></tr><tr><td>Excluded noise/water (retained)</td><td>%7</td></tr></table>"
			"<h2>Parameters, reasons and provenance</h2><pre>%8</pre><h2>Scientific basis</h2><p><a href='https://doi.org/10.5194/isprs-archives-XLVIII-M-9-2025-821-2025'>Mediterranean archaeological vegetation classification (ISPRS, 2025)</a> · <a href='https://arxiv.org/abs/1107.0550'>Multiscale dimensionality (Brodu &amp; Lague)</a> · <a href='https://lidar.univ-rennes.fr/en/3dmasc'>3DMASC</a></p></html>")
			.arg(cloud->getName().toHtmlEscaped(),auditText.toHtmlEscaped(),countText(histogram[0]),countText(histogram[1]),countText(histogram[2]),countText(histogram[3]),countText(histogram[4]),QString::fromUtf8(bytes).toHtmlEscaped());
		QSaveFile humanReport(path+QStringLiteral(".html"));const auto htmlBytes=html.toUtf8();
		if(!humanReport.open(QIODevice::WriteOnly)||humanReport.write(htmlBytes)!=htmlBytes.size()||!humanReport.commit()){showError(QStringLiteral("Could not save readable report; preview not published."));return;}
		// Allocate all three new fields before replacing old ones: memory failures
		// cannot remove a previous result or leave a partial new result.
		const char* names[]={"qAL_VegetationReview","qAL_VegetationEvidence","qAL_VegetationASPRS"};
		const std::vector<float>* outputs[]={&category,&evidence,&asprs};
		std::array<ccScalarField*,3> staged{{nullptr,nullptr,nullptr}};
		for(int j=0;j<3;++j){staged[j]=new ccScalarField(names[j]);staged[j]->link();if(!staged[j]->resizeSafe(cloud->size())){for(auto* f:staged)if(f)f->release();showError(QStringLiteral("Not enough RAM to publish vegetation fields."));return;}for(unsigned i=0;i<cloud->size();++i)staged[j]->setValue(i,(*outputs[j])[i]);staged[j]->computeMinAndMax();}
		for(int j=0;j<3;++j){int old=cloud->getScalarFieldIndexByName(names[j]);if(old>=0)cloud->deleteScalarField(old);cloud->addScalarField(staged[j]);staged[j]->release();}
		auto palette=ccColorScale::Create(QStringLiteral("ALiS vegetation review"));palette->insert(ccColorScaleElement(0,QColor(245,190,45)),false);palette->insert(ccColorScaleElement(.25,QColor(40,170,70)),false);palette->insert(ccColorScaleElement(.5,QColor(70,145,230)),false);palette->insert(ccColorScaleElement(.75,QColor(160,110,65)),false);palette->insert(ccColorScaleElement(1,QColor(120,120,120)),false);palette->update();palette->setAbsolute(0,4);
		scalar(names[0])->setColorScale(palette);
		cloud->setMetaData(QStringLiteral("ALiS.vegetationPreview"),QJsonDocument(report).toJson(QJsonDocument::Compact));
		ProcessingRecord record;record.operation="Vegetation.AdaptivePreview";record.timestampUtc=QDateTime::currentDateTimeUtc();record.sourceEntityUid=s->entityUid();record.affectedPoints=cloud->size();record.elapsedMilliseconds=elapsed;record.parameters=report;record.parameters.insert("report_path",path);record.outputFields=QStringList{names[0],names[1],names[2]};s->addProcessingRecord(record);
		QString error;s->displayScalarField(QString::fromLatin1(names[0]),error);refreshHost();updateModelFeatureChoices();syncDisplayControls();updateDockState();
		const QString summary=QStringLiteral("Vegetation proposal: %1 points. Other surfaces: %2. Uncertain retained: %3. Ground protected: %4.\nRadii: %5 / %6 / %7 m.\nReport: %8\nNot certified: review before extraction or training.")
			.arg(countText(histogram[1]),countText(histogram[2]),countText(histogram[0]),countText(histogram[3]),QString::number(scales[0],'g',3),QString::number(scales[1],'g',3),QString::number(scales[2],'g',3),path);
		showReady(summary);if(m_app)QMessageBox::information(m_dock,QStringLiteral("Vegetation preview — review required"),summary+QStringLiteral("\n\n")+auditText);
	}

	void WorkspaceController::extractVegetation()
	{
		if(!m_selectedCloud||!session()||!m_app)return;
		auto* cloud=m_selectedCloud;const int index=cloud->getScalarFieldIndexByName("qAL_VegetationReview");
		const auto report=QJsonDocument::fromJson(cloud->getMetaData("ALiS.vegetationPreview").toByteArray()).object();
		if(index<0||report.value("source_uid").toString()!=QString::number(cloud->getUniqueID())||report.value("source_revision").toString()!=QString::number(session()->sourceRevision())){
			showError(QStringLiteral("Run a fresh vegetation preview after changes or reload before extraction."));return;}
		if(QMessageBox::warning(m_dock,QStringLiteral("Unvalidated vegetation proposal"),QStringLiteral("This geometric preview may include structural points in vegetation and retain some vegetation. It is NOT a certified classifier. Continue with two review copies? The original will be preserved; uncertain points stay in the retained cloud."),QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
		CCCoreLib::ReferenceCloud vegetation(cloud),retained(cloud);auto* classes=cloud->getScalarField(index);
		for(unsigned i=0;i<cloud->size();++i)if(!(classes->getValue(i)==1?vegetation.addPointIndex(i):retained.addPointIndex(i))){showError(QStringLiteral("Not enough RAM for separation indices."));return;}
		if(!vegetation.size()||!retained.size()){showError(QStringLiteral("Preview does not contain both vegetation and retained points. Review parameters first."));return;}
		m_dock->setBusy(true);ccProgressDialog progress(false,m_dock);progress.setWindowModality(Qt::ApplicationModal);progress.setRange(0,0);progress.setMethodTitle(QStringLiteral("Copying vegetation and retained points — original preserved"));progress.show();QCoreApplication::processEvents();
		int warningA=0,warningB=0;std::unique_ptr<ccPointCloud> a(cloud->partialClone(&vegetation,&warningA,false)),b(cloud->partialClone(&retained,&warningB,false));
		progress.hide();m_dock->setBusy(false);if(!a||!b||warningA||warningB){showError(QStringLiteral("Cloud copies incomplete (memory limit); no partial result imported."));return;}
		a->setName(cloud->getName()+QStringLiteral(" — vegetation PROPOSAL"));b->setName(cloud->getName()+QStringLiteral(" — retained (ground + other + uncertain)"));
		a->setDisplay(cloud->getDisplay());b->setDisplay(cloud->getDisplay());
		auto* group=new ccHObject(QStringLiteral("ALiS vegetation separation — review required"));group->addChild(a.release());group->addChild(b.release());group->setDisplay(cloud->getDisplay());
		cloud->setEnabled(false);m_app->addToDB(group);refreshHost();showReady(QStringLiteral("Created two full-resolution clouds. Original preserved (hidden). Retained includes every uncertain point. Save the new group with CloudCompare Save."));
	}

	void WorkspaceController::suggestScales()
	{
		if (!requireMetricUnits(QStringLiteral("Scale suggestion"))) return;
		if (!m_cloudProfile.contains("nnMedian")) refreshCloudProfile(true, false);
		auto preset=captureProcessingPreset(); ProcessingPreset validated; QString explanation;
		if(!processingPresetFromJson(processingPresetToJson(preset),validated,explanation)
		   || !suggestProcessingPreset(preset,m_cloudProfile,explanation)){showError(explanation);return;}
		// Same scale policy as Terrain, but this button changes feature radii only.
		m_dock->showScaleSuggestion(preset.radii,explanation);
		showReady(QStringLiteral("Suggested %1 feature scales; ground settings unchanged.").arg(preset.radii.size()));
	}

	FeatureId WorkspaceController::featureIdFromUi(const QString& id, bool& valid) const
	{
		valid = true;
		if (id == QStringLiteral("density")) return FeatureId::Density3D;
		if (id == QStringLiteral("curvature")) return FeatureId::MeanCurvature;
		if (id == QStringLiteral("normal_orientation")) return FeatureId::NormalZ;
		if (id == QStringLiteral("local_relief")) return FeatureId::ZRange;
		for (const FeatureUiMapping& mapping : FeatureUiMappings)
		{
			if (id == QString::fromLatin1(mapping.stableId)) return mapping.feature;
		}
		valid = false; return FeatureId::NeighborCount;
	}

	QString WorkspaceController::featureUiId(FeatureId id) const
	{
		for (const FeatureUiMapping& mapping : FeatureUiMappings)
		{
			if (mapping.feature == id) return QString::fromLatin1(mapping.stableId);
		}
		return QString();
	}

	FeatureKey WorkspaceController::featureKey(const QString& id, double radiusMetres, bool& valid) const
	{
		FeatureKey key; key.feature = featureIdFromUi(id, valid); key.radius = radiusMetres; return key;
	}

	void WorkspaceController::computeFeatures(const QStringList& ids, const QVector<double>& radiiMetres)
	{
		if (m_featureComputationActive) return;
		if (!requireMetricUnits(QStringLiteral("Feature computation"))) return;
		if (!m_selectedCloud || ids.isEmpty() || radiiMetres.isEmpty()) { showError(QStringLiteral("Choose at least one feature and scale.")); return; }
		auto found = m_featureEngines.find(static_cast<quint64>(m_selectedCloud->getUniqueID())); if (found == m_featureEngines.end()) return;
		std::vector<FeatureRequest> requests;
		for (const QString& id : ids)
		{
			if (id == QStringLiteral("hag")) continue;
			bool valid = false; const FeatureId feature = featureIdFromUi(id, valid); if (!valid) continue;
			for (double radius : cleanPositiveRadii(radiiMetres)) { FeatureRequest request; request.feature = feature; request.radius = radius; requests.push_back(request); }
		}
		if (requests.empty()) { showError(QStringLiteral("Only HAG was selected; compute it in Terrain.")); return; }
		QScopedValueRollback<bool> computing(m_featureComputationActive, true);
		m_dock->setBusy(true); m_dock->setProgress(QStringLiteral("Multiscale features"), 0, 0, false);
		ccProgressDialog progress(true, m_dock);
		progress.setWindowModality(Qt::ApplicationModal);
		progress.setAutoClose(false);
		progress.setAutoReset(false);
		progress.setMinimumDuration(0);
		progress.setMethodTitle(QStringLiteral("ALiS — multiscale feature computation"));
		progress.setInfo(QStringLiteral("Building/reusing the octree and computing neighbourhood features…"));
		ComputeReport report; std::string error; ComputeOptions options;
		FeatureProgress workerProgress;
		connect(&progress, &QProgressDialog::canceled, &progress, [&]() { workerProgress.canceled.store(true); });
		progress.show();
		bool success = false;
		try
		{
			auto future = std::async(std::launch::async, [&]() { return found->second->compute(requests, report, error, &workerProgress, options); });
			QEventLoop loop;
			QTimer poll;
			connect(&poll, &QTimer::timeout, &loop, [&]() {
				QString title, info;
				{ std::lock_guard<std::mutex> lock(workerProgress.mutex); title = workerProgress.title; info = workerProgress.info; }
				progress.setWindowTitle(title);
				progress.setLabelText(workerProgress.canceled.load() ? QStringLiteral("Canceling — waiting for workers to finish safely…") : info);
				progress.setValue(qBound(0, static_cast<int>(workerProgress.percent.load()), 100));
				if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) loop.quit();
			});
			poll.start(50);
			loop.exec();
			success = future.get();
		}
		catch (const std::exception& exception) { error = exception.what(); }
		if (!success || workerProgress.canceled.load()) { progress.hide(); m_dock->clearProgress(); showError(workerProgress.canceled.load() ? QStringLiteral("Feature computation canceled. No new Scalar Fields were published.") : QString::fromStdString(error)); return; }
		QStringList published;QString publicationError;
		m_dock->setProgress(QStringLiteral("Publishing feature scalar fields"),0,0,false);
		progress.setMethodTitle(QStringLiteral("ALiS — publishing feature fields"));
		progress.setInfo(QStringLiteral("Writing computed values to CloudCompare Scalar Fields…"));
		progress.setRange(0, 0); progress.show(); QCoreApplication::processEvents();
		if(!publishComputedFeatures(requests,published,publicationError)){progress.hide();m_dock->clearProgress();refreshHost();updateModelFeatureChoices();syncDisplayControls();showError(publicationError);return;}
		progress.hide();
		m_dock->clearProgress();
		ALiSSession* s = session(); ProcessingRecord record; record.operation = QStringLiteral("Features.ComputeMultiscale");
		record.timestampUtc = QDateTime::currentDateTimeUtc(); record.sourceEntityUid = s->entityUid(); record.affectedPoints = s->pointCount();
		record.elapsedMilliseconds = static_cast<qint64>(report.elapsedSeconds * 1000.0); record.parameters.insert(QStringLiteral("requestedFields"), static_cast<int>(report.requestedFields));
		record.outputFields=published;
		record.parameters.insert(QStringLiteral("computedFields"), static_cast<int>(report.computedFields)); record.parameters.insert(QStringLiteral("octreeReused"), report.octreeReused); s->addProcessingRecord(record);
		if(!published.isEmpty()){m_selectedCloud->setCurrentDisplayedScalarField(m_selectedCloud->getScalarFieldIndexByName(published.first().toUtf8().constData()));m_selectedCloud->showColors(false);m_selectedCloud->showSF(true);}
		refreshHost();updateModelFeatureChoices();updateDockState();showReady(QStringLiteral("Published %1 Scalar Fields at %2 scales. Select any field in Display above. Computation: %3 s.").arg(published.size()).arg(report.processedScales).arg(report.elapsedSeconds,0,'f',2));
	}

	QString WorkspaceController::serializeModelFeatureKey(const FeatureKey& key) const
	{
		return QStringLiteral("%1@%2").arg(featureUiId(key.feature), QString::number(key.radius, 'g', 17));
	}

	bool WorkspaceController::parseModelFeatureKey(const QString& serialized, FeatureKey& key) const
	{
		const int separator = serialized.lastIndexOf(QLatin1Char('@'));
		if (separator <= 0) return false;
		bool radiusOk = false;
		const double radius = serialized.mid(separator + 1).toDouble(&radiusOk);
		bool featureOk = false;
		const FeatureId feature = featureIdFromUi(serialized.left(separator), featureOk);
		if (!radiusOk || !featureOk || !std::isfinite(radius) || radius <= 0.0) return false;
		key.feature = feature;
		key.radius = radius;
		key.sourceScalarField.clear();
		return true;
	}

	void WorkspaceController::updateModelFeatureChoices()
	{
		if (!m_selectedCloud)
		{
			m_dock->setModelFeatureChoices(QStringList(), QStringList());
			m_dock->setBootstrapFeatureChoices(QStringList(), QStringList(), QStringList());
			return;
		}
		auto found = m_featureEngines.find(static_cast<quint64>(m_selectedCloud->getUniqueID()));
		QStringList keys, names;
		if (found != m_featureEngines.end()) for (const FeatureKey& key : found->second->cachedKeys())
		{
			if (featureUiId(key.feature).isEmpty() || !key.sourceScalarField.empty()) continue;
			keys << serializeModelFeatureKey(key);
			names << QStringLiteral("%1 — radius %2 m")
				.arg(QString::fromLatin1(featureName(key.feature)), QString::number(key.radius, 'g', 8));
		}
		if (m_selectedCloud->getScalarFieldIndexByName(field::HeightAboveGround) >= 0)
		{
			keys << QStringLiteral("hag@0");
			names << QStringLiteral("Height Above Ground — qAL_HAG");
		}
		const auto saved=QJsonDocument::fromJson(featureFieldMetadata(*m_selectedCloud)).object();
		for(auto i=saved.begin();i!=saved.end();++i){FeatureKey key;if(keys.contains(i.key())||!parseModelFeatureKey(i.key(),key))continue;int sf=m_selectedCloud->getScalarFieldIndexByName(i.value().toString().toUtf8().constData());if(sf>=0&&m_selectedCloud->getScalarField(sf)->currentSize()==m_selectedCloud->size()){keys<<i.key();names<<i.value().toString()+QStringLiteral(" — saved SF");}}
		m_dock->setModelFeatureChoices(keys, names);
		// Clustering may use the cloud's existing attributes directly. Keep the
		// supervised schema/defaults intact so existing trained models still match.
		QStringList excluded;
		for (unsigned i = 0; i < m_selectedCloud->getNumberOfScalarFields(); ++i)
		{
			const auto* scalar = m_selectedCloud->getScalarField(i);
			if (!scalar || scalar->currentSize() != m_selectedCloud->size()) continue;
			const QString name = QString::fromUtf8(scalar->getName());
			bool alreadyListed = name == QString::fromUtf8(field::HeightAboveGround);
			for (auto entry = saved.begin(); entry != saved.end() && !alreadyListed; ++entry)
				alreadyListed = entry.value().toString() == name && keys.contains(entry.key());
			if (alreadyListed) continue;
			const QString key = scalarInputKey(name);
			keys << key;
			names << QStringLiteral("%1 — existing scalar field").arg(name);
			if (potentiallyLabelDerivedField(name)) excluded << key;
		}
		if (m_selectedCloud->hasColors())
		{
			for (const QString& channel : {QStringLiteral("red"), QStringLiteral("green"), QStringLiteral("blue")})
			{
				keys << QStringLiteral("rgb:%1").arg(channel);
				names << QStringLiteral("RGB %1 — existing colour channel (0–255)").arg(channel);
			}
		}
		m_dock->setBootstrapFeatureChoices(keys, names, excluded);
	}

	QString WorkspaceController::createMlJobDirectory(const QString& prefix, QString& error) const
	{
		const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
			.filePath(QStringLiteral("ALiS/ml"));
		if (!QDir().mkpath(root))
		{
			error = QStringLiteral("Cannot create the ML workspace: %1").arg(root);
			return QString();
		}
		const quint64 uid = m_selectedCloud ? static_cast<quint64>(m_selectedCloud->getUniqueID()) : 0;
		const QString name = QStringLiteral("%1_%2_%3")
			.arg(prefix).arg(uid).arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
		const QString path = QDir(root).filePath(name);
		if (!QDir().mkpath(path))
		{
			error = QStringLiteral("Cannot create the ML job directory: %1").arg(path);
			return QString();
		}
		return path;
	}

	bool WorkspaceController::writeMlDataset(const QStringList& serializedKeys,
	                                        const QString& outputDirectory,
	                                        QString& error,
	                                        bool requireMetricCoordinates)
	{
		if (!m_selectedCloud || (requireMetricCoordinates && !requireMetricUnits(QStringLiteral("ML dataset export"))))
		{
			error = QStringLiteral("A metric point-cloud Session is required.");
			return false;
		}
		if (serializedKeys.isEmpty())
		{
			error = QStringLiteral("Select at least one existing scalar field, RGB channel or computed feature.");
			return false;
		}
		auto engineIt = m_featureEngines.find(static_cast<quint64>(m_selectedCloud->getUniqueID()));
		std::vector<FeatureKey> keys;
		std::vector<const std::vector<ScalarType>*> columns;
		std::vector<CloudAttributeInput> attributes;
		keys.reserve(serializedKeys.size()); columns.reserve(serializedKeys.size()); attributes.reserve(serializedKeys.size());
		std::set<QString> uniqueKeys;
		for (const QString& serialized : serializedKeys)
		{
			if (!uniqueKeys.insert(serialized).second) { error = QStringLiteral("Duplicate input field: %1").arg(serialized); return false; }
			if (serialized.startsWith(QStringLiteral("sf:")) || serialized.startsWith(QStringLiteral("rgb:")) || serialized.startsWith(QStringLiteral("xyz:")))
			{
				CloudAttributeInput input;
				if (!resolveCloudAttribute(*m_selectedCloud, serialized, input, error)) return false;
				keys.push_back(FeatureKey()); columns.push_back(nullptr); attributes.push_back(input);
				continue;
			}
			if (serialized == QStringLiteral("hag@0"))
			{
				CloudAttributeInput input;
				if (!resolveCloudAttribute(*m_selectedCloud, scalarInputKey(QString::fromUtf8(field::HeightAboveGround)), input, error)) return false;
				keys.push_back(FeatureKey()); columns.push_back(nullptr);
				attributes.push_back(input);
				continue;
			}
			FeatureKey key;
			if (!parseModelFeatureKey(serialized, key))
			{
				error = QStringLiteral("Invalid cached feature key: %1").arg(serialized);
				return false;
			}
			const std::vector<ScalarType>* values = engineIt == m_featureEngines.end() ? nullptr : engineIt->second->cachedValues(key);
			if (!values || values->size() != static_cast<std::size_t>(m_selectedCloud->size()))
			{
				const auto saved=QJsonDocument::fromJson(featureFieldMetadata(*m_selectedCloud)).object();
				const QString name=saved.value(serialized).toString();const int sf=name.isEmpty()?-1:m_selectedCloud->getScalarFieldIndexByName(name.toUtf8().constData());
				if(sf<0||m_selectedCloud->getScalarField(sf)->currentSize()!=m_selectedCloud->size()){error=QStringLiteral("Feature unavailable in cache and saved SF: %1").arg(serialized);return false;}
				CloudAttributeInput input; input.name = name; input.scalar = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(sf));
				keys.push_back(key);columns.push_back(nullptr);attributes.push_back(input);continue;
			}
			keys.push_back(key); columns.push_back(values); attributes.push_back(CloudAttributeInput());
		}
		if (!QDir().mkpath(outputDirectory))
		{
			error = QStringLiteral("Cannot create dataset directory: %1").arg(outputDirectory);
			return false;
		}

		const quint64 rows = m_selectedCloud->size();
		m_dock->setBusy(true);
		m_dock->setProgress(QStringLiteral("Export ML dataset"), 0, 100, false);

		QSaveFile featureFile(QDir(outputDirectory).filePath(QStringLiteral("features.f32")));
		if (!featureFile.open(QIODevice::WriteOnly)) { error = featureFile.errorString(); showError(error); return false; }
		QDataStream featureStream(&featureFile);
		featureStream.setByteOrder(QDataStream::LittleEndian);
		featureStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
		for (quint64 row = 0; row < rows; ++row)
		{
			for (std::size_t column = 0; column < columns.size(); ++column)
			{
				const float value = columns[column]
					? static_cast<float>((*columns[column])[static_cast<std::size_t>(row)])
					: attributes[column].value(*m_selectedCloud, static_cast<unsigned>(row));
				featureStream << (std::isfinite(value) ? value : std::numeric_limits<float>::quiet_NaN());
			}
			if ((row & 0xffffu) == 0u) m_dock->setProgress(QStringLiteral("Export features"), static_cast<int>((row * 55) / std::max<quint64>(1, rows)), 100, false);
		}
		if (featureStream.status() != QDataStream::Ok || !featureFile.commit())
		{
			error = QStringLiteral("Failed to write features.f32: %1").arg(featureFile.errorString()); showError(error); return false;
		}

		QSaveFile xyFile(QDir(outputDirectory).filePath(QStringLiteral("xy.f64")));
		QSaveFile zFile(QDir(outputDirectory).filePath(QStringLiteral("z.f64")));
		if (!xyFile.open(QIODevice::WriteOnly) || !zFile.open(QIODevice::WriteOnly)) { error = QStringLiteral("Cannot create XY/Z geometry arrays."); showError(error); return false; }
		QDataStream xyStream(&xyFile); xyStream.setByteOrder(QDataStream::LittleEndian); xyStream.setFloatingPointPrecision(QDataStream::DoublePrecision);
		QDataStream zStream(&zFile); zStream.setByteOrder(QDataStream::LittleEndian); zStream.setFloatingPointPrecision(QDataStream::DoublePrecision);
		double zMean = 0.0, zM2 = 0.0; quint64 finiteZ = 0;
		for (quint64 row = 0; row < rows; ++row)
		{
			const CCVector3d global = m_selectedCloud->toGlobal3d(*m_selectedCloud->getPoint(static_cast<unsigned>(row)));
			xyStream << global.x << global.y;
			zStream << global.z;
			if (std::isfinite(global.z))
			{
				++finiteZ; const double delta = global.z - zMean; zMean += delta / finiteZ; zM2 += delta * (global.z - zMean);
			}
		}
		if (xyStream.status() != QDataStream::Ok || zStream.status() != QDataStream::Ok || !xyFile.commit() || !zFile.commit())
		{ error = QStringLiteral("Failed to write XY/Z geometry arrays."); showError(error); return false; }
		m_dock->setProgress(QStringLiteral("Export labels and IDs"), 75, 100, false);

		const int classIndex = m_selectedCloud->getScalarFieldIndexByName(field::WorkingAsprs);
		const int trustedIndex = m_selectedCloud->getScalarFieldIndexByName(field::AsprsTrainingClass);
		if (classIndex < 0 || trustedIndex < 0) { error = QStringLiteral("ASPRS Working or manually confirmed ASPRS classes are unavailable."); showError(error); return false; }
		const ccScalarField* classes = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(classIndex));
		const ccScalarField* trusted = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(trustedIndex));
		const int returnIndex = m_selectedCloud->getScalarFieldIndexByName("Return Number");
		const int echoCountIndex = m_selectedCloud->getScalarFieldIndexByName("Number Of Returns");
		const ccScalarField* returnNumbers = returnIndex >= 0 ? static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(returnIndex)) : nullptr;
		const ccScalarField* echoCounts = echoCountIndex >= 0 ? static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(echoCountIndex)) : nullptr;
		quint64 returnHistogram[16] = {}; quint64 echoCountHistogram[16] = {};
		QSaveFile labelFile(QDir(outputDirectory).filePath(QStringLiteral("labels.u8")));
		QSaveFile trustedFile(QDir(outputDirectory).filePath(QStringLiteral("trusted.u8")));
		QSaveFile idsFile(QDir(outputDirectory).filePath(QStringLiteral("point_ids.u64")));
		if (!labelFile.open(QIODevice::WriteOnly) || !trustedFile.open(QIODevice::WriteOnly) || !idsFile.open(QIODevice::WriteOnly))
		{
			error = QStringLiteral("Cannot create label/trusted/point ID files."); showError(error); return false;
		}
		QDataStream idStream(&idsFile); idStream.setByteOrder(QDataStream::LittleEndian);
		QByteArray labelChunk; QByteArray trustedChunk;
		labelChunk.reserve(65536); trustedChunk.reserve(65536);
		const bool testOnlyCloud = m_selectedCloud->getMetaData(QStringLiteral("ALiS.TestOnly")).toBool()
			|| m_selectedCloud->getMetaData(QStringLiteral("qArchaeoLiDAR.TestOnly")).toBool();
		for (quint64 row = 0; row < rows; ++row)
		{
			const double rawClass = static_cast<double>(classes->getValue(static_cast<unsigned>(row)));
			const bool trustedPoint = !testOnlyCloud
				&& isTrustedAsprsLabel(static_cast<float>(rawClass), trusted->getValue(static_cast<unsigned>(row)));
			if (trustedPoint && (!std::isfinite(rawClass) || rawClass < 0.0 || rawClass > 255.0))
			{
				error = QStringLiteral("Trusted ASPRS class is invalid at point %1.").arg(row); showError(error); return false;
			}
			labelChunk.append(static_cast<char>(std::isfinite(rawClass) && rawClass >= 0.0 && rawClass <= 255.0 ? std::lround(rawClass) : 0));
			trustedChunk.append(static_cast<char>(trustedPoint ? 1 : 0));
			if (returnNumbers)
			{
				const int value = static_cast<int>(std::lround(returnNumbers->getValue(static_cast<unsigned>(row))));
				if (value >= 0 && value < 16) ++returnHistogram[value];
			}
			if (echoCounts)
			{
				const int value = static_cast<int>(std::lround(echoCounts->getValue(static_cast<unsigned>(row))));
				if (value >= 0 && value < 16) ++echoCountHistogram[value];
			}
			idStream << static_cast<quint64>(row);
			if (labelChunk.size() >= 65536)
			{
				if (labelFile.write(labelChunk) != labelChunk.size() || trustedFile.write(trustedChunk) != trustedChunk.size())
				{
					error = QStringLiteral("Failed while writing compact labels."); showError(error); return false;
				}
				labelChunk.clear(); trustedChunk.clear();
			}
		}
		if ((!labelChunk.isEmpty() && (labelFile.write(labelChunk) != labelChunk.size() || trustedFile.write(trustedChunk) != trustedChunk.size()))
		    || idStream.status() != QDataStream::Ok || !labelFile.commit() || !trustedFile.commit() || !idsFile.commit())
		{
			error = QStringLiteral("Failed to finalize compact labels and point IDs."); showError(error); return false;
		}

		QJsonObject manifest;
		manifest.insert(QStringLiteral("schema"), QStringLiteral("qal-ml-dataset/1.0"));
		manifest.insert(QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
		manifest.insert(QStringLiteral("target_domain"), QStringLiteral("asprs_working"));
		manifest.insert(QStringLiteral("point_count"), static_cast<double>(rows));
		manifest.insert(QStringLiteral("feature_count"), static_cast<int>(keys.size()));
		manifest.insert(QStringLiteral("feature_names"), QJsonArray::fromStringList(serializedKeys));
		manifest.insert(QStringLiteral("labels_trusted"), true);
		manifest.insert(QStringLiteral("label_source"), QStringLiteral("qAL_ASPRS_TrainingClass.matchesWorking"));
		// Rich aliases are retained for human inspection; canonical keys above drive the worker.
		manifest.insert(QStringLiteral("rows"), static_cast<double>(rows));
		manifest.insert(QStringLiteral("columns"), static_cast<int>(keys.size()));
		QJsonObject source;
		source.insert(QStringLiteral("entity_uid"), static_cast<double>(m_selectedCloud->getUniqueID()));
		source.insert(QStringLiteral("name"), m_selectedCloud->getName());
		source.insert(QStringLiteral("point_count"), static_cast<double>(rows));
		source.insert(QStringLiteral("coordinate_units"), m_metricUnitsConfirmed ? QStringLiteral("metre") : QStringLiteral("unconfirmed"));
		source.insert(QStringLiteral("global_scale"), m_selectedCloud->getGlobalScale());
		source.insert(QStringLiteral("estimated_spacing_m"), m_estimatedSpacingMetres > 0.0 ? QJsonValue(m_estimatedSpacingMetres) : QJsonValue(QJsonValue::Null));
		source.insert(QStringLiteral("estimated_density_points_m2"), m_estimatedDensityPerSquareMetre > 0.0 ? QJsonValue(m_estimatedDensityPerSquareMetre) : QJsonValue(QJsonValue::Null));
		source.insert(QStringLiteral("z_mean_m"), finiteZ ? QJsonValue(zMean) : QJsonValue(QJsonValue::Null));
		source.insert(QStringLiteral("z_stddev_m"), finiteZ > 1 ? QJsonValue(std::sqrt(zM2 / (finiteZ - 1))) : QJsonValue(QJsonValue::Null));
		CCVector3d globalMinimum, globalMaximum;
		if (m_selectedCloud->getOwnGlobalBB(globalMinimum, globalMaximum))
		{
			QJsonArray minimum; minimum.append(globalMinimum.x); minimum.append(globalMinimum.y); minimum.append(globalMinimum.z);
			QJsonArray maximum; maximum.append(globalMaximum.x); maximum.append(globalMaximum.y); maximum.append(globalMaximum.z);
			source.insert(QStringLiteral("bounds_min_xyz"), minimum); source.insert(QStringLiteral("bounds_max_xyz"), maximum);
			const double area = std::max(0.0, globalMaximum.x - globalMinimum.x) * std::max(0.0, globalMaximum.y - globalMinimum.y);
			source.insert(QStringLiteral("bounding_box_area_m2"), area);
			source.insert(QStringLiteral("bounding_box_density_points_m2"), area > 0.0 ? QJsonValue(rows / area) : QJsonValue(QJsonValue::Null));
		}
		QJsonObject lidarDimensions;
		for (const char* name : {"Intensity", "Return Number", "Number Of Returns", "Scan Angle", "Scan Angle Rank", "Point Source ID", "Gps Time"})
		{
			const int index = m_selectedCloud->getScalarFieldIndexByName(name);
			QJsonObject descriptor; descriptor.insert(QStringLiteral("available"), index >= 0);
			if (index >= 0)
			{
				const ccScalarField* scalar = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(index));
				descriptor.insert(QStringLiteral("minimum"), static_cast<double>(scalar->getMin()));
				descriptor.insert(QStringLiteral("maximum"), static_cast<double>(scalar->getMax()));
			}
			lidarDimensions.insert(QString::fromLatin1(name), descriptor);
		}
		QJsonObject returnsJson, echoCountJson;
		for (int value = 0; value < 16; ++value)
		{
			if (returnHistogram[value]) returnsJson.insert(QString::number(value), static_cast<double>(returnHistogram[value]));
			if (echoCountHistogram[value]) echoCountJson.insert(QString::number(value), static_cast<double>(echoCountHistogram[value]));
		}
		lidarDimensions.insert(QStringLiteral("return_number_histogram"), returnsJson);
		lidarDimensions.insert(QStringLiteral("number_of_returns_histogram"), echoCountJson);
		source.insert(QStringLiteral("lidar_dimensions"), lidarDimensions);
		auto currentProfile=cloudProfile(*m_selectedCloud,m_metricUnitsConfirmed);
		for(auto i=m_cloudProfile.begin();i!=m_cloudProfile.end();++i)if(i.key().startsWith("nn"))currentProfile.insert(i.key(),i.value());
		source.insert(QStringLiteral("cloud_profile"), currentProfile);
		manifest.insert(QStringLiteral("processing_preset"), processingPresetToJson(captureProcessingPreset()));
		QJsonArray shift; shift.append(m_selectedCloud->getGlobalShift().x); shift.append(m_selectedCloud->getGlobalShift().y); shift.append(m_selectedCloud->getGlobalShift().z);
		source.insert(QStringLiteral("cloudcompare_global_shift"), shift);
		manifest.insert(QStringLiteral("source"), source);
		QJsonArray featureArray;
		for (int column = 0; column < static_cast<int>(keys.size()); ++column)
		{
			QJsonObject descriptor;
			descriptor.insert(QStringLiteral("column"), column);
			descriptor.insert(QStringLiteral("key"), serializedKeys.at(column));
			const bool hag = serializedKeys.at(column) == QStringLiteral("hag@0");
			const bool existing = serializedKeys.at(column).startsWith(QStringLiteral("sf:"));
			const bool rgb = serializedKeys.at(column).startsWith(QStringLiteral("rgb:"));
			if (existing || rgb)
			{
				descriptor.insert(QStringLiteral("feature_id"), rgb ? QStringLiteral("rgb_channel") : QStringLiteral("existing_scalar_field"));
				descriptor.insert(QStringLiteral("feature_name"), attributes[static_cast<std::size_t>(column)].name);
				descriptor.insert(QStringLiteral("source_kind"), rgb ? QStringLiteral("cloudcompare_rgb_8bit") : QStringLiteral("cloudcompare_scalar_field"));
				descriptor.insert(QStringLiteral("potential_label_leakage"), existing && potentiallyLabelDerivedField(attributes[static_cast<std::size_t>(column)].name));
				if (rgb) { descriptor.insert(QStringLiteral("minimum"), 0); descriptor.insert(QStringLiteral("maximum"), 255); }
			}
			else
			{
				descriptor.insert(QStringLiteral("feature_id"), hag ? QStringLiteral("hag") : featureUiId(keys[static_cast<std::size_t>(column)].feature));
				descriptor.insert(QStringLiteral("feature_name"), hag ? QStringLiteral("HeightAboveGround") : QString::fromLatin1(featureName(keys[static_cast<std::size_t>(column)].feature)));
				if (!hag) descriptor.insert(QStringLiteral("radius_m"), keys[static_cast<std::size_t>(column)].radius);
			}
			descriptor.insert(QStringLiteral("nonfinite_values"), QStringLiteral("NaN; source point order retained"));
			featureArray.append(descriptor);
		}
		manifest.insert(QStringLiteral("features"), featureArray);
		QJsonObject files;
		files.insert(QStringLiteral("features"), QJsonObject{{QStringLiteral("path"), QStringLiteral("features.f32")}, {QStringLiteral("dtype"), QStringLiteral("<f4")}, {QStringLiteral("order"), QStringLiteral("C")}});
		files.insert(QStringLiteral("xy"), QJsonObject{{QStringLiteral("path"), QStringLiteral("xy.f64")}, {QStringLiteral("dtype"), QStringLiteral("<f8")}, {QStringLiteral("columns"), 2}});
		files.insert(QStringLiteral("z"), QJsonObject{{QStringLiteral("path"), QStringLiteral("z.f64")}, {QStringLiteral("dtype"), QStringLiteral("<f8")}});
		files.insert(QStringLiteral("labels"), QJsonObject{{QStringLiteral("path"), QStringLiteral("labels.u8")}, {QStringLiteral("dtype"), QStringLiteral("u1")}});
		files.insert(QStringLiteral("trusted"), QJsonObject{{QStringLiteral("path"), QStringLiteral("trusted.u8")}, {QStringLiteral("dtype"), QStringLiteral("u1")}, {QStringLiteral("manual_trusted_value"), 1}});
		files.insert(QStringLiteral("point_ids"), QJsonObject{{QStringLiteral("path"), QStringLiteral("point_ids.u64")}, {QStringLiteral("dtype"), QStringLiteral("<u8")}});
		manifest.insert(QStringLiteral("files"), files);
		manifest.insert(QStringLiteral("trusted_training_points"), static_cast<double>(session() ? session()->trustedTrainingCount() : 0));
		const QJsonObject provenance{{QStringLiteral("producer"), QStringLiteral("ALiS CloudCompare plugin")},
			{QStringLiteral("xyz_exported_as_features"), false}, {QStringLiteral("alignment"), QStringLiteral("source point-index order")}};
		manifest.insert(QStringLiteral("provenance"), provenance);
		manifest.insert(QStringLiteral("metadata"), QJsonObject{{QStringLiteral("source_cloud_uid"), static_cast<double>(m_selectedCloud->getUniqueID())},
			{QStringLiteral("source_revision"), static_cast<double>(session() ? session()->sourceRevision() : 0)},
			{QStringLiteral("source_cloud"), source},
			{QStringLiteral("input_fields"), featureArray},
			{QStringLiteral("trusted_mask_file"), QStringLiteral("trusted.u8")},
			{QStringLiteral("provenance"), provenance}});
		QSaveFile manifestFile(QDir(outputDirectory).filePath(QStringLiteral("manifest.json")));
		if (!manifestFile.open(QIODevice::WriteOnly)
		    || manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0
		    || !manifestFile.commit())
		{
			error = QStringLiteral("Failed to write dataset manifest: %1").arg(manifestFile.errorString()); showError(error); return false;
		}
		m_dock->setProgress(QStringLiteral("Export ML dataset"), 100, 100, false);
		m_dock->clearProgress();
		m_dock->setBusy(false);
		return true;
	}

	void WorkspaceController::exportModelDataset(const QStringList& featureKeys, const QString& repositoryPath)
	{
		if (!requireMetricUnits(QStringLiteral("ML dataset export"))) return;
		QString parent;
		if (repositoryPath.trimmed().isEmpty())
		{
			parent = QFileDialog::getExistingDirectory(m_dock, QStringLiteral("Choose parent folder for ALiS dataset"));
			if (parent.isEmpty()) return;
		}
		else
		{
			const QString repository = QDir::fromNativeSeparators(repositoryPath.trimmed());
			parent = QDir(repository).filePath(QStringLiteral("datasets"));
			if (!QDir().mkpath(parent)) { showError(QStringLiteral("Cannot create repository datasets folder: %1").arg(parent)); return; }
			const QString manifestPath = QDir(repository).filePath(QStringLiteral("repository.json"));
			if (!QFileInfo::exists(manifestPath))
			{
				QSaveFile repositoryFile(manifestPath);
				const QJsonObject manifest{{QStringLiteral("schema"), QStringLiteral("qal-model-repository/1.0")},
					{QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
					{QStringLiteral("description"), QStringLiteral("Local ALiS datasets, models, runs and reports")}};
				if (!repositoryFile.open(QIODevice::WriteOnly)
				    || repositoryFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0
				    || !repositoryFile.commit())
				{
					showError(QStringLiteral("Cannot initialize repository: %1").arg(repositoryFile.errorString())); return;
				}
			}
		}
		QString error;
		const quint64 uid = m_selectedCloud ? static_cast<quint64>(m_selectedCloud->getUniqueID()) : 0;
		const QString output = QDir(parent).filePath(QStringLiteral("qal_dataset_%1_%2").arg(uid)
			.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
		if (!writeMlDataset(featureKeys, output, error)) { if (!error.isEmpty()) showError(error); return; }
		m_mlDatasetPath = output; m_mlDatasetFeatureKeys = featureKeys;
		m_mlDatasetRevision = session() ? session()->sourceRevision() : std::numeric_limits<quint64>::max();
		m_mlDatasetCloudUid = m_selectedCloud ? static_cast<quint64>(m_selectedCloud->getUniqueID()) : 0;
		m_mlSummary = QStringLiteral("Dataset schema qal-ml-dataset/1.0 exported.");
		updateDockState();
		showReady(QStringLiteral("Compact ML dataset exported: %1").arg(QDir::toNativeSeparators(output)));
	}

	void WorkspaceController::trainModels(const QStringList& featureKeys,
	                                     const QString& pythonExecutable,
	                                     const QString& workerScript,
	                                     const QString& modelPath,
	                                     const QString& repositoryPath,
	                                     const QStringList& modelSpecifications,
	                                     double blockSizeMetres,
	                                     double testFraction,
	                                     int seed,
	                                     const QString& validationMode)
	{
		if (modelSpecifications.isEmpty()) { showError(QStringLiteral("Select at least one classifier card.")); return; }
		if (modelSpecifications.size() > 1 && repositoryPath.trimmed().isEmpty())
		{
			showError(QStringLiteral("Choose a model repository before comparing multiple classifiers.")); return;
		}
		m_pendingModelRuns.clear();
		m_mlReportPaths.clear();
		for (const QString& specification : modelSpecifications)
		{
			const QStringList fields = specification.split(QLatin1Char('|'));
			if ((fields.size() != 13 && fields.size() != 15) || fields[0].trimmed().isEmpty())
			{
				m_pendingModelRuns.clear(); showError(QStringLiteral("Invalid classifier-card settings.")); return;
			}
			bool primaryOk = false, batchOk = false, learningOk = false, depthOk = false, leafOk = false, subsampleOk = false, columnSampleOk = false;
			const int primary = fields[2].toInt(&primaryOk);
			const int batch = fields[3].toInt(&batchOk);
			const double learning = fields[4].toDouble(&learningOk);
			const int maxDepth = fields[5].toInt(&depthOk); const int minimumLeaf = fields[6].toInt(&leafOk);
			const double subsample = fields[10].toDouble(&subsampleOk); const double columnSample = fields[11].toDouble(&columnSampleOk);
			if (!primaryOk || !batchOk || !learningOk || !depthOk || !leafOk || !subsampleOk || !columnSampleOk)
			{
				m_pendingModelRuns.clear(); showError(QStringLiteral("A classifier card contains invalid numeric settings.")); return;
			}
			PendingModelRun run;
			run.featureKeys = featureKeys; run.pythonExecutable = pythonExecutable; run.workerScript = workerScript;
			run.modelPath = modelPath; run.repositoryPath = repositoryPath; run.classifierId = fields[0]; run.device = fields[1];
			if (run.classifierId == QStringLiteral("pointnet"))
			{
				run.featureKeys = QStringList{QStringLiteral("xyz:x"), QStringLiteral("xyz:y"), QStringLiteral("xyz:z")};
				if (fields.size() == 15) { run.pointnetRadius = fields[13].toDouble(); run.pointnetNeighbors = fields[14].toInt(); }
			}
			if (run.classifierId == QStringLiteral("multiscale_mlp") || run.classifierId == QStringLiteral("pointnet")) run.epochs = primary;
			else run.trees = primary;
			run.batchSize = batch; run.learningRate = learning; run.blockSizeMetres = blockSizeMetres;
			run.maxDepth = maxDepth; run.minimumLeaf = minimumLeaf; run.maxFeatures = fields[7]; run.classWeight = fields[8];
			run.criterion = fields[9]; run.subsample = subsample; run.columnSample = columnSample;
			run.imputeMissing = fields[12] == QStringLiteral("1");
			run.testFraction = testFraction; run.seed = seed; run.validationMode = validationMode;
			m_pendingModelRuns.push_back(run);
		}
		double commonBuffer = 0.0;
		for (const PendingModelRun& run : m_pendingModelRuns)
			if (run.classifierId == QStringLiteral("pointnet")) commonBuffer = std::max(commonBuffer, 2.0 * run.pointnetRadius + 0.001);
		for (PendingModelRun& run : m_pendingModelRuns) run.validationBuffer = commonBuffer;
		m_trainingBatchTotal = m_pendingModelRuns.size(); m_trainingBatchCompleted = 0;
		startNextTrainingRun();
	}

	void WorkspaceController::startNextTrainingRun()
	{
		if (m_pendingModelRuns.isEmpty())
		{
			if (m_trainingBatchTotal > 1)
			{
				m_mlSummary = QStringLiteral("Comparison batch complete: %1/%2 models trained. Reports are saved in each model folder.")
					.arg(m_trainingBatchCompleted).arg(m_trainingBatchTotal);
				updateDockState(); showReady(m_mlSummary);
				if (!m_mlReportPaths.isEmpty()) QTimer::singleShot(0, this, [this]() { showModelResults(); });
			}
			return;
		}
		const PendingModelRun run = m_pendingModelRuns.takeFirst();
		m_dock->setStatus(QStringLiteral("Model %1/%2: preparing %3…")
			.arg(m_trainingBatchCompleted + 1).arg(m_trainingBatchTotal).arg(run.classifierId), WorkspaceDock::StatusTone::Neutral);
		trainModel(run.featureKeys, run.pythonExecutable, run.workerScript, run.modelPath, run.repositoryPath,
			run.classifierId, run.device, run.trees, run.epochs, run.batchSize, run.learningRate,
			run.maxDepth, run.minimumLeaf, run.maxFeatures, run.classWeight, run.criterion, run.subsample, run.columnSample,
			run.imputeMissing,
			run.blockSizeMetres, run.testFraction, run.seed, run.validationMode, run.pointnetRadius, run.pointnetNeighbors, run.validationBuffer);
		if (m_mlProcess->state() == QProcess::NotRunning) m_pendingModelRuns.clear();
	}

	void WorkspaceController::trainModel(const QStringList& featureKeys,
	                                    const QString& pythonExecutable,
	                                    const QString& workerScript,
	                                    const QString& modelPath,
	                                    const QString& repositoryPath,
	                                    const QString& classifierId,
	                                    const QString& device,
	                                    int trees,
	                                    int epochs,
	                                    int batchSize,
	                                    double learningRate,
	                                    int maxDepth,
	                                    int minimumLeaf,
	                                    const QString& maxFeatures,
	                                    const QString& classWeight,
	                                    const QString& criterion,
	                                    double subsample,
	                                    double columnSample,
	                                    bool imputeMissing,
	                                    double blockSizeMetres,
	                                    double testFraction,
	                                    int seed,
	                                    const QString& validationMode, double pointnetRadius, int pointnetNeighbors, double validationBuffer)
	{
		ccPointCloud* trainingCloud = m_selectedCloud;
		const auto designatedTraining = m_sessions.find(m_trainingCloudUid);
		if (designatedTraining != m_sessions.end()) trainingCloud = &designatedTraining->second->cloud();
		if (!trainingCloud) { showError(QStringLiteral("Select a certified training set first.")); return; }
		if (trainingCloud != m_selectedCloud) selectCloud(trainingCloud);
		ALiSSession* s = session();
		if (!s || !requireMetricUnits(QStringLiteral("classifier training"))) return;
		if (s->trustedTrainingCount() < 2) { showError(QStringLiteral("At least two Manual/Trusted ASPRS points are required.")); return; }
		const ccScalarField* classes = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(m_selectedCloud->getScalarFieldIndexByName(field::WorkingAsprs)));
		const ccScalarField* trusted = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(m_selectedCloud->getScalarFieldIndexByName(field::AsprsTrainingClass)));
		std::set<int> uniqueClasses;
		for (unsigned i = 0; i < m_selectedCloud->size(); ++i)
		{
			if (isTrustedAsprsLabel(classes->getValue(i), trusted->getValue(i))) uniqueClasses.insert(static_cast<int>(classes->getValue(i)));
		}
		if (uniqueClasses.size() < 2) { showError(QStringLiteral("Training needs Manual/Trusted examples from at least two ASPRS classes.")); return; }
		if (m_trainingCloudUid == 0) m_trainingCloudUid = static_cast<quint64>(trainingCloud->getUniqueID());
		QString error;
		const QString repository = QDir::fromNativeSeparators(repositoryPath.trimmed());
		if (m_mlDatasetPath.isEmpty() || m_mlDatasetFeatureKeys != featureKeys || m_mlDatasetRevision != s->sourceRevision()
			|| m_mlDatasetCloudUid != static_cast<quint64>(m_selectedCloud->getUniqueID()))
		{
			m_mlDatasetPath = repository.isEmpty()
				? createMlJobDirectory(QStringLiteral("dataset"), error)
				: QDir(repository).filePath(QStringLiteral("datasets/qal_dataset_%1_%2").arg(m_selectedCloud->getUniqueID())
					.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
			if (m_mlDatasetPath.isEmpty() || !writeMlDataset(featureKeys, m_mlDatasetPath, error)) { if (!error.isEmpty()) showError(error); return; }
			m_mlDatasetFeatureKeys = featureKeys;
			m_mlDatasetRevision = s->sourceRevision();
			m_mlDatasetCloudUid = static_cast<quint64>(m_selectedCloud->getUniqueID());
		}
		if (!repository.isEmpty())
		{
			const QString safeClassifier = classifierId.isEmpty() ? QStringLiteral("random_forest") : classifierId;
			const QString modelDirectory = QDir(repository).filePath(QStringLiteral("models/%1/%2")
				.arg(safeClassifier, QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
			m_mlModelPath = QDir(modelDirectory).filePath(QStringLiteral("model.joblib"));
		}
		else
		{
			m_mlModelPath = QDir::fromNativeSeparators(modelPath);
		}
		if (m_mlModelPath.isEmpty()) { showError(QStringLiteral("Choose a repository or model output path.")); return; }
		if (!QDir().mkpath(QFileInfo(m_mlModelPath).absolutePath())) { showError(QStringLiteral("Cannot create the model directory.")); return; }
		m_mlExternalTestDatasetPath.clear();
		if (validationMode == QStringLiteral("external_cloud"))
		{
			const auto designatedTest = m_sessions.find(m_testCloudUid);
			if (designatedTest == m_sessions.end()) { showError(QStringLiteral("Select an independent certified test cloud.")); return; }
			ccPointCloud* testCloud = &designatedTest->second->cloud();
			if (testCloud == trainingCloud) { showError(QStringLiteral("External test cloud must be different from the training cloud.")); return; }
			if (designatedTest->second->trustedTrainingCount() < 1) { showError(QStringLiteral("External test cloud has no trusted class labels.")); return; }
			m_selectedCloud = testCloud;
			m_mlExternalTestDatasetPath = repository.isEmpty()
				? createMlJobDirectory(QStringLiteral("external_test_dataset"), error)
				: QDir(repository).filePath(QStringLiteral("datasets/qal_test_%1_%2").arg(testCloud->getUniqueID())
					.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
			const bool testWritten = !m_mlExternalTestDatasetPath.isEmpty()
				&& writeMlDataset(featureKeys, m_mlExternalTestDatasetPath, error);
			m_selectedCloud = trainingCloud;
			if (!testWritten)
			{
				updateDockState();
				showError(error.isEmpty() ? QStringLiteral("Could not export the independent test dataset. Compute the same features on both clouds.") : error);
				return;
			}
		}
		QStringList args;
		args << QStringLiteral("train") << QStringLiteral("--dataset") << m_mlDatasetPath
			<< QStringLiteral("--model-out") << m_mlModelPath
			<< QStringLiteral("--classifier") << classifierId
			<< QStringLiteral("--device") << device
			<< QStringLiteral("--block-size") << QString::number(blockSizeMetres, 'g', 17)
			<< QStringLiteral("--test-fraction") << QString::number(testFraction, 'g', 17)
			<< QStringLiteral("--seed") << QString::number(seed)
			<< QStringLiteral("--trees") << QString::number(trees)
			<< QStringLiteral("--epochs") << QString::number(epochs)
			<< QStringLiteral("--batch-size") << QString::number(batchSize)
			<< QStringLiteral("--learning-rate") << QString::number(learningRate, 'g', 17)
			<< QStringLiteral("--min-samples-leaf") << QString::number(minimumLeaf)
			<< QStringLiteral("--max-features") << maxFeatures
			<< QStringLiteral("--class-weight") << classWeight
			<< QStringLiteral("--criterion") << criterion
			<< QStringLiteral("--subsample") << QString::number(subsample, 'g', 17)
			<< QStringLiteral("--column-sample") << QString::number(columnSample, 'g', 17);
		if (maxDepth > 0) args << QStringLiteral("--max-depth") << QString::number(maxDepth);
		if (imputeMissing) args << QStringLiteral("--impute-missing");
		if (validationBuffer > 0) args << QStringLiteral("--spatial-buffer") << QString::number(validationBuffer, 'g', 17);
		if (classifierId == QStringLiteral("pointnet"))
			args << QStringLiteral("--pointnet-radius") << QString::number(pointnetRadius, 'g', 17)
			     << QStringLiteral("--pointnet-neighbors") << QString::number(pointnetNeighbors);
		if (!m_mlExternalTestDatasetPath.isEmpty())
			args << QStringLiteral("--test-dataset") << m_mlExternalTestDatasetPath;
		if (!repository.isEmpty()) args << QStringLiteral("--repository") << repository;
		m_mlClassifierId = classifierId;
		m_mlRequestedDevice = device;
		launchMlWorker(QStringLiteral("train"), pythonExecutable, workerScript, args);
	}

	void WorkspaceController::predictModel(const QStringList& requestedFeatureKeys,
	                                      const QString& pythonExecutable,
	                                      const QString& workerScript,
	                                      const QString& modelPath,
	                                      const QString& repositoryPath,
	                                      int chunkSize)
	{
		if (m_predictionCloudUid != 0)
		{
			const auto target = m_sessions.find(m_predictionCloudUid);
			if (target == m_sessions.end()) { showError(QStringLiteral("The selected classification cloud is no longer available.")); return; }
			if (&target->second->cloud() != m_selectedCloud) selectCloud(&target->second->cloud());
		}
		if (!session() || !requireMetricUnits(QStringLiteral("classifier prediction"))) return;
		m_mlModelPath = QDir::fromNativeSeparators(modelPath);
		if (!QFileInfo::exists(m_mlModelPath)) { showError(QStringLiteral("Model file does not exist: %1").arg(modelPath)); return; }
		QStringList featureKeys = requestedFeatureKeys;
		const QFileInfo modelInfo(m_mlModelPath);
		QFile manifest(modelInfo.fileName() == QStringLiteral("model.joblib")
			? modelInfo.dir().filePath(QStringLiteral("manifest.json")) : m_mlModelPath + QStringLiteral(".manifest.json"));
		if (manifest.open(QIODevice::ReadOnly)
			&& QJsonDocument::fromJson(manifest.readAll()).object().value(QStringLiteral("classifier_id")).toString() == QStringLiteral("pointnet"))
			featureKeys = QStringList{QStringLiteral("xyz:x"), QStringLiteral("xyz:y"), QStringLiteral("xyz:z")};
		QString error;
		const QString repository = QDir::fromNativeSeparators(repositoryPath.trimmed());
		if (m_mlDatasetPath.isEmpty() || m_mlDatasetFeatureKeys != featureKeys || m_mlDatasetRevision != session()->sourceRevision()
			|| m_mlDatasetCloudUid != static_cast<quint64>(m_selectedCloud->getUniqueID()))
		{
			m_mlDatasetPath = repository.isEmpty()
				? createMlJobDirectory(QStringLiteral("dataset"), error)
				: QDir(repository).filePath(QStringLiteral("datasets/qal_dataset_%1_%2").arg(m_selectedCloud->getUniqueID())
					.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
			if (m_mlDatasetPath.isEmpty() || !writeMlDataset(featureKeys, m_mlDatasetPath, error)) { if (!error.isEmpty()) showError(error); return; }
			m_mlDatasetFeatureKeys = featureKeys;
			m_mlDatasetRevision = session()->sourceRevision();
			m_mlDatasetCloudUid = static_cast<quint64>(m_selectedCloud->getUniqueID());
		}
		if (!repository.isEmpty())
		{
			m_mlPredictionPath = QDir(repository).filePath(QStringLiteral("runs/%1/prediction")
				.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
			if (!QDir().mkpath(m_mlPredictionPath)) { showError(QStringLiteral("Cannot create repository run folder.")); return; }
		}
		else
		{
			m_mlPredictionPath = createMlJobDirectory(QStringLiteral("prediction"), error);
		}
		if (m_mlPredictionPath.isEmpty()) { showError(error); return; }
		QStringList args;
		args << QStringLiteral("predict") << QStringLiteral("--dataset") << m_mlDatasetPath
			<< QStringLiteral("--model") << m_mlModelPath
			<< QStringLiteral("--output") << m_mlPredictionPath
			<< QStringLiteral("--chunk-size") << QString::number(chunkSize);
		launchMlWorker(QStringLiteral("predict"), pythonExecutable, workerScript, args);
	}

	void WorkspaceController::predictModels(const QStringList& featureKeys,
	                                      const QString& pythonExecutable,
	                                      const QString& workerScript,
	                                      const QStringList& modelPaths,
	                                      const QString& repositoryPath,
	                                      int chunkSize)
	{
		if (modelPaths.isEmpty()) { showError(QStringLiteral("Select at least one compatible model in the catalog. Use Ctrl-click or 'Select all compatible'.")); return; }
		m_pendingPredictionRuns.clear(); m_predictionComparisonStatistics.clear();
		for (const QString& path : modelPaths)
		{
			if (!QFileInfo::exists(path)) continue;
			PendingPredictionRun run; run.featureKeys = featureKeys; run.pythonExecutable = pythonExecutable;
			run.workerScript = workerScript; run.modelPath = path; run.repositoryPath = repositoryPath; run.chunkSize = chunkSize;
			m_pendingPredictionRuns.push_back(run);
		}
		if (m_pendingPredictionRuns.isEmpty()) { showError(QStringLiteral("None of the selected model files exists.")); return; }
		m_predictionBatchTotal = m_pendingPredictionRuns.size(); m_predictionBatchCompleted = 0;
		startNextPredictionRun();
	}

	void WorkspaceController::startNextPredictionRun()
	{
		if (m_pendingPredictionRuns.isEmpty()) return;
		const PendingPredictionRun run = m_pendingPredictionRuns.takeFirst();
		m_dock->setStatus(QStringLiteral("Prediction %1/%2: %3")
			.arg(m_predictionBatchCompleted + 1).arg(m_predictionBatchTotal).arg(QFileInfo(run.modelPath).absoluteDir().dirName()), WorkspaceDock::StatusTone::Neutral);
		predictModel(run.featureKeys, run.pythonExecutable, run.workerScript, run.modelPath, run.repositoryPath, run.chunkSize);
		if (m_mlProcess->state() == QProcess::NotRunning) { m_pendingPredictionRuns.clear(); m_predictionBatchTotal = 0; }
	}

	bool WorkspaceController::preservePredictionFields(const QString& modelPath, QString& label, QString& error)
	{
		if (!m_selectedCloud) { error = QStringLiteral("No prediction cloud is active."); return false; }
		label = QFileInfo(modelPath).absoluteDir().dirName();
		QFile report(QFileInfo(modelPath).absoluteDir().filePath(QStringLiteral("report.json")));
		if (report.open(QIODevice::ReadOnly))
		{
			const QJsonObject object = QJsonDocument::fromJson(report.readAll()).object();
			label = object.value(QStringLiteral("classifier_id")).toString(label);
		}
		label.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]+")), QStringLiteral("_"));
		const QString suffix = label.left(24) + QLatin1Char('_') + QString::fromLatin1(QCryptographicHash::hash(modelPath.toUtf8(), QCryptographicHash::Sha256).toHex().left(6));
		const int prediction = m_selectedCloud->getScalarFieldIndexByName(field::AsprsPrediction);
		const int confidence = m_selectedCloud->getScalarFieldIndexByName(field::AsprsConfidence);
		if (prediction < 0 || confidence < 0) { error = QStringLiteral("Derived prediction fields are missing."); return false; }
		for (const QPair<int, QString>& source : {QPair<int, QString>(prediction, QStringLiteral("qAL_Pred_%1").arg(suffix)), QPair<int, QString>(confidence, QStringLiteral("qAL_Conf_%1").arg(suffix))})
		{
			int destination = m_selectedCloud->getScalarFieldIndexByName(source.second.toUtf8().constData());
			if (destination < 0) destination = m_selectedCloud->addScalarField(source.second.toUtf8().constData());
			auto* from = static_cast<ccScalarField*>(m_selectedCloud->getScalarField(source.first));
			auto* to = destination >= 0 ? static_cast<ccScalarField*>(m_selectedCloud->getScalarField(destination)) : nullptr;
			if (!from || !to) { error = QStringLiteral("Could not create comparison Scalar Field %1.").arg(source.second); return false; }
			for (unsigned i = 0; i < m_selectedCloud->size(); ++i) to->setValue(i, from->getValue(i));
			to->computeMinAndMax();
		}
		return true;
	}

	void WorkspaceController::launchMlWorker(const QString& operation,
	                                        const QString& pythonExecutable,
	                                        const QString& workerScript,
	                                        const QStringList& arguments)
	{
		if (m_mlProcess->state() != QProcess::NotRunning)
		{
			showError(QStringLiteral("An ML worker job is already running."));
			return;
		}
		QString python = QDir::fromNativeSeparators(pythonExecutable.trimmed());
		if (!QFileInfo::exists(python)) python = QStandardPaths::findExecutable(pythonExecutable.trimmed());
		if (python.isEmpty()) { showError(QStringLiteral("Python executable was not found.")); return; }
		const QString worker = QDir::fromNativeSeparators(workerScript.trimmed());
		if (!QFileInfo::exists(worker)) { showError(QStringLiteral("ML worker script was not found: %1").arg(workerScript)); return; }

		m_mlOperation = operation;
		m_mlStdoutBuffer.clear(); m_mlStderrBuffer.clear(); m_mlSummary.clear();
		m_mlSourceUid = m_selectedCloud ? static_cast<quint64>(m_selectedCloud->getUniqueID()) : 0;
		m_mlStartedMilliseconds = QDateTime::currentMSecsSinceEpoch();
		m_mlCancelled = false;
		m_mlProcess->setProgram(python);
		m_mlProcess->setArguments(QStringList() << QStringLiteral("-u") << worker << arguments);
		m_mlProcess->setWorkingDirectory(QFileInfo(worker).absolutePath());
		m_dock->setBusy(true);
		QString progressTitle = QStringLiteral("Predict ASPRS");
		if (operation == QStringLiteral("train")) progressTitle = QStringLiteral("Train classifier");
		else if (operation == QStringLiteral("bootstrap")) progressTitle = QStringLiteral("Bootstrap labels");
		else if (operation == QStringLiteral("vegetation-predict")) progressTitle = QStringLiteral("Vegetation model — fixed compatible scales [CPU]");
		else if (operation == QStringLiteral("bootstrap-capabilities")) progressTitle = QStringLiteral("Check ML/DL engines");
		else if (operation == QStringLiteral("pretrained-install")) progressTitle = QStringLiteral("Install pre-trained model");
		else if (operation == QStringLiteral("pretrained-remove")) progressTitle = QStringLiteral("Remove pre-trained model");
		else if (operation == QStringLiteral("pretrained-check")) progressTitle = QStringLiteral("Check model updates and runtimes");
		else if (operation == QStringLiteral("pretrained-update-all")) progressTitle = QStringLiteral("Update installed models");
		else if (operation == QStringLiteral("pretrained-prune")) progressTitle = QStringLiteral("Remove obsolete managed models");
		m_dock->setProgress(progressTitle, 0, 100, true);
		m_dock->setStatus(QStringLiteral("ML worker starting…"), WorkspaceDock::StatusTone::Neutral);
		m_mlProcess->start();
	}

	void WorkspaceController::readMlStandardOutput()
	{
		m_mlStdoutBuffer.append(m_mlProcess->readAllStandardOutput());
		for (;;)
		{
			const int newline = m_mlStdoutBuffer.indexOf('\n');
			if (newline < 0) break;
			const QByteArray line = m_mlStdoutBuffer.left(newline).trimmed();
			m_mlStdoutBuffer.remove(0, newline + 1);
			if (line.isEmpty()) continue;
			QJsonParseError parseError;
			const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
			if (parseError.error != QJsonParseError::NoError || !document.isObject())
			{
				m_mlStderrBuffer.append("Worker emitted non-JSON output: "); m_mlStderrBuffer.append(line); m_mlStderrBuffer.append('\n');
				continue;
			}
			const QJsonObject object = document.object();
			const QString event = object.value(QStringLiteral("event")).toString(object.value(QStringLiteral("type")).toString());
			const QString message = object.value(QStringLiteral("message")).toString();
			if (event == QStringLiteral("progress"))
			{
				double value = object.value(QStringLiteral("progress")).toDouble(object.value(QStringLiteral("value")).toDouble(0.0));
				if (value <= 1.0) value *= 100.0;
				m_dock->setProgress(message.isEmpty() ? (m_mlOperation == QStringLiteral("train") ? QStringLiteral("Train classifier") : QStringLiteral("Predict ASPRS")) : message,
					static_cast<int>(std::lround(std::max(0.0, std::min(100.0, value)))), 100, true);
			}
			else if (event == QStringLiteral("error"))
			{
				m_mlStderrBuffer.append(message.toUtf8()); m_mlStderrBuffer.append('\n');
			}
			else if (!message.isEmpty())
			{
				m_mlSummary = message;
				m_dock->setStatus(message, WorkspaceDock::StatusTone::Neutral);
			}
			if (m_mlOperation == QStringLiteral("bootstrap-capabilities") && event == QStringLiteral("completed"))
			{
				const QJsonObject data = object.value(QStringLiteral("data")).toObject();
				bool myriaAvailable = false;
				QString myriaReason = QStringLiteral("Myria3D provider was not reported.");
				for (const QJsonValue value : data.value(QStringLiteral("pretrained")).toArray())
				{
					const QJsonObject provider = value.toObject();
					if (provider.value(QStringLiteral("id")).toString() == QStringLiteral("myria3d_fractal"))
					{
						myriaAvailable = provider.value(QStringLiteral("available")).toBool();
						myriaReason = provider.value(QStringLiteral("reason")).toString(myriaReason);
					}
				}
				m_dock->setBootstrapStatus(QStringLiteral("Clustering engines: ready. Myria3D: %1\n%2")
					.arg(myriaAvailable ? QStringLiteral("detected") : QStringLiteral("not available"), myriaReason), myriaAvailable);
			}
		}
	}

	void WorkspaceController::inspectBootstrapCapabilities(const QString& pythonExecutable, const QString& workerScript)
	{
		launchMlWorker(QStringLiteral("bootstrap-capabilities"), pythonExecutable, workerScript,
			QStringList() << QStringLiteral("bootstrap-capabilities"));
	}

	void WorkspaceController::installPretrainedModel(const QString& modelId, const QString& pythonExecutable,
		const QString& workerScript, const QString& repositoryPath)
	{
		if (repositoryPath.trimmed().isEmpty()) { showError(QStringLiteral("Choose an ALiS repository before installing a model.")); return; }
		launchMlWorker(QStringLiteral("pretrained-install"), pythonExecutable, workerScript,
			QStringList() << QStringLiteral("pretrained-install") << QStringLiteral("--repository") << repositoryPath
				<< QStringLiteral("--model-id") << modelId);
	}

	void WorkspaceController::removePretrainedModel(const QString& modelId, const QString& pythonExecutable,
		const QString& workerScript, const QString& repositoryPath)
	{
		if (repositoryPath.trimmed().isEmpty()) { showError(QStringLiteral("Choose an ALiS repository before removing a model.")); return; }
		launchMlWorker(QStringLiteral("pretrained-remove"), pythonExecutable, workerScript,
			QStringList() << QStringLiteral("pretrained-remove") << QStringLiteral("--repository") << repositoryPath
				<< QStringLiteral("--model-id") << modelId);
	}

	void WorkspaceController::maintainPretrainedModels(const QString& action, const QString& pythonExecutable,
		const QString& workerScript, const QString& repositoryPath)
	{
		static const QStringList allowed = {QStringLiteral("pretrained-check"), QStringLiteral("pretrained-update-all"), QStringLiteral("pretrained-prune")};
		if (!allowed.contains(action)) { showError(QStringLiteral("Unsupported pre-trained maintenance action.")); return; }
		if (repositoryPath.trimmed().isEmpty()) { showError(QStringLiteral("Choose an ALiS repository first.")); return; }
		launchMlWorker(action, pythonExecutable, workerScript,
			QStringList() << action << QStringLiteral("--repository") << repositoryPath);
	}

	void WorkspaceController::bootstrapLabels(const QStringList& featureKeys,
	                                         const QString& pythonExecutable,
	                                         const QString& workerScript,
	                                         const QString& repositoryPath,
	                                         const QString& algorithm,
	                                         int clusterCount,
	                                         int fitSample,
	                                         int chunkSize,
	                                         int seed)
	{
		if (!session() || !m_selectedCloud) return;
		QString error;
		const QString repository = QDir::fromNativeSeparators(repositoryPath.trimmed());
		// Native CC tools can edit SF/RGB without incrementing our Session revision.
		// Export current values for each cluster run; this is an aligned copy, never
		// a geometric-feature recomputation. Clustering does not consume XY units.
		{
			m_mlDatasetPath = repository.isEmpty()
				? createMlJobDirectory(QStringLiteral("bootstrap_dataset"), error)
				: QDir(repository).filePath(QStringLiteral("datasets/qal_bootstrap_%1_%2").arg(m_selectedCloud->getUniqueID())
					.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
			if (m_mlDatasetPath.isEmpty() || !writeMlDataset(featureKeys, m_mlDatasetPath, error, false))
			{
				if (!error.isEmpty()) showError(error);
				return;
			}
			m_mlDatasetFeatureKeys = featureKeys;
			m_mlDatasetRevision = session()->sourceRevision();
			m_mlDatasetCloudUid = static_cast<quint64>(m_selectedCloud->getUniqueID());
		}
		m_bootstrapOutputPath = repository.isEmpty()
			? createMlJobDirectory(QStringLiteral("bootstrap"), error)
			: QDir(repository).filePath(QStringLiteral("runs/bootstrap_%1/output")
				.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
		if (m_bootstrapOutputPath.isEmpty() || !QDir().mkpath(m_bootstrapOutputPath))
		{
			showError(error.isEmpty() ? QStringLiteral("Cannot create the bootstrap output folder.") : error);
			return;
		}
		QStringList args;
		args << QStringLiteral("cluster") << QStringLiteral("--dataset") << m_mlDatasetPath
			<< QStringLiteral("--output") << m_bootstrapOutputPath
			<< QStringLiteral("--algorithm") << algorithm
			<< QStringLiteral("--clusters") << QString::number(clusterCount)
			<< QStringLiteral("--fit-sample") << QString::number(fitSample)
			<< QStringLiteral("--chunk-size") << QString::number(chunkSize)
			<< QStringLiteral("--seed") << QString::number(seed);
		launchMlWorker(QStringLiteral("bootstrap"), pythonExecutable, workerScript, args);
	}

	bool WorkspaceController::loadBootstrapBundle(const QString& directory, QString& error)
	{
		ALiSSession* s = session();
		if (!s || !m_selectedCloud || static_cast<quint64>(m_selectedCloud->getUniqueID()) != m_mlSourceUid)
		{
			error = QStringLiteral("The active Session changed while bootstrap labeling was running; output was not imported.");
			return false;
		}
		const quint64 rows = m_selectedCloud->size();
		QFile clusterFile(QDir(directory).filePath(QStringLiteral("clusters.i32")));
		QFile confidenceFile(QDir(directory).filePath(QStringLiteral("confidence.f32")));
		if (!clusterFile.open(QIODevice::ReadOnly) || !confidenceFile.open(QIODevice::ReadOnly)
		    || static_cast<quint64>(clusterFile.size()) != rows * 4u
		    || static_cast<quint64>(confidenceFile.size()) != rows * 4u)
		{
			error = QStringLiteral("Bootstrap bundle is incomplete or not aligned with the Session.");
			return false;
		}
		std::vector<std::int32_t> clusters(static_cast<std::size_t>(rows));
		std::vector<float> confidence(static_cast<std::size_t>(rows));
		QDataStream clusterStream(&clusterFile); clusterStream.setByteOrder(QDataStream::LittleEndian);
		QDataStream confidenceStream(&confidenceFile); confidenceStream.setByteOrder(QDataStream::LittleEndian);
		confidenceStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
		for (quint64 row = 0; row < rows; ++row)
		{
			qint32 clusterId = -1; float score = 0.0f;
			clusterStream >> clusterId; confidenceStream >> score;
			clusters[static_cast<std::size_t>(row)] = static_cast<std::int32_t>(clusterId);
			confidence[static_cast<std::size_t>(row)] = score;
		}
		if (clusterStream.status() != QDataStream::Ok || confidenceStream.status() != QDataStream::Ok)
		{
			error = QStringLiteral("Failed to read bootstrap output.");
			return false;
		}
		QJsonObject provenance;
		QFile resultFile(QDir(directory).filePath(QStringLiteral("result.json")));
		if (resultFile.open(QIODevice::ReadOnly)) provenance = QJsonDocument::fromJson(resultFile.readAll()).object();
		provenance.insert(QStringLiteral("dataset"), m_mlDatasetPath);
		provenance.insert(QStringLiteral("output_bundle"), directory);
		return s->setBootstrapClusters(clusters, confidence, provenance,
			QDateTime::currentMSecsSinceEpoch() - m_mlStartedMilliseconds, error);
	}

	bool WorkspaceController::loadPredictionBundle(const QString& directory, QString& error)
	{
		ALiSSession* s = session();
		if (!s || !m_selectedCloud || static_cast<quint64>(m_selectedCloud->getUniqueID()) != m_mlSourceUid)
		{
			error = QStringLiteral("The active Session changed while prediction was running; output was not imported.");
			return false;
		}
		const quint64 rows = m_selectedCloud->size();
		QFile predictionFile(QDir(directory).filePath(QStringLiteral("predictions.i16")));
		QFile confidenceFile(QDir(directory).filePath(QStringLiteral("confidence.f32")));
		if (!predictionFile.open(QIODevice::ReadOnly) || !confidenceFile.open(QIODevice::ReadOnly))
		{
			error = QStringLiteral("Prediction bundle is incomplete (predictions.i16/confidence.f32).");
			return false;
		}
		if (static_cast<quint64>(predictionFile.size()) != rows * 2u || static_cast<quint64>(confidenceFile.size()) != rows * 4u)
		{
			error = QStringLiteral("Prediction bundle row count does not match the Session.");
			return false;
		}
		std::vector<std::int16_t> predictions(static_cast<std::size_t>(rows));
		std::vector<float> confidence(static_cast<std::size_t>(rows));
		std::map<int, quint64> predictionCounts;
		std::map<int, quint64> referenceCounts, correctCounts;
		quint64 trustedCompared = 0, trustedCorrect = 0;
		const int trustedIndex = m_selectedCloud->getScalarFieldIndexByName(field::AsprsTrainingClass);
		const ccScalarField* trustedClasses = trustedIndex >= 0 ? static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(trustedIndex)) : nullptr;
		std::vector<float> validConfidence;
		validConfidence.reserve(static_cast<std::size_t>(rows));
		QDataStream predictionStream(&predictionFile); predictionStream.setByteOrder(QDataStream::LittleEndian);
		QDataStream confidenceStream(&confidenceFile); confidenceStream.setByteOrder(QDataStream::LittleEndian);
		confidenceStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
		quint64 valid = 0;
		double confidenceSum = 0.0;
		for (quint64 row = 0; row < rows; ++row)
		{
			qint16 value = -1; float probability = std::numeric_limits<float>::quiet_NaN();
			predictionStream >> value; confidenceStream >> probability;
			predictions[static_cast<std::size_t>(row)] = static_cast<std::int16_t>(value);
			confidence[static_cast<std::size_t>(row)] = probability;
			if (value >= 0 && std::isfinite(probability))
			{
				++valid; confidenceSum += probability; ++predictionCounts[static_cast<int>(value)];
				validConfidence.push_back(probability);
				const double reference = trustedClasses ? trustedClasses->getValue(static_cast<unsigned>(row)) : std::numeric_limits<double>::quiet_NaN();
				if (std::isfinite(reference) && reference >= 0.0 && reference <= 255.0)
				{
					const int expected = static_cast<int>(std::lround(reference)); ++trustedCompared; ++referenceCounts[expected];
					if (expected == value) { ++trustedCorrect; ++correctCounts[expected]; }
				}
			}
		}
		if (predictionStream.status() != QDataStream::Ok || confidenceStream.status() != QDataStream::Ok)
		{
			error = QStringLiteral("Failed to read the prediction bundle."); return false;
		}
		QJsonObject provenance;
		QFile resultFile(QDir(directory).filePath(QStringLiteral("result.json")));
		if (resultFile.open(QIODevice::ReadOnly))
		{
			const QJsonDocument result = QJsonDocument::fromJson(resultFile.readAll());
			if (result.isObject()) provenance = result.object();
		}
		provenance.insert(QStringLiteral("worker_schema"), QStringLiteral("qal-ml-event/1.0"));
		provenance.insert(QStringLiteral("dataset"), m_mlDatasetPath);
		provenance.insert(QStringLiteral("model"), m_mlModelPath);
		provenance.insert(QStringLiteral("prediction_bundle"), directory);
		if (!s->setAsprsPredictions(predictions, confidence, provenance,
			QDateTime::currentMSecsSinceEpoch() - m_mlStartedMilliseconds, error)) return false;
		m_mlSummary = QStringLiteral("%1/%2 aligned predictions; mean confidence %3. Review Derived fields before Apply.")
			.arg(valid).arg(rows).arg(valid ? confidenceSum / valid : 0.0, 0, 'f', 3);
		std::sort(validConfidence.begin(), validConfidence.end());
		auto percentile = [&validConfidence](double fraction)
		{
			if (validConfidence.empty()) return 0.0f;
			const std::size_t index = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(validConfidence.size() - 1)));
			return validConfidence[std::min(index, validConfidence.size() - 1)];
		};
		QStringList classRows;
		for (const auto& item : predictionCounts)
			classRows << QStringLiteral("ASPRS %1: %2 points (%3%)").arg(item.first).arg(countText(item.second))
				.arg(valid ? 100.0 * static_cast<double>(item.second) / static_cast<double>(valid) : 0.0, 0, 'f', 1);
		QString trustedDiagnostic = QStringLiteral("No Manual/Trusted reference labels are available on this target cloud.");
		if (trustedCompared > 0)
		{
			double recallSum = 0.0; QStringList recalls;
			for (const auto& item : referenceCounts)
			{
				const double recall = item.second ? static_cast<double>(correctCounts[item.first]) / item.second : 0.0;
				recallSum += recall; recalls << QStringLiteral("ASPRS %1: %2/%3 correct (recall %4)")
					.arg(item.first).arg(correctCounts[item.first]).arg(item.second).arg(recall, 0, 'f', 3);
			}
			trustedDiagnostic = QStringLiteral("Agreement on %1 Manual/Trusted points: accuracy %2; balanced recall %3.\n%4\nThis is a same-cloud diagnostic, not independent validation.")
				.arg(countText(trustedCompared)).arg(static_cast<double>(trustedCorrect) / trustedCompared, 0, 'f', 3)
				.arg(recallSum / std::max<std::size_t>(1, referenceCounts.size()), 0, 'f', 3).arg(recalls.join(QLatin1Char('\n')));
		}
		m_mlPredictionStatistics = QStringLiteral(
			"Prediction completed for every valid point without changing ASPRS Working.\n\n"
			"Aligned predictions: %1 / %2\nMean confidence: %3\nConfidence P10 / median / P90: %4 / %5 / %6\n\n"
			"Class distribution:\n%7\n\nReference diagnostic:\n%8\n\n"
			"Changing the downstream confidence threshold does not rerun Predict. It can filter the view, or limit the optional copy to ASPRS Working.")
			.arg(countText(valid)).arg(countText(rows))
			.arg(valid ? confidenceSum / valid : 0.0, 0, 'f', 3)
			.arg(percentile(0.10), 0, 'f', 3).arg(percentile(0.50), 0, 'f', 3).arg(percentile(0.90), 0, 'f', 3)
			.arg(classRows.join(QLatin1Char('\n'))).arg(trustedDiagnostic);
		m_predictionStatisticsCache[static_cast<quint64>(m_selectedCloud->getUniqueID())] = m_mlPredictionStatistics;
		return true;
	}

	void WorkspaceController::finishMlWorker(int exitCode, int exitStatus)
	{
		readMlStandardOutput();
		if (!m_mlStdoutBuffer.trimmed().isEmpty())
		{
			m_mlStdoutBuffer.append('\n'); readMlStandardOutput();
		}
		const QString operation = m_mlOperation;
		if (operation.isEmpty()) return;
		m_mlOperation.clear();
		m_mlStderrBuffer.append(m_mlProcess->readAllStandardError());
		m_dock->clearProgress();
		m_dock->setBusy(false);
		if (m_mlCancelled)
		{
			m_pendingModelRuns.clear(); m_pendingPredictionRuns.clear(); m_predictionBatchTotal = 0;
			m_mlSummary = QStringLiteral("ML job cancelled; no output was applied.");
			updateDockState(); showError(m_mlSummary); return;
		}
		if (exitStatus != static_cast<int>(QProcess::NormalExit) || exitCode != 0)
		{
			m_pendingModelRuns.clear(); m_pendingPredictionRuns.clear(); m_predictionBatchTotal = 0;
			QString details = QString::fromUtf8(m_mlStderrBuffer).trimmed();
			if (details.size() > 1000) details = details.right(1000);
			m_mlSummary = QStringLiteral("ML worker failed (exit %1)%2").arg(exitCode)
				.arg(details.isEmpty() ? QString() : QStringLiteral(": %1").arg(details));
			updateDockState(); showError(m_mlSummary); return;
		}
		if (operation == QStringLiteral("bootstrap-capabilities"))
		{
			updateDockState();
			showReady(QStringLiteral("Bootstrap engine check completed."));
			return;
		}
		if (operation == QStringLiteral("pretrained-install") || operation == QStringLiteral("pretrained-remove")
			|| operation == QStringLiteral("pretrained-check") || operation == QStringLiteral("pretrained-update-all")
			|| operation == QStringLiteral("pretrained-prune"))
		{
			m_dock->refreshPretrainedCatalog();
			updateDockState();
			showReady(m_mlSummary.isEmpty() ? QStringLiteral("Pre-trained model library updated.") : m_mlSummary);
			return;
		}
		if (!m_selectedCloud || static_cast<quint64>(m_selectedCloud->getUniqueID()) != m_mlSourceUid)
		{
			showError(QStringLiteral("The active Session changed; worker output was not imported.")); return;
		}
		if (operation == QStringLiteral("vegetation-predict"))
		{
			QString error;
			if(!loadVegetationScores(error)){showError(error);return;}
			updateDockState();return;
		}
		if (operation == QStringLiteral("train"))
		{
			if (!QFileInfo::exists(m_mlModelPath)) { m_pendingModelRuns.clear(); showError(QStringLiteral("Worker completed but did not create the model file.")); return; }
			ProcessingRecord record; record.operation = QStringLiteral("Models.Train.%1").arg(m_mlClassifierId);
			record.timestampUtc = QDateTime::currentDateTimeUtc(); record.sourceEntityUid = m_mlSourceUid;
			record.affectedPoints = session()->trustedTrainingCount();
			record.parameters.insert(QStringLiteral("dataset"), m_mlDatasetPath);
			record.parameters.insert(QStringLiteral("model"), m_mlModelPath);
			record.parameters.insert(QStringLiteral("classifier"), m_mlClassifierId);
			record.parameters.insert(QStringLiteral("requested_device"), m_mlRequestedDevice);
			record.elapsedMilliseconds = QDateTime::currentMSecsSinceEpoch() - m_mlStartedMilliseconds;
			session()->addProcessingRecord(record);
			m_mlSummary = QStringLiteral("%1 trained; technical report saved beside the model. Predict remains explicit.")
				.arg(m_mlClassifierId.isEmpty() ? QStringLiteral("Classifier") : m_mlClassifierId);
			m_dock->setModelWorkerPaths(QString(), QString(), QDir::toNativeSeparators(m_mlModelPath));
			m_dock->refreshModelCatalog();
			const QString reportPath = ModelResultsDialog::reportPathForModel(m_mlModelPath);
			if (!reportPath.isEmpty() && !m_mlReportPaths.contains(reportPath)) m_mlReportPaths << reportPath;
			++m_trainingBatchCompleted;
			updateDockState(); showReady(m_mlSummary);
			if (!m_pendingModelRuns.isEmpty()) QTimer::singleShot(0, this, &WorkspaceController::startNextTrainingRun);
			else if (m_trainingBatchTotal > 1) QTimer::singleShot(0, this, &WorkspaceController::startNextTrainingRun);
			else if (!m_mlReportPaths.isEmpty()) QTimer::singleShot(0, this, [this]() { showModelResults(); });
		}
		else if (operation == QStringLiteral("predict"))
		{
			QString error;
			if (!loadPredictionBundle(m_mlPredictionPath, error)) { m_pendingPredictionRuns.clear(); m_predictionBatchTotal = 0; updateDockState(); showError(error); return; }
			if (m_predictionBatchTotal > 0)
			{
				QString label;
				if (!preservePredictionFields(m_mlModelPath, label, error)) { m_pendingPredictionRuns.clear(); m_predictionBatchTotal = 0; updateDockState(); showError(error); return; }
				m_predictionComparisonStatistics << QStringLiteral("%1\n%2").arg(label, m_mlPredictionStatistics);
				++m_predictionBatchCompleted;
				refreshHost(); updateModelFeatureChoices(); updateDockState();
				if (!m_pendingPredictionRuns.isEmpty()) QTimer::singleShot(0, this, &WorkspaceController::startNextPredictionRun);
				else
				{
					m_mlPredictionStatistics = QStringLiteral("TARGET-CLOUD PREDICTION COMPARISON (%1 models)\n\n%2")
						.arg(m_predictionBatchCompleted).arg(m_predictionComparisonStatistics.join(QStringLiteral("\n\n────────────────────────\n\n")));
					m_mlSummary = QStringLiteral("Prediction comparison complete: %1 model-specific class/confidence Scalar Field pairs were added.").arg(m_predictionBatchCompleted);
					m_predictionStatisticsCache[static_cast<quint64>(m_selectedCloud->getUniqueID())] = m_mlPredictionStatistics;
					m_predictionBatchTotal = 0; updateDockState(); showReady(m_mlSummary); showPredictionStatistics();
				}
			}
			else
			{
				refreshHost(); updateDockState(); showReady(m_mlSummary); showPredictionStatistics();
			}
		}
		else if (operation == QStringLiteral("bootstrap"))
		{
			QString error;
			if (!loadBootstrapBundle(m_bootstrapOutputPath, error))
			{
				updateDockState(); showError(error); return;
			}
			m_mlSummary = QStringLiteral("Derived cluster and confidence Scalar Fields added. Cluster IDs are not ASPRS classes; review them in Annotation Studio.");
			m_dock->setBootstrapStatus(m_mlSummary, false);
			refreshHost(); updateModelFeatureChoices(); updateDockState(); showReady(m_mlSummary);
		}
	}

	void WorkspaceController::showModelResults(const QString& modelPath)
	{
		QStringList reports;
		if (!modelPath.trimmed().isEmpty())
		{
			const QString report = ModelResultsDialog::reportPathForModel(QDir::fromNativeSeparators(modelPath.trimmed()));
			if (!report.isEmpty()) reports << report;
		}
		else
		{
			reports = m_mlReportPaths;
			if (reports.isEmpty())
			{
				const QString report = ModelResultsDialog::reportPathForModel(m_mlModelPath);
				if (!report.isEmpty()) reports << report;
			}
		}
		if (reports.isEmpty())
		{
			showError(QStringLiteral("No technical report was found beside this model. Train it with ALiS or select a catalog model that includes report.json."));
			return;
		}
		QWidget* parentWidget = m_app ? static_cast<QWidget*>(m_app->getMainWindow()) : static_cast<QWidget*>(m_dock);
		ModelResultsDialog dialog(reports, parentWidget);
		dialog.exec();
	}

	void WorkspaceController::cancelMlWorker()
	{
		if (m_mlProcess->state() == QProcess::NotRunning) return;
		m_mlCancelled = true;
		m_dock->setProgress(QStringLiteral("Cancelling ML worker…"), 0, 0, false);
		m_mlProcess->terminate();
		QTimer::singleShot(3000, this, [this]()
		{
			if (m_mlProcess->state() != QProcess::NotRunning) m_mlProcess->kill();
		});
	}

	void WorkspaceController::applyModelPredictions(double minimumConfidence)
	{
		ALiSSession* s = session(); if (!s) return;
		QString error;
		if (!s->applyAsprsPredictions(minimumConfidence, error)) { showError(error); return; }
		markCloudProfileStale(QStringLiteral("model predictions changed ASPRS Working classes"));
		refreshHost(); updateDockState();
		showReady(QStringLiteral("Predictions with confidence ≥ %1 applied to ASPRS Working; Undo is available.")
			.arg(minimumConfidence, 0, 'f', 2));
	}

	void WorkspaceController::filterPredictionByConfidence(bool enabled, double minimumConfidence)
	{
		if (!m_selectedCloud || !session()) return;
		m_predictionConfidenceFilterEnabled = enabled; m_predictionConfidenceThreshold = minimumConfidence;
		if (!enabled)
		{
			m_selectedCloud->unallocateVisibilityArray(); refreshHost(); updateDockState();
			showReady(QStringLiteral("Confidence view filter disabled; all valid predictions remain available.")); return;
		}
		const int predictionIndex = m_selectedCloud->getScalarFieldIndexByName(field::AsprsPrediction);
		const int confidenceIndex = m_selectedCloud->getScalarFieldIndexByName(field::AsprsConfidence);
		if (predictionIndex < 0 || confidenceIndex < 0) { showError(QStringLiteral("Run Predict before filtering by confidence.")); return; }
		const auto* prediction = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(predictionIndex));
		const auto* confidence = static_cast<const ccScalarField*>(m_selectedCloud->getScalarField(confidenceIndex));
		auto& visibility = m_selectedCloud->getTheVisibilityArray(); visibility.resize(m_selectedCloud->size()); quint64 shown = 0;
		for (unsigned i = 0; i < m_selectedCloud->size(); ++i)
		{
			const bool keep = std::isfinite(prediction->getValue(i)) && std::isfinite(confidence->getValue(i)) && confidence->getValue(i) >= minimumConfidence;
			visibility[i] = keep ? CCCoreLib::POINT_VISIBLE : CCCoreLib::POINT_HIDDEN; if (keep) ++shown;
		}
		QString error; session()->displayScalarField(QString::fromUtf8(field::AsprsPrediction), error);
		refreshHost(); updateDockState();
		showReady(QStringLiteral("Confidence view filter ≥ %1: %2/%3 points shown. Predict was not rerun.")
			.arg(minimumConfidence, 0, 'f', 2).arg(countText(shown)).arg(countText(m_selectedCloud->size())));
	}

	void WorkspaceController::showPredictionStatistics()
	{
		if (m_mlPredictionStatistics.trimmed().isEmpty()) { showError(QStringLiteral("No prediction statistics are available yet. Run Predict first.")); return; }
		QWidget* parentWidget = m_app ? static_cast<QWidget*>(m_app->getMainWindow()) : static_cast<QWidget*>(m_dock);
		QMessageBox box(QMessageBox::Information, QStringLiteral("ALiS — Prediction statistics"), m_mlPredictionStatistics, QMessageBox::Ok, parentWidget);
		box.setTextInteractionFlags(Qt::TextSelectableByMouse); box.exec();
	}

	void WorkspaceController::applyLabels(int asprsCode, int archaeologyCode)
	{
		ALiSSession* s = session(); if (!s) return; QString error; const std::vector<unsigned> indices = s->visibleSubset(error); if (indices.empty()) { showError(error); return; }
		Q_UNUSED(archaeologyCode);
		if (asprsCode < 0) { showError(QStringLiteral("Choose one ASPRS or ALiS user-defined class.")); return; }
		const quint64 previousGroundRevision = s->groundRevision();
		if (!s->applyManualAsprs(indices, static_cast<std::uint8_t>(asprsCode), error)) { showError(error); return; }
		if (s->groundRevision() != previousGroundRevision) resetGroundDerivedState();
		markCloudProfileStale(QStringLiteral("manual ASPRS labels changed"));
		refreshHost(); updateDockState(); showReady(QStringLiteral("Applied Manual/Trusted labels to %1 visible points.").arg(countText(indices.size())));
	}

	void WorkspaceController::selectTrainingSet(bool loadFromFile, bool externalTest)
	{
		ccPointCloud* cloud = nullptr;
		if (loadFromFile)
		{
			const QString path = QFileDialog::getOpenFileName(m_app ? m_app->getMainWindow() : nullptr,
				QStringLiteral("Select certified training point cloud"), QString(),
				QStringLiteral("LiDAR point clouds (*.las *.laz);;CloudCompare files (*.bin);;All files (*)"));
			if (path.isEmpty()) return;
			ccHObject* loaded = m_app ? m_app->loadFile(path, false) : nullptr;
			if (!loaded) { showError(QStringLiteral("CloudCompare could not load the selected training set.")); return; }
			ccHObject::Container clouds;
			if (loaded->isA(CC_TYPES::POINT_CLOUD)) clouds.push_back(loaded);
			loaded->filterChildren(clouds, true, CC_TYPES::POINT_CLOUD);
			if (clouds.empty()) { delete loaded; showError(QStringLiteral("The selected file contains no point cloud.")); return; }
			m_app->addToDB(loaded, true, true, false, true);
			cloud = dynamic_cast<ccPointCloud*>(clouds.front());
			if (cloud) { m_app->setSelectedInDB(cloud, true); selectCloud(cloud); }
		}
		else
		{
			const auto& selected = m_app->getSelectedEntities();
			for (ccHObject* entity : selected)
			{
				if ((cloud = dynamic_cast<ccPointCloud*>(entity))) break;
				ccHObject::Container clouds;
				entity->filterChildren(clouds, true, CC_TYPES::POINT_CLOUD);
				if (!clouds.empty()) { cloud = dynamic_cast<ccPointCloud*>(clouds.front()); break; }
			}
			if (!cloud) cloud = m_selectedCloud;
			if (cloud && cloud != m_selectedCloud) selectCloud(cloud);
		}
		if (!cloud) { showError(QStringLiteral("Select a loaded point cloud in the DB Tree first.")); return; }
		ALiSSession* s = session();
		if (!s) return;
		QStringList fields;
		for (unsigned i = 0; i < cloud->getNumberOfScalarFields(); ++i)
		{
			const auto* sf = cloud->getScalarField(i);
			if (sf && sf->currentSize() == cloud->size()) fields << QString::fromUtf8(sf->getName());
		}
		if (fields.isEmpty()) { showError(QStringLiteral("The training cloud has no aligned Scalar Field to use as classes.")); return; }
		int defaultIndex = fields.indexOf(QStringLiteral("Classification"));
		if (defaultIndex < 0) defaultIndex = fields.indexOf(QString::fromUtf8(field::OriginalClassification));
		if (defaultIndex < 0) defaultIndex = 0;
		bool accepted = false;
		const QString classField = QInputDialog::getItem(m_app ? m_app->getMainWindow() : nullptr,
			QStringLiteral("Training class field"),
			QStringLiteral("Field containing certified ASPRS class codes:"), fields, defaultIndex, false, &accepted);
		if (!accepted || classField.isEmpty()) return;
		const int sfIndex = cloud->getScalarFieldIndexByName(classField.toUtf8().constData());
		const auto* sf = sfIndex >= 0 ? cloud->getScalarField(sfIndex) : nullptr;
		std::map<int, quint64> counts;
		quint64 invalid = 0;
		for (unsigned i = 0; sf && i < cloud->size(); ++i)
		{
			const float value = sf->getValue(i);
			if (std::isfinite(value) && value >= 0.0f && value <= 255.0f && std::floor(value) == value)
				++counts[static_cast<int>(value)];
			else ++invalid;
		}
		QStringList summary;
		for (const auto& item : counts) summary << QStringLiteral("%1: %2").arg(item.first).arg(countText(item.second));
		if (invalid) summary << QStringLiteral("invalid/skipped: %1").arg(countText(invalid));
		const auto answer = QMessageBox::question(m_app ? m_app->getMainWindow() : nullptr,
			QStringLiteral("Use certified training set"),
			QStringLiteral("Cloud: %1\nClass field: %2\nPoints: %3\nClasses: %4\n\n"
			               "Use these values as ASPRS Working and Manual/Trusted training labels? "
			               "Continue only for an independently checked field; the operation is undoable.")
				.arg(cloud->getName(), classField, countText(cloud->size()), summary.join(QStringLiteral("; "))),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes) return;
		QString error;
		if (!s->trustAsprsFromField(classField, error)) { showError(error); return; }
		markCloudProfileStale(QStringLiteral("a certified class field replaced ASPRS Working"));
		refreshHost(); updateDockState();
		if (externalTest)
		{
			m_testCloudUid = static_cast<quint64>(cloud->getUniqueID());
			m_mlExternalTestDatasetPath.clear();
		}
		else
		{
			m_trainingCloudUid = static_cast<quint64>(cloud->getUniqueID());
			m_mlDatasetPath.clear(); m_mlDatasetFeatureKeys.clear();
			m_mlDatasetRevision = std::numeric_limits<quint64>::max();
			m_mlDatasetCloudUid = 0;
		}
		updateDockState();
		showReady(QStringLiteral("%1 set selected: %2 trusted ASPRS labels from '%3'.")
			.arg(externalTest ? QStringLiteral("External test") : QStringLiteral("Training"),
			     countText(s->trustedTrainingCount()), classField));
	}

	void WorkspaceController::selectClassificationCloud(bool loadFromFile)
	{
		ccPointCloud* cloud = nullptr;
		if (loadFromFile)
		{
			const QString path = QFileDialog::getOpenFileName(m_app ? m_app->getMainWindow() : nullptr,
				QStringLiteral("Select cloud to classify"), QString(),
				QStringLiteral("LiDAR point clouds (*.las *.laz);;CloudCompare files (*.bin);;All files (*)"));
			if (path.isEmpty()) return;
			ccHObject* loaded = m_app ? m_app->loadFile(path, false) : nullptr;
			if (!loaded) { showError(QStringLiteral("CloudCompare could not load the selected classification cloud.")); return; }
			ccHObject::Container clouds;
			if (loaded->isA(CC_TYPES::POINT_CLOUD)) clouds.push_back(loaded);
			loaded->filterChildren(clouds, true, CC_TYPES::POINT_CLOUD);
			if (clouds.empty()) { delete loaded; showError(QStringLiteral("The selected file contains no point cloud.")); return; }
			m_app->addToDB(loaded, true, true, false, true);
			cloud = dynamic_cast<ccPointCloud*>(clouds.front());
		}
		else
		{
			const auto& selected = m_app->getSelectedEntities();
			for (ccHObject* entity : selected)
			{
				if ((cloud = dynamic_cast<ccPointCloud*>(entity))) break;
				ccHObject::Container clouds;
				entity->filterChildren(clouds, true, CC_TYPES::POINT_CLOUD);
				if (!clouds.empty()) { cloud = dynamic_cast<ccPointCloud*>(clouds.front()); break; }
			}
			if (!cloud) cloud = m_selectedCloud;
		}
		if (!cloud) { showError(QStringLiteral("Select a loaded point cloud in CloudCompare's DB Tree first.")); return; }
		m_app->setSelectedInDB(cloud, true);
		selectCloud(cloud);
		if (!session()) return;
		m_predictionCloudUid = static_cast<quint64>(cloud->getUniqueID());
		updateDockState();
		showReady(QStringLiteral("Classification target selected: %1. Prepare the feature schema required by the model, then Predict.").arg(cloud->getName()));
	}

	void WorkspaceController::restoreLabels()
	{
		ALiSSession* s = session(); if (!s) return; QString error; const std::vector<unsigned> indices = s->visibleSubset(error);
		if (indices.empty() || !s->restoreOriginalAsprs(indices, error)) { showError(error); return; }
		markCloudProfileStale(QStringLiteral("original ASPRS labels were restored")); refreshHost(); updateDockState();
	}

	void WorkspaceController::clearArchaeology()
	{
		ALiSSession* s = session(); if (!s) return; QString error; const std::vector<unsigned> indices = s->visibleSubset(error);
		if (indices.empty() || !s->clearArchaeology(indices, error)) { showError(error); return; } refreshHost(); updateDockState();
	}

	void WorkspaceController::displayMaskMode(bool ground)
	{
		ALiSSession* s = session(); if (!s || !s->hasAppliedGround()) return;
		const std::vector<bool> mask = s->appliedGroundMask(); auto& visibility = m_selectedCloud->getTheVisibilityArray(); visibility.resize(m_selectedCloud->size());
		for (unsigned i = 0; i < m_selectedCloud->size(); ++i) visibility[i] = (mask[i] == ground) ? CCCoreLib::POINT_VISIBLE : CCCoreLib::POINT_HIDDEN;
		refreshHost();
	}

	void WorkspaceController::visualize(const QString& mode)
	{
		ALiSSession* s = session(); if (!s) return; QString error;
		if (mode == QStringLiteral("original")) { m_selectedCloud->unallocateVisibilityArray(); m_selectedCloud->showSF(false); m_selectedCloud->showColors(m_selectedCloud->hasColors()); }
		else if (mode == QStringLiteral("ground")) displayMaskMode(true);
		else if (mode == QStringLiteral("non_ground")) displayMaskMode(false);
		else if (mode == QStringLiteral("hag")) { m_selectedCloud->unallocateVisibilityArray(); s->displayScalarField(QString::fromUtf8(field::HeightAboveGround), error); }
		else if (mode == QStringLiteral("asprs")) { m_selectedCloud->unallocateVisibilityArray(); s->displayScalarField(QString::fromUtf8(field::WorkingAsprs), error); }
		else if (mode == QStringLiteral("prediction"))
		{
			if (m_predictionConfidenceFilterEnabled) { filterPredictionByConfidence(true, m_predictionConfidenceThreshold); return; }
			m_selectedCloud->unallocateVisibilityArray(); s->displayScalarField(QString::fromUtf8(field::AsprsPrediction), error);
		}
		else if (mode == QStringLiteral("prediction_confidence")) { m_selectedCloud->unallocateVisibilityArray(); s->displayScalarField(QString::fromUtf8(field::AsprsConfidence), error); }
		else if (mode.startsWith(QStringLiteral("sf:"))) { s->displayScalarField(mode.mid(3), error); }
		if (!error.isEmpty()) { showError(error); return; }
		refreshHost();
		updateDockState();
	}
}
