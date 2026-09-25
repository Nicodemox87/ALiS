// SPDX-License-Identifier: GPL-2.0-or-later
// Non-GUI integration checks. This target needs SessionModel.cpp, QCC_DB_LIB,
// ALiSAuditLogic, and Qt Core when wired into CloudCompare's CMake tree.
#include "SessionModel.h"

#include <ccPointCloud.h>
#include <ccScalarField.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void check(bool condition, const char* message)
	{
		++g_checks;
		if (!condition)
		{
			++g_failures;
			std::cerr << "FAIL: " << message << '\n';
		}
	}

	const ccScalarField* lookupField(const ccPointCloud& cloud, const char* name)
	{
		const int index = cloud.getScalarFieldIndexByName(name);
		return index >= 0 ? static_cast<const ccScalarField*>(cloud.getScalarField(index)) : nullptr;
	}

	bool valueIs(const ccPointCloud& cloud, const char* name, unsigned index, float expected)
	{
		const ccScalarField* scalarField = lookupField(cloud, name);
		return scalarField && std::abs(static_cast<double>(scalarField->getValue(index)) - expected) < 1.0e-6;
	}
}

int main()
{
	using namespace alis;
	ccPointCloud cloud(QStringLiteral("session-fixture"));
	check(cloud.reserve(4), "synthetic cloud allocation succeeds");
	cloud.addPoint(CCVector3(0.0f, 0.0f, 1.0f));
	cloud.addPoint(CCVector3(1.0f, 0.0f, 2.0f));
	cloud.addPoint(CCVector3(0.0f, 1.0f, 3.0f));
	cloud.addPoint(CCVector3(1.0f, 1.0f, 4.0f));

	const int classificationIndex = cloud.addScalarField("Classification");
	check(classificationIndex >= 0, "source Classification field allocation succeeds");
	ccScalarField* classification = static_cast<ccScalarField*>(cloud.getScalarField(classificationIndex));
	classification->setValue(0, 1.0f);
	classification->setValue(1, 5.0f);
	classification->setValue(2, 6.0f);
	classification->setValue(3, 2.0f);
	classification->computeMinAndMax();

	const CCVector3 before0 = *cloud.getPoint(0);
	const unsigned beforeSize = cloud.size();
	ALiSSession session(cloud);
	QString error;
	check(session.initialize(error), "session initialization succeeds on a valid ccPointCloud");
	check(error.isEmpty(), "successful initialization leaves no error message");
	check(session.initialized(), "session records initialized state");
	check(session.pointCount() == beforeSize && cloud.size() == beforeSize,
		"session attribute initialization does not duplicate or remove XYZ points");
	const CCVector3& after0 = *cloud.getPoint(0);
	check(after0.x == before0.x && after0.y == before0.y && after0.z == before0.z,
		"session attribute initialization preserves source XYZ");

	for (const char* name : {field::OriginalClassification, field::WorkingAsprs, field::TrainingLabel})
	{
		check(lookupField(cloud, name) && lookupField(cloud, name)->size() == cloud.size(),
			"session-owned fields remain point-index aligned");
	}
	check(valueIs(cloud, field::OriginalClassification, 2, 6.0f),
		"original Classification is preserved in the immutable working-session copy");
	check(valueIs(cloud, field::WorkingAsprs, 2, 6.0f),
		"working ASPRS starts as an exact Classification copy");
	check(lookupField(cloud, field::ArchaeologyClass) == nullptr
		&& valueIs(cloud, field::TrainingLabel, 2, 0.0f),
		"new sessions use one ASPRS field and do not create a separate archaeology SF");

	check(session.applyManualAsprs({1}, 11, error), "manual ASPRS edit succeeds for a valid subset");
	check(valueIs(cloud, field::WorkingAsprs, 1, 11.0f), "manual ASPRS edit changes only the working class");
	check(valueIs(cloud, field::TrainingLabel, 1, static_cast<float>(TrainingLabel::ManualTrusted)),
		"manual ASPRS edit marks the training provenance Manual/Trusted");
	check(valueIs(cloud, field::OriginalClassification, 1, 5.0f)
		&& lookupField(cloud, field::ArchaeologyClass) == nullptr,
		"manual ASPRS edit preserves Original and does not create a separate archaeology layer");
	QString undoMessage;
	check(session.undo(undoMessage) && valueIs(cloud, field::WorkingAsprs, 1, 5.0f),
		"undo restores a manual ASPRS edit");
	check(session.redo(undoMessage) && valueIs(cloud, field::WorkingAsprs, 1, 11.0f),
		"redo reapplies a manual ASPRS edit");
	check(session.restoreOriginalAsprs({1}, error)
		&& valueIs(cloud, field::WorkingAsprs, 1, 5.0f),
		"Restore Original copies only the selected indices");

	check(!session.setGroundPreview({true, false}, error),
		"a point-index-misaligned ground mask is rejected");
	error.clear();
	check(session.setGroundPreview({true, false, true, false}, error),
		"aligned ground preview is materialized without making subset clouds");
	check(session.hasGroundPreview() && valueIs(cloud, field::GroundPreview, 0, 1.0f)
		&& valueIs(cloud, field::GroundPreview, 1, 0.0f),
		"ground preview preserves source point order");
	check(session.applyGroundPreview(error), "ground preview can be committed");
	check(valueIs(cloud, field::WorkingAsprs, 0, 2.0f)
		&& valueIs(cloud, field::WorkingAsprs, 2, 2.0f)
		&& valueIs(cloud, field::WorkingAsprs, 3, 1.0f),
		"applying Ground synchronizes class 2 and demotes rejected former Ground to Unclassified");
	check(!session.hasGroundPreview() && lookupField(cloud, field::GroundPreview) == nullptr,
		"committing ground releases the temporary preview Scalar Field");
	check(session.hasAppliedGround(), "committing the first Ground preview creates an applied Ground state");
	const std::vector<bool> appliedMask = session.appliedGroundMask();
	check(appliedMask.size() == 4 && appliedMask[0] && !appliedMask[1] && appliedMask[2] && !appliedMask[3],
		"applied Ground mask remains index aligned");
	check(session.undo(undoMessage) && !session.hasAppliedGround()
		&& lookupField(cloud, field::GroundMask) == nullptr,
		"undo of the first Ground apply atomically restores the absence of GroundMask");
	check(session.redo(undoMessage) && session.hasAppliedGround(),
		"redo atomically recreates the applied Ground state");
	check(session.restoreGround(undoMessage) && !session.hasAppliedGround(),
		"dedicated Restore reverses the latest Ground edit");
	check(session.redo(undoMessage) && session.hasAppliedGround(),
		"a dedicated Ground Restore remains redoable");
	const std::vector<bool> redoneMask = session.appliedGroundMask();
	check(redoneMask.size() == 4 && redoneMask[0] && !redoneMask[1]
		&& redoneMask[2] && !redoneMask[3],
		"redo recreates the exact index-aligned Ground mask");
	check(session.assignGroundToWorkingAsprs(error), "applied Ground can be assigned to ASPRS Working");
	check(valueIs(cloud, field::WorkingAsprs, 0, 2.0f) && valueIs(cloud, field::WorkingAsprs, 2, 2.0f),
		"ground assignment updates exactly the masked working classes");
	check(valueIs(cloud, field::OriginalClassification, 0, 1.0f)
		&& valueIs(cloud, field::OriginalClassification, 2, 6.0f),
		"ground assignment never overwrites Original Classification");
	check(session.applyManualAsprs({1}, 2, error), "manual annotation can add a point to Ground");
	check(session.appliedGroundMask()[1] && valueIs(cloud, field::GroundMask, 1, 1.0f),
		"manual ASPRS class 2 is immediately included in the DTM Ground mask");
	check(session.applyManualAsprs({1}, 5, error), "manual annotation can move a point out of Ground");
	check(!session.appliedGroundMask()[1] && valueIs(cloud, field::GroundMask, 1, 0.0f),
		"manual non-Ground class immediately removes the point from the DTM Ground mask");

	check(session.applyManualArchaeology({2}, ArchaeologyClass::Wall, error),
		"manual archaeology label succeeds independently of ASPRS");
	check(valueIs(cloud, field::ArchaeologyClass, 2, static_cast<float>(ArchaeologyClass::Wall))
		&& valueIs(cloud, field::TrainingLabel, 2, static_cast<float>(TrainingLabel::ManualTrusted)),
		"manual archaeology writes both semantic and provenance layers");
	check(!session.restoreGround(undoMessage),
		"Ground Restore refuses to undo an unrelated manual label");
	check(valueIs(cloud, field::WorkingAsprs, 2, 2.0f),
		"manual archaeology leaves ASPRS Working untouched");
	check(session.undo(undoMessage)
		&& valueIs(cloud, field::ArchaeologyClass, 2, 0.0f)
		&& valueIs(cloud, field::TrainingLabel, 2, 0.0f),
		"one undo atomically restores archaeology and Manual/Trusted provenance");
	check(session.redo(undoMessage)
		&& valueIs(cloud, field::ArchaeologyClass, 2, static_cast<float>(ArchaeologyClass::Wall))
		&& valueIs(cloud, field::TrainingLabel, 2, static_cast<float>(TrainingLabel::ManualTrusted)),
		"one redo atomically reapplies archaeology and Manual/Trusted provenance");
	check(session.clearArchaeology({2}, error)
		&& valueIs(cloud, field::ArchaeologyClass, 2, 0.0f)
		&& valueIs(cloud, field::TrainingLabel, 2, 0.0f),
		"Clear Archaeology atomically clears semantic and Manual/Trusted layers");
	check(valueIs(cloud, field::OriginalClassification, 2, 6.0f)
		&& valueIs(cloud, field::WorkingAsprs, 2, 2.0f),
		"Clear Archaeology preserves Original Classification and ASPRS Working");
	check(session.undo(undoMessage)
		&& valueIs(cloud, field::ArchaeologyClass, 2, static_cast<float>(ArchaeologyClass::Wall))
		&& valueIs(cloud, field::TrainingLabel, 2, static_cast<float>(TrainingLabel::ManualTrusted)),
		"one undo atomically restores both layers cleared by Clear Archaeology");
	check(session.redo(undoMessage)
		&& valueIs(cloud, field::ArchaeologyClass, 2, 0.0f)
		&& valueIs(cloud, field::TrainingLabel, 2, 0.0f),
		"one redo atomically clears both Archaeology layers again");
	check(valueIs(cloud, field::OriginalClassification, 2, 6.0f),
		"Archaeology undo/redo never modifies Original Classification");

	const double nan = std::numeric_limits<double>::quiet_NaN();
	check(session.setHagValues({0.0, 1.5, nan, 3.0}, QJsonObject(), 12, error),
		"aligned HAG values can be materialized");
	check(valueIs(cloud, field::HeightAboveGround, 1, 1.5f), "finite HAG values remain aligned");
	check(lookupField(cloud, field::HeightAboveGround)
		&& !std::isfinite(lookupField(cloud, field::HeightAboveGround)->getValue(2)),
		"HAG NODATA is represented as a non-finite Scalar Field value");
	check(!session.setHagValues({0.0}, QJsonObject(), 0, error), "misaligned HAG output is rejected");

	const float workingBeforeBootstrap = lookupField(cloud, field::WorkingAsprs)->getValue(0);
	const quint64 trustedBeforeBootstrap = session.trustedTrainingCount();
	check(!session.setBootstrapClusters({0}, {0.8f}, QJsonObject(), 0, error),
		"misaligned bootstrap output is rejected");
	error.clear();
	check(session.setBootstrapClusters({0, 1, 1, 2}, {0.9f, 0.7f, 0.6f, 0.8f},
		QJsonObject{{QStringLiteral("algorithm"), QStringLiteral("fixture")}}, 4, error),
		"aligned bootstrap cluster IDs materialize as Derived fields");
	check(valueIs(cloud, field::BootstrapCluster, 1, 1.0f)
		&& valueIs(cloud, field::BootstrapConfidence, 1, 0.7f),
		"bootstrap fields preserve source point order");
	check(valueIs(cloud, field::WorkingAsprs, 0, workingBeforeBootstrap)
		&& session.trustedTrainingCount() == trustedBeforeBootstrap,
		"bootstrap clusters never modify Working or Manual/Trusted labels");
	check(session.history().back().operation == QStringLiteral("Models.BootstrapClusters"),
		"bootstrap provenance is recorded separately from supervised prediction");

	check(!session.setAsprsPredictions({2}, {0.9f}, QJsonObject(), 0, error),
		"misaligned model predictions are rejected");
	error.clear();
	check(session.setAsprsPredictions({6, -1, 5, 3}, {0.90f, std::numeric_limits<float>::quiet_NaN(), 0.40f, 0.95f},
		QJsonObject{{QStringLiteral("model"), QStringLiteral("fixture")}}, 7, error),
		"aligned ASPRS predictions materialize as Derived fields");
	check(valueIs(cloud, field::AsprsPrediction, 0, 6.0f)
		&& valueIs(cloud, field::AsprsConfidence, 0, 0.90f),
		"prediction and confidence fields preserve source point order");
	check(session.history().back().operation == QStringLiteral("Models.Predict"),
		"prediction history does not mislabel every ML/DL classifier as Random Forest");
	check(!std::isfinite(lookupField(cloud, field::AsprsPrediction)->getValue(1)),
		"prediction -1 NODATA is stored as a non-finite Derived value");
	const float workingBeforePrediction = lookupField(cloud, field::WorkingAsprs)->getValue(0);
	check(valueIs(cloud, field::WorkingAsprs, 0, workingBeforePrediction),
		"Predict never changes ASPRS Working automatically");
	check(session.applyAsprsPredictions(0.65, error)
		&& valueIs(cloud, field::WorkingAsprs, 0, 6.0f)
		&& valueIs(cloud, field::WorkingAsprs, 3, 3.0f),
		"explicit Apply promotes only valid predictions above the confidence threshold");
	check(valueIs(cloud, field::WorkingAsprs, 2, 2.0f),
		"low-confidence prediction stays out of ASPRS Working");
	check(session.undo(undoMessage) && valueIs(cloud, field::WorkingAsprs, 0, workingBeforePrediction),
		"one Undo restores explicitly applied model predictions");
	check(!session.applyAsprsPredictions(0.99, error),
		"Apply rejects a threshold that produces no Working changes");
	check(session.dirty() && session.sourceRevision() > 0 && session.groundRevision() > 0
		&& session.hagRevision() > 0, "session revisions expose downstream cache-invalidating edits");
	check(!session.history().empty() && !session.historyAsJson().isEmpty(),
		"processing history is available in structured JSON form");

	check(!isTrustedAsprsLabel(2.5f, 2.5f) && !isTrustedAsprsLabel(256, 256)
		&& !isTrustedAsprsLabel(std::numeric_limits<float>::quiet_NaN(), 2),
		"invalid/fractional classes are not trusted");
	check(session.applyManualAsprs({0}, 6, error), "ASPRS reference can be confirmed manually");
	const auto trustedCount = session.trustedTrainingCount();
	check(session.applyManualArchaeology({3}, ArchaeologyClass::Wall, error)
		&& session.trustedTrainingCount() == trustedCount,
		"archaeology-only annotation never promotes an ASPRS label to training truth");
	check(session.applyManualArchaeology({0}, ArchaeologyClass::Wall, error)
		&& session.clearArchaeology({0}, error) && session.trustedTrainingCount() == trustedCount,
		"clearing archaeology does not invalidate separately confirmed ASPRS truth");
	check(session.restoreOriginalAsprs({0}, error) && session.trustedTrainingCount() == trustedCount-1,
		"changing Working to an unconfirmed class excludes it from training");
	check(session.undo(undoMessage) && session.trustedTrainingCount() == trustedCount,
		"undo restores the exact manually confirmed class and eligibility");
	check(session.setAsprsPredictions({5, -1, -1, -1}, {.95f, 0, 0, 0}, QJsonObject(), 0, error)
		&& session.applyAsprsPredictions(.8, error) && session.trustedTrainingCount() == trustedCount-1,
		"automatic prediction cannot inherit trust from a different manual class");
	const QJsonArray savedHistory = session.historyAsJson();
	ALiSSession reopened(cloud);
	check(reopened.initialize(error) && reopened.historyAsJson().size() == savedHistory.size()+1,
		"new Session restores cloud-attached processing history");
	check(reopened.sourceRevision() == session.sourceRevision() && !reopened.canUndo(),
		"revisions persist, but undo stack is intentionally in-memory only");
	cloud.setMetaData(QStringLiteral("ALiS.TestOnly"), true);
	check(reopened.trustedTrainingCount() == 0, "artificial persistence-test labels cannot train through UI");
	cloud.removeMetaData(QStringLiteral("ALiS.TestOnly"));
	// Legacy clouds retain their labels, but the ambiguous general provenance is not auto-trusted.
	cloud.deleteScalarField(cloud.getScalarFieldIndexByName(field::AsprsTrainingClass));
	ALiSSession legacy(cloud);
	check(legacy.initialize(error) && legacy.trustedTrainingCount() == 0,
		"legacy mixed-domain provenance requires explicit ASPRS reconfirmation");
	check(valueIs(cloud, field::WorkingAsprs, 0, 5), "legacy migration preserves Working labels");
	check(legacy.applyManualAsprs({0}, 5, error) && legacy.trustedTrainingCount() == 1,
		"confirming an unchanged Working class establishes domain-specific trust");
	check(legacy.undo(undoMessage) && legacy.trustedTrainingCount() == 0
		&& legacy.redo(undoMessage) && legacy.trustedTrainingCount() == 1,
		"reference provenance is atomic with label undo/redo");

	if (g_failures)
	{
		std::cerr << g_failures << " of " << g_checks << " session checks failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "ALiS session integration tests passed (" << g_checks << " checks)\n";
	return EXIT_SUCCESS;
}
