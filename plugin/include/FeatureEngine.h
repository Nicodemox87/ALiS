// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <CCTypes.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ccPointCloud;

namespace CCCoreLib
{
	class GenericProgressCallback;
}

namespace alis
{
	//! Features evaluated in a spherical neighbourhood around each source point.
	enum class FeatureId
	{
		NeighborCount,
		Density2D,
		Density3D,

		Eigenvalue1,
		Eigenvalue2,
		Eigenvalue3,
		EigenvaluesSum,
		PCA1,
		PCA2,
		Linearity,
		Planarity,
		Sphericity,
		Anisotropy,
		Omnivariance,
		Eigenentropy,
		SurfaceVariation,
		Verticality,

		NormalX,
		NormalY,
		NormalZ,
		NormalZAbsolute,
		Dip,
		DipDirection,

		Roughness,
		SignedRoughness,
		MeanCurvature,
		GaussianCurvature,
		NormalChangeRate,
		MomentOrder1,
		//! q3DMASC ANISO: distance(query, neighbourhood centroid) / radius.
		BarycenterOffsetRatio,

		ZMinimum,
		ZMaximum,
		ZRange,
		ZMean,
		ZStandardDeviation,
		ZAboveMinimum,
		ZBelowMaximum,
		ZRelativeToMean,
		ZPercentile10,
		ZPercentile25,
		ZMedian,
		ZPercentile75,
		ZPercentile90,

		//! Statistics of FeatureRequest::sourceScalarField over the neighbourhood.
		SourceValidCount,
		SourceMinimum,
		SourceMaximum,
		SourceRange,
		SourceMean,
		SourceStandardDeviation,
		SourcePercentile10,
		SourcePercentile25,
		SourceMedian,
		SourcePercentile75,
		SourcePercentile90
	};

	const char* featureName(FeatureId feature);

	//! Exact cache identity. Radius is in source/global coordinate units (never a diameter).
	/** The engine converts it to CloudCompare local coordinates with getGlobalScale().
	 *  When the source CRS is metric, the radius and dimensional outputs are therefore metres.
	 */
	struct FeatureKey
	{
		FeatureId feature = FeatureId::NeighborCount;
		double radius = 0.0;
		std::string sourceScalarField;

		bool operator<(const FeatureKey& other) const;
		bool operator==(const FeatureKey& other) const;
	};

	struct FeatureRequest
	{
		FeatureId feature = FeatureId::NeighborCount;
		double radius = 0.0;
		//! Required only by Source* features; ignored and canonicalized to empty otherwise.
		std::string sourceScalarField;
	};

	struct ComputeOptions
	{
		//! Uses CCCoreLib cell-level parallelism (QtConcurrent/TBB, as configured by CloudCompare).
		bool multiThread = true;
		//! 0 lets CCCoreLib use its configured maximum.
		int maxThreadCount = 0;
	};

	struct ComputeReport
	{
		std::uint64_t pointCount = 0;
		std::size_t requestedFields = 0;
		std::size_t computedFields = 0;
		std::size_t reusedFields = 0;
		std::size_t processedScales = 0;
		bool octreeBuilt = false;
		bool octreeReused = false;
		double elapsedSeconds = 0.0;
	};

	struct FeatureStatistics
	{
		std::uint64_t totalCount = 0;
		std::uint64_t validCount = 0;
		std::uint64_t invalidCount = 0;
		double minimum = 0.0;
		double maximum = 0.0;
		double mean = 0.0;
		double variance = 0.0; //!< Population variance.
		double standardDeviation = 0.0;
		double percentile05 = 0.0;
		double percentile25 = 0.0;
		double median = 0.0;
		double percentile75 = 0.0;
		double percentile95 = 0.0;
		std::size_t quantileSampleCount = 0;
		bool quantilesApproximate = false;
	};

