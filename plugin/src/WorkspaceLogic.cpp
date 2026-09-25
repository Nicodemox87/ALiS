// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace
{
	double clampValue(double value, double minimum, double maximum)
	{
		return std::max(minimum, std::min(maximum, value));
	}

	double preferredScale(double value)
	{
		if (!(value > 0.0) || !std::isfinite(value))
		{
			return 0.0;
		}
		const double exponent = std::floor(std::log10(value));
		const double magnitude = std::pow(10.0, exponent);
		const double normalized = value / magnitude;
		double preferred = 1.0;
		if (normalized > 1.0 && normalized <= 2.0)
		{
			preferred = 2.0;
		}
		else if (normalized > 2.0 && normalized <= 5.0)
		{
			preferred = 5.0;
		}
		else if (normalized > 5.0)
		{
			preferred = 10.0;
		}
		return preferred * magnitude;
	}
}

namespace alis
{
	const std::vector<AspClass>& asprsClasses()
	{
		// ASPRS LAS 1.4 R16 / LAS 1.5 R00, published 2025. Classification
		// flags are separate dimensions and deliberately do not appear here.
		static const std::vector<AspClass> classes = {
			{0, "Created, never classified", false},
			{1, "Unclassified", false},
			{2, "Ground", false},
			{3, "Low vegetation", false},
			{4, "Medium vegetation", false},
			{5, "High vegetation", false},
			{6, "Building", false},
			{7, "Low point (noise)", false},
			{8, "Reserved", true},
			{9, "Water", false},
			{10, "Rail", false},
			{11, "Road surface", false},
			{12, "Reserved", true},
			{13, "Wire - guard (shield)", false},
			{14, "Wire - conductor (phase)", false},
			{15, "Transmission tower", false},
			{16, "Wire-structure connector", false},
			{17, "Bridge deck", false},
			{18, "High noise", false},
			{19, "Overhead structure", false},
			{20, "Ignored ground", false},
			{21, "Snow", false},
			{22, "Temporal exclusion", false},
			{23, "Reserved", true}, {24, "Reserved", true}, {25, "Reserved", true},
			{26, "Reserved", true}, {27, "Reserved", true},
			{28, "Reserved", true}, {29, "Reserved", true}, {30, "Reserved", true},
			{31, "Reserved", true}, {32, "Reserved", true}, {33, "Reserved", true},
			{34, "Reserved", true}, {35, "Reserved", true}, {36, "Reserved", true},
			{37, "Reserved", true}, {38, "Reserved", true}, {39, "Reserved", true},
			{40, "Reserved", true}, {41, "Reserved", true}, {42, "Reserved", true},
			{43, "Reserved", true}, {44, "Reserved", true}, {45, "Reserved", true},
			{46, "Reserved", true}, {47, "Reserved", true}, {48, "Reserved", true},
			{49, "Reserved", true}, {50, "Reserved", true}, {51, "Reserved", true},
			{52, "Reserved", true}, {53, "Reserved", true}, {54, "Reserved", true},
			{55, "Reserved", true}, {56, "Reserved", true}, {57, "Reserved", true},
			{58, "Reserved", true}, {59, "Reserved", true}, {60, "Reserved", true},
			{61, "Reserved", true}, {62, "Reserved", true}, {63, "Reserved", true},
		};
		return classes;
	}

	const AspClass* findAsprsClass(unsigned code)
	{
		const auto& classes = asprsClasses();
		if (code < classes.size())
		{
			return &classes[code];
		}
		return nullptr; // 64..255 are user definable, not ASPRS semantic classes.
	}

	const std::vector<AspClass>& archaeologyAsprsClasses()
	{
		// ALiS classification profile v1. These are intentionally in the
		// LAS/ASPRS user-definable range and must not be presented as standard ASPRS names.
		static const std::vector<AspClass> classes = {
			{64, "Archaeological structure", false},
			{65, "Archaeological wall", false},
			{66, "Collapsed archaeological wall", false},
			{67, "Archaeological terrace", false},
			{68, "Archaeological mound", false},
			{69, "Archaeological ditch", false},
			{70, "Archaeological embankment", false},
			{71, "Archaeological platform", false},
			{72, "Archaeological road / path", false},
			{73, "Archaeological stone concentration", false},
			{74, "Possible archaeology", false},
			{75, "Uncertain archaeology", false},
		};
		return classes;
	}

