// SPDX-License-Identifier: GPL-2.0-or-later
#include "FeatureEngine.h"

#include <ccBBox.h>
#include <ccNormalVectors.h>
#include <ccOctree.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>

#include <CCConst.h>
#include <DgmOctree.h>
#include <GenericProgressCallback.h>
#include <Jacobi.h>
#include <ReferenceCloud.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <utility>

namespace
{
	constexpr double Pi = 3.141592653589793238462643383279502884;
	constexpr double UnitSphereVolume = 4.0 * Pi / 3.0;

	double quietNaN()
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	bool finite(double value)
	{
		return std::isfinite(value) != 0;
	}

	bool isSourceFeature(alis::FeatureId feature)
	{
		using alis::FeatureId;
		switch (feature)
		{
		case FeatureId::SourceValidCount:
		case FeatureId::SourceMinimum:
		case FeatureId::SourceMaximum:
		case FeatureId::SourceRange:
		case FeatureId::SourceMean:
		case FeatureId::SourceStandardDeviation:
		case FeatureId::SourcePercentile10:
		case FeatureId::SourcePercentile25:
		case FeatureId::SourceMedian:
		case FeatureId::SourcePercentile75:
		case FeatureId::SourcePercentile90:
			return true;
		default:
			return false;
		}
	}

	bool needsSourceQuantiles(alis::FeatureId feature)
	{
		using alis::FeatureId;
		return feature == FeatureId::SourcePercentile10
		       || feature == FeatureId::SourcePercentile25
		       || feature == FeatureId::SourceMedian
		       || feature == FeatureId::SourcePercentile75
		       || feature == FeatureId::SourcePercentile90;
	}

	bool needsZQuantiles(alis::FeatureId feature)
	{
		using alis::FeatureId;
		return feature == FeatureId::ZPercentile10
		       || feature == FeatureId::ZPercentile25
		       || feature == FeatureId::ZMedian
		       || feature == FeatureId::ZPercentile75
		       || feature == FeatureId::ZPercentile90;
	}

	bool needsPca(alis::FeatureId feature)
	{
		using alis::FeatureId;
		switch (feature)
		{
		case FeatureId::Eigenvalue1:
		case FeatureId::Eigenvalue2:
		case FeatureId::Eigenvalue3:
		case FeatureId::EigenvaluesSum:
		case FeatureId::PCA1:
		case FeatureId::PCA2:
		case FeatureId::Linearity:
		case FeatureId::Planarity:
		case FeatureId::Sphericity:
		case FeatureId::Anisotropy:
		case FeatureId::Omnivariance:
		case FeatureId::Eigenentropy:
		case FeatureId::SurfaceVariation:
		case FeatureId::Verticality:
		case FeatureId::NormalX:
		case FeatureId::NormalY:
		case FeatureId::NormalZ:
		case FeatureId::NormalZAbsolute:
		case FeatureId::Dip:
		case FeatureId::DipDirection:
		case FeatureId::Roughness:
		case FeatureId::SignedRoughness:
		case FeatureId::MeanCurvature:
		case FeatureId::GaussianCurvature:
		case FeatureId::NormalChangeRate:
		case FeatureId::MomentOrder1:
			return true;
		default:
			return false;
		}
	}

	bool needsCurvature(alis::FeatureId feature)
	{
		using alis::FeatureId;
		return feature == FeatureId::MeanCurvature || feature == FeatureId::GaussianCurvature;
	}

	bool needsMoment(alis::FeatureId feature)
	{
		return feature == alis::FeatureId::MomentOrder1;
	}

	bool needsGeometry(alis::FeatureId feature)
	{
		using alis::FeatureId;
		return !isSourceFeature(feature)
		       && feature != FeatureId::NeighborCount
		       && feature != FeatureId::Density2D
		       && feature != FeatureId::Density3D;
	}

	alis::FeatureKey canonicalKey(const alis::FeatureKey& input)
	{
		alis::FeatureKey key = input;
		if (!isSourceFeature(key.feature))
		{
			key.sourceScalarField.clear();
		}
		return key;
	}

	alis::FeatureKey canonicalKey(const alis::FeatureRequest& input)
	{
		alis::FeatureKey key;
		key.feature = input.feature;
		key.radius = input.radius;
		key.sourceScalarField = input.sourceScalarField;
		return canonicalKey(key);
	}

	double quantileOfSorted(const std::vector<double>& sortedValues, double probability)
	{
		if (sortedValues.empty())
		{
			return quietNaN();
		}
		if (sortedValues.size() == 1)
		{
			return sortedValues.front();
		}

		probability = std::max(0.0, std::min(1.0, probability));
		const double position = probability * static_cast<double>(sortedValues.size() - 1);
		const std::size_t low = static_cast<std::size_t>(std::floor(position));
		const std::size_t high = static_cast<std::size_t>(std::ceil(position));
		const double fraction = position - static_cast<double>(low);
		return sortedValues[low] + fraction * (sortedValues[high] - sortedValues[low]);
	}

	struct RunningStatistics
	{
		std::size_t count = 0;
		double mean = 0.0;
		double m2 = 0.0;
		double minimum = std::numeric_limits<double>::infinity();
		double maximum = -std::numeric_limits<double>::infinity();
		std::vector<double> values;

		void add(double value, bool keepValue)
		{
			if (!finite(value))
			{
				return;
			}
			++count;
			const double delta = value - mean;
			mean += delta / static_cast<double>(count);
			m2 += delta * (value - mean);
			minimum = std::min(minimum, value);
			maximum = std::max(maximum, value);
			if (keepValue)
			{
				values.push_back(value);
			}
		}

		double standardDeviation() const
		{
			return count ? std::sqrt(std::max(0.0, m2 / static_cast<double>(count))) : quietNaN();
		}
	};

	struct GeometrySummary
	{
		std::size_t count = 0;
		double mean[3] = { 0.0, 0.0, 0.0 };
		double covarianceNumerator[3][3] = {};
		double zMinimum = std::numeric_limits<double>::infinity();
		double zMaximum = -std::numeric_limits<double>::infinity();
		std::vector<double> zValues;

		void add(double x, double y, double z, bool keepZ)
		{
			if (!finite(x) || !finite(y) || !finite(z))
			{
				return;
			}

			const double value[3] = { x, y, z };
			const double oldDelta[3] = { value[0] - mean[0], value[1] - mean[1], value[2] - mean[2] };
			++count;
			for (int axis = 0; axis < 3; ++axis)
			{
				mean[axis] += oldDelta[axis] / static_cast<double>(count);
			}
			const double newDelta[3] = { value[0] - mean[0], value[1] - mean[1], value[2] - mean[2] };
			for (int row = 0; row < 3; ++row)
			{
				for (int column = row; column < 3; ++column)
				{
					covarianceNumerator[row][column] += oldDelta[row] * newDelta[column];
					covarianceNumerator[column][row] = covarianceNumerator[row][column];
				}
			}
			zMinimum = std::min(zMinimum, z);
			zMaximum = std::max(zMaximum, z);
			if (keepZ)
			{
				zValues.push_back(z);
			}
		}
	};

	struct PcaSummary
	{
		bool valid = false;
		double eigenvalue[3] = { quietNaN(), quietNaN(), quietNaN() };
		double eigenvector[3][3] = {};
	};

