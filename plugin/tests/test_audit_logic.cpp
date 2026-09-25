// SPDX-License-Identifier: GPL-2.0-or-later
#include "AuditLogic.h"
#include "WorkspaceLogic.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace
{
	int failures = 0;
	int assertions = 0;

	void expect(bool condition, const std::string& message)
	{
		++assertions;
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	bool close(double lhs, double rhs)
	{
		return std::abs(lhs - rhs) < 1.0e-12;
	}
}

int main()
{
	using namespace alis;

	expect(validateSelection(0, false) == SelectionState::None, "no selection is handled");
	expect(validateSelection(1, true) == SelectionState::SinglePointCloud, "a single point cloud is accepted");
	expect(validateSelection(1, false) == SelectionState::UnsupportedEntity, "a single unsupported entity is rejected");
	expect(validateSelection(2, true) == SelectionState::MultipleEntities, "multiple entities are rejected even when one is a cloud");

	Bounds3d bounds;
	bounds.valid = true;
	bounds.minimum = {10.0, -4.0, 2.5};
	bounds.maximum = {31.5, 6.0, 9.25};
	const Dimensions3d dimensions = calculateDimensions(bounds);
	expect(dimensions.valid, "valid bounds produce dimensions");
	expect(close(dimensions.x, 21.5) && close(dimensions.y, 10.0) && close(dimensions.z, 6.75), "dimensions are max minus min");

	Bounds3d inverted = bounds;
	inverted.maximum.x = 9.0;
	expect(!calculateDimensions(inverted).valid, "inverted bounds are rejected");
	expect(!calculateDimensions(Bounds3d{}).valid, "invalid bounds are handled");

	CloudFacts emptyCloud;
	emptyCloud.name = "empty";
	const AuditSummary emptySummary = summarizeCloud(emptyCloud);
	expect(emptySummary.empty, "an empty cloud is reported as empty");
	expect(emptySummary.scalarFieldCount == 0, "an empty cloud can have zero scalar fields");

	CloudFacts noFields;
	noFields.name = "no-fields";
	noFields.pointCount = 42;
	noFields.bounds = bounds;
	const AuditSummary noFieldSummary = summarizeCloud(noFields);
	expect(!noFieldSummary.empty, "a populated cloud is not empty");
	expect(noFieldSummary.scalarFieldCount == 0, "zero scalar fields are handled");

	CloudFacts withFields = noFields;
	withFields.scalarFields = {"Intensity", "Classification", "Gps Time"};
	const AuditSummary fieldSummary = summarizeCloud(withFields);
	expect(fieldSummary.scalarFieldCount == 3, "scalar-field names are counted without modification");
	expect(withFields.scalarFields.at(1) == "Classification", "scalar-field names are preserved");

	expect(asprsClasses().size() == 64, "ASPRS catalogue covers standard codes 0 through 63");
	expect(findAsprsClass(2) && std::string(findAsprsClass(2)->name) == "Ground", "ASPRS Ground is centralized as code 2");
	expect(findAsprsClass(64) == nullptr, "user-definable codes are not presented as ASPRS semantic classes");
	expect(archaeologyAsprsClasses().size() == 12 && archaeologyAsprsClasses().front().code == 64,
		"archaeology profile starts in the LAS user-defined range");
	expect(std::string(archaeologyClassName(ArchaeologyClass::Mound)) == "Mound", "archaeology classes are independent and named");

	const GroundParameters archaeological = groundPresetParameters(GroundPreset::ArchaeologicalSite, 0.025);
	expect(validGroundParameters(archaeological), "archaeological ground preset produces valid parameters");
	expect(archaeological.rigidness == 2 && archaeological.clothResolution >= 0.5, "archaeological preset balances microrelief and shrub rejection");
	const GroundParameters protectedMicrorelief = applySimpleGroundControls(archaeological, SimpleLevel::Medium, SimpleLevel::High, SimpleLevel::Medium, 0.025);
	expect(protectedMicrorelief.clothResolution < archaeological.clothResolution, "high microrelief preservation selects a moderately finer cloth");
	expect(protectedMicrorelief.rigidness >= archaeological.rigidness, "microrelief control does not make the cloth follow shrubs more easily");

	ScaleSuggestionInput scaleInput;
	scaleInput.spacing = 0.025;
	scaleInput.densityPerSquareUnit = 1600.0;
	scaleInput.extentX = 20.0;
	scaleInput.extentY = 20.0;
	scaleInput.metricUnitsConfirmed = true;
	const ScaleSuggestion scaleSuggestion = suggestScales(scaleInput);
	expect(!scaleSuggestion.radii.empty() && scaleSuggestion.radii.front() >= 4.0 * scaleInput.spacing, "suggested scales respect point spacing");
	expect(scaleSuggestion.radii.back() <= 5.0, "suggested scales respect the tile extent");
	scaleInput.metricUnitsConfirmed = false;
	expect(suggestScales(scaleInput).radii.empty(), "metre scales require an explicit unit confirmation");

	const Descriptors descriptors = descriptorsFromEigenvalues(4.0, 2.0, 1.0, 20, 1.0);
	expect(close(descriptors.linearity, 0.5) && close(descriptors.planarity, 0.25), "PCA descriptor formulas use ordered eigenvalues once");
	expect(close(descriptors.sphericity, 0.25) && close(descriptors.surfaceVariation, 1.0 / 7.0), "PCA shape descriptors are normalized");

	const double nan = std::numeric_limits<double>::quiet_NaN();
	const SummaryStatistics stats = summarizeFinite({1.0, 2.0, nan, 3.0});
	expect(stats.validCount == 3 && stats.nodataCount == 1 && close(stats.mean, 2.0), "feature statistics distinguish valid and NODATA values");
	const std::vector<double> grid = {0.0, 2.0, 2.0, 4.0};
	expect(close(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, 1.0, 1.0), 2.0), "bilinear DTM sampling interpolates four cell centers");
	expect(std::isnan(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, -1.0, 0.0)), "DTM sampling never extrapolates beyond the grid");

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed\n";
		return 1;
	}

	std::cout << "ALiS logic tests: " << assertions << " assertions passed\n";
	return 0;
}
