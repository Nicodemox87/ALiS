// SPDX-License-Identifier: GPL-2.0-or-later
#include "SessionModel.h"

#include <GenericIndexedCloudPersist.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>

#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace alis
{
	namespace field
	{
		const char* const OriginalClassification = "qAL_ASPRS_Original";
		const char* const WorkingAsprs = "qAL_ASPRS_Working";
		const char* const ArchaeologyClass = "qAL_ArchaeologyClass";
		const char* const TrainingLabel = "qAL_TrainingLabel";
		const char* const AsprsTrainingClass = "qAL_ASPRS_TrainingClass";
		const char* const GroundPreview = "qAL_GroundPreview";
		const char* const GroundMask = "qAL_GroundMask";
		const char* const HeightAboveGround = "qAL_HAG";
		const char* const AsprsPrediction = "qAL_ASPRS_Prediction";
		const char* const AsprsConfidence = "qAL_ASPRS_Confidence";
		const char* const BootstrapCluster = "qAL_BootstrapCluster";
		const char* const BootstrapConfidence = "qAL_BootstrapConfidence";
	}

	bool isTrustedAsprsLabel(float workingClass, float manuallyConfirmedClass)
	{
		return std::isfinite(workingClass) && workingClass >= 0 && workingClass <= 255
			&& std::floor(workingClass) == workingClass && workingClass == manuallyConfirmedClass;
	}

	QJsonObject ProcessingRecord::toJson() const
	{
		QJsonObject object;
		object.insert(QStringLiteral("operation"), operation);
		object.insert(QStringLiteral("timestampUtc"), timestampUtc.toUTC().toString(Qt::ISODateWithMs));
		object.insert(QStringLiteral("parameters"), parameters);
		object.insert(QStringLiteral("sourceEntityUid"), static_cast<double>(sourceEntityUid));
		object.insert(QStringLiteral("affectedPoints"), static_cast<double>(affectedPoints));
		QJsonArray outputs;
		for (const QString& fieldName : outputFields)
		{
			outputs.append(fieldName);
		}
		object.insert(QStringLiteral("outputFields"), outputs);
		object.insert(QStringLiteral("elapsedMilliseconds"), static_cast<double>(elapsedMilliseconds));
		return object;
	}

	ALiSSession::ALiSSession(ccPointCloud& cloud)
		: m_cloud(&cloud)
		, m_entityUid(static_cast<quint64>(cloud.getUniqueID()))
		, m_pointCount(static_cast<quint64>(cloud.size()))
	{
		CCVector3d minimum;
		CCVector3d maximum;
		m_bounds.valid = cloud.getOwnGlobalBB(minimum, maximum);
		if (m_bounds.valid)
		{
			m_bounds.minimum[0] = minimum.x;
			m_bounds.minimum[1] = minimum.y;
			m_bounds.minimum[2] = minimum.z;
			m_bounds.maximum[0] = maximum.x;
			m_bounds.maximum[1] = maximum.y;
			m_bounds.maximum[2] = maximum.z;
		}
	}

	ccPointCloud& ALiSSession::cloud() const { return *m_cloud; }
	quint64 ALiSSession::entityUid() const { return m_entityUid; }
	quint64 ALiSSession::pointCount() const { return m_pointCount; }
	const SessionBounds& ALiSSession::bounds() const { return m_bounds; }
	bool ALiSSession::initialized() const { return m_initialized; }
	bool ALiSSession::dirty() const { return m_dirty; }
	quint64 ALiSSession::sourceRevision() const { return m_sourceRevision; }
	quint64 ALiSSession::groundRevision() const { return m_groundRevision; }
	quint64 ALiSSession::hagRevision() const { return m_hagRevision; }
	bool ALiSSession::hasGroundPreview() const { return m_hasGroundPreview; }

	bool ALiSSession::isCompatibleWith(const ccPointCloud& candidate) const
	{
		return m_cloud == &candidate
			&& m_entityUid == static_cast<quint64>(candidate.getUniqueID())
			&& m_pointCount == static_cast<quint64>(candidate.size());
	}

	int ALiSSession::ensureScalarField(const char* name, float initialValue, QString& errorMessage)
	{
		int index = m_cloud->getScalarFieldIndexByName(name);
		if (index >= 0)
		{
			ccScalarField* scalarField = static_cast<ccScalarField*>(m_cloud->getScalarField(index));
			if (!scalarField || scalarField->size() != m_cloud->size())
			{
				errorMessage = QStringLiteral("Scalar field '%1' is not aligned with the source cloud.").arg(QString::fromUtf8(name));
				return -1;
			}
			return index;
		}
		index = m_cloud->addScalarField(name);
		if (index < 0)
		{
			errorMessage = QStringLiteral("Unable to allocate scalar field '%1'.").arg(QString::fromUtf8(name));
			return -1;
		}
		ccScalarField* scalarField = static_cast<ccScalarField*>(m_cloud->getScalarField(index));
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			scalarField->setValue(i, static_cast<ScalarType>(initialValue));
		}
		scalarField->computeMinAndMax();
		return index;
	}

	bool ALiSSession::copyScalarField(const char* destination, const char* source, float missingValue, QString& errorMessage)
	{
		const int destinationIndex = ensureScalarField(destination, missingValue, errorMessage);
		if (destinationIndex < 0)
		{
			return false;
		}
		ccScalarField* output = static_cast<ccScalarField*>(m_cloud->getScalarField(destinationIndex));
		const int sourceIndex = m_cloud->getScalarFieldIndexByName(source);
		const ccScalarField* input = sourceIndex >= 0 ? static_cast<const ccScalarField*>(m_cloud->getScalarField(sourceIndex)) : nullptr;
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			output->setValue(i, input ? input->getValue(i) : static_cast<ScalarType>(missingValue));
		}
		output->computeMinAndMax();
		return true;
	}

	bool ALiSSession::initialize(QString& errorMessage)
	{
		if (m_initialized)
		{
			return true;
		}
		if (!isCompatibleWith(*m_cloud))
		{
			errorMessage = QStringLiteral("The source entity changed after this session was created.");
			return false;
		}
		// Reject incomplete/misaligned saved attributes before copying or reading them.
		for (unsigned i = 0; i < m_cloud->getNumberOfScalarFields(); ++i)
		{
			const auto* sf = m_cloud->getScalarField(i);
			if (sf->size() != m_cloud->size())
			{
				errorMessage = QStringLiteral("Saved scalar field '%1' is not aligned with the cloud.").arg(sf->getName());
				return false;
			}
		}
		QString metadataKey = QStringLiteral("ALiS.Session.v1");
		if (!m_cloud->hasMetaData(metadataKey)
			&& m_cloud->hasMetaData(QStringLiteral("qArchaeoLiDAR.Session.v1")))
		{
			metadataKey = QStringLiteral("qArchaeoLiDAR.Session.v1");
		}
		if (m_cloud->hasMetaData(metadataKey))
		{
			const auto document = QJsonDocument::fromJson(m_cloud->getMetaData(metadataKey).toByteArray());
			if (!document.isObject() || document.object().value(QStringLiteral("schema")).toString() != QStringLiteral("qal-session/1"))
			{
				errorMessage = QStringLiteral("Saved ALiS session metadata is invalid; the cloud has not been reinitialized.");
				return false;
			}
			const auto saved = document.object();
			m_sourceRevision = saved.value(QStringLiteral("sourceRevision")).toVariant().toULongLong();
			m_groundRevision = saved.value(QStringLiteral("groundRevision")).toVariant().toULongLong();
			m_hagRevision = saved.value(QStringLiteral("hagRevision")).toVariant().toULongLong();
			for (const auto value : saved.value(QStringLiteral("history")).toArray())
			{
				const auto object = value.toObject();
				ProcessingRecord previous;
				previous.operation = object.value(QStringLiteral("operation")).toString();
				previous.timestampUtc = QDateTime::fromString(object.value(QStringLiteral("timestampUtc")).toString(), Qt::ISODateWithMs);
				previous.parameters = object.value(QStringLiteral("parameters")).toObject();
				previous.sourceEntityUid = object.value(QStringLiteral("sourceEntityUid")).toVariant().toULongLong();
				previous.affectedPoints = object.value(QStringLiteral("affectedPoints")).toVariant().toULongLong();
				previous.elapsedMilliseconds = object.value(QStringLiteral("elapsedMilliseconds")).toVariant().toLongLong();
				for (const auto output : object.value(QStringLiteral("outputFields")).toArray()) previous.outputFields.append(output.toString());
				m_history.push_back(previous);
			}
		}
		const bool needsOriginal = m_cloud->getScalarFieldIndexByName(field::OriginalClassification) < 0;
		const bool needsWorking = m_cloud->getScalarFieldIndexByName(field::WorkingAsprs) < 0;
		if ((needsOriginal && !copyScalarField(field::OriginalClassification, "Classification", 0.0f, errorMessage))
			|| (needsWorking && !copyScalarField(field::WorkingAsprs, field::OriginalClassification, 0.0f, errorMessage))
			|| ensureScalarField(field::TrainingLabel, 0.0f, errorMessage) < 0
			// Legacy TrainingLabel mixes ASPRS and archaeology: never infer ASPRS truth from it.
			|| ensureScalarField(field::AsprsTrainingClass, std::numeric_limits<float>::quiet_NaN(), errorMessage) < 0)
		{
			return false;
		}
		m_initialized = true;
		m_hasGroundPreview = m_cloud->getScalarFieldIndexByName(field::GroundPreview) >= 0;
		ProcessingRecord record;
		record.operation = m_history.empty() ? QStringLiteral("Session.Initialize") : QStringLiteral("Session.Reopen");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = m_pointCount;
		record.outputFields = QStringList() << QString::fromUtf8(field::OriginalClassification) << QString::fromUtf8(field::WorkingAsprs)
			<< QString::fromUtf8(field::TrainingLabel)
			<< QString::fromUtf8(field::AsprsTrainingClass);
		record.parameters.insert(QStringLiteral("classificationProfile"), QStringLiteral("ASPRS + qAL user-defined archaeology 64-75"));
		record.parameters.insert(QStringLiteral("legacyArchaeologyFieldPresent"), m_cloud->getScalarFieldIndexByName(field::ArchaeologyClass) >= 0);
		record.parameters.insert(QStringLiteral("sourceClassificationPresent"), m_cloud->getScalarFieldIndexByName("Classification") >= 0);
		addProcessingRecord(record);
		return true;
	}

	bool ALiSSession::writeDenseField(const char* name, const std::vector<float>& values, QString& errorMessage)
	{
		if (values.size() != static_cast<std::size_t>(m_cloud->size()))
		{
			errorMessage = QStringLiteral("Field '%1' has %2 values for %3 points.")
				.arg(QString::fromUtf8(name)).arg(values.size()).arg(m_cloud->size());
			return false;
		}
		const int index = ensureScalarField(name, 0.0f, errorMessage);
		if (index < 0)
		{
			return false;
		}
		ccScalarField* scalarField = static_cast<ccScalarField*>(m_cloud->getScalarField(index));
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			scalarField->setValue(i, static_cast<ScalarType>(values[i]));
		}
		scalarField->computeMinAndMax();
		return true;
	}

	bool ALiSSession::removeScalarField(const char* name, QString& errorMessage)
	{
		const int index = m_cloud->getScalarFieldIndexByName(name);
		if (index < 0)
		{
			return true;
		}
		m_cloud->deleteScalarField(index);
		return true;
	}

	bool ALiSSession::setGroundPreview(const std::vector<bool>& mask, QString& errorMessage)
	{
		if (mask.size() != static_cast<std::size_t>(m_cloud->size()))
		{
			errorMessage = QStringLiteral("Ground preview is not aligned with the session point indices.");
			return false;
		}
		std::vector<float> values(mask.size());
		std::transform(mask.begin(), mask.end(), values.begin(), [](bool ground) { return ground ? 1.0f : 0.0f; });
		if (!writeDenseField(field::GroundPreview, values, errorMessage))
		{
			return false;
		}
		m_hasGroundPreview = true;
		return displayScalarField(QString::fromUtf8(field::GroundPreview), errorMessage);
	}

	bool ALiSSession::discardGroundPreview(QString& errorMessage)
	{
		if (!removeScalarField(field::GroundPreview, errorMessage))
		{
			return false;
		}
		m_hasGroundPreview = false;
		return true;
	}

	bool ALiSSession::applyGroundPreview(QString& errorMessage)
	{
		const int previewIndex = m_cloud->getScalarFieldIndexByName(field::GroundPreview);
		if (!m_hasGroundPreview || previewIndex < 0)
		{
			errorMessage = QStringLiteral("Run a Ground preview before Apply.");
			return false;
		}
		const ccScalarField* preview = static_cast<const ccScalarField*>(m_cloud->getScalarField(previewIndex));
		ScalarCommand command;
		command.description = QStringLiteral("Apply Ground preview");
		command.fieldName = QString::fromUtf8(field::GroundMask);
		command.changesGround = true;
		int groundIndex = m_cloud->getScalarFieldIndexByName(field::GroundMask);
		command.removeFieldOnUndo = groundIndex < 0;
		if (groundIndex < 0)
		{
			groundIndex = ensureScalarField(field::GroundMask, 0.0f, errorMessage);
		}
		if (groundIndex < 0)
		{
			return false;
		}
		const ccScalarField* ground = static_cast<const ccScalarField*>(m_cloud->getScalarField(groundIndex));
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			const float before = static_cast<float>(ground->getValue(i));
			const float after = preview->getValue(i) >= 0.5f ? 1.0f : 0.0f;
			if (before != after)
			{
				command.indices.push_back(i);
				command.before.push_back(before);
				command.after.push_back(after);
			}
		}
		const int workingIndex = ensureScalarField(field::WorkingAsprs, 0.0f, errorMessage);
		if (workingIndex < 0)
		{
			return false;
		}
		const ccScalarField* working = static_cast<const ccScalarField*>(m_cloud->getScalarField(workingIndex));
		ScalarCommand classCommand;
		classCommand.description = QStringLiteral("Synchronize Ground with ASPRS Working");
		classCommand.fieldName = QString::fromUtf8(field::WorkingAsprs);
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			const float before = static_cast<float>(working->getValue(i));
			const bool isGround = preview->getValue(i) >= 0.5f;
			const float after = isGround ? 2.0f : (before == 2.0f ? 1.0f : before);
			if (before != after)
			{
				classCommand.indices.push_back(i);
				classCommand.before.push_back(before);
				classCommand.after.push_back(after);
			}
		}
		Transaction transaction;
		transaction.description = QStringLiteral("Apply Ground preview to ASPRS Working");
		transaction.commands.push_back(std::move(command));
		transaction.commands.push_back(std::move(classCommand));
		if (!applyTransaction(std::move(transaction), errorMessage))
		{
			return false;
		}
		if (!discardGroundPreview(errorMessage))
		{
			return false;
		}
		ProcessingRecord record;
		record.operation = QStringLiteral("Ground.ApplyPreview");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = m_pointCount;
		record.outputFields = QStringList() << QString::fromUtf8(field::GroundMask) << QString::fromUtf8(field::WorkingAsprs);
		addProcessingRecord(record);
		return displayScalarField(QString::fromUtf8(field::GroundMask), errorMessage);
	}

	bool ALiSSession::hasAppliedGround() const
	{
		return m_cloud->getScalarFieldIndexByName(field::GroundMask) >= 0;
	}

	bool ALiSSession::restoreGround(QString& message)
	{
		if (m_undoStack.empty() || m_undoStack.back().commands.empty())
		{
			message = QStringLiteral("No Ground edit is available to restore.");
			return false;
		}
		bool changesGround = false;
		for (const ScalarCommand& command : m_undoStack.back().commands)
		{
			changesGround = changesGround || command.changesGround;
		}
		if (!changesGround)
		{
			message = QStringLiteral("The latest edit is not a Ground edit; use Undo instead.");
			return false;
		}
		return undo(message);
	}

	std::vector<bool> ALiSSession::appliedGroundMask() const
	{
		std::vector<bool> mask;
		const int groundIndex = m_cloud->getScalarFieldIndexByName(field::GroundMask);
		if (groundIndex < 0)
		{
			return mask;
		}
		// Once a Ground result exists, ASPRS Working is authoritative. This makes
		// manual annotation and applied model classes immediately visible to a new DTM.
		const int workingIndex = m_cloud->getScalarFieldIndexByName(field::WorkingAsprs);
		const ccScalarField* scalarField = static_cast<const ccScalarField*>(
			m_cloud->getScalarField(workingIndex >= 0 ? workingIndex : groundIndex));
		mask.resize(m_cloud->size());
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			mask[i] = workingIndex >= 0 ? scalarField->getValue(i) == 2.0f : scalarField->getValue(i) >= 0.5f;
		}
		return mask;
	}

	bool ALiSSession::validateIndices(const std::vector<unsigned>& indices, QString& errorMessage) const
	{
		for (unsigned index : indices)
		{
			if (index >= m_cloud->size())
			{
				errorMessage = QStringLiteral("Point index %1 is outside the session cloud.").arg(index);
				return false;
			}
		}
		return true;
	}

	bool ALiSSession::writeCommandValues(const ScalarCommand& command, bool useAfter, QString& errorMessage)
	{
		if (!useAfter && command.removeFieldOnUndo)
		{
			return removeScalarField(command.fieldName.toUtf8().constData(), errorMessage);
		}
		const int index = ensureScalarField(command.fieldName.toUtf8().constData(), 0.0f, errorMessage);
		if (index < 0)
		{
			return false;
		}
		ccScalarField* scalarField = static_cast<ccScalarField*>(m_cloud->getScalarField(index));
		const std::vector<float>& values = useAfter ? command.after : command.before;
		if (values.size() != command.indices.size())
		{
			errorMessage = QStringLiteral("Undo command '%1' is corrupt.").arg(command.description);
			return false;
		}
		for (std::size_t i = 0; i < command.indices.size(); ++i)
		{
			scalarField->setValue(command.indices[i], static_cast<ScalarType>(values[i]));
		}
		scalarField->computeMinAndMax();
		return true;
	}

	void ALiSSession::markChanged(bool ground, bool hag)
	{
		m_dirty = true;
		++m_sourceRevision;
		if (ground) ++m_groundRevision;
		if (hag) ++m_hagRevision;
		persistSessionMetadata();
	}

	bool ALiSSession::applyScalarCommand(ScalarCommand command, QString& errorMessage)
	{
		Transaction transaction;
		transaction.description = command.description;
		transaction.commands.push_back(std::move(command));
		return applyTransaction(std::move(transaction), errorMessage);
	}

	bool ALiSSession::applyTransaction(Transaction transaction, QString& errorMessage)
	{
		for (const ScalarCommand& command : transaction.commands)
		{
			if (!validateIndices(command.indices, errorMessage))
			{
				return false;
			}
		}
		std::size_t applied = 0;
		for (; applied < transaction.commands.size(); ++applied)
		{
			if (!writeCommandValues(transaction.commands[applied], true, errorMessage))
			{
				while (applied > 0)
				{
					--applied;
					QString rollbackError;
					writeCommandValues(transaction.commands[applied], false, rollbackError);
				}
				return false;
			}
		}
		bool changesGround = false;
		bool changesHag = false;
		for (const ScalarCommand& command : transaction.commands)
		{
			changesGround = changesGround || command.changesGround;
			changesHag = changesHag || command.changesHag;
		}
		markChanged(changesGround, changesHag);
		recordTransaction(transaction, QStringLiteral("Edit.Apply"));
		m_undoStack.push_back(std::move(transaction));
		m_redoStack.clear();
		return true;
	}

	bool ALiSSession::assignGroundToWorkingAsprs(QString& errorMessage)
	{
		const std::vector<bool> mask = appliedGroundMask();
		if (mask.empty())
		{
			errorMessage = QStringLiteral("Apply a Ground mask before assigning ASPRS Ground.");
			return false;
		}
		const int fieldIndex = ensureScalarField(field::WorkingAsprs, 0.0f, errorMessage);
		if (fieldIndex < 0) return false;
		const ccScalarField* scalarField = static_cast<const ccScalarField*>(m_cloud->getScalarField(fieldIndex));
		ScalarCommand command;
		command.description = QStringLiteral("Assign Ground to ASPRS Working");
		command.fieldName = QString::fromUtf8(field::WorkingAsprs);
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			if (mask[i] && scalarField->getValue(i) != 2.0f)
			{
				command.indices.push_back(i);
				command.before.push_back(static_cast<float>(scalarField->getValue(i)));
				command.after.push_back(2.0f);
			}
		}
		const quint64 affected = command.indices.size();
		if (!applyScalarCommand(std::move(command), errorMessage)) return false;
		ProcessingRecord record;
		record.operation = QStringLiteral("ASPRS.AssignGround");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = affected;
		record.outputFields = QStringList() << QString::fromUtf8(field::WorkingAsprs);
		addProcessingRecord(record);
		return true;
	}

	bool ALiSSession::applyManualAsprs(const std::vector<unsigned>& indices, std::uint8_t code, QString& errorMessage)
	{
		if (!validateIndices(indices, errorMessage)) return false;
		const int fieldIndex = ensureScalarField(field::WorkingAsprs, 0.0f, errorMessage);
		const int trustedIndex = ensureScalarField(field::TrainingLabel, 0.0f, errorMessage);
		if (fieldIndex < 0 || trustedIndex < 0) return false;
		const ccScalarField* scalarField = static_cast<const ccScalarField*>(m_cloud->getScalarField(fieldIndex));
		ScalarCommand command;
		command.description = QStringLiteral("Manual ASPRS label");
		command.fieldName = QString::fromUtf8(field::WorkingAsprs);
		for (unsigned pointIndex : indices)
		{
			const float before = static_cast<float>(scalarField->getValue(pointIndex));
			if (before != code)
			{
				command.indices.push_back(pointIndex); command.before.push_back(before); command.after.push_back(static_cast<float>(code));
			}
		}
		const ccScalarField* trusted = static_cast<const ccScalarField*>(m_cloud->getScalarField(trustedIndex));
		ScalarCommand trustedCommand;
		trustedCommand.description = QStringLiteral("Mark ASPRS label Manual/Trusted");
		trustedCommand.fieldName = QString::fromUtf8(field::TrainingLabel);
		for (unsigned pointIndex : indices)
		{
			const float before = static_cast<float>(trusted->getValue(pointIndex));
			if (before != 1.0f)
			{
				trustedCommand.indices.push_back(pointIndex);
				trustedCommand.before.push_back(before);
				trustedCommand.after.push_back(1.0f);
			}
		}
		Transaction transaction;
		transaction.description = QStringLiteral("Manual ASPRS + Ground synchronization + Manual/Trusted provenance");
		transaction.commands.push_back(std::move(command));
		const int groundIndex = m_cloud->getScalarFieldIndexByName(field::GroundMask);
		bool selectedWasGround = false;
		for (unsigned pointIndex : indices)
			selectedWasGround = selectedWasGround || scalarField->getValue(pointIndex) == 2.0f;
		if (groundIndex >= 0 || code == 2 || selectedWasGround)
		{
			ScalarCommand groundCommand;
			groundCommand.description = QStringLiteral("Synchronize ASPRS class 2 with Ground mask");
			groundCommand.fieldName = QString::fromUtf8(field::GroundMask);
			groundCommand.changesGround = false;
			groundCommand.removeFieldOnUndo = groundIndex < 0;
			if (groundIndex >= 0)
			{
				const auto* ground = m_cloud->getScalarField(groundIndex);
				for (unsigned pointIndex : indices)
				{
					const float before = ground->getValue(pointIndex);
					const float after = code == 2 ? 1.0f : 0.0f;
					if (before != after)
					{
						groundCommand.indices.push_back(pointIndex);
						groundCommand.before.push_back(before);
						groundCommand.after.push_back(after);
					}
				}
			}
			else
			{
				std::vector<bool> selected(m_cloud->size(), false);
				for (unsigned pointIndex : indices) selected[pointIndex] = true;
				for (unsigned i = 0; i < m_cloud->size(); ++i)
				{
					const bool afterGround = selected[i] ? code == 2 : scalarField->getValue(i) == 2.0f;
					if (afterGround)
					{
						groundCommand.indices.push_back(i);
						groundCommand.before.push_back(0.0f);
						groundCommand.after.push_back(1.0f);
					}
				}
			}
			groundCommand.changesGround = groundCommand.removeFieldOnUndo || !groundCommand.indices.empty();
			if (groundCommand.changesGround) transaction.commands.push_back(std::move(groundCommand));
		}
		transaction.commands.push_back(std::move(trustedCommand));
		const int referenceIndex = ensureScalarField(field::AsprsTrainingClass, std::numeric_limits<float>::quiet_NaN(), errorMessage);
		if (referenceIndex < 0) return false;
		const auto* reference = m_cloud->getScalarField(referenceIndex);
		ScalarCommand referenceCommand;
		referenceCommand.fieldName = QString::fromUtf8(field::AsprsTrainingClass);
		for (unsigned pointIndex : indices)
		{
			const float before = reference->getValue(pointIndex);
			if (before != code)
			{
				referenceCommand.indices.push_back(pointIndex);
				referenceCommand.before.push_back(before);
				referenceCommand.after.push_back(static_cast<float>(code));
			}
		}
		transaction.commands.push_back(std::move(referenceCommand));
		return applyTransaction(std::move(transaction), errorMessage);
	}

	bool ALiSSession::trustAsprsFromField(const QString& sourceFieldName, QString& errorMessage)
	{
		const int sourceIndex = m_cloud->getScalarFieldIndexByName(sourceFieldName.toUtf8().constData());
		if (sourceIndex < 0)
		{
			errorMessage = QStringLiteral("Selected class field is unavailable: %1").arg(sourceFieldName);
			return false;
		}
		const int classIndex = ensureScalarField(field::WorkingAsprs, 0.0f, errorMessage);
		const int referenceIndex = ensureScalarField(field::AsprsTrainingClass,
			std::numeric_limits<float>::quiet_NaN(), errorMessage);
		if (classIndex < 0 || referenceIndex < 0) return false;
		const auto* source = m_cloud->getScalarField(sourceIndex);
		const auto* classes = m_cloud->getScalarField(classIndex);
		const auto* reference = m_cloud->getScalarField(referenceIndex);
		ScalarCommand workingCommand;
		workingCommand.description = QStringLiteral("Set ASPRS Working from training field");
		workingCommand.fieldName = QString::fromUtf8(field::WorkingAsprs);
		ScalarCommand command;
		command.description = QStringLiteral("Certify selected ASPRS training field");
		command.fieldName = QString::fromUtf8(field::AsprsTrainingClass);
		command.indices.reserve(m_cloud->size());
		command.before.reserve(m_cloud->size());
		command.after.reserve(m_cloud->size());
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			const float value = source->getValue(i);
			if (!std::isfinite(value) || value < 0.0f || value > 255.0f || std::floor(value) != value)
				continue;
			const float workingBefore = classes->getValue(i);
			if (workingBefore != value)
			{
				workingCommand.indices.push_back(i);
				workingCommand.before.push_back(workingBefore);
				workingCommand.after.push_back(value);
			}
			const float before = reference->getValue(i);
			if (before != value)
			{
				command.indices.push_back(i);
				command.before.push_back(before);
				command.after.push_back(value);
			}
		}
		const quint64 affected = command.indices.size();
		if (affected == 0)
		{
			errorMessage = QStringLiteral("No valid untrusted integer ASPRS labels were found in %1.").arg(sourceFieldName);
			return false;
		}
		Transaction transaction;
		transaction.description = QStringLiteral("Select certified ASPRS training set");
		transaction.commands.push_back(std::move(workingCommand));
		transaction.commands.push_back(std::move(command));
		if (!applyTransaction(std::move(transaction), errorMessage)) return false;
		ProcessingRecord record;
		record.operation = QStringLiteral("ASPRS.SelectTrainingSet");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = affected;
		record.parameters.insert(QStringLiteral("class_field"), sourceFieldName);
		record.outputFields = QStringList() << QString::fromUtf8(field::WorkingAsprs)
			<< QString::fromUtf8(field::AsprsTrainingClass);
		addProcessingRecord(record);
		return true;
	}

	quint64 ALiSSession::trustedTrainingCount() const
	{
		if (m_cloud->getMetaData(QStringLiteral("ALiS.TestOnly")).toBool()) return 0;
		const int trustedIndex = m_cloud->getScalarFieldIndexByName(field::AsprsTrainingClass);
		const int classIndex = m_cloud->getScalarFieldIndexByName(field::WorkingAsprs);
		if (trustedIndex < 0 || classIndex < 0) return 0;
		const ccScalarField* trusted = static_cast<const ccScalarField*>(m_cloud->getScalarField(trustedIndex));
		const ccScalarField* classes = static_cast<const ccScalarField*>(m_cloud->getScalarField(classIndex));
		quint64 count = 0;
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			const double value = static_cast<double>(classes->getValue(i));
			if (isTrustedAsprsLabel(static_cast<float>(value), trusted->getValue(i)))
			{
				++count;
			}
		}
		return count;
	}

	bool ALiSSession::setAsprsPredictions(const std::vector<std::int16_t>& predictions,
	                                             const std::vector<float>& confidence,
	                                             const QJsonObject& provenance,
	                                             qint64 elapsedMilliseconds,
	                                             QString& errorMessage)
	{
		if (predictions.size() != static_cast<std::size_t>(m_cloud->size()) || confidence.size() != predictions.size())
		{
			errorMessage = QStringLiteral("Model output is not aligned with the Session point indices.");
			return false;
		}
		std::vector<float> classValues(predictions.size(), std::numeric_limits<float>::quiet_NaN());
		std::vector<float> confidenceValues(confidence.size(), std::numeric_limits<float>::quiet_NaN());
		for (std::size_t i = 0; i < predictions.size(); ++i)
		{
			const std::int16_t code = predictions[i];
			const float probability = confidence[i];
			if (code == -1)
			{
				continue;
			}
			if (code < 0 || code > 255 || !std::isfinite(probability) || probability < 0.0f || probability > 1.0f)
			{
				errorMessage = QStringLiteral("Invalid model output at point index %1.").arg(i);
				return false;
			}
			classValues[i] = static_cast<float>(code);
			confidenceValues[i] = probability;
		}
		if (!writeDenseField(field::AsprsPrediction, classValues, errorMessage)
		    || !writeDenseField(field::AsprsConfidence, confidenceValues, errorMessage))
		{
			return false;
		}
		markChanged(false, false);
		ProcessingRecord record;
		// The worker provenance identifies the actual model family (ML or DL).
		record.operation = QStringLiteral("Models.Predict");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.parameters = provenance;
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = m_pointCount;
		record.outputFields = QStringList() << QString::fromUtf8(field::AsprsPrediction)
			<< QString::fromUtf8(field::AsprsConfidence);
		record.elapsedMilliseconds = elapsedMilliseconds;
		addProcessingRecord(record);
		return displayScalarField(QString::fromUtf8(field::AsprsPrediction), errorMessage);
	}

	bool ALiSSession::setBootstrapClusters(const std::vector<std::int32_t>& clusters,
	                                      const std::vector<float>& confidence,
	                                      const QJsonObject& provenance,
	                                      qint64 elapsedMilliseconds,
	                                      QString& errorMessage)
	{
		if (clusters.size() != static_cast<std::size_t>(m_cloud->size()) || confidence.size() != clusters.size())
		{
			errorMessage = QStringLiteral("Bootstrap output is not aligned with the Session point indices.");
			return false;
		}
		std::vector<float> clusterValues(clusters.size());
		std::vector<float> confidenceValues(confidence.size());
		for (std::size_t i = 0; i < clusters.size(); ++i)
		{
			if (clusters[i] < 0 || clusters[i] > 16777215 || !std::isfinite(confidence[i])
			    || confidence[i] < 0.0f || confidence[i] > 1.0f)
			{
				errorMessage = QStringLiteral("Invalid bootstrap output at point index %1.").arg(i);
				return false;
			}
			clusterValues[i] = static_cast<float>(clusters[i]);
			confidenceValues[i] = confidence[i];
		}
		if (!writeDenseField(field::BootstrapCluster, clusterValues, errorMessage)
		    || !writeDenseField(field::BootstrapConfidence, confidenceValues, errorMessage))
		{
			return false;
		}
		markChanged(false, false);
		ProcessingRecord record;
		record.operation = QStringLiteral("Models.BootstrapClusters");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.parameters = provenance;
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = m_pointCount;
		record.outputFields = QStringList() << QString::fromUtf8(field::BootstrapCluster)
			<< QString::fromUtf8(field::BootstrapConfidence);
		record.elapsedMilliseconds = elapsedMilliseconds;
		addProcessingRecord(record);
		return displayScalarField(QString::fromUtf8(field::BootstrapCluster), errorMessage);
	}

	bool ALiSSession::applyAsprsPredictions(double minimumConfidence, QString& errorMessage)
	{
		if (!std::isfinite(minimumConfidence) || minimumConfidence < 0.0 || minimumConfidence > 1.0)
		{
			errorMessage = QStringLiteral("Prediction confidence threshold must be in [0, 1].");
			return false;
		}
		const int predictionIndex = m_cloud->getScalarFieldIndexByName(field::AsprsPrediction);
		const int confidenceIndex = m_cloud->getScalarFieldIndexByName(field::AsprsConfidence);
		const int workingIndex = ensureScalarField(field::WorkingAsprs, 0.0f, errorMessage);
		if (predictionIndex < 0 || confidenceIndex < 0 || workingIndex < 0)
		{
			errorMessage = QStringLiteral("Run Predict before applying model output.");
			return false;
		}
		const ccScalarField* predictions = static_cast<const ccScalarField*>(m_cloud->getScalarField(predictionIndex));
		const ccScalarField* confidence = static_cast<const ccScalarField*>(m_cloud->getScalarField(confidenceIndex));
		const ccScalarField* working = static_cast<const ccScalarField*>(m_cloud->getScalarField(workingIndex));
		ScalarCommand command;
		command.description = QStringLiteral("Apply ASPRS predictions");
		command.fieldName = QString::fromUtf8(field::WorkingAsprs);
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			const double predicted = static_cast<double>(predictions->getValue(i));
			const double probability = static_cast<double>(confidence->getValue(i));
			if (!std::isfinite(predicted) || predicted < 0.0 || predicted > 255.0
			    || !std::isfinite(probability) || probability < minimumConfidence)
			{
				continue;
			}
			const float after = static_cast<float>(std::lround(predicted));
			const float before = static_cast<float>(working->getValue(i));
			if (before != after)
			{
				command.indices.push_back(i);
				command.before.push_back(before);
				command.after.push_back(after);
			}
		}
		const quint64 affected = command.indices.size();
		if (affected == 0)
		{
			errorMessage = QStringLiteral("No valid prediction meets the confidence threshold or changes ASPRS Working.");
			return false;
		}
		if (!applyScalarCommand(std::move(command), errorMessage)) return false;
		ProcessingRecord record;
		record.operation = QStringLiteral("Models.ApplyPrediction");
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.parameters.insert(QStringLiteral("minimumConfidence"), minimumConfidence);
		record.sourceEntityUid = m_entityUid;
		record.affectedPoints = affected;
		record.outputFields = QStringList() << QString::fromUtf8(field::WorkingAsprs);
		addProcessingRecord(record);
		return true;
	}

	bool ALiSSession::restoreOriginalAsprs(const std::vector<unsigned>& indices, QString& errorMessage)
	{
		if (!validateIndices(indices, errorMessage)) return false;
		const int workingIndex = ensureScalarField(field::WorkingAsprs, 0.0f, errorMessage);
		const int originalIndex = m_cloud->getScalarFieldIndexByName(field::OriginalClassification);
		if (workingIndex < 0 || originalIndex < 0)
		{
			errorMessage = QStringLiteral("The Original/Working ASPRS fields are unavailable."); return false;
		}
		const ccScalarField* working = static_cast<const ccScalarField*>(m_cloud->getScalarField(workingIndex));
		const ccScalarField* original = static_cast<const ccScalarField*>(m_cloud->getScalarField(originalIndex));
		ScalarCommand command;
		command.description = QStringLiteral("Restore Original ASPRS");
		command.fieldName = QString::fromUtf8(field::WorkingAsprs);
		for (unsigned pointIndex : indices)
		{
			const float before = static_cast<float>(working->getValue(pointIndex));
			const float after = static_cast<float>(original->getValue(pointIndex));
			if (before != after) { command.indices.push_back(pointIndex); command.before.push_back(before); command.after.push_back(after); }
		}
		return applyScalarCommand(std::move(command), errorMessage);
	}

	bool ALiSSession::applyManualArchaeology(const std::vector<unsigned>& indices, ArchaeologyClass value, QString& errorMessage)
	{
		if (!validateIndices(indices, errorMessage)) return false;
		const int archIndex = ensureScalarField(field::ArchaeologyClass, 0.0f, errorMessage);
		const int trustedIndex = ensureScalarField(field::TrainingLabel, 0.0f, errorMessage);
		if (archIndex < 0 || trustedIndex < 0) return false;
		const ccScalarField* arch = static_cast<const ccScalarField*>(m_cloud->getScalarField(archIndex));
		ScalarCommand archCommand;
		archCommand.description = QStringLiteral("Manual Archaeology label");
		archCommand.fieldName = QString::fromUtf8(field::ArchaeologyClass);
		const float afterValue = static_cast<float>(value);
		for (unsigned pointIndex : indices)
		{
			const float before = static_cast<float>(arch->getValue(pointIndex));
			if (before != afterValue) { archCommand.indices.push_back(pointIndex); archCommand.before.push_back(before); archCommand.after.push_back(afterValue); }
		}
		const ccScalarField* trusted = static_cast<const ccScalarField*>(m_cloud->getScalarField(trustedIndex));
		ScalarCommand trustedCommand;
		trustedCommand.description = QStringLiteral("Mark labels Manual/Trusted");
		trustedCommand.fieldName = QString::fromUtf8(field::TrainingLabel);
		for (unsigned pointIndex : indices)
		{
			const float before = static_cast<float>(trusted->getValue(pointIndex));
			if (before != 1.0f) { trustedCommand.indices.push_back(pointIndex); trustedCommand.before.push_back(before); trustedCommand.after.push_back(1.0f); }
		}
		Transaction transaction;
		transaction.description = QStringLiteral("Manual Archaeology + Manual/Trusted label");
		transaction.commands.push_back(std::move(archCommand));
		transaction.commands.push_back(std::move(trustedCommand));
		return applyTransaction(std::move(transaction), errorMessage);
	}

	bool ALiSSession::clearArchaeology(const std::vector<unsigned>& indices, QString& errorMessage)
	{
		if (!validateIndices(indices, errorMessage)) return false;
		const int archIndex = ensureScalarField(field::ArchaeologyClass, 0.0f, errorMessage);
		const int trustedIndex = ensureScalarField(field::TrainingLabel, 0.0f, errorMessage);
		if (archIndex < 0 || trustedIndex < 0) return false;

		const ccScalarField* arch = static_cast<const ccScalarField*>(m_cloud->getScalarField(archIndex));
		ScalarCommand archCommand;
		archCommand.description = QStringLiteral("Clear Archaeology class");
		archCommand.fieldName = QString::fromUtf8(field::ArchaeologyClass);
		const ccScalarField* trusted = static_cast<const ccScalarField*>(m_cloud->getScalarField(trustedIndex));
		ScalarCommand trustedCommand;
		trustedCommand.description = QStringLiteral("Clear Manual/Trusted provenance");
		trustedCommand.fieldName = QString::fromUtf8(field::TrainingLabel);

		for (unsigned pointIndex : indices)
		{
			const float archaeologyBefore = static_cast<float>(arch->getValue(pointIndex));
			if (archaeologyBefore != 0.0f)
			{
				archCommand.indices.push_back(pointIndex);
				archCommand.before.push_back(archaeologyBefore);
				archCommand.after.push_back(0.0f);
			}
			const float trainingBefore = static_cast<float>(trusted->getValue(pointIndex));
			if (trainingBefore != 0.0f)
			{
				trustedCommand.indices.push_back(pointIndex);
				trustedCommand.before.push_back(trainingBefore);
				trustedCommand.after.push_back(0.0f);
			}
		}

		Transaction transaction;
		transaction.description = QStringLiteral("Clear Archaeology + Manual/Trusted labels");
		transaction.commands.push_back(std::move(archCommand));
		transaction.commands.push_back(std::move(trustedCommand));
		return applyTransaction(std::move(transaction), errorMessage);
	}

	std::vector<unsigned> ALiSSession::visibleSubset(QString& errorMessage) const
	{
		std::vector<unsigned> indices;
		const auto& visibility = m_cloud->getTheVisibilityArray();
		if (visibility.size() != m_cloud->size())
		{
			errorMessage = QStringLiteral("No CloudCompare point-visibility subset exists. Use a CC selection/segmentation or scalar-field filter first.");
			return indices;
		}
		indices.reserve(m_cloud->size());
		std::size_t hidden = 0;
		for (unsigned i = 0; i < m_cloud->size(); ++i)
		{
			if (visibility[i] == CCCoreLib::POINT_VISIBLE) indices.push_back(i); else ++hidden;
		}
		if (hidden == 0 || indices.empty())
		{
			indices.clear();
			errorMessage = hidden == 0
				? QStringLiteral("All points are visible; refusing to label the full cloud as an accidental manual selection.")
				: QStringLiteral("The current visibility subset contains no points.");
		}
		return indices;
	}

	bool ALiSSession::setHagValues(const std::vector<double>& values, const QJsonObject& parameters,
		qint64 elapsedMilliseconds, QString& errorMessage)
	{
		if (values.size() != static_cast<std::size_t>(m_cloud->size()))
		{
			errorMessage = QStringLiteral("HAG output is not aligned with the session point indices."); return false;
		}
		std::vector<float> output(values.size());
		for (std::size_t i = 0; i < values.size(); ++i)
		{
			output[i] = std::isfinite(values[i]) ? static_cast<float>(values[i]) : std::numeric_limits<float>::quiet_NaN();
		}
		if (!writeDenseField(field::HeightAboveGround, output, errorMessage)) return false;
		markChanged(false, true);
		ProcessingRecord record;
		record.operation = QStringLiteral("Terrain.ComputeHAG"); record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.parameters = parameters; record.sourceEntityUid = m_entityUid; record.affectedPoints = m_pointCount;
		record.outputFields = QStringList() << QString::fromUtf8(field::HeightAboveGround); record.elapsedMilliseconds = elapsedMilliseconds;
		addProcessingRecord(record);
		return true;
	}

	bool ALiSSession::displayScalarField(const QString& name, QString& errorMessage)
	{
		const QByteArray utf8 = name.toUtf8();
		const int index = m_cloud->getScalarFieldIndexByName(utf8.constData());
		if (index < 0)
		{
			errorMessage = QStringLiteral("Scalar field '%1' is not available.").arg(name); return false;
		}
		m_cloud->setCurrentDisplayedScalarField(index);
		m_cloud->showColors(false);
		m_cloud->showSF(true);
		m_cloud->prepareDisplayForRefresh();
		return true;
	}

	bool ALiSSession::undo(QString& message)
	{
		if (m_undoStack.empty()) { message = QStringLiteral("Nothing to undo."); return false; }
		Transaction transaction = std::move(m_undoStack.back()); m_undoStack.pop_back();
		for (auto command = transaction.commands.rbegin(); command != transaction.commands.rend(); ++command)
		{
			if (!writeCommandValues(*command, false, message)) { m_undoStack.push_back(std::move(transaction)); return false; }
		}
		bool ground = false, hag = false; for (const ScalarCommand& command : transaction.commands) { ground |= command.changesGround; hag |= command.changesHag; }
		markChanged(ground, hag);
		message = QStringLiteral("Undid: %1").arg(transaction.description);
		recordTransaction(transaction, QStringLiteral("Edit.Undo"));
		m_redoStack.push_back(std::move(transaction)); return true;
	}

	bool ALiSSession::redo(QString& message)
	{
		if (m_redoStack.empty()) { message = QStringLiteral("Nothing to redo."); return false; }
		Transaction transaction = std::move(m_redoStack.back()); m_redoStack.pop_back();
		for (const ScalarCommand& command : transaction.commands)
		{
			if (!writeCommandValues(command, true, message)) { m_redoStack.push_back(std::move(transaction)); return false; }
		}
		bool ground = false, hag = false; for (const ScalarCommand& command : transaction.commands) { ground |= command.changesGround; hag |= command.changesHag; }
		markChanged(ground, hag);
		message = QStringLiteral("Redid: %1").arg(transaction.description);
		recordTransaction(transaction, QStringLiteral("Edit.Redo"));
		m_undoStack.push_back(std::move(transaction)); return true;
	}

	bool ALiSSession::canUndo() const { return !m_undoStack.empty(); }
	bool ALiSSession::canRedo() const { return !m_redoStack.empty(); }
	void ALiSSession::persistSessionMetadata()
	{
		const QJsonObject saved{
			{QStringLiteral("schema"), QStringLiteral("qal-session/1")},
			{QStringLiteral("pointCount"), static_cast<double>(m_pointCount)},
			{QStringLiteral("sourceRevision"), static_cast<double>(m_sourceRevision)},
			{QStringLiteral("groundRevision"), static_cast<double>(m_groundRevision)},
			{QStringLiteral("hagRevision"), static_cast<double>(m_hagRevision)},
			{QStringLiteral("history"), historyAsJson()}};
		m_cloud->setMetaData(QStringLiteral("ALiS.Session.v1"), QJsonDocument(saved).toJson(QJsonDocument::Compact));
	}
	void ALiSSession::recordTransaction(const Transaction& transaction, const QString& operation)
	{
		ProcessingRecord record;
		record.operation = operation;
		record.timestampUtc = QDateTime::currentDateTimeUtc();
		record.sourceEntityUid = m_entityUid;
		record.parameters.insert(QStringLiteral("description"), transaction.description);
		QJsonObject changedValues;
		for (const auto& command : transaction.commands)
		{
			record.outputFields.append(command.fieldName);
			changedValues.insert(command.fieldName, static_cast<double>(command.indices.size()));
		}
		// Counts are per field; summing would count semantic/provenance edits twice.
		record.parameters.insert(QStringLiteral("changedValuesPerField"), changedValues);
		addProcessingRecord(record);
	}
	void ALiSSession::addProcessingRecord(const ProcessingRecord& record)
	{
		m_history.push_back(record);
		persistSessionMetadata();
	}
	const std::vector<ProcessingRecord>& ALiSSession::history() const { return m_history; }
	QJsonArray ALiSSession::historyAsJson() const
	{
		QJsonArray array; for (const ProcessingRecord& record : m_history) array.append(record.toJson()); return array;
	}
}