	PcaSummary computePca(const GeometrySummary& geometry)
	{
		PcaSummary result;
		if (geometry.count < 3)
		{
			return result;
		}

		CCCoreLib::SquareMatrixd covariance(3);
		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 3; ++column)
			{
				covariance.m_values[row][column] = geometry.covarianceNumerator[row][column] / static_cast<double>(geometry.count);
			}
		}

		CCCoreLib::SquareMatrixd eigenvectors;
		std::vector<double> eigenvalues;
		if (!CCCoreLib::Jacobi<double>::ComputeEigenValuesAndVectors(covariance, eigenvectors, eigenvalues, true)
		    || eigenvalues.size() != 3
		    || !CCCoreLib::Jacobi<double>::SortEigenValuesAndVectors(eigenvectors, eigenvalues))
		{
			return result;
		}

		const double tolerance = std::numeric_limits<double>::epsilon()
		                         * std::max(1.0, std::abs(eigenvalues[0])) * 128.0;
		for (int i = 0; i < 3; ++i)
		{
			if (!finite(eigenvalues[i]) || eigenvalues[i] < -tolerance)
			{
				return result;
			}
			result.eigenvalue[i] = std::max(0.0, eigenvalues[i]);
			if (!CCCoreLib::Jacobi<double>::GetEigenVector(eigenvectors, static_cast<unsigned>(i), result.eigenvector[i]))
			{
				return PcaSummary();
			}
		}

		// Eigenvector sign is arbitrary. Use a deterministic +Z, then +Y, then +X convention.
		double* normal = result.eigenvector[2];
		const bool flip = normal[2] < 0.0
		                  || (normal[2] == 0.0 && normal[1] < 0.0)
		                  || (normal[2] == 0.0 && normal[1] == 0.0 && normal[0] < 0.0);
		if (flip)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				normal[axis] = -normal[axis];
			}
		}

		result.valid = true;
		return result;
	}

	bool solveSixBySix(double augmented[6][7], double solution[6])
	{
		double maximum = 0.0;
		for (int row = 0; row < 6; ++row)
		{
			for (int column = 0; column < 6; ++column)
			{
				maximum = std::max(maximum, std::abs(augmented[row][column]));
			}
		}
		const double tolerance = std::numeric_limits<double>::epsilon() * std::max(1.0, maximum) * 1024.0;

		for (int column = 0; column < 6; ++column)
		{
			int pivot = column;
			for (int row = column + 1; row < 6; ++row)
			{
				if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
				{
					pivot = row;
				}
			}
			if (!finite(augmented[pivot][column]) || std::abs(augmented[pivot][column]) <= tolerance)
			{
				return false;
			}
			if (pivot != column)
			{
				for (int j = column; j < 7; ++j)
				{
					std::swap(augmented[pivot][j], augmented[column][j]);
				}
			}

			const double divisor = augmented[column][column];
			for (int j = column; j < 7; ++j)
			{
				augmented[column][j] /= divisor;
			}
			for (int row = 0; row < 6; ++row)
			{
				if (row == column)
				{
					continue;
				}
				const double factor = augmented[row][column];
				for (int j = column; j < 7; ++j)
				{
					augmented[row][j] -= factor * augmented[column][j];
				}
			}
		}

		for (int row = 0; row < 6; ++row)
		{
			solution[row] = augmented[row][6];
			if (!finite(solution[row]))
			{
				return false;
			}
		}
		return true;
	}

	struct CurvatureSummary
	{
		bool valid = false;
		double mean = quietNaN();
		double gaussian = quietNaN();
	};

	CurvatureSummary computeCurvature(const CCCoreLib::DgmOctree::NeighboursSet& neighbours,
	                                  std::size_t eligibleCount,
	                                  unsigned queryIndex,
	                                  const GeometrySummary& geometry,
	                                  const PcaSummary& pca,
	                                  const double query[3],
	                                  double inverseGlobalScale,
	                                  const CCVector3d& globalShift)
	{
		CurvatureSummary result;
		if (!pca.valid || geometry.count < 6)
		{
			return result;
		}

		const double* normal = pca.eigenvector[2];
		int zAxis = 2;
		if (std::abs(normal[0]) > std::abs(normal[zAxis]))
		{
			zAxis = 0;
		}
		if (std::abs(normal[1]) > std::abs(normal[zAxis]))
		{
			zAxis = 1;
		}
		const int xAxis = (zAxis == 0 ? 1 : (zAxis == 1 ? 2 : 0));
		const int yAxis = (zAxis == 0 ? 2 : (zAxis == 1 ? 0 : 1));

		double normalEquations[6][7] = {};
		std::size_t used = 0;
		for (std::size_t i = 0; i < eligibleCount; ++i)
		{
			const CCCoreLib::DgmOctree::PointDescriptor& descriptor = neighbours[i];
			if (descriptor.pointIndex == queryIndex || !descriptor.point)
			{
				continue;
			}
			const double point[3] = {
				static_cast<double>(descriptor.point->x) * inverseGlobalScale - globalShift.x,
				static_cast<double>(descriptor.point->y) * inverseGlobalScale - globalShift.y,
				static_cast<double>(descriptor.point->z) * inverseGlobalScale - globalShift.z
			};
			if (!finite(point[0]) || !finite(point[1]) || !finite(point[2]))
			{
				continue;
			}

			const double x = point[xAxis] - geometry.mean[xAxis];
			const double y = point[yAxis] - geometry.mean[yAxis];
			const double z = point[zAxis] - geometry.mean[zAxis];
			const double row[6] = { 1.0, x, y, x * x, x * y, y * y };
			for (int r = 0; r < 6; ++r)
			{
				for (int c = 0; c < 6; ++c)
				{
					normalEquations[r][c] += row[r] * row[c];
				}
				normalEquations[r][6] += row[r] * z;
			}
			++used;
		}
		if (used < 6)
		{
			return result;
		}

		double coefficient[6] = {};
		if (!solveSixBySix(normalEquations, coefficient))
		{
			return result;
		}

		const double x = query[xAxis] - geometry.mean[xAxis];
		const double y = query[yAxis] - geometry.mean[yAxis];
		const double fx = coefficient[1] + 2.0 * coefficient[3] * x + coefficient[4] * y;
		const double fy = coefficient[2] + coefficient[4] * x + 2.0 * coefficient[5] * y;
		const double fxx = 2.0 * coefficient[3];
		const double fyy = 2.0 * coefficient[5];
		const double fxy = coefficient[4];
		const double q = 1.0 + fx * fx + fy * fy;
		if (!finite(q) || q <= 0.0)
		{
			return result;
		}

		result.gaussian = std::abs(fxx * fyy - fxy * fxy) / (q * q);
		result.mean = std::abs((1.0 + fx * fx) * fyy - 2.0 * fx * fy * fxy + (1.0 + fy * fy) * fxx)
		              / (2.0 * std::sqrt(q) * q);
		result.valid = finite(result.gaussian) && finite(result.mean);
		return result;
	}

	struct CacheEntry
	{
		std::vector<ScalarType> values;
	};

	struct PendingField
	{
		alis::FeatureKey key;
		std::vector<ScalarType> values;
	};

	struct SourceGroup
	{
		CCCoreLib::ScalarField* scalarField = nullptr;
		std::vector<std::size_t> pendingFieldIndexes;
		bool needQuantiles = false;
	};

	struct ScaleWork
	{
		double radius = 0.0;      // source/global coordinate units
		double localRadius = 0.0; // octree/local coordinate units
		double localRadiusSquared = 0.0;
		std::vector<std::size_t> pendingFieldIndexes;
		std::vector<SourceGroup> sourceGroups;
		bool needGeometry = false;
		bool needPca = false;
		bool needCurvature = false;
		bool needMoment = false;
		bool needZQuantiles = false;
	};

	struct ComputeContext
	{
		std::vector<PendingField>* pendingFields = nullptr;
		std::vector<ScaleWork>* scales = nullptr; // ascending radii
		double inverseGlobalScale = 1.0;
		CCVector3d globalShift { 0.0, 0.0, 0.0 };
		std::atomic<bool> failed { false };
	};

	double safeRatio(double numerator, double denominator)
	{
		return std::abs(denominator) > std::numeric_limits<double>::epsilon()
		           ? numerator / denominator
		           : quietNaN();
	}

	double eigenEntropyTerm(double eigenvalue)
	{
		return eigenvalue > 0.0 ? eigenvalue * std::log(eigenvalue) : 0.0;
	}

	double momentOrder1(const CCCoreLib::DgmOctree::NeighboursSet& neighbours,
	                    std::size_t eligibleCount,
	                    unsigned queryIndex,
	                    const double query[3],
	                    const PcaSummary& pca,
	                    double inverseGlobalScale,
	                    const CCVector3d& globalShift)
	{
		if (!pca.valid)
		{
			return quietNaN();
		}
		double m1 = 0.0;
		double m2 = 0.0;
		for (std::size_t i = 0; i < eligibleCount; ++i)
		{
			const CCCoreLib::DgmOctree::PointDescriptor& descriptor = neighbours[i];
			if (descriptor.pointIndex == queryIndex || !descriptor.point)
			{
				continue;
			}
			const double point[3] = {
				static_cast<double>(descriptor.point->x) * inverseGlobalScale - globalShift.x,
				static_cast<double>(descriptor.point->y) * inverseGlobalScale - globalShift.y,
				static_cast<double>(descriptor.point->z) * inverseGlobalScale - globalShift.z
			};
			const double projection = (point[0] - query[0]) * pca.eigenvector[1][0]
			                          + (point[1] - query[1]) * pca.eigenvector[1][1]
			                          + (point[2] - query[2]) * pca.eigenvector[1][2];
			m1 += projection;
			m2 += projection * projection;
		}
		return m2 > std::numeric_limits<double>::epsilon() ? (m1 * m1) / m2 : quietNaN();
	}

	double geometricFeatureValue(alis::FeatureId feature,
	                             std::size_t neighbourCount,
	                             double radius,
	                             const double query[3],
	                             GeometrySummary& geometry,
	                             const PcaSummary& pca,
	                             const CurvatureSummary& curvature,
	                             double moment)
	{
		using alis::FeatureId;
		const double l1 = pca.eigenvalue[0];
		const double l2 = pca.eigenvalue[1];
		const double l3 = pca.eigenvalue[2];
		const double sum = l1 + l2 + l3;

		switch (feature)
		{
		case FeatureId::NeighborCount:
			return static_cast<double>(neighbourCount);
		case FeatureId::Density2D:
			return radius > 0.0 ? static_cast<double>(neighbourCount) / (Pi * radius * radius) : quietNaN();
		case FeatureId::Density3D:
			return radius > 0.0 ? static_cast<double>(neighbourCount) / (UnitSphereVolume * radius * radius * radius) : quietNaN();
		case FeatureId::Eigenvalue1:
			return pca.valid ? l1 : quietNaN();
		case FeatureId::Eigenvalue2:
			return pca.valid ? l2 : quietNaN();
		case FeatureId::Eigenvalue3:
			return pca.valid ? l3 : quietNaN();
		case FeatureId::EigenvaluesSum:
			return pca.valid ? sum : quietNaN();
		case FeatureId::PCA1:
			return pca.valid ? safeRatio(l1, sum) : quietNaN();
		case FeatureId::PCA2:
			return pca.valid ? safeRatio(l2, sum) : quietNaN();
		case FeatureId::Linearity:
			return pca.valid ? safeRatio(l1 - l2, l1) : quietNaN();
		case FeatureId::Planarity:
			return pca.valid ? safeRatio(l2 - l3, l1) : quietNaN();
		case FeatureId::Sphericity:
			return pca.valid ? safeRatio(l3, l1) : quietNaN();
		case FeatureId::Anisotropy:
			return pca.valid ? safeRatio(l1 - l3, l1) : quietNaN();
		case FeatureId::Omnivariance:
			return pca.valid ? std::cbrt(std::max(0.0, l1 * l2 * l3)) : quietNaN();
		case FeatureId::Eigenentropy:
			return pca.valid ? -(eigenEntropyTerm(l1) + eigenEntropyTerm(l2) + eigenEntropyTerm(l3)) : quietNaN();
		case FeatureId::SurfaceVariation:
		case FeatureId::NormalChangeRate:
			return pca.valid ? safeRatio(l3, sum) : quietNaN();
		case FeatureId::Verticality:
			return pca.valid ? 1.0 - std::abs(pca.eigenvector[2][2]) : quietNaN();
		case FeatureId::NormalX:
			return pca.valid ? pca.eigenvector[2][0] : quietNaN();
		case FeatureId::NormalY:
			return pca.valid ? pca.eigenvector[2][1] : quietNaN();
		case FeatureId::NormalZ:
			return pca.valid ? pca.eigenvector[2][2] : quietNaN();
		case FeatureId::NormalZAbsolute:
			return pca.valid ? std::abs(pca.eigenvector[2][2]) : quietNaN();
		case FeatureId::Dip:
		case FeatureId::DipDirection:
			if (pca.valid)
			{
				double dip = quietNaN();
				double dipDirection = quietNaN();
				ccNormalVectors::ConvertNormalToDipAndDipDir(
					CCVector3d(pca.eigenvector[2][0], pca.eigenvector[2][1], pca.eigenvector[2][2]),
					dip,
					dipDirection);
				return feature == FeatureId::Dip ? dip : dipDirection;
			}
			return quietNaN();
		case FeatureId::Roughness:
		case FeatureId::SignedRoughness:
			if (pca.valid)
			{
				const double signedDistance = (query[0] - geometry.mean[0]) * pca.eigenvector[2][0]
				                              + (query[1] - geometry.mean[1]) * pca.eigenvector[2][1]
				                              + (query[2] - geometry.mean[2]) * pca.eigenvector[2][2];
				return feature == FeatureId::Roughness ? std::abs(signedDistance) : signedDistance;
			}
			return quietNaN();
		case FeatureId::MeanCurvature:
			return curvature.valid ? curvature.mean : quietNaN();
		case FeatureId::GaussianCurvature:
			return curvature.valid ? curvature.gaussian : quietNaN();
		case FeatureId::MomentOrder1:
			return moment;
		case FeatureId::BarycenterOffsetRatio:
			if (geometry.count && radius > 0.0)
			{
				const double dx = query[0] - geometry.mean[0];
				const double dy = query[1] - geometry.mean[1];
				const double dz = query[2] - geometry.mean[2];
				return std::sqrt(dx * dx + dy * dy + dz * dz) / radius;
			}
			return quietNaN();
		case FeatureId::ZMinimum:
			return geometry.count ? geometry.zMinimum : quietNaN();
		case FeatureId::ZMaximum:
			return geometry.count ? geometry.zMaximum : quietNaN();
		case FeatureId::ZRange:
			return geometry.count ? geometry.zMaximum - geometry.zMinimum : quietNaN();
		case FeatureId::ZMean:
			return geometry.count ? geometry.mean[2] : quietNaN();
		case FeatureId::ZStandardDeviation:
			return geometry.count
			           ? std::sqrt(std::max(0.0, geometry.covarianceNumerator[2][2] / static_cast<double>(geometry.count)))
			           : quietNaN();
		case FeatureId::ZAboveMinimum:
			return geometry.count ? query[2] - geometry.zMinimum : quietNaN();
		case FeatureId::ZBelowMaximum:
			return geometry.count ? geometry.zMaximum - query[2] : quietNaN();
		case FeatureId::ZRelativeToMean:
			return geometry.count ? query[2] - geometry.mean[2] : quietNaN();
		case FeatureId::ZPercentile10:
			return quantileOfSorted(geometry.zValues, 0.10);
		case FeatureId::ZPercentile25:
			return quantileOfSorted(geometry.zValues, 0.25);
		case FeatureId::ZMedian:
			return quantileOfSorted(geometry.zValues, 0.50);
		case FeatureId::ZPercentile75:
			return quantileOfSorted(geometry.zValues, 0.75);
		case FeatureId::ZPercentile90:
			return quantileOfSorted(geometry.zValues, 0.90);
		default:
			return quietNaN();
		}
	}

	double sourceFeatureValue(alis::FeatureId feature, const RunningStatistics& statistics)
	{
		using alis::FeatureId;
		switch (feature)
		{
		case FeatureId::SourceValidCount:
			return static_cast<double>(statistics.count);
		case FeatureId::SourceMinimum:
			return statistics.count ? statistics.minimum : quietNaN();
		case FeatureId::SourceMaximum:
			return statistics.count ? statistics.maximum : quietNaN();
		case FeatureId::SourceRange:
			return statistics.count ? statistics.maximum - statistics.minimum : quietNaN();
		case FeatureId::SourceMean:
			return statistics.count ? statistics.mean : quietNaN();
		case FeatureId::SourceStandardDeviation:
			return statistics.standardDeviation();
		case FeatureId::SourcePercentile10:
			return quantileOfSorted(statistics.values, 0.10);
		case FeatureId::SourcePercentile25:
			return quantileOfSorted(statistics.values, 0.25);
		case FeatureId::SourceMedian:
			return quantileOfSorted(statistics.values, 0.50);
		case FeatureId::SourcePercentile75:
			return quantileOfSorted(statistics.values, 0.75);
		case FeatureId::SourcePercentile90:
			return quantileOfSorted(statistics.values, 0.90);
		default:
			return quietNaN();
		}
	}

	bool computeFeaturesInCell(const CCCoreLib::DgmOctree::octreeCell& cell,
	                           void** additionalParameters,
	                           CCCoreLib::NormalizedProgress* progress)
	{
		ComputeContext* context = static_cast<ComputeContext*>(additionalParameters[0]);
		if (!context || !context->pendingFields || !context->scales || context->scales->empty())
		{
			return false;
		}

		try
		{
			CCCoreLib::DgmOctree::NearestNeighboursSearchStruct search;
			search.level = cell.level;
			cell.parentOctree->getCellPos(cell.truncatedCode, cell.level, search.cellPos, true);
			cell.parentOctree->computeCellCenter(search.cellPos, cell.level, search.cellCenter);

			const unsigned cellPointCount = cell.points->size();
			search.pointsInNeighbourhood.resize(cellPointCount);
			for (unsigned i = 0; i < cellPointCount; ++i)
			{
				search.pointsInNeighbourhood[i].point = cell.points->getPointPersistentPtr(i);
				search.pointsInNeighbourhood[i].pointIndex = cell.points->getPointGlobalIndex(i);
			}
			search.alreadyVisitedNeighbourhoodSize = 1;

			const ScaleWork& largestScale = context->scales->back();
			for (unsigned localIndex = 0; localIndex < cellPointCount; ++localIndex)
			{
				if (context->failed.load())
				{
					return false;
				}

				cell.points->getPoint(localIndex, search.queryPoint);
				const unsigned queryIndex = cell.points->getPointGlobalIndex(localIndex);
				const int found = cell.parentOctree->findNeighborsInASphereStartingFromCell(search,
				                                                                        largestScale.localRadius,
				                                                                        true);
				if (found < 0)
				{
					context->failed.store(true);
					return false;
				}

				std::size_t eligibleCount = static_cast<std::size_t>(found);
				const double query[3] = {
					static_cast<double>(search.queryPoint.x) * context->inverseGlobalScale - context->globalShift.x,
					static_cast<double>(search.queryPoint.y) * context->inverseGlobalScale - context->globalShift.y,
					static_cast<double>(search.queryPoint.z) * context->inverseGlobalScale - context->globalShift.z
				};

				for (std::size_t reverseIndex = context->scales->size(); reverseIndex > 0; --reverseIndex)
				{
					ScaleWork& scale = (*context->scales)[reverseIndex - 1];
					while (eligibleCount > 0
					       && search.pointsInNeighbourhood[eligibleCount - 1].squareDistd > scale.localRadiusSquared)
					{
						--eligibleCount;
					}

					std::size_t neighbourCount = 0;
					GeometrySummary geometry;
					if (scale.needZQuantiles)
					{
						geometry.zValues.reserve(eligibleCount);
					}
					for (std::size_t i = 0; i < eligibleCount; ++i)
					{
						const CCCoreLib::DgmOctree::PointDescriptor& descriptor = search.pointsInNeighbourhood[i];
						if (descriptor.pointIndex == queryIndex || !descriptor.point)
						{
							continue;
						}
						++neighbourCount;
						if (scale.needGeometry)
						{
							geometry.add(
								static_cast<double>(descriptor.point->x) * context->inverseGlobalScale - context->globalShift.x,
								static_cast<double>(descriptor.point->y) * context->inverseGlobalScale - context->globalShift.y,
								static_cast<double>(descriptor.point->z) * context->inverseGlobalScale - context->globalShift.z,
								scale.needZQuantiles);
						}
					}
					if (scale.needZQuantiles)
					{
						std::sort(geometry.zValues.begin(), geometry.zValues.end());
					}

					const PcaSummary pca = scale.needPca ? computePca(geometry) : PcaSummary();
					const CurvatureSummary curvature = scale.needCurvature
						? computeCurvature(search.pointsInNeighbourhood,
						                   eligibleCount,
						                   queryIndex,
						                   geometry,
						                   pca,
						                   query,
						                   context->inverseGlobalScale,
						                   context->globalShift)
						: CurvatureSummary();
					const double moment = scale.needMoment
						? momentOrder1(search.pointsInNeighbourhood,
						               eligibleCount,
						               queryIndex,
						               query,
						               pca,
						               context->inverseGlobalScale,
						               context->globalShift)
						: quietNaN();

					for (std::size_t pendingIndex : scale.pendingFieldIndexes)
					{
						PendingField& field = (*context->pendingFields)[pendingIndex];
						if (!isSourceFeature(field.key.feature))
						{
							const double value = geometricFeatureValue(field.key.feature,
							                                                 neighbourCount,
							                                                 scale.radius,
							                                                 query,
							                                                 geometry,
							                                                 pca,
							                                                 curvature,
							                                                 moment);
							field.values[queryIndex] = static_cast<ScalarType>(value);
						}
					}

					for (const SourceGroup& sourceGroup : scale.sourceGroups)
					{
						RunningStatistics sourceStatistics;
						if (sourceGroup.needQuantiles)
						{
							sourceStatistics.values.reserve(eligibleCount);
						}
						for (std::size_t i = 0; i < eligibleCount; ++i)
						{
							const CCCoreLib::DgmOctree::PointDescriptor& descriptor = search.pointsInNeighbourhood[i];
							if (descriptor.pointIndex == queryIndex)
							{
								continue;
							}
							const ScalarType value = sourceGroup.scalarField->getValue(descriptor.pointIndex);
							sourceStatistics.add(static_cast<double>(value), sourceGroup.needQuantiles);
						}
						if (sourceGroup.needQuantiles)
						{
							std::sort(sourceStatistics.values.begin(), sourceStatistics.values.end());
						}
						for (std::size_t pendingIndex : sourceGroup.pendingFieldIndexes)
						{
							PendingField& field = (*context->pendingFields)[pendingIndex];
							field.values[queryIndex] = static_cast<ScalarType>(sourceFeatureValue(field.key.feature, sourceStatistics));
						}
					}
				}

				if (progress && !progress->oneStep())
				{
					return false;
				}
			}
		}
		catch (const std::bad_alloc&)
		{
			context->failed.store(true);
			return false;
		}
		catch (...)
		{
			context->failed.store(true);
			return false;
		}

		return true;
	}

	struct GeometrySignature
	{
		ccPointCloud* cloud = nullptr;
		unsigned uniqueId = 0;
		unsigned pointCount = 0;
		bool boundingBoxValid = false;
		std::array<double, 6> boundingBox {{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }};
		std::array<double, 9> anchors {{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }};
		CCVector3d globalShift;
		double globalScale = 1.0;

		bool operator==(const GeometrySignature& other) const
		{
			return cloud == other.cloud
			       && uniqueId == other.uniqueId
			       && pointCount == other.pointCount
			       && boundingBoxValid == other.boundingBoxValid
			       && boundingBox == other.boundingBox
			       && anchors == other.anchors
			       && globalShift.x == other.globalShift.x
			       && globalShift.y == other.globalShift.y
			       && globalShift.z == other.globalShift.z
			       && globalScale == other.globalScale;
		}
	};

	GeometrySignature makeGeometrySignature(ccPointCloud* cloud)
	{
		GeometrySignature signature;
		signature.cloud = cloud;
		if (!cloud)
		{
			return signature;
		}
		signature.uniqueId = cloud->getUniqueID();
		signature.pointCount = cloud->size();
		signature.globalShift = cloud->getGlobalShift();
		signature.globalScale = cloud->getGlobalScale();

		const ccBBox box = cloud->getOwnBB();
		signature.boundingBoxValid = box.isValid();
		if (box.isValid())
		{
			signature.boundingBox = {{
				box.minCorner().x, box.minCorner().y, box.minCorner().z,
				box.maxCorner().x, box.maxCorner().y, box.maxCorner().z
			}};
		}
		if (cloud->size())
		{
			const unsigned indexes[3] = { 0, cloud->size() / 2, cloud->size() - 1 };
			for (int i = 0; i < 3; ++i)
			{
				const CCVector3* point = cloud->getPoint(indexes[i]);
				signature.anchors[3 * i] = point->x;
				signature.anchors[3 * i + 1] = point->y;
				signature.anchors[3 * i + 2] = point->z;
			}
		}
		return signature;
	}

	std::string sanitizeName(const std::string& input)
	{
		std::string output;
		output.reserve(input.size());
		for (char character : input)
		{
			const unsigned char value = static_cast<unsigned char>(character);
			output.push_back(std::isalnum(value) ? character : '_');
		}
		return output;
	}
}