	const char* archaeologyClassName(ArchaeologyClass value)
	{
		static const char* names[] = {"None", "ArchaeologicalStructure", "Wall", "CollapsedWall",
			"Terrace", "Mound", "Ditch", "Embankment", "Platform", "RoadPath",
			"StoneConcentration", "PossibleArchaeology", "Uncertain"};
		const unsigned index = static_cast<unsigned>(value);
		return index < sizeof(names) / sizeof(names[0]) ? names[index] : "Unknown";
	}

	GroundParameters groundPresetParameters(GroundPreset preset, double spacingMetres)
	{
		// Canopy-dominated spacing is neither ground spacing nor vertical noise.
		(void)spacingMetres;
		GroundParameters p;
		p.clothResolution = 2.0;
		p.classificationThreshold = 0.5;
		switch (preset)
		{
		case GroundPreset::Flat:
			p.rigidness = 3;
			break;
		case GroundPreset::ArchaeologicalSite:
			p.clothResolution = 1.0;
			p.rigidness = 2;
			break;
		case GroundPreset::TerracedLandscape:
			p.clothResolution = 1.0;
			p.rigidness = 2;
			p.slopeProcessing = true;
			break;
		case GroundPreset::Hilly:
			p.clothResolution = 1.0;
			p.rigidness = 1;
			p.slopeProcessing = true;
			break;
		case GroundPreset::Rocky:
			p.clothResolution = 0.5;
			p.rigidness = 1;
			p.slopeProcessing = true;
			break;
		case GroundPreset::Forest:
		case GroundPreset::DenseVegetation:
		case GroundPreset::CloudCompareReference:
			// Vegetation does not imply flat terrain or justify a narrower band.
			break;
		}
		return p;
	}

	GroundParameters applySimpleGroundControls(const GroundParameters& base,
		SimpleLevel terrainComplexity,
		SimpleLevel preserveMicrorelief,
		SimpleLevel vegetationDensity,
		double spacingMetres)
	{
		GroundParameters p = base;
		if (terrainComplexity == SimpleLevel::Low) { p.rigidness = 3; p.slopeProcessing = false; }
		if (terrainComplexity == SimpleLevel::High) { p.rigidness = 1; p.slopeProcessing = true; }
		if (preserveMicrorelief == SimpleLevel::Low)
		{
			p.clothResolution *= 1.4;
		}
		else if (preserveMicrorelief == SimpleLevel::High)
		{
			p.clothResolution *= 0.75;
		}
		if (vegetationDensity == SimpleLevel::Low)
		{
			p.clothResolution *= 0.8;
		}
		else if (vegetationDensity == SimpleLevel::High)
		{
			p.clothResolution *= 1.25;
			// Canopy is not terrain shape: retain tolerance, rigidity and slope recovery.
		}
		// Density adaptation is an explicit, inspectable action. Neutral controls
		// must preserve an imported/manual recipe, even on a sparse cloud.
		(void)spacingMetres;
		p.rigidness = std::max(1, std::min(3, p.rigidness));
		return p;
	}

	bool validGroundParameters(const GroundParameters& p)
	{
		return std::isfinite(p.clothResolution) && p.clothResolution > 0.0
			&& std::isfinite(p.classificationThreshold) && p.classificationThreshold > 0.0
			&& std::isfinite(p.timeStep) && p.timeStep > 0.0
			&& p.rigidness >= 1 && p.rigidness <= 3 && p.iterations > 0;
	}

	ScaleSuggestion suggestScales(const ScaleSuggestionInput& input, std::size_t maximumCount)
	{
		ScaleSuggestion result;
		if (!input.metricUnitsConfirmed || maximumCount == 0)
		{
			result.explanation = "Metric units must be confirmed before metre-based scale suggestions are enabled.";
			return result;
		}
		double spacing = input.spacing;
		if (!(spacing > 0.0) && input.densityPerSquareUnit > 0.0)
		{
			spacing = 1.0 / std::sqrt(input.densityPerSquareUnit);
		}
		const double smallerExtent = std::min(input.extentX, input.extentY);
		if (!(spacing > 0.0) || !(smallerExtent > 0.0) || !std::isfinite(spacing) || !std::isfinite(smallerExtent))
		{
			result.explanation = "A finite point spacing and horizontal bounding box are required.";
			return result;
		}
		const double first = preferredScale(std::max(4.0 * spacing, 0.05));
		const double maximum = std::min(smallerExtent / 4.0, std::max(first, 256.0 * spacing));
		for (double scale = first; scale <= maximum * 1.000001 && result.radii.size() < maximumCount; scale = preferredScale(scale * 1.01))
		{
			if (result.radii.empty() || scale > result.radii.back() * 1.01)
			{
				result.radii.push_back(scale);
			}
			else
			{
				break;
			}
		}
		std::ostringstream message;
		message << "Smallest radius is the next 1-2-5 preferred value at or above four times spacing ("
			<< spacing << " m); larger radii follow a deterministic near-doubling sequence and stop at min(extent/4, 256 x spacing).";
		result.explanation = message.str();
		return result;
	}

