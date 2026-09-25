// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDockWidget>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QVector>
#include "ProcessingPreset.h"

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;

namespace alis
{
	//! UI-only Archaeological LiDAR Studio.
	/** The dock owns no point-cloud or processing state. Controllers connect its
	 *  command signals to a Session Model and report state through the public slots.
	 */
	class WorkspaceDock final : public QDockWidget
	{
		Q_OBJECT

	public:
		enum class StatusTone
		{
			Neutral,
			Ready,
			Warning,
			Error
		};

		explicit WorkspaceDock(QWidget* parent = nullptr);

		QVector<double> scaleRadii() const;
		QStringList selectedFeatureIds() const;
		QStringList selectedModelFeatureKeys() const;
		QStringList selectedBootstrapFeatureKeys() const;
		QStringList selectedModelSpecifications() const;
		QStringList selectedCatalogModelPaths() const;
		QString modelRepositoryPath() const;
		ProcessingPreset processingPreset() const;
		void setProcessingPreset(const ProcessingPreset& preset);
		void setPresetLibrary(const QStringList& paths,const QStringList& names);
		void setCloudProfileText(const QString& text);
		void setProcessingSuggestion(const QString& text);
		void showAnnotationWorkspace();
		void showPrepareFeatures();
		void setModelRepositoryPath(const QString& path);
		void setCatalogCloudProfile(const QJsonObject& profile);
		void refreshModelCatalog();
		void refreshPretrainedCatalog();

	public Q_SLOTS:
		void setSessionInfo(const QString& cloudName,
		                    quint64 entityUid,
		                    quint64 pointCount,
		                    const QString& unitDescription,
		                    bool dirty);
		void clearSession();
		void setSessionDirty(bool dirty);
		void setMetricUnitConfirmation(bool confirmed, bool lockedByMetadata);
		void setStatus(const QString& text, StatusTone tone = StatusTone::Neutral);
		void setBusy(bool busy);
		void setProgress(const QString& operation, int value, int maximum, bool cancellable = false);
		void clearProgress();

		void setGroundState(bool previewAvailable,
		                    bool groundApplied,
		                    bool dtmAvailable,
		                    bool hagAvailable,
		                    const QString& summary = QString());
		void setAdvancedGroundParameters(double clothResolution,
		                                 double classificationThreshold,
		                                 double timeStep,
		                                 int rigidness,
		                                 int iterations,
		                                 bool slopeProcessing);
		void setPmfParameters(const QString& windowSizes, const QString& heightThresholds, double cellSize = 0.5);
		void resetSimpleGroundControls();
		void setScaleRadii(const QVector<double>& radii);
		void showScaleSuggestion(const QVector<double>& radii, const QString& explanation);
		void setModelFeatureChoices(const QStringList& keys, const QStringList& displayNames);
		void setBootstrapFeatureChoices(const QStringList& keys, const QStringList& displayNames, const QStringList& excludedByDefault);
		void setModelWorkerPaths(const QString& pythonExecutable,
		                         const QString& workerScript,
		                         const QString& modelPath,
		                         const QString& repositoryPath = QString());
		void setModelState(quint64 trustedTrainingPoints,
		                   const QString& datasetPath,
		                   bool modelAvailable,
		                   bool predictionsAvailable,
		                   const QString& summary = QString());
		void setTrainingSetState(const QString& trainingCloud, const QString& testCloud);
		void setClassificationTargetState(const QString& cloudName);
		void setVisibleSubset(const QString& description, quint64 pointCount);
		void setUndoRedoAvailable(bool undoAvailable, bool redoAvailable);
		void appendHistoryEntry(const QString& timestamp,
		                        const QString& operation,
		                        const QString& details);
		void clearHistory();
		void setDisplayFields(const QStringList& names, const QString& activeField, bool hasVisibilityMask);
		void setDisplayPalettes(const QStringList& ids, const QStringList& names, const QString& activeId);
		void setDisplayRange(bool enabled, double minimum, double maximum, double start, double stop);
		void setDisplayLegendState(bool enabled, bool visible);
		void setPredictionStatistics(const QString& text);
		void setBootstrapStatus(const QString& text, bool myriaAvailable = false);