namespace alis
{
	const char* featureName(FeatureId feature)
	{
		switch (feature)
		{
		case FeatureId::NeighborCount: return "NeighborCount";
		case FeatureId::Density2D: return "Density2D";
		case FeatureId::Density3D: return "Density3D";
		case FeatureId::Eigenvalue1: return "Eigenvalue1";
		case FeatureId::Eigenvalue2: return "Eigenvalue2";
		case FeatureId::Eigenvalue3: return "Eigenvalue3";
		case FeatureId::EigenvaluesSum: return "EigenvaluesSum";
		case FeatureId::PCA1: return "PCA1";
		case FeatureId::PCA2: return "PCA2";
		case FeatureId::Linearity: return "Linearity";
		case FeatureId::Planarity: return "Planarity";
		case FeatureId::Sphericity: return "Sphericity";
		case FeatureId::Anisotropy: return "Anisotropy";
		case FeatureId::Omnivariance: return "Omnivariance";
		case FeatureId::Eigenentropy: return "Eigenentropy";
		case FeatureId::SurfaceVariation: return "SurfaceVariation";
		case FeatureId::Verticality: return "Verticality";
		case FeatureId::NormalX: return "NormalX";
		case FeatureId::NormalY: return "NormalY";
		case FeatureId::NormalZ: return "NormalZ";
		case FeatureId::NormalZAbsolute: return "NormalZAbsolute";
		case FeatureId::Dip: return "Dip";
		case FeatureId::DipDirection: return "DipDirection";
		case FeatureId::Roughness: return "Roughness";
		case FeatureId::SignedRoughness: return "SignedRoughness";
		case FeatureId::MeanCurvature: return "MeanCurvature";
		case FeatureId::GaussianCurvature: return "GaussianCurvature";
		case FeatureId::NormalChangeRate: return "NormalChangeRate";
		case FeatureId::MomentOrder1: return "MomentOrder1";
		case FeatureId::BarycenterOffsetRatio: return "BarycenterOffsetRatio";
		case FeatureId::ZMinimum: return "ZMinimum";
		case FeatureId::ZMaximum: return "ZMaximum";
		case FeatureId::ZRange: return "ZRange";
		case FeatureId::ZMean: return "ZMean";
		case FeatureId::ZStandardDeviation: return "ZStandardDeviation";
		case FeatureId::ZAboveMinimum: return "ZAboveMinimum";
		case FeatureId::ZBelowMaximum: return "ZBelowMaximum";
		case FeatureId::ZRelativeToMean: return "ZRelativeToMean";
		case FeatureId::ZPercentile10: return "ZPercentile10";
		case FeatureId::ZPercentile25: return "ZPercentile25";
		case FeatureId::ZMedian: return "ZMedian";
		case FeatureId::ZPercentile75: return "ZPercentile75";
		case FeatureId::ZPercentile90: return "ZPercentile90";
		case FeatureId::SourceValidCount: return "SourceValidCount";
		case FeatureId::SourceMinimum: return "SourceMinimum";
		case FeatureId::SourceMaximum: return "SourceMaximum";
		case FeatureId::SourceRange: return "SourceRange";
		case FeatureId::SourceMean: return "SourceMean";
		case FeatureId::SourceStandardDeviation: return "SourceStandardDeviation";
		case FeatureId::SourcePercentile10: return "SourcePercentile10";
		case FeatureId::SourcePercentile25: return "SourcePercentile25";
		case FeatureId::SourceMedian: return "SourceMedian";
		case FeatureId::SourcePercentile75: return "SourcePercentile75";
		case FeatureId::SourcePercentile90: return "SourcePercentile90";
		}
		return "UnknownFeature";
	}