	Descriptors descriptorsFromEigenvalues(double l1, double l2, double l3, std::size_t count, double radius)
	{
		Descriptors d;
		if (!(radius > 0.0) || !std::isfinite(radius))
		{
			return d;
		}
		d.density = static_cast<double>(count) / ((4.0 / 3.0) * 3.14159265358979323846 * radius * radius * radius);
		if (!std::isfinite(l1) || !std::isfinite(l2) || !std::isfinite(l3) || l1 <= 0.0 || l2 < 0.0 || l3 < 0.0)
		{
			return d;
		}
		if (l2 > l1 || l3 > l2)
		{
			return d;
		}
		const double sum = l1 + l2 + l3;
		d.linearity = (l1 - l2) / l1;
		d.planarity = (l2 - l3) / l1;
		d.sphericity = l3 / l1;
		d.anisotropy = (l1 - l3) / l1;
		d.omnivariance = std::cbrt(l1 * l2 * l3);
		d.surfaceVariation = sum > 0.0 ? l3 / sum : 0.0;
		d.eigenentropy = 0.0;
		for (double value : {l1, l2, l3})
		{
			if (value > 0.0)
			{
				d.eigenentropy -= value * std::log(value);
			}
		}
		return d;
	}

	SummaryStatistics summarizeFinite(const std::vector<double>& values)
	{
		SummaryStatistics result;
		double mean = 0.0;
		double m2 = 0.0;
		for (double value : values)
		{
			if (!std::isfinite(value))
			{
				++result.nodataCount;
				continue;
			}
			++result.validCount;
			if (result.validCount == 1)
			{
				result.minimum = result.maximum = value;
			}
			else
			{
				result.minimum = std::min(result.minimum, value);
				result.maximum = std::max(result.maximum, value);
			}
			const double delta = value - mean;
			mean += delta / static_cast<double>(result.validCount);
			m2 += delta * (value - mean);
		}
		result.mean = result.validCount ? mean : std::numeric_limits<double>::quiet_NaN();
		result.standardDeviation = result.validCount ? std::sqrt(m2 / static_cast<double>(result.validCount)) : std::numeric_limits<double>::quiet_NaN();
		return result;
	}

	double bilinearHeight(const std::vector<double>& grid, std::size_t width, std::size_t height,
		double minX, double minY, double step, double x, double y)
	{
		const double nodata = std::numeric_limits<double>::quiet_NaN();
		if (width < 2 || height < 2 || grid.size() != width * height || !(step > 0.0))
		{
			return nodata;
		}
		const double gx = (x - minX) / step - 0.5;
		const double gy = (y - minY) / step - 0.5;
		if (gx < 0.0 || gy < 0.0 || gx > static_cast<double>(width - 1) || gy > static_cast<double>(height - 1))
		{
			return nodata;
		}
		const std::size_t x0 = static_cast<std::size_t>(std::floor(gx));
		const std::size_t y0 = static_cast<std::size_t>(std::floor(gy));
		const std::size_t x1 = std::min(x0 + 1, width - 1);
		const std::size_t y1 = std::min(y0 + 1, height - 1);
		const double q00 = grid[y0 * width + x0];
		const double q10 = grid[y0 * width + x1];
		const double q01 = grid[y1 * width + x0];
		const double q11 = grid[y1 * width + x1];
		if (!std::isfinite(q00) || !std::isfinite(q10) || !std::isfinite(q01) || !std::isfinite(q11))
		{
			return nodata;
		}
		const double tx = gx - static_cast<double>(x0);
		const double ty = gy - static_cast<double>(y0);
		return (1.0 - ty) * ((1.0 - tx) * q00 + tx * q10) + ty * ((1.0 - tx) * q01 + tx * q11);
	}
}
