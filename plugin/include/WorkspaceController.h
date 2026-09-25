// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "FeatureEngine.h"
#include "GroundFilter.h"
#include "SessionModel.h"
#include "TerrainEngine.h"
#include "WorkspaceLogic.h"
#include "ProcessingPreset.h"

#include <QObject>
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <QPointer>

#include <memory>
#include <limits>
#include <unordered_map>
#include <vector>

class ccMainAppInterface;
class ccPointCloud;
class QProcess;

namespace alis
{
	class WorkspaceDock;
	class AnnotationStudio;

	class WorkspaceController final : public QObject
	{
		Q_OBJECT

	public:
		WorkspaceController(ccMainAppInterface* app, WorkspaceDock* dock, QObject* parent = nullptr);
		~WorkspaceController() override;

		void selectCloud(ccPointCloud* cloud);
		ccPointCloud* selectedCloud() const;
		ALiSSession* session() const;
		void openAnnotationStudio();
		// Shared entry point for the interval panel and reproducible headless tests.
		bool applyFeatureRanges(const QJsonArray& rules, const QString& directory, QString& error);
		bool refineVegetationWithRanges(const QString& directory, QString& error);

	private:
		ALiSSession* ensureSession(QString& error);
		void connectDock();
		void updateDockState(const QString& status = QString());
		void refreshHost();
		void syncDisplayControls();
		void refreshCloudProfile(bool measureSpacing, bool forceScan);
		void markCloudProfileStale(const QString& reason);
		ProcessingPreset captureProcessingPreset() const;
		void applyProcessingPreset(const ProcessingPreset& preset);
		void saveProcessingPreset();
		void importProcessingPreset();
		void loadProcessingPreset(const QString& path,bool addToLibrary=false);
		void adaptProcessingPreset();
		void reloadProcessingLibrary();
		bool publishComputedFeatures(const std::vector<FeatureRequest>& requests,QStringList& names,QString& error);
		void showError(const QString& message);
		void showReady(const QString& message);
		void recordGroundRun(const GroundFilterResult& result);
		bool requireMetricUnits(const QString& operation);
		void syncGroundParametersToDock();
		void resetGroundDerivedState();
		void performUndoRedo(bool redo);
		GroundFilterParameters currentGroundParameters() const;
		FeatureId featureIdFromUi(const QString& id, bool& valid) const;
		QString featureUiId(FeatureId id) const;
		FeatureKey featureKey(const QString& id, double radius, bool& valid) const;
		void updateModelFeatureChoices();
		void displayMaskMode(bool ground);
		bool parseModelFeatureKey(const QString& serialized, FeatureKey& key) const;
		QString serializeModelFeatureKey(const FeatureKey& key) const;
		bool writeMlDataset(const QStringList& serializedKeys, const QString& outputDirectory, QString& error,
		                    bool requireMetricCoordinates = true);
		QString createMlJobDirectory(const QString& prefix, QString& error) const;
		void exportModelDataset(const QStringList& featureKeys, const QString& repositoryPath);
		void trainModels(const QStringList& featureKeys,
		                const QString& pythonExecutable,
		                const QString& workerScript,
		                const QString& modelPath,
		                const QString& repositoryPath,
		                const QStringList& modelSpecifications,
		                double blockSizeMetres,
		                double testFraction,
		                int seed,
		                const QString& validationMode);
		void trainModel(const QStringList& featureKeys,
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
		                const QString& validationMode, double pointnetRadius = 0.5, int pointnetNeighbors = 64, double validationBuffer = 0.0);
		void predictModel(const QStringList& featureKeys,
		                  const QString& pythonExecutable,
		                  const QString& workerScript,
		                  const QString& modelPath,
		                  const QString& repositoryPath,
		                  int chunkSize);
		void predictModels(const QStringList& featureKeys,
		                   const QString& pythonExecutable,
		                   const QString& workerScript,
		                   const QStringList& modelPaths,
		                   const QString& repositoryPath,
		                   int chunkSize);
		void startNextPredictionRun();
		bool preservePredictionFields(const QString& modelPath, QString& label, QString& error);
		void filterPredictionByConfidence(bool enabled, double minimumConfidence);
		void showPredictionStatistics();
		void applyModelPredictions(double minimumConfidence);
		void showModelResults(const QString& modelPath = QString());
		void launchMlWorker(const QString& operation,
		                    const QString& pythonExecutable,
		                    const QString& workerScript,
		                    const QStringList& arguments);
		void readMlStandardOutput();
		void finishMlWorker(int exitCode, int exitStatus);
		void startNextTrainingRun();
		void cancelMlWorker();
		bool loadPredictionBundle(const QString& directory, QString& error);
		void inspectBootstrapCapabilities(const QString& pythonExecutable, const QString& workerScript);
		void bootstrapLabels(const QStringList& featureKeys,
		                     const QString& pythonExecutable,
		                     const QString& workerScript,
		                     const QString& repositoryPath,
		                     const QString& algorithm,
		                     int clusterCount,
		                     int fitSample,
		                     int chunkSize,
		                     int seed);
		bool loadBootstrapBundle(const QString& directory, QString& error);
		void installPretrainedModel(const QString& modelId,
		                            const QString& pythonExecutable,
		                            const QString& workerScript,
		                            const QString& repositoryPath);
		void removePretrainedModel(const QString& modelId,
		                           const QString& pythonExecutable,
		                           const QString& workerScript,
		                           const QString& repositoryPath);
		void maintainPretrainedModels(const QString& action,
		                              const QString& pythonExecutable,
		                              const QString& workerScript,
		                              const QString& repositoryPath);