	bool FeatureKey::operator<(const FeatureKey& other) const
	{
		if (feature != other.feature)
		{
			return static_cast<int>(feature) < static_cast<int>(other.feature);
		}
		const bool thisRadiusIsNaN = std::isnan(radius) != 0;
		const bool otherRadiusIsNaN = std::isnan(other.radius) != 0;
		if (thisRadiusIsNaN != otherRadiusIsNaN)
		{
			return thisRadiusIsNaN;
		}
		if (!thisRadiusIsNaN && radius != other.radius)
		{
			return radius < other.radius;
		}
		return sourceScalarField < other.sourceScalarField;
	}

	bool FeatureKey::operator==(const FeatureKey& other) const
	{
		return feature == other.feature && radius == other.radius && sourceScalarField == other.sourceScalarField;
	}

	struct FeatureEngine::Impl
	{
		ccPointCloud* cloud = nullptr;
		ccOctree::Shared octree;
		std::map<FeatureKey, CacheEntry> cache;
		std::size_t byteLimit = 0;
		GeometrySignature signature;
		bool signatureInitialized = false;

		void setCloud(ccPointCloud* newCloud)
		{
			cloud = newCloud;
			octree.clear();
			cache.clear();
			signature = makeGeometrySignature(cloud);
			signatureInitialized = true;
		}