	Q_SIGNALS:
		void visualizationRequested(const QString& visualizationId);
		void displayPaletteRequested(const QString& uuid);
		void displayRangeRequested(double start, double stop);
		void displayRangeResetRequested();
		void displayPaletteEditorRequested();
		void displayRefreshRequested();
		void displayLegendVisibilityRequested(bool visible);
		void metricUnitsConfirmationChanged(bool confirmed);

		void groundAlgorithmChanged(const QString& algorithmId);
		void cloudProfileRequested();
		void saveProcessingPresetRequested();
		void importProcessingPresetRequested();
		void loadProcessingPresetRequested(const QString& path);
		void adaptProcessingPresetRequested();
		void groundPresetChanged(int presetIndex);
		void simpleGroundControlsChanged(int terrainComplexity,
		                                 int preserveMicrorelief,
		                                 int vegetationDensity);
		void advancedGroundParametersChanged(double clothResolution,
		                                     double classificationThreshold,
		                                     double timeStep,
		                                     int rigidness,
		                                     int iterations,
		                                     bool slopeProcessing);
		void pmfParametersChanged(const QString& windowSizes, const QString& heightThresholds, double cellSize);
		void groundPreviewRequested();
		void groundApplyRequested();
		void groundDiscardRequested();
		void groundRestoreRequested();
		void assignGroundToAsprsRequested();
		void preprocessingActionRequested(const QString& actionId);
		void computeDtmRequested(double gridStep, bool fillEmptyCells, double maximumInterpolationEdgeLength);
		void computeHagRequested();
		void annotationActionRequested(const QString& actionId);

		void scaleRadiiChanged(const QVector<double>& radii);
		void suggestScalesRequested();
		void saveScalePresetRequested(const QVector<double>& radii);
		void featureSelectionChanged(const QStringList& featureIds);
		void computeFeaturesRequested(const QStringList& featureIds, const QVector<double>& radii);
		void compareScalesRequested(const QString& featureId);

		void exportModelDatasetRequested(const QStringList& featureKeys, const QString& repositoryPath);
		//! loadFromFile=false uses the cloud selected in CloudCompare; true opens LAS/LAZ.
		void selectTrainingSetRequested(bool loadFromFile, bool externalTest);
		void selectClassificationCloudRequested(bool loadFromFile);
		void trainModelRequested(const QStringList& featureKeys,
		                         const QString& pythonExecutable,
		                         const QString& workerScript,
		                         const QString& modelPath,
		                         const QString& repositoryPath,
		                         const QStringList& modelSpecifications,
		                         double spatialBlockMetres,
		                         double testFraction,
		                         int seed,
		                         const QString& validationMode);
		void predictModelRequested(const QStringList& featureKeys,
		                           const QString& pythonExecutable,
		                           const QString& workerScript,
		                           const QString& modelPath,
		                           const QString& repositoryPath,
		                           int chunkSize);
		void applyModelPredictionsRequested(double minimumConfidence);
		void compareModelPredictionsRequested(const QStringList& featureKeys,
		                                     const QString& pythonExecutable,
		                                     const QString& workerScript,
		                                     const QStringList& modelPaths,
		                                     const QString& repositoryPath,
		                                     int chunkSize);
		void predictionConfidenceFilterRequested(bool enabled, double minimumConfidence);
		void showPredictionStatisticsRequested();
		void showModelResultsRequested(const QString& modelPath);
		void bootstrapCapabilitiesRequested(const QString& pythonExecutable, const QString& workerScript);
		void vegetationPreviewRequested(int preset, double maximumRadius, int minimumNeighbors,
		                                double lowHeight, double mediumHeight, const QString& outputDirectory,
		                                int targetNeighbors = 16, double fineRadiusLimit = .25);
		void vegetationModelRequested(const QString& model, const QString& outputDirectory,
		                              const QString& python, const QString& worker, bool trusted);
		void vegetationThresholdRequested(double threshold);
		void featureRangeReviewRequested();
		void vegetationFeatureRefinementRequested(const QString& directory);
		void vegetationExtractRequested();
		void bootstrapLabelsRequested(const QStringList& featureKeys,
		                              const QString& pythonExecutable,
		                              const QString& workerScript,
		                              const QString& repositoryPath,
		                              const QString& algorithm,
		                              int clusterCount,
		                              int fitSample,
		                              int chunkSize,
		                              int seed);
		void pretrainedInstallRequested(const QString& modelId,
		                                const QString& pythonExecutable,
		                                const QString& workerScript,
		                                const QString& repositoryPath);
		void pretrainedRemoveRequested(const QString& modelId,
		                               const QString& pythonExecutable,
		                               const QString& workerScript,
		                               const QString& repositoryPath);
		void pretrainedMaintenanceRequested(const QString& action,
		                                    const QString& pythonExecutable,
		                                    const QString& workerScript,
		                                    const QString& repositoryPath);