		void runGroundPreview();
		void applyGround();
		void discardGround();
		void restoreGround();
		void assignGroundToAsprs();
		void computeDtm(double gridStepMetres, bool fillEmptyCells, double maximumInterpolationEdgeMetres);
		void computeHag();
		void suggestScales();
		void previewVegetation(int preset, double maximumRadius, int minimumNeighbors,
		                       double lowHeight, double mediumHeight, const QString& outputDirectory,
		                       int targetNeighbors = 16, double fineRadiusLimit = .25);
		void predictVegetationModel(const QString& model, const QString& outputDirectory,
		                            const QString& python, const QString& worker, bool trusted);
		bool loadVegetationScores(QString& error);
		void filterVegetationScore(double threshold);
		void openFeatureRangeReview();
		void extractVegetation();
		void computeFeatures(const QStringList& ids, const QVector<double>& radiiMetres);
		void selectTrainingSet(bool loadFromFile, bool externalTest);
		void selectClassificationCloud(bool loadFromFile);
		void applyLabels(int asprsCode, int archaeologyCode);
		void restoreLabels();
		void clearArchaeology();
		void visualize(const QString& mode);

		ccMainAppInterface* m_app = nullptr;
		WorkspaceDock* m_dock = nullptr;
		ccPointCloud* m_selectedCloud = nullptr;
		QPointer<AnnotationStudio> m_annotationStudio;
		std::unordered_map<quint64, std::unique_ptr<ALiSSession>> m_sessions;
		std::unordered_map<quint64, std::unique_ptr<FeatureEngine>> m_featureEngines;
		std::unordered_map<quint64, QJsonObject> m_cloudProfileCache;
		std::unique_ptr<GroundFilter> m_groundFilter;
		QString m_groundAlgorithmId = QStringLiteral("csf.cloudcompare.v2.13.2");
		TerrainEngine m_terrainEngine;
		GroundParameters m_groundParameters;
		GroundParameters m_groundBaseParameters;
		QJsonObject m_cloudProfile;
		QString m_processingPresetName = QStringLiteral("CloudCompare reference");
		QString m_pmfWindowSizes = QStringLiteral("3, 6, 12, 20");
		QString m_pmfThresholds = QStringLiteral("0.5, 0.8, 1.4, 2.0");
		double m_pmfCellSizeMetres = 0.5;
		bool m_featureComputationActive = false;
		bool m_metricUnitsConfirmed = false;
		bool m_metricUnitsLockedByMetadata = false;
		bool m_metricUnitsRemembered = false;
		bool m_dtmAvailable = false;
		std::unordered_map<quint64, bool> m_explicitMetricConfirmations;
		double m_estimatedSpacingMetres = 0.0;
		double m_estimatedDensityPerSquareMetre = 0.0;
		std::unique_ptr<TerrainResult> m_terrainResult;
		QProcess* m_mlProcess = nullptr;
		QString m_mlOperation;
		QString m_mlDatasetPath;
		QStringList m_mlDatasetFeatureKeys;
		quint64 m_mlDatasetRevision = std::numeric_limits<quint64>::max();
		quint64 m_mlDatasetCloudUid = 0;
		QString m_mlModelPath;
		QString m_mlPredictionPath;
		QString m_bootstrapOutputPath;
		QString m_vegetationOutputPath;
		quint64 m_vegetationSourceRevision = 0;
		QString m_mlSummary;
		QString m_mlPredictionStatistics;
		std::unordered_map<quint64, QString> m_predictionStatisticsCache;
		QStringList m_mlReportPaths;
		QString m_mlClassifierId;
		QString m_mlRequestedDevice;
		QByteArray m_mlStdoutBuffer;
		QByteArray m_mlStderrBuffer;
		quint64 m_mlSourceUid = 0;
		qint64 m_mlStartedMilliseconds = 0;
		bool m_mlCancelled = false;
		quint64 m_trainingCloudUid = 0;
		quint64 m_testCloudUid = 0;
		quint64 m_predictionCloudUid = 0;
		QString m_mlExternalTestDatasetPath;
		struct PendingModelRun
		{
			QStringList featureKeys;
			QString pythonExecutable;
			QString workerScript;
			QString modelPath;
			QString repositoryPath;
			QString classifierId;
			QString device;
			int trees = 300;
			int epochs = 30;
			int batchSize = 4096;
			double learningRate = 0.001;
			int maxDepth = 0;
			int minimumLeaf = 1;
			QString maxFeatures = QStringLiteral("sqrt");
			QString classWeight = QStringLiteral("balanced_subsample");
			QString criterion = QStringLiteral("gini");
			double subsample = 0.9;
			double columnSample = 0.9;
			bool imputeMissing = true;
			double pointnetRadius = 0.5;
			int pointnetNeighbors = 64;
			double validationBuffer = 0.0;
			double blockSizeMetres = 20.0;
			double testFraction = 0.2;
			int seed = 42;
			QString validationMode;
		};
		QList<PendingModelRun> m_pendingModelRuns;
		int m_trainingBatchTotal = 0;
		int m_trainingBatchCompleted = 0;
		struct PendingPredictionRun
		{
			QStringList featureKeys;
			QString pythonExecutable;
			QString workerScript;
			QString modelPath;
			QString repositoryPath;
			int chunkSize = 250000;
		};
		QList<PendingPredictionRun> m_pendingPredictionRuns;
		QStringList m_predictionComparisonStatistics;
		int m_predictionBatchTotal = 0;
		int m_predictionBatchCompleted = 0;
		bool m_predictionConfidenceFilterEnabled = false;
		double m_predictionConfidenceThreshold = 0.0;
	};
}