		bool synchronizeGeometry()
		{
			const GeometrySignature current = makeGeometrySignature(cloud);
			if (!signatureInitialized || !(current == signature))
			{
				cache.clear();
				octree.clear();
				signature = current;
				signatureInitialized = true;
				return true;
			}
			return false;
		}

		std::size_t bytes() const
		{
			std::size_t result = 0;
			for (const auto& item : cache)
			{
				const std::size_t fieldBytes = item.second.values.capacity() * sizeof(ScalarType);
				if (fieldBytes > std::numeric_limits<std::size_t>::max() - result)
				{
					return std::numeric_limits<std::size_t>::max();
				}
				result += fieldBytes;
			}
			return result;
		}

		bool acquireOctree(bool& built, bool& reused, CCCoreLib::GenericProgressCallback* progress, std::string& error)
		{
			built = false;
			reused = false;
			if (!cloud || cloud->size() == 0)
			{
				error = "A non-empty point cloud is required.";
				return false;
			}

			ccOctree::Shared attached = cloud->getOctree();
			if (attached
			    && attached->associatedCloud() == cloud
			    && attached->getNumberOfProjectedPoints() == cloud->size())
			{
				octree = attached;
				reused = true;
				return true;
			}

			octree.clear();
			octree = cloud->computeOctree(progress);
			if (!octree)
			{
				error = "CloudCompare could not build the cloud octree (canceled or insufficient memory).";
				return false;
			}
			built = true;
			return true;
		}
	};

	FeatureEngine::FeatureEngine(ccPointCloud* cloud)
		: m_impl(new Impl)
	{
		m_impl->setCloud(cloud);
	}

	FeatureEngine::~FeatureEngine() = default;
	FeatureEngine::FeatureEngine(FeatureEngine&&) noexcept = default;
	FeatureEngine& FeatureEngine::operator=(FeatureEngine&&) noexcept = default;

	void FeatureEngine::setCloud(ccPointCloud* cloud)
	{
		m_impl->setCloud(cloud);
	}

	ccPointCloud* FeatureEngine::cloud() const
	{
		return m_impl->cloud;
	}

	void FeatureEngine::setCacheByteLimit(std::size_t bytes)
	{
		m_impl->byteLimit = bytes;
	}

	std::size_t FeatureEngine::cacheByteLimit() const
	{
		return m_impl->byteLimit;
	}

	std::size_t FeatureEngine::cacheBytes() const
	{
		return m_impl->bytes();
	}