		//! Label application always targets the current visible subset.
		/** A value of -1 means "do not change" for that classification domain. */
		void applyLabelsRequested(int asprsCode, int archaeologyCode);
		void restoreOriginalLabelsRequested();
		void clearArchaeologyRequested();
		void undoRequested();
		void redoRequested();

		void cancelProcessingRequested();

	private:
		void buildSessionTab();
		void buildPrepareArea();
		void buildPreprocessingTab();
		void buildAnnotationArea();
		void buildTerrainTab();
		void buildFeaturesTab();
		void buildVegetationTab();
		void buildModelsTab();
		void buildAnnotationTab();
		void buildLabelsTab();
		void buildHistoryTab();
		void registerCommandButton(QAbstractButton* button);
		void emitSimpleGroundControls();
		void emitAdvancedGroundParameters();
		void emitPmfParameters();
		void updateScaleEditors();
		void updateCommandAvailability();
		void updateModelWorkflowGuide();
		void useSelectedCatalogModel();

		QTabWidget* m_tabs = nullptr;
		QTabWidget* m_prepareTabs = nullptr;
		QTabWidget* m_annotationTabs = nullptr;
		QTabWidget* m_modelsTabs = nullptr;

		QLabel* m_cloudNameValue = nullptr;
		QLabel* m_entityUidValue = nullptr;
		QLabel* m_pointCountValue = nullptr;
		QLabel* m_unitsValue = nullptr;
		QCheckBox* m_metricUnitsCheck = nullptr;
		QLabel* m_dirtyValue = nullptr;
		QComboBox* m_visualizationCombo = nullptr;
		QGroupBox* m_displayPanel = nullptr;
		QComboBox* m_displayPaletteCombo = nullptr;
		QDoubleSpinBox* m_displayMinSpin = nullptr;
		QDoubleSpinBox* m_displayMaxSpin = nullptr;
		QPushButton* m_displayAutoButton = nullptr;
		QPushButton* m_displayEditButton = nullptr;
		QCheckBox* m_displayLegendCheck = nullptr;

