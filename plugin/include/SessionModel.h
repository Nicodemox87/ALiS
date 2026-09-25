// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "WorkspaceLogic.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <vector>

class ccPointCloud;

namespace alis
{
	namespace field
	{
		extern const char* const OriginalClassification;
		extern const char* const WorkingAsprs;
		extern const char* const ArchaeologyClass;
		extern const char* const TrainingLabel;
		//! Last manually confirmed ASPRS class; NaN means unverified for this domain.
		extern const char* const AsprsTrainingClass;
		extern const char* const GroundPreview;
		extern const char* const GroundMask;
		extern const char* const HeightAboveGround;
		extern const char* const AsprsPrediction;
		extern const char* const AsprsConfidence;
		extern const char* const BootstrapCluster;
		extern const char* const BootstrapConfidence;
	}

	struct ProcessingRecord
	{
		QString operation;
		QDateTime timestampUtc;
		QJsonObject parameters;
		quint64 sourceEntityUid = 0;
		quint64 affectedPoints = 0;
		QStringList outputFields;
		qint64 elapsedMilliseconds = 0;

		QJsonObject toJson() const;
	};
	bool isTrustedAsprsLabel(float workingClass, float manuallyConfirmedClass);

	struct SessionBounds
	{
		double minimum[3] = {0.0, 0.0, 0.0};
		double maximum[3] = {0.0, 0.0, 0.0};
		bool valid = false;
	};

	class ALiSSession
	{
	public:
		explicit ALiSSession(ccPointCloud& cloud);

		ccPointCloud& cloud() const;
		quint64 entityUid() const;
		quint64 pointCount() const;
		const SessionBounds& bounds() const;
		bool isCompatibleWith(const ccPointCloud& cloud) const;

		//! Creates aligned attribute fields only; XYZ stays owned by the source cloud.
		bool initialize(QString& errorMessage);
		bool initialized() const;
		bool dirty() const;
		quint64 sourceRevision() const;
		quint64 groundRevision() const;
		quint64 hagRevision() const;

		bool setGroundPreview(const std::vector<bool>& mask, QString& errorMessage);
		bool hasGroundPreview() const;
		bool discardGroundPreview(QString& errorMessage);
		bool applyGroundPreview(QString& errorMessage);
		//! Restores only when the top undo transaction is a Ground edit.
		bool restoreGround(QString& message);
		bool hasAppliedGround() const;
		std::vector<bool> appliedGroundMask() const;
		bool assignGroundToWorkingAsprs(QString& errorMessage);

		bool applyManualAsprs(const std::vector<unsigned>& pointIndices, std::uint8_t code, QString& errorMessage);
		//! Copies a chosen integer Scalar Field into ASPRS Working and certifies it as Manual/Trusted.
		bool trustAsprsFromField(const QString& sourceFieldName, QString& errorMessage);
		bool restoreOriginalAsprs(const std::vector<unsigned>& pointIndices, QString& errorMessage);
		bool applyManualArchaeology(const std::vector<unsigned>& pointIndices, ArchaeologyClass value, QString& errorMessage);
		bool clearArchaeology(const std::vector<unsigned>& pointIndices, QString& errorMessage);

		//! Number of index-aligned ASPRS labels explicitly marked Manual/Trusted.
		quint64 trustedTrainingCount() const;
		//! Stores model output as Derived fields; ASPRS Working is never changed here.
		bool setAsprsPredictions(const std::vector<std::int16_t>& predictions,
		                         const std::vector<float>& confidence,
		                         const QJsonObject& provenance,
		                         qint64 elapsedMilliseconds,
		                         QString& errorMessage);
		//! Stores exploratory cluster IDs as Derived fields only; they are never ASPRS or Trusted labels.
		bool setBootstrapClusters(const std::vector<std::int32_t>& clusters,
		                          const std::vector<float>& confidence,
		                          const QJsonObject& provenance,
		                          qint64 elapsedMilliseconds,
		                          QString& errorMessage);
		//! Explicit, thresholded and undoable promotion from Derived prediction to Working.
		bool applyAsprsPredictions(double minimumConfidence, QString& errorMessage);

		//! Returns a manual subset only when CloudCompare has a non-trivial visibility mask.
		std::vector<unsigned> visibleSubset(QString& errorMessage) const;

		bool setHagValues(const std::vector<double>& values, const QJsonObject& parameters,
			qint64 elapsedMilliseconds, QString& errorMessage);

		bool displayScalarField(const QString& name, QString& errorMessage);
		bool undo(QString& message);
		bool redo(QString& message);
		bool canUndo() const;
		bool canRedo() const;

		void addProcessingRecord(const ProcessingRecord& record);
		const std::vector<ProcessingRecord>& history() const;
		QJsonArray historyAsJson() const;

	private:
		struct ScalarCommand
		{
			QString description;
			QString fieldName;
			std::vector<unsigned> indices;
			std::vector<float> before;
			std::vector<float> after;
			//! Undo removes a field that did not exist before this command.
			bool removeFieldOnUndo = false;
			bool changesGround = false;
			bool changesHag = false;
		};
		struct Transaction
		{
			QString description;
			std::vector<ScalarCommand> commands;
		};

		int ensureScalarField(const char* name, float initialValue, QString& errorMessage);
		bool copyScalarField(const char* destination, const char* source, float missingValue, QString& errorMessage);
		bool removeScalarField(const char* name, QString& errorMessage);
		bool writeDenseField(const char* name, const std::vector<float>& values, QString& errorMessage);
		bool applyScalarCommand(ScalarCommand command, QString& errorMessage);
		bool applyTransaction(Transaction transaction, QString& errorMessage);
		bool writeCommandValues(const ScalarCommand& command, bool useAfter, QString& errorMessage);
		bool validateIndices(const std::vector<unsigned>& pointIndices, QString& errorMessage) const;
		void markChanged(bool ground, bool hag);
		void persistSessionMetadata();
		void recordTransaction(const Transaction& transaction, const QString& operation);

		ccPointCloud* m_cloud = nullptr;
		quint64 m_entityUid = 0;
		quint64 m_pointCount = 0;
		SessionBounds m_bounds;
		bool m_initialized = false;
		bool m_dirty = false;
		bool m_hasGroundPreview = false;
		quint64 m_sourceRevision = 0;
		quint64 m_groundRevision = 0;
		quint64 m_hagRevision = 0;
		std::vector<Transaction> m_undoStack;
		std::vector<Transaction> m_redoStack;
		std::vector<ProcessingRecord> m_history;
	};
}