	bool FeatureEngine::compute(const std::vector<FeatureRequest>& requests,
	                            ComputeReport& report,
	                            std::string& error,
	                            CCCoreLib::GenericProgressCallback* progress,
	                            const ComputeOptions& options)
	{
		report = ComputeReport();
		error.clear();
		const auto start = std::chrono::steady_clock::now();
		if (!m_impl->cloud)
		{
			error = "No point cloud is associated with the FeatureEngine.";
			return false;
		}
		m_impl->synchronizeGeometry();
		report.pointCount = m_impl->cloud->size();
		if (m_impl->cloud->size() == 0)
		{
			error = "The selected point cloud is empty.";
			return false;
		}
		const double globalScale = m_impl->cloud->getGlobalScale();
		if (!finite(globalScale) || globalScale <= 0.0)
		{
			error = "The point cloud has an invalid global scale.";
			return false;
		}

		std::set<FeatureKey> uniqueRequests;
		for (const FeatureRequest& request : requests)
		{
			FeatureKey key = canonicalKey(request);
			if (!finite(key.radius) || key.radius <= 0.0)
			{
				error = std::string("Invalid radius for feature ") + featureName(key.feature) + ".";
				return false;
			}
			const double localRadius = key.radius * globalScale;
			if (!finite(localRadius) || localRadius <= 0.0
			    || localRadius > static_cast<double>(std::numeric_limits<PointCoordinateType>::max()))
			{
				error = std::string("Radius cannot be represented in CloudCompare local coordinates for feature ")
				        + featureName(key.feature) + ".";
				return false;
			}
			if (isSourceFeature(key.feature))
			{
				if (key.sourceScalarField.empty())
				{
					error = std::string("Feature ") + featureName(key.feature) + " requires a source Scalar Field name.";
					return false;
				}
				if (m_impl->cloud->getScalarFieldIndexByName(key.sourceScalarField.c_str()) < 0)
				{
					error = "Source Scalar Field not found: " + key.sourceScalarField;
					return false;
				}
			}
			uniqueRequests.insert(key);
		}

		report.requestedFields = uniqueRequests.size();
		std::vector<FeatureKey> missingKeys;
		for (const FeatureKey& key : uniqueRequests)
		{
			if (m_impl->cache.find(key) == m_impl->cache.end())
			{
				missingKeys.push_back(key);
			}
			else
			{
				++report.reusedFields;
			}
		}
		if (missingKeys.empty())
		{
			report.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			return true;
		}

		const std::size_t pointCount = m_impl->cloud->size();
		if (pointCount > std::numeric_limits<std::size_t>::max() / sizeof(ScalarType)
		    || missingKeys.size() > std::numeric_limits<std::size_t>::max() / (pointCount * sizeof(ScalarType)))
		{
			error = "Requested feature cache size overflows the platform size type.";
			return false;
		}
		const std::size_t requestedBytes = missingKeys.size() * pointCount * sizeof(ScalarType);
		const std::size_t existingBytes = m_impl->bytes();
		if (m_impl->byteLimit
		    && (existingBytes > m_impl->byteLimit || requestedBytes > m_impl->byteLimit - existingBytes))
		{
			std::ostringstream message;
			message << "Feature cache limit exceeded: " << requestedBytes
			        << " additional bytes requested with " << existingBytes << " bytes already cached.";
			error = message.str();
			return false;
		}

		bool octreeBuilt = false;
		bool octreeReused = false;
		if (!m_impl->acquireOctree(octreeBuilt, octreeReused, progress, error))
		{
			return false;
		}
		report.octreeBuilt = octreeBuilt;
		report.octreeReused = octreeReused;

		std::vector<PendingField> pendingFields;
		std::vector<ScaleWork> scales;
		try
		{
			pendingFields.reserve(missingKeys.size());
			for (const FeatureKey& key : missingKeys)
			{
				PendingField field;
				field.key = key;
				field.values.assign(pointCount, CCCoreLib::NAN_VALUE);
				pendingFields.push_back(std::move(field));
			}

			std::map<double, std::vector<std::size_t>> fieldsPerRadius;
			for (std::size_t i = 0; i < pendingFields.size(); ++i)
			{
				fieldsPerRadius[pendingFields[i].key.radius].push_back(i);
			}
			for (const auto& radiusFields : fieldsPerRadius)
			{
				ScaleWork scale;
				scale.radius = radiusFields.first;
				scale.localRadius = radiusFields.first * globalScale;
				scale.localRadiusSquared = scale.localRadius * scale.localRadius;
				scale.pendingFieldIndexes = radiusFields.second;

				std::map<std::string, std::size_t> sourceGroupIndexes;
				for (std::size_t pendingIndex : scale.pendingFieldIndexes)
				{
					const FeatureKey& key = pendingFields[pendingIndex].key;
					scale.needGeometry = scale.needGeometry || needsGeometry(key.feature);
					scale.needPca = scale.needPca || needsPca(key.feature);
					scale.needCurvature = scale.needCurvature || needsCurvature(key.feature);
					scale.needMoment = scale.needMoment || needsMoment(key.feature);
					scale.needZQuantiles = scale.needZQuantiles || needsZQuantiles(key.feature);
					if (isSourceFeature(key.feature))
					{
						auto found = sourceGroupIndexes.find(key.sourceScalarField);
						if (found == sourceGroupIndexes.end())
						{
							SourceGroup group;
							const int scalarFieldIndex = m_impl->cloud->getScalarFieldIndexByName(key.sourceScalarField.c_str());
							group.scalarField = m_impl->cloud->getScalarField(scalarFieldIndex);
							scale.sourceGroups.push_back(group);
							found = sourceGroupIndexes.emplace(key.sourceScalarField, scale.sourceGroups.size() - 1).first;
						}
						SourceGroup& group = scale.sourceGroups[found->second];
						group.pendingFieldIndexes.push_back(pendingIndex);
						group.needQuantiles = group.needQuantiles || needsSourceQuantiles(key.feature);
					}
				}
				scales.push_back(std::move(scale));
			}
		}
		catch (const std::bad_alloc&)
		{
			error = "Not enough memory to allocate the requested feature cache.";
			return false;
		}

		ComputeContext context;
		context.pendingFields = &pendingFields;
		context.scales = &scales;
		context.inverseGlobalScale = 1.0 / globalScale;
		context.globalShift = m_impl->cloud->getGlobalShift();
		void* parameters[] = { &context };
		const PointCoordinateType maximumLocalRadius = static_cast<PointCoordinateType>(scales.back().localRadius);
		const unsigned char octreeLevel = m_impl->octree->findBestLevelForAGivenNeighbourhoodSizeExtraction(maximumLocalRadius);
		const unsigned processedCells = m_impl->octree->executeFunctionForAllCellsAtLevel(
			octreeLevel,
			&computeFeaturesInCell,
			parameters,
			options.multiThread,
			progress,
			"ALiS multiscale features",
			options.maxThreadCount);
		if (processedCells == 0 || context.failed.load() || (progress && progress->isCancelRequested()))
		{
			error = context.failed.load()
			        ? "Feature computation failed (most likely insufficient memory or a numerical worker failure)."
			        : "Feature computation was canceled or no octree cell could be processed.";
			return false;
		}

		std::vector<FeatureKey> committedKeys;
		try
		{
			committedKeys.reserve(pendingFields.size());
			for (PendingField& pending : pendingFields)
			{
				CacheEntry entry;
				entry.values = std::move(pending.values);
				committedKeys.push_back(pending.key);
				m_impl->cache.emplace(pending.key, std::move(entry));
			}
		}
		catch (const std::bad_alloc&)
		{
			for (const FeatureKey& key : committedKeys)
			{
				m_impl->cache.erase(key);
			}
			error = "Not enough memory to commit the computed feature cache.";
			return false;
		}

		report.computedFields = missingKeys.size();
		report.processedScales = scales.size();
		report.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		return true;
	}

