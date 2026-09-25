// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alis
{
	struct AspClass
	{
		std::uint8_t code = 0;
		const char* name = "";
		bool reserved = false;
	};

	//! LAS 1.4/1.5 ASPRS class catalogue. Codes 64-255 are user definable.
	const std::vector<AspClass>& asprsClasses();
	const AspClass* findAsprsClass(unsigned code);
	//! ALiS profile inside the LAS user-definable range (64-255).
	const std::vector<AspClass>& archaeologyAsprsClasses();

	enum class ArchaeologyClass : std::uint8_t
	{
		None = 0,
		ArchaeologicalStructure,
		Wall,
		CollapsedWall,
		Terrace,
		Mound,
		Ditch,
		Embankment,
		Platform,
		RoadPath,
		StoneConcentration,
		PossibleArchaeology,
		Uncertain
	};

	enum class TrainingLabel : std::uint8_t
	{
		None = 0,
		ManualTrusted = 1
	};

	//! Legacy separate-field taxonomy, retained only to migrate older qAL sessions.
	const char* archaeologyClassName(ArchaeologyClass value);

	enum class GroundPreset
	{
		Flat,
		ArchaeologicalSite,
		TerracedLandscape,
		Hilly,
		Rocky,
		Forest,
		DenseVegetation,
		CloudCompareReference
	};

	enum class SimpleLevel
	{
		Low,
		Medium,
		High
	};

	struct GroundParameters
	{
		double clothResolution = 1.0;
		double classificationThreshold = 0.5;
		double timeStep = 0.65;
		int rigidness = 2;
		int iterations = 500;
		bool slopeProcessing = false;
	};

	GroundParameters groundPresetParameters(GroundPreset preset, double spacingMetres);
	GroundParameters applySimpleGroundControls(const GroundParameters& base,
		SimpleLevel terrainComplexity,
		SimpleLevel preserveMicrorelief,
		SimpleLevel vegetationDensity,
		double spacingMetres);
	bool validGroundParameters(const GroundParameters& parameters);

	struct ScaleSuggestionInput
	{
		double spacing = 0.0;
		double densityPerSquareUnit = 0.0;
		double extentX = 0.0;
		double extentY = 0.0;
		bool metricUnitsConfirmed = false;
	};

	struct ScaleSuggestion
	{
		std::vector<double> radii;
		std::string explanation;
	};

	ScaleSuggestion suggestScales(const ScaleSuggestionInput& input, std::size_t maximumCount = 7);

	struct Descriptors
	{
		double density = 0.0;
		double linearity = 0.0;
		double planarity = 0.0;
		double sphericity = 0.0;
		double anisotropy = 0.0;
		double omnivariance = 0.0;
		double eigenentropy = 0.0;
		double surfaceVariation = 0.0;
	};

	//! Computes normalized PCA descriptors for lambda1 >= lambda2 >= lambda3.
	Descriptors descriptorsFromEigenvalues(double lambda1,
		double lambda2,
		double lambda3,
		std::size_t neighbourCount,
		double radius);

	struct SummaryStatistics
	{
		double minimum = 0.0;
		double maximum = 0.0;
		double mean = 0.0;
		double standardDeviation = 0.0;
		std::size_t validCount = 0;
		std::size_t nodataCount = 0;
	};

	SummaryStatistics summarizeFinite(const std::vector<double>& values);

	//! Bilinear DTM lookup with strict NODATA propagation and no extrapolation.
	double bilinearHeight(const std::vector<double>& grid,
		std::size_t width,
		std::size_t height,
		double gridMinX,
		double gridMinY,
		double step,
		double x,
		double y);
}