		QComboBox* m_groundAlgorithmCombo = nullptr;
		QLabel* m_cloudProfileValue = nullptr;
		QLabel* m_processingSuggestion = nullptr;
		QComboBox* m_savedPresetCombo = nullptr;
		QDoubleSpinBox* m_presetMinRadius = nullptr;
		QDoubleSpinBox* m_presetMaxRadius = nullptr;
		QCheckBox* m_presetUseReturns = nullptr;
		QComboBox* m_groundPresetCombo = nullptr;
		QLabel* m_groundPresetGuidance = nullptr;
		QComboBox* m_terrainComplexityCombo = nullptr;
		QComboBox* m_microreliefCombo = nullptr;
		QComboBox* m_vegetationCombo = nullptr;
		QDoubleSpinBox* m_clothResolutionSpin = nullptr;
		QDoubleSpinBox* m_classificationThresholdSpin = nullptr;
		QDoubleSpinBox* m_timeStepSpin = nullptr;
		QComboBox* m_csfSceneCombo = nullptr;
		QSpinBox* m_iterationsSpin = nullptr;
		QCheckBox* m_slopeProcessingCheck = nullptr;
		QGroupBox* m_csfParametersGroup = nullptr;
		QGroupBox* m_pmfParametersGroup = nullptr;
		QLineEdit* m_pmfWindowsEdit = nullptr;
		QLineEdit* m_pmfThresholdsEdit = nullptr;
		QComboBox* m_pmfPresetCombo = nullptr;
		QDoubleSpinBox* m_pmfCellSizeSpin = nullptr;
		QDoubleSpinBox* m_dtmGridStepSpin = nullptr;
		QCheckBox* m_dtmFillEmptyCheck = nullptr;
		QDoubleSpinBox* m_dtmMaxEdgeSpin = nullptr;
		QPushButton* m_groundPreviewButton = nullptr;
		QPushButton* m_groundApplyButton = nullptr;
		QPushButton* m_groundDiscardButton = nullptr;
		QPushButton* m_groundRestoreButton = nullptr;
		QPushButton* m_computeDtmButton = nullptr;
		QPushButton* m_openDemButton = nullptr;
		QPushButton* m_computeHagButton = nullptr;
		QLabel* m_groundSummaryValue = nullptr;

		QListWidget* m_scaleList = nullptr;
		QDoubleSpinBox* m_newScaleSpin = nullptr;
		QPushButton* m_addScaleButton = nullptr;
		QPushButton* m_removeScaleButton = nullptr;
		QPushButton* m_suggestScalesButton = nullptr;
		QPushButton* m_saveScalePresetButton = nullptr;
		QLabel* m_scaleExplanationValue = nullptr;
		QListWidget* m_featureList = nullptr;
		QPushButton* m_computeFeaturesButton = nullptr;

