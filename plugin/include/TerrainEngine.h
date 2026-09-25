// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class ccMainAppInterface;
class ccPointCloud;
class ccProgressDialog;

namespace alis
{
	//! Parameters for a 2.5D DTM generated in the source cloud's local coordinates.
	/** A UI accepting metres must convert gridStep and maximumInterpolationEdgeLength
	 *  to CloudCompare local coordinate units before calling TerrainEngine.
	 */
	struct TerrainParameters
	{
		double gridStep = 1.0;
		bool interpolateEmptyCells = false;
		//! Maximum Delaunay edge length in local coordinate units (0 means unlimited).
		double maximumInterpolationEdgeLength = 0.0;
		//! Safety ceiling matching CloudCompare's own "huge grid" threshold by default.
		std::uint64_t maximumCellCount = (std::uint64_t{1} << 26);
	};

	struct TerrainContext
	{
		ccMainAppInterface* app = nullptr;
		//! Optional non-owning progress dialog; cancellation is propagated as failure.
		ccProgressDialog* progressDialog = nullptr;
	};

	struct TerrainValidation
	{
		bool valid = false;
		QString message;
		std::uint64_t groundPointCount = 0;
		std::uint64_t estimatedGridWidth = 0;
		std::uint64_t estimatedGridHeight = 0;
		std::uint64_t estimatedCellCount = 0;
	};

	enum class TerrainStatus
	{
		Success,
		InvalidInput,
		OutOfMemory,
		RasterizationFailedOrCancelled,
		DtmCloudCreationFailedOrCancelled,
		InvalidOutput
	};

	enum class TerrainCellState : std::uint8_t
	{
		NoData = 0,
		ObservedGround = 1,
		Interpolated = 2
	};

	//! Pointer-free row-major DTM grid suitable for session persistence.
	/** Heights and grid coordinates are kept in CloudCompare local coordinates so
	 *  that the grid can be reconstructed without losing precision. Global Shift
	 *  and Scale are recorded explicitly. Cell (column, row) has centre:
	 *  (minimumCenterX + column * step, minimumCenterY + row * step).
	 */
	struct TerrainGrid
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		double step = 0.0;
		double minimumCenterX = 0.0;
		double minimumCenterY = 0.0;
		std::vector<double> heightsLocal;
		std::vector<TerrainCellState> cellStates;

		std::uint64_t sourceEntityUid = 0;
		std::uint64_t sourcePointCount = 0;
		double sourceGlobalShiftX = 0.0;
		double sourceGlobalShiftY = 0.0;
		double sourceGlobalShiftZ = 0.0;
		double sourceGlobalScale = 1.0;

		bool isValid() const;

		//! Strict bilinear lookup in local coordinates.
		/** Returns NaN outside the grid or when any of the four supporting cells is
		 *  NODATA. Values are never clamped and the method never extrapolates.
		 */
		double bilinearHeightLocal(double xLocal, double yLocal) const;
	};

	struct TerrainValueStatistics
	{
		double minimum = 0.0;
		double maximum = 0.0;
		double mean = 0.0;
		double standardDeviation = 0.0;
		std::uint64_t validCount = 0;
		std::uint64_t nodataCount = 0;
	};

	struct TerrainStatistics
	{
		std::uint64_t sourcePointCount = 0;
		std::uint64_t groundPointCount = 0;
		std::uint64_t totalCellCount = 0;
		std::uint64_t observedCellCount = 0;
		std::uint64_t interpolatedCellCount = 0;
		std::uint64_t nodataCellCount = 0;
		//! DTM elevations expressed in the source/original coordinate units.
		TerrainValueStatistics dtmElevation;
		//! HAG values expressed in the source/original coordinate units.
		TerrainValueStatistics heightAboveGround;
		std::uint64_t negativeHeightAboveGroundCount = 0;
	};

	struct TerrainProvenance
	{
		QString algorithmId;
		QString implementation;
		std::uint64_t sourceEntityUid = 0;
		std::uint64_t sourcePointCount = 0;
		double sourceGlobalScale = 1.0;
		std::int64_t elapsedMilliseconds = 0;
		QJsonObject effectiveParameters;
	};

	//! Result whose DTM entity remains outside the CloudCompare DB until committed.
	struct TerrainResult
	{
		TerrainResult();
		~TerrainResult();
		TerrainResult(TerrainResult&&) noexcept;
		TerrainResult& operator=(TerrainResult&&) noexcept;
		TerrainResult(const TerrainResult&) = delete;
		TerrainResult& operator=(const TerrainResult&) = delete;

		TerrainStatus status = TerrainStatus::InvalidInput;
		QString message;
		TerrainGrid grid;
		//! Ownership transfers to the caller, which may commit it to the DB Tree.
		std::unique_ptr<ccPointCloud> dtmCloud;
		//! One value per source point; NODATA is represented by NaN.
		std::vector<double> heightAboveGround;
		TerrainStatistics statistics;
		TerrainProvenance provenance;

		bool succeeded() const
		{
			return status == TerrainStatus::Success;
		}
	};

	//! CloudCompare-native DTM/HAG engine.
	/** The ground mask and HAG output preserve source point-index alignment. A
	 *  temporary point-only ground cloud is used solely because ccRasterGrid's
	 *  public fillWith API accepts ccGenericPointCloud; no Ground/Non-ground DB
	 *  entities are created.
	 */
	class TerrainEngine final
	{
	public:
		QString algorithmId() const;
		TerrainValidation validate(const ccPointCloud& source,
		                           const std::vector<bool>& isGround,
		                           const TerrainParameters& parameters) const;
		TerrainResult run(const ccPointCloud& source,
		                  const std::vector<bool>& isGround,
		                  const TerrainParameters& parameters,
		                  const TerrainContext& context = TerrainContext()) const;
	};
}