	struct SpacingSummary
	{
		bool valid = false;
		std::uint64_t requestedSamples = 0;
		std::uint64_t sampledPoints = 0;
		std::uint64_t positiveSpacingCount = 0;
		std::uint64_t duplicateSamples = 0;
		std::uint64_t unresolvedSamples = 0;
		double mean = 0.0; //!< Mean positive nearest-neighbour distance in the bounded sample.
		double minimum = 0.0;
		double percentile10 = 0.0;
		double percentile25 = 0.0;
		double median = 0.0;
		double percentile75 = 0.0;
		double percentile90 = 0.0;
		double maximum = 0.0;
		double medianAbsoluteDeviation = 0.0;
		double estimatedDensity2D = 0.0;
		double extentX = 0.0;
		double extentY = 0.0;
		double extentZ = 0.0;
	};

	struct ScaleSuggestionOptions
	{
		//! The caller must confirm metric units before presenting the results as metres.
		bool metricUnitsConfirmed = false;
		//! Expected neighbours excluding the query point under the documented 2D Poisson heuristic.
		std::vector<unsigned> targetNeighborCounts = { 8, 16, 32, 64, 128 };
		double minimumRadius = 0.0; //!< 0 means no explicit lower clamp.
		double maximumRadius = 0.0; //!< 0 derives a conservative clamp from the XY bounding box.
	};

	enum class MaterializePolicy
	{
		FailIfExists,
		OverwriteExisting
	};

	//! Lazy multiscale feature engine bound to one CloudCompare point cloud.
	/**
	 * Cached values never duplicate XYZ. A compute request performs one maximum-radius
	 * octree query per point, reuses its sorted neighbours at every smaller requested
	 * radius, and performs at most one covariance eigendecomposition per point/radius.
	 * The query point itself is excluded consistently from counts, PCA and statistics;
	 * coincident points having a different point index remain valid neighbours.
	 * Cache lifetime is explicitly controlled by the owning ALiS session.
	 */
	class FeatureEngine
	{
	public:
		explicit FeatureEngine(ccPointCloud* cloud = nullptr);
		~FeatureEngine();

		FeatureEngine(FeatureEngine&&) noexcept;
		FeatureEngine& operator=(FeatureEngine&&) noexcept;

		FeatureEngine(const FeatureEngine&) = delete;
		FeatureEngine& operator=(const FeatureEngine&) = delete;

		void setCloud(ccPointCloud* cloud);
		ccPointCloud* cloud() const;

		//! 0 means unlimited. The limit covers cached and newly requested ScalarType arrays.
		void setCacheByteLimit(std::size_t bytes);
		std::size_t cacheByteLimit() const;
		std::size_t cacheBytes() const;

		bool compute(const std::vector<FeatureRequest>& requests,
		             ComputeReport& report,
		             std::string& error,
		             CCCoreLib::GenericProgressCallback* progress = nullptr,
		             const ComputeOptions& options = ComputeOptions());

		bool estimateSpacing(SpacingSummary& summary,
		                     std::string& error,
		                     std::size_t sampleLimit = 4096,
		                     CCCoreLib::GenericProgressCallback* progress = nullptr);

		static std::vector<double> suggestScales(const SpacingSummary& spacing,
		                                         const ScaleSuggestionOptions& options,
		                                         std::string& error);

		bool hasCached(const FeatureKey& key);
		const std::vector<ScalarType>* cachedValues(const FeatureKey& key);
		std::vector<FeatureKey> cachedKeys() const;

		//! Full-field Welford statistics plus deterministic bounded-memory quantiles.
		bool statistics(const FeatureKey& key,
		                FeatureStatistics& output,
		                std::string& error,
		                std::size_t quantileSampleLimit = 100000);

		//! Materialization is deliberately separate from computation/cache population.
		int materialize(const FeatureKey& key,
		                const std::string& requestedName,
		                MaterializePolicy policy,
		                std::string& error);

		std::string defaultScalarFieldName(const FeatureKey& key) const;

		void discard(const FeatureKey& key);
		void clearCache();

		//! Call after XYZ/order changes. CloudCompare 2.13 has no public geometry revision counter.
		void invalidateGeometry();
		//! Invalidates only cached Source* features that depend on this Scalar Field.
		void invalidateSourceScalarField(const std::string& scalarFieldName);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
