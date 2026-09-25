// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceLogic.h"
#include "AnnotationGeometry.h"

#include <algorithm>
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

	bool nearlyEqual(double actual, double expected, double tolerance = 1.0e-12)
	{
		return std::isfinite(actual) && std::isfinite(expected)
			&& std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
	}

	void testClassCatalogues()
	{
		using namespace alis;
		const std::vector<AspClass>& classes = asprsClasses();
		check(classes.size() == 64, "ASPRS catalogue covers every standardized code 0..63");
		check(findAsprsClass(2) && std::string(findAsprsClass(2)->name) == "Ground", "ASPRS code 2 is Ground");
		check(findAsprsClass(8) && findAsprsClass(8)->reserved, "ASPRS code 8 remains reserved");
		check(findAsprsClass(18) && std::string(findAsprsClass(18)->name) == "High noise", "ASPRS code 18 is High noise");
		check(findAsprsClass(63) && findAsprsClass(63)->reserved, "ASPRS code 63 remains reserved");
		check(findAsprsClass(64) == nullptr, "user-definable codes are not presented as ASPRS semantic classes");
		check(archaeologyAsprsClasses().size() == 12
			&& archaeologyAsprsClasses().front().code == 64
			&& archaeologyAsprsClasses().back().code == 75,
			"ALiS archaeology uses a stable LAS user-defined class profile");
		check(std::string(archaeologyClassName(ArchaeologyClass::CollapsedWall)) == "CollapsedWall",
			"archaeology labels are independent, named values");
		check(std::string(archaeologyClassName(static_cast<ArchaeologyClass>(255))) == "Unknown",
			"unknown archaeology labels are handled safely");
	}

	void testGroundParameterLogic()
	{
		using namespace alis;
		for (GroundPreset preset : {GroundPreset::Flat,
			 GroundPreset::ArchaeologicalSite,
			 GroundPreset::TerracedLandscape,
			 GroundPreset::Hilly,
			 GroundPreset::Rocky,
			 GroundPreset::Forest,
			 GroundPreset::DenseVegetation})
		{
			check(validGroundParameters(groundPresetParameters(preset, 0.025)),
				"every ground preset yields valid effective parameters");
		}

		const GroundParameters archaeological = groundPresetParameters(GroundPreset::ArchaeologicalSite, 0.025);
		const GroundParameters conservative = applySimpleGroundControls(archaeological,
			SimpleLevel::Low,
			SimpleLevel::Low,
			SimpleLevel::High,
			0.025);
		check(validGroundParameters(conservative), "simple controls preserve the CSF parameter domain");
		check(conservative.rigidness >= 1 && conservative.rigidness <= 3, "simple controls clamp CSF rigidness");
		check(conservative.clothResolution >= 0.1, "simple controls enforce a spacing-aware cloth lower bound");

		GroundParameters invalid = archaeological;
		invalid.timeStep = std::numeric_limits<double>::quiet_NaN();
		check(!validGroundParameters(invalid), "non-finite ground parameters are rejected");
		invalid = archaeological;
		invalid.iterations = 0;
		check(!validGroundParameters(invalid), "zero CSF iterations are rejected");
	}

	void testScaleSuggestion()
	{
		using namespace alis;
		ScaleSuggestionInput input;
		input.spacing = 0.025;
		input.densityPerSquareUnit = 1600.0;
		input.extentX = 20.0;
		input.extentY = 20.0;

		const ScaleSuggestion guarded = suggestScales(input, 7);
		check(guarded.radii.empty(), "metre-based suggestions require an explicit metric-unit confirmation");
		check(!guarded.explanation.empty(), "a rejected scale suggestion explains why it was rejected");

		input.metricUnitsConfirmed = true;
		const ScaleSuggestion first = suggestScales(input, 7);
		const ScaleSuggestion second = suggestScales(input, 7);
		check(!first.radii.empty(), "valid spacing and extent produce scales");
		check(first.radii == second.radii, "scale suggestion is deterministic");
		check(first.radii.size() <= 7, "scale suggestion respects the requested maximum count");
		for (std::size_t i = 0; i < first.radii.size(); ++i)
		{
			check(first.radii[i] > 0.0, "suggested radii are positive");
			check(first.radii[i] <= 5.0 + 1.0e-12, "suggested radii do not exceed one quarter of the smaller XY extent");
			if (i)
			{
				check(first.radii[i] > first.radii[i - 1], "suggested radii are strictly increasing");
			}
		}

		ScaleSuggestionInput densityFallback = input;
		densityFallback.spacing = 0.0;
		const ScaleSuggestion fromDensity = suggestScales(densityFallback, 3);
		check(!fromDensity.radii.empty(), "point density provides a deterministic spacing fallback");
		check(fromDensity.radii.size() <= 3, "density fallback respects the maximum scale count");

		input.extentX = 0.0;
		check(suggestScales(input).radii.empty(), "degenerate horizontal extents are rejected");
		check(suggestScales(densityFallback, 0).radii.empty(), "requesting zero scales returns no scales");
	}

	void testPcaDescriptors()
	{
		using namespace alis;
		const Descriptors d = descriptorsFromEigenvalues(3.0, 2.0, 1.0, 8, 2.0);
		check(nearlyEqual(d.linearity, 1.0 / 3.0), "linearity follows the documented eigenvalue formula");
		check(nearlyEqual(d.planarity, 1.0 / 3.0), "planarity follows the documented eigenvalue formula");
		check(nearlyEqual(d.sphericity, 1.0 / 3.0), "sphericity follows the documented eigenvalue formula");
		check(nearlyEqual(d.anisotropy, 2.0 / 3.0), "anisotropy follows the documented eigenvalue formula");
		check(nearlyEqual(d.omnivariance, std::cbrt(6.0)), "omnivariance follows the documented eigenvalue formula");
		check(nearlyEqual(d.surfaceVariation, 1.0 / 6.0), "surface variation is lambda3 divided by the eigenvalue sum");
		check(nearlyEqual(d.eigenentropy, -(3.0 * std::log(3.0) + 2.0 * std::log(2.0))),
			"eigenentropy follows the implemented unnormalised-eigenvalue convention");
		check(nearlyEqual(d.density, 8.0 / ((4.0 / 3.0) * 3.14159265358979323846 * 8.0)),
			"3D density uses a spherical neighbourhood volume");

		const Descriptors invalidOrder = descriptorsFromEigenvalues(1.0, 2.0, 0.5, 4, 1.0);
		check(invalidOrder.linearity == 0.0 && invalidOrder.planarity == 0.0,
			"unsorted eigenvalues do not produce misleading descriptors");
		check(invalidOrder.density > 0.0, "density remains independent of PCA validity");
		const Descriptors invalidRadius = descriptorsFromEigenvalues(3.0, 2.0, 1.0, 8, 0.0);
		check(invalidRadius.density == 0.0 && invalidRadius.planarity == 0.0,
			"non-positive radii are rejected without division by zero");
	}

	void testStatistics()
	{
		using namespace alis;
		const double nan = std::numeric_limits<double>::quiet_NaN();
		const SummaryStatistics s = summarizeFinite({1.0, 2.0, nan, 3.0, 4.0,
			std::numeric_limits<double>::infinity()});
		check(s.validCount == 4 && s.nodataCount == 2, "statistics count finite and NODATA samples separately");
		check(nearlyEqual(s.minimum, 1.0) && nearlyEqual(s.maximum, 4.0), "statistics preserve finite extrema");
		check(nearlyEqual(s.mean, 2.5), "statistics use an online finite-only mean");
		check(nearlyEqual(s.standardDeviation, std::sqrt(1.25)), "statistics report population standard deviation");

		const SummaryStatistics empty = summarizeFinite({nan});
		check(empty.validCount == 0 && empty.nodataCount == 1, "all-NODATA statistics retain their counts");
		check(std::isnan(empty.mean) && std::isnan(empty.standardDeviation),
			"all-NODATA statistics do not fabricate numeric summaries");
	}

	void testBilinearRasterEdges()
	{
		using namespace alis;
		// Grid values are at cell centres (0.5,0.5), (1.5,0.5), ...
		const std::vector<double> grid = {1.0, 2.0, 3.0, 4.0};
		check(nearlyEqual(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, 0.5, 0.5), 1.0),
			"DTM lookup honours the lower-left cell centre");
		check(nearlyEqual(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, 1.0, 1.0), 2.5),
			"DTM lookup bilinearly interpolates the four supporting cells");
		check(nearlyEqual(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, 1.5, 1.5), 4.0),
			"DTM lookup honours the upper-right cell centre");
		check(std::isnan(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, 0.49, 0.5)),
			"DTM lookup never extrapolates past the lower grid edge");
		check(std::isnan(bilinearHeight(grid, 2, 2, 0.0, 0.0, 1.0, 1.51, 1.5)),
			"DTM lookup never extrapolates past the upper grid edge");

		std::vector<double> withNoData = grid;
		withNoData[3] = std::numeric_limits<double>::quiet_NaN();
		check(std::isnan(bilinearHeight(withNoData, 2, 2, 0.0, 0.0, 1.0, 1.0, 1.0)),
			"one missing supporting cell propagates NODATA");
		check(std::isnan(bilinearHeight({1.0}, 1, 1, 0.0, 0.0, 1.0, 0.0, 0.0)),
			"grids too small for bilinear interpolation are rejected");
		check(std::isnan(bilinearHeight(grid, 2, 2, 0.0, 0.0, 0.0, 1.0, 1.0)),
			"a non-positive DTM step is rejected");
	}
}