		QListWidget* m_modelFeatureList = nullptr;
		QListWidget* m_bootstrapFeatureList = nullptr;
		QComboBox* m_bootstrapEngineCombo = nullptr;
		QComboBox* m_bootstrapQualityCombo = nullptr;
		QSpinBox* m_bootstrapClusterSpin = nullptr;
		QSpinBox* m_bootstrapFitSampleSpin = nullptr;
		QSpinBox* m_bootstrapChunkSpin = nullptr;
		QPushButton* m_bootstrapRunButton = nullptr;
		QLabel* m_bootstrapStatusValue = nullptr;
		QLabel* m_trustedTrainingValue = nullptr;
		QLabel* m_trainingCloudValue = nullptr;
		QLabel* m_testCloudValue = nullptr;
		QPushButton* m_selectLoadedTrainingButton = nullptr;
		QPushButton* m_loadTrainingButton = nullptr;
		QComboBox* m_validationModeCombo = nullptr;
		QPushButton* m_selectLoadedTestButton = nullptr;
		QPushButton* m_loadTestButton = nullptr;
		QLineEdit* m_pythonExecutableEdit = nullptr;
		QLineEdit* m_workerScriptEdit = nullptr;
		QLineEdit* m_modelPathEdit = nullptr;
		QLineEdit* m_modelRepositoryEdit = nullptr;
		QTreeWidget* m_modelCatalogTree = nullptr;
		QLabel* m_modelCatalogSummary = nullptr;
		QPushButton* m_refreshCatalogButton = nullptr;
		QPushButton* m_useCatalogModelButton = nullptr;
		QList<QAbstractButton*> m_modelCardChecks;
		QList<QComboBox*> m_modelCardDevices;
		QList<QSpinBox*> m_modelCardPrimaryParameters;
		QList<QSpinBox*> m_modelCardBatchSizes;
		QList<QDoubleSpinBox*> m_modelCardLearningRates;
		QList<QSpinBox*> m_modelCardMaxDepths;
		QList<QSpinBox*> m_modelCardMinimumLeaves;
		QList<QComboBox*> m_modelCardMaxFeatures;
		QList<QComboBox*> m_modelCardClassWeights;
		QList<QComboBox*> m_modelCardCriteria;
		QList<QDoubleSpinBox*> m_modelCardSubsamples;
		QList<QDoubleSpinBox*> m_modelCardColumnSamples;
		QList<QCheckBox*> m_modelCardImputeMissing;
		QDoubleSpinBox* m_modelBlockSizeSpin = nullptr;
		QDoubleSpinBox* m_modelTrainFractionSpin = nullptr;
		QDoubleSpinBox* m_modelTestFractionSpin = nullptr;
		QSpinBox* m_modelSeedSpin = nullptr;
		QSpinBox* m_predictionChunkSpin = nullptr;
		QDoubleSpinBox* m_predictionConfidenceSpin = nullptr;
		QCheckBox* m_predictionConfidenceFilterCheck = nullptr;
		QPushButton* m_exportDatasetButton = nullptr;
		QPushButton* m_trainModelButton = nullptr;
		QPushButton* m_predictModelButton = nullptr;
		QPushButton* m_applyPredictionsButton = nullptr;
		QPushButton* m_viewModelResultsButton = nullptr;
		QPushButton* m_comparePredictionsButton = nullptr;
		QPushButton* m_viewPredictionStatisticsButton = nullptr;
		QPushButton* m_selectLoadedClassificationButton = nullptr;
		QPushButton* m_loadClassificationButton = nullptr;
		QLabel* m_classificationCloudValue = nullptr;
		QLabel* m_modelDatasetValue = nullptr;
		QLabel* m_modelSummaryValue = nullptr;
		QList<QLabel*> m_modelWorkflowLabels;

		QComboBox* m_annotationSectionAxisCombo = nullptr;
		QDoubleSpinBox* m_annotationSectionThicknessSpin = nullptr;

		QLabel* m_visibleSubsetValue = nullptr;
		QComboBox* m_asprsCombo = nullptr;
		QPushButton* m_applyLabelsButton = nullptr;
		QPushButton* m_restoreOriginalButton = nullptr;
		QPushButton* m_undoButton = nullptr;
		QPushButton* m_redoButton = nullptr;

		QTreeWidget* m_historyTree = nullptr;
		QLabel* m_statusValue = nullptr;
		QLabel* m_progressOperationValue = nullptr;
		QProgressBar* m_progressBar = nullptr;
		QPushButton* m_cancelButton = nullptr;
		QList<QAbstractButton*> m_commandButtons;

		bool m_hasSession = false;
		bool m_metricUnitsConfirmed = false;
		bool m_metricUnitsLockedByMetadata = false;
		QStringList m_cachedFeatureIds;
		QStringList m_cachedFeatureNames;
		QVector<double> m_cachedFeatureRadii;
		bool m_busy = false;
		bool m_previewAvailable = false;
		bool m_groundApplied = false;
		bool m_dtmAvailable = false;
		bool m_hagAvailable = false;
		bool m_computedFeatureAvailable = false;
		bool m_modelAvailable = false;
		bool m_predictionsAvailable = false;
		bool m_classificationTargetSelected = false;
		quint64 m_trustedTrainingPoints = 0;
		bool m_undoAvailable = false;
		bool m_redoAvailable = false;
		bool m_updatingScales = false;
		QJsonObject m_catalogCloudProfile;
	};
}