	bool FeatureEngine::estimateSpacing(SpacingSummary& summary,
	                                    std::string& error,
	                                    std::size_t sampleLimit,
	                                    CCCoreLib::GenericProgressCallback* progress)
	{
		summary = SpacingSummary();
		error.clear();
		if (!m_impl->cloud || m_impl->cloud->size() < 2)
		{
			error = "At least two points are required to estimate spacing.";
			return false;
		}
		if (sampleLimit == 0)
		{
			error = "Spacing sample limit must be greater than zero.";
			return false;
		}
		m_impl->synchronizeGeometry();
		const double globalScale = m_impl->cloud->getGlobalScale();
		if (!finite(globalScale) || globalScale <= 0.0)
		{
			error = "The point cloud has an invalid global scale.";
			return false;
		}

		bool built = false;
		bool reused = false;
		if (!m_impl->acquireOctree(built, reused, progress, error))
		{
			return false;
		}

		const ccBBox box = m_impl->cloud->getOwnBB();
		if (box.isValid())
		{
			summary.extentX = static_cast<double>(box.maxCorner().x - box.minCorner().x) / globalScale;
			summary.extentY = static_cast<double>(box.maxCorner().y - box.minCorner().y) / globalScale;
			summary.extentZ = static_cast<double>(box.maxCorner().z - box.minCorner().z) / globalScale;
		}

		const unsigned targetCells = static_cast<unsigned>(std::min<std::size_t>(sampleLimit, std::numeric_limits<unsigned>::max()));
		const unsigned char sampleLevel = m_impl->octree->findBestLevelForAGivenCellNumber(targetCells);
		CCCoreLib::DgmOctree::cellIndexesContainer cellIndexes;
		if (!m_impl->octree->getCellIndexes(sampleLevel, cellIndexes) || cellIndexes.empty())
		{
			error = "Could not enumerate occupied octree cells for deterministic spacing sampling.";
			return false;
		}
		const std::size_t actualSamples = std::min<std::size_t>(sampleLimit, cellIndexes.size());
		summary.requestedSamples = sampleLimit;

		std::vector<double> positiveSpacings;
		try
		{
			positiveSpacings.reserve(actualSamples);
		}
		catch (const std::bad_alloc&)
		{
			error = "Not enough memory for spacing samples.";
			return false;
		}

		if (progress)
		{
			progress->setMethodTitle("ALiS spacing estimate");
			progress->setInfo("Deterministic occupied-cell nearest-neighbour sample");
			progress->start();
		}

		CCCoreLib::ReferenceCloud cellPoints(m_impl->cloud);
		const unsigned char neighbourLevel = m_impl->octree->findBestLevelForAGivenPopulationPerCell(3);
		bool canceled = false;
		for (std::size_t sampleIndex = 0; sampleIndex < actualSamples; ++sampleIndex)
		{
			const std::size_t occupiedCellPosition = actualSamples == 1
				? cellIndexes.size() / 2
				: (sampleIndex * (cellIndexes.size() - 1)) / (actualSamples - 1);
			if (!m_impl->octree->getPointsInCellByCellIndex(&cellPoints,
			                                                     cellIndexes[occupiedCellPosition],
			                                                     sampleLevel,
			                                                     true)
			    || cellPoints.size() == 0)
			{
				++summary.unresolvedSamples;
				continue;
			}

			const unsigned pointIndex = cellPoints.getPointGlobalIndex(cellPoints.size() / 2);
			CCCoreLib::DgmOctree::NearestNeighboursSearchStruct search;
			search.queryPoint = *m_impl->cloud->getPoint(pointIndex);
			search.level = neighbourLevel;
			search.minNumberOfNeighbors = 8;
			m_impl->octree->getTheCellPosWhichIncludesThePoint(&search.queryPoint, search.cellPos, search.level);
			m_impl->octree->computeCellCenter(search.cellPos, search.level, search.cellCenter);

			try
			{
				const unsigned found = m_impl->octree->findNearestNeighborsStartingFromCell(search);
				bool sawDuplicate = false;
				bool resolved = false;
				for (unsigned i = 0; i < found; ++i)
				{
					const CCCoreLib::DgmOctree::PointDescriptor& descriptor = search.pointsInNeighbourhood[i];
					if (descriptor.pointIndex == pointIndex)
					{
						continue;
					}
					if (descriptor.squareDistd == 0.0)
					{
						sawDuplicate = true;
						continue;
					}
					if (descriptor.squareDistd > 0.0 && finite(descriptor.squareDistd))
					{
						positiveSpacings.push_back(std::sqrt(descriptor.squareDistd) / globalScale);
						resolved = true;
						break;
					}
				}
				if (sawDuplicate)
				{
					++summary.duplicateSamples;
				}
				if (!resolved)
				{
					++summary.unresolvedSamples;
				}
			}
			catch (const std::bad_alloc&)
			{
				if (progress)
				{
					progress->stop();
				}
				error = "Not enough memory during nearest-neighbour spacing sampling.";
				return false;
			}

			++summary.sampledPoints;
			if (progress)
			{
				progress->update(static_cast<float>(100.0 * static_cast<double>(sampleIndex + 1) / static_cast<double>(actualSamples)));
				if (progress->isCancelRequested())
				{
					canceled = true;
					break;
				}
			}
		}
		if (progress)
		{
			progress->stop();
		}
		if (canceled)
		{
			error = "Spacing estimation was canceled.";
			return false;
		}
		if (positiveSpacings.empty())
		{
			error = "No positive nearest-neighbour spacing could be resolved (the sample may contain only duplicates).";
			return false;
		}

		std::sort(positiveSpacings.begin(), positiveSpacings.end());
		summary.positiveSpacingCount = positiveSpacings.size();
		for(double value:positiveSpacings) summary.mean += value / positiveSpacings.size();
		summary.minimum = positiveSpacings.front();
		summary.percentile10 = quantileOfSorted(positiveSpacings, 0.10);
		summary.percentile25 = quantileOfSorted(positiveSpacings, 0.25);
		summary.median = quantileOfSorted(positiveSpacings, 0.50);
		summary.percentile75 = quantileOfSorted(positiveSpacings, 0.75);
		summary.percentile90 = quantileOfSorted(positiveSpacings, 0.90);
		summary.maximum = positiveSpacings.back();

		std::vector<double> deviations;
		try
		{
			deviations.reserve(positiveSpacings.size());
			for (double spacing : positiveSpacings)
			{
				deviations.push_back(std::abs(spacing - summary.median));
			}
		}
		catch (const std::bad_alloc&)
		{
			error = "Not enough memory to compute spacing dispersion.";
			return false;
		}
		std::sort(deviations.begin(), deviations.end());
		summary.medianAbsoluteDeviation = quantileOfSorted(deviations, 0.50);
		// For a homogeneous 2D Poisson process: median nearest-neighbour distance^2 = ln(2)/(pi*density).
		summary.estimatedDensity2D = std::log(2.0) / (Pi * summary.median * summary.median);
		summary.valid = finite(summary.estimatedDensity2D) && summary.estimatedDensity2D > 0.0;
		return summary.valid;
	}

	std::vector<double> FeatureEngine::suggestScales(const SpacingSummary& spacing,
	                                                const ScaleSuggestionOptions& options,
	                                                std::string& error)
	{
		error.clear();
		std::vector<double> result;
		if (!options.metricUnitsConfirmed)
		{
			error = "Metric/source units must be confirmed before suggesting physical radii.";
			return result;
		}
		if (!spacing.valid || !finite(spacing.estimatedDensity2D) || spacing.estimatedDensity2D <= 0.0)
		{
			error = "A valid spacing/density summary is required.";
			return result;
		}
		if (options.targetNeighborCounts.empty())
		{
			error = "At least one target neighbour count is required.";
			return result;
		}
		if (!finite(options.minimumRadius) || !finite(options.maximumRadius)
		    || options.minimumRadius < 0.0 || options.maximumRadius < 0.0
		    || (options.maximumRadius > 0.0 && options.minimumRadius > options.maximumRadius))
		{
			error = "Invalid scale suggestion clamps.";
			return result;
		}

		double maximumRadius = options.maximumRadius;
		if (maximumRadius == 0.0)
		{
			const double minimumHorizontalExtent = (spacing.extentX > 0.0 && spacing.extentY > 0.0)
				? std::min(spacing.extentX, spacing.extentY)
				: std::max(spacing.extentX, spacing.extentY);
			if (minimumHorizontalExtent > 0.0)
			{
				maximumRadius = 0.25 * minimumHorizontalExtent;
			}
		}

		for (unsigned targetCount : options.targetNeighborCounts)
		{
			if (targetCount == 0)
			{
				continue;
			}
			double radius = std::sqrt(static_cast<double>(targetCount) / (Pi * spacing.estimatedDensity2D));
			if (options.minimumRadius > 0.0)
			{
				radius = std::max(radius, options.minimumRadius);
			}
			if (maximumRadius > 0.0)
			{
				radius = std::min(radius, maximumRadius);
			}
			if (!finite(radius) || radius <= 0.0)
			{
				continue;
			}
			const bool duplicate = std::any_of(result.begin(), result.end(), [radius](double existing)
			{
				return std::abs(existing - radius) <= 1.0e-9 * std::max(1.0, std::max(existing, radius));
			});
			if (!duplicate)
			{
				result.push_back(radius);
			}
		}
		std::sort(result.begin(), result.end());
		if (result.empty())
		{
			error = "No positive, distinct scale could be suggested with the current constraints.";
		}
		return result;
	}