void testAnnotationGeometry()
{
	using namespace alis;
	SectionIndex index;
	std::vector<double> coordinates;
	for (int i = 0; i < 10003; ++i) coordinates.push_back((i * 317 % 10003) * .013 - 15.0);
	index.build(static_cast<unsigned>(coordinates.size()), -15, 116, [&](unsigned id) { return coordinates[id]; });
	std::vector<unsigned> all = index.ids(); std::sort(all.begin(), all.end());
	bool complete = all.size() == coordinates.size();
	for (unsigned i = 0; i < all.size(); ++i) complete = complete && all[i] == i;
	check(complete, "section index retains each original ID exactly once without sampling");
	for (const auto limits : {std::pair<double,double>{-100,-90}, {-15,-15}, {0,0.1}, {14.1,19.9}, {100,120}, {200,210}, {-100,1000}})
	{
		const auto range = index.range(limits.first, limits.second);
		std::vector<unsigned> actual, expected;
		for (auto at = range.first; at < range.second; ++at)
		{ const unsigned id = index.ids()[at]; if (coordinates[id] >= limits.first && coordinates[id] <= limits.second) actual.push_back(id); }
		for (unsigned id = 0; id < coordinates.size(); ++id) if (coordinates[id] >= limits.first && coordinates[id] <= limits.second) expected.push_back(id);
		std::sort(actual.begin(), actual.end());
		check(actual == expected, "indexed slab selection equals full-cloud brute force including bin boundaries");
	}
	index.build(5, 7, 7, [](unsigned) { return 7.; });
	check(index.range(7,7).second == 5, "zero-span cloud is indexed without division by zero");
	index.build(0, 0, 0, [](unsigned) { return 0.; });
	check(index.range(0,1).second == 0, "empty section index is safe");
	const AnnotationPolygon square{{0,0},{10,0},{10,10},{0,10}};
	check(annotationContains(square, 5, 5), "rectangle includes interior");
	check(annotationContains(square, 0, 5) && annotationContains(square,10,10), "selection includes exact polygon edges and vertices");
	check(!annotationContains(square, 11, 5), "rectangle excludes exterior");
	check(!annotationContains({}, 0, 0), "unfinished polygon selects nothing");
	check(!annotationContains(square, std::numeric_limits<double>::quiet_NaN(), 0), "selection rejects NaN coordinates");
	const AnnotationPolygon concave{{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}};
	check(annotationContains(concave,.5,3) && !annotationContains(concave,2,2), "lasso supports concave shapes");
}

int main()
{
	testAnnotationGeometry();
	testClassCatalogues();
	testGroundParameterLogic();
	testScaleSuggestion();
	testPcaDescriptors();
	testStatistics();
	testBilinearRasterEdges();

	if (g_failures)
	{
		std::cerr << g_failures << " of " << g_checks << " workspace-logic checks failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "ALiS workspace logic tests passed (" << g_checks << " checks)\n";
	return EXIT_SUCCESS;
}