	bool FeatureEngine::hasCached(const FeatureKey& inputKey)
	{
		m_impl->synchronizeGeometry();
		return m_impl->cache.find(canonicalKey(inputKey)) != m_impl->cache.end();
	}

	const std::vector<ScalarType>* FeatureEngine::cachedValues(const FeatureKey& inputKey)
	{
		m_impl->synchronizeGeometry();
		const auto found = m_impl->cache.find(canonicalKey(inputKey));
		return found == m_impl->cache.end() ? nullptr : &found->second.values;
	}

	std::vector<FeatureKey> FeatureEngine::cachedKeys() const
	{
		std::vector<FeatureKey> result;
		result.reserve(m_impl->cache.size());
		for (const auto& item : m_impl->cache)
		{
			result.push_back(item.first);
		}
		return result;
	}

	bool FeatureEngine::statistics(const FeatureKey& inputKey,
	                               FeatureStatistics& output,
	                               std::string& error,
	                               std::size_t quantileSampleLimit)
	{
		output = FeatureStatistics();
		error.clear();
		m_impl->synchronizeGeometry();
		const auto found = m_impl->cache.find(canonicalKey(inputKey));
		if (found == m_impl->cache.end())
		{
			error = "The requested feature is not cached.";
			return false;
		}
		if (quantileSampleLimit == 0)
		{
			error = "Quantile sample limit must be greater than zero.";
			return false;
		}

		const std::vector<ScalarType>& values = found->second.values;
		output.totalCount = values.size();
		double mean = 0.0;
		double m2 = 0.0;
		double minimum = std::numeric_limits<double>::infinity();
		double maximum = -std::numeric_limits<double>::infinity();
		for (ScalarType scalar : values)
		{
			const double value = static_cast<double>(scalar);
			if (!finite(value))
			{
				++output.invalidCount;
				continue;
			}
			++output.validCount;
			const double delta = value - mean;
			mean += delta / static_cast<double>(output.validCount);
			m2 += delta * (value - mean);
			minimum = std::min(minimum, value);
			maximum = std::max(maximum, value);
		}
		if (output.validCount == 0)
		{
			error = "The cached feature contains no finite values.";
			return false;
		}

		output.minimum = minimum;
		output.maximum = maximum;
		output.mean = mean;
		output.variance = std::max(0.0, m2 / static_cast<double>(output.validCount));
		output.standardDeviation = std::sqrt(output.variance);
		output.quantileSampleCount = static_cast<std::size_t>(std::min<std::uint64_t>(output.validCount, quantileSampleLimit));
		output.quantilesApproximate = output.validCount > output.quantileSampleCount;

		std::vector<double> quantileValues;
		try
		{
			quantileValues.reserve(output.quantileSampleCount);
			std::uint64_t validOrdinal = 0;
			std::size_t targetIndex = 0;
			for (ScalarType scalar : values)
			{
				const double value = static_cast<double>(scalar);
				if (!finite(value))
				{
					continue;
				}
				while (targetIndex < output.quantileSampleCount)
				{
					const std::uint64_t targetOrdinal = output.quantileSampleCount == 1
						? (output.validCount - 1) / 2
						: (static_cast<std::uint64_t>(targetIndex) * (output.validCount - 1))
						  / static_cast<std::uint64_t>(output.quantileSampleCount - 1);
					if (targetOrdinal != validOrdinal)
					{
						break;
					}
					quantileValues.push_back(value);
					++targetIndex;
				}
				++validOrdinal;
			}
		}
		catch (const std::bad_alloc&)
		{
			error = "Not enough memory for feature quantile sampling.";
			return false;
		}
		std::sort(quantileValues.begin(), quantileValues.end());
		output.percentile05 = quantileOfSorted(quantileValues, 0.05);
		output.percentile25 = quantileOfSorted(quantileValues, 0.25);
		output.median = quantileOfSorted(quantileValues, 0.50);
		output.percentile75 = quantileOfSorted(quantileValues, 0.75);
		output.percentile95 = quantileOfSorted(quantileValues, 0.95);
		return true;
	}

	int FeatureEngine::materialize(const FeatureKey& inputKey,
	                               const std::string& requestedName,
	                               MaterializePolicy policy,
	                               std::string& error)
	{
		error.clear();
		m_impl->synchronizeGeometry();
		if (!m_impl->cloud)
		{
			error = "No point cloud is associated with the FeatureEngine.";
			return -1;
		}
		const FeatureKey key = canonicalKey(inputKey);
		const auto found = m_impl->cache.find(key);
		if (found == m_impl->cache.end())
		{
			error = "The requested feature is not cached.";
			return -1;
		}
		if (found->second.values.size() != m_impl->cloud->size())
		{
			error = "Cached feature size no longer matches the point cloud.";
			return -1;
		}

		std::string name = requestedName.empty() ? defaultScalarFieldName(key) : requestedName;
		if (name.empty())
		{
			error = "Scalar Field name cannot be empty.";
			return -1;
		}
		if (name.size() > 255)
		{
			name.resize(255);
		}

		int scalarFieldIndex = m_impl->cloud->getScalarFieldIndexByName(name.c_str());
		ccScalarField* scalarField = nullptr;
		if (scalarFieldIndex >= 0)
		{
			if (policy == MaterializePolicy::FailIfExists)
			{
				error = "A Scalar Field with the requested name already exists.";
				return -1;
			}
			scalarField = static_cast<ccScalarField*>(m_impl->cloud->getScalarField(scalarFieldIndex));
			if (!scalarField->resizeSafe(m_impl->cloud->size()))
			{
				error = "Not enough memory to resize the existing Scalar Field.";
				return -1;
			}
		}
		else
		{
			scalarField = new ccScalarField(name.c_str());
			if (!scalarField->resizeSafe(m_impl->cloud->size()))
			{
				scalarField->release();
				error = "Not enough memory to allocate the Scalar Field.";
				return -1;
			}
			scalarFieldIndex = m_impl->cloud->addScalarField(scalarField);
			if (scalarFieldIndex < 0)
			{
				scalarField->release();
				error = "CloudCompare rejected the new Scalar Field.";
				return -1;
			}
		}

		std::copy(found->second.values.begin(), found->second.values.end(), scalarField->begin());
		scalarField->setModificationFlag(true);
		scalarField->computeMinAndMax();
		// The overwritten/created field may itself be an input of cached Source* features.
		invalidateSourceScalarField(name);
		return scalarFieldIndex;
	}

	std::string FeatureEngine::defaultScalarFieldName(const FeatureKey& inputKey) const
	{
		const FeatureKey key = canonicalKey(inputKey);
		std::ostringstream name;
		name << "qAL_" << featureName(key.feature) << "_R" << std::setprecision(10) << key.radius;
		if (!key.sourceScalarField.empty())
		{
			name << "_" << sanitizeName(key.sourceScalarField);
		}
		std::string result = name.str();
		if (result.size() > 255)
		{
			result.resize(255);
		}
		return result;
	}

	void FeatureEngine::discard(const FeatureKey& inputKey)
	{
		m_impl->cache.erase(canonicalKey(inputKey));
	}

	void FeatureEngine::clearCache()
	{
		m_impl->cache.clear();
	}

	void FeatureEngine::invalidateGeometry()
	{
		m_impl->cache.clear();
		m_impl->octree.clear();
		m_impl->signature = makeGeometrySignature(m_impl->cloud);
		m_impl->signatureInitialized = true;
	}

	void FeatureEngine::invalidateSourceScalarField(const std::string& scalarFieldName)
	{
		for (auto iterator = m_impl->cache.begin(); iterator != m_impl->cache.end(); )
		{
			if (isSourceFeature(iterator->first.feature) && iterator->first.sourceScalarField == scalarFieldName)
			{
				iterator = m_impl->cache.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
	}
}
