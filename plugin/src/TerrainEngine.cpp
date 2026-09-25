// SPDX-License-Identifier: GPL-2.0-or-later

#include "TerrainEngine.h"

// CloudCompare
#include <ccBBox.h>
#include <ccMainAppInterface.h>
#include <ccPointCloud.h>
#include <ccProgressDialog.h>
#include <ccRasterGrid.h>
#include <ccScalarField.h>

// Qt
#include <QElapsedTimer>
#include <QVariant>

// Standard library
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace
{
	constexpr const char* TERRAIN_IMPLEMENTATION =
		"CloudCompare ccRasterGrid v2.13.2@49dbbb662f296c7780aae717897c85b3cb3764ed";
	constexpr const char* DTM_CLOUD_NAME = "qAL DTM";
	constexpr const char* DTM_ELEVATION_FIELD_NAME = "qAL DTM elevation";

	double nanValue()
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	bool finitePositive(double value)
	{
		return std::isfinite(value) && value > std::numeric_limits<double>::epsilon();
	}

	ccBBox localBoundingBox(const ccPointCloud& source)
	{
		// CloudCompare v2.13.2 exposes getOwnBB as non-const because it may lazily
		// refresh its cache. The operation does not alter point coordinates or fields.
		return const_cast<ccPointCloud&>(source).getOwnBB();
	}

	void logMessage(ccMainAppInterface* app,
	                const QString& message,
	                ccMainAppInterface::ConsoleMessageLevel level)
	{
		if (app)
		{
			app->dispToConsole(QStringLiteral("[ALiS][Terrain] %1").arg(message), level);
		}
	}

	QJsonObject parametersToJson(const alis::TerrainParameters& parameters)
	{
		QJsonObject json;
		json.insert(QStringLiteral("coordinateUnits"), QStringLiteral("CloudCompare local units"));
		json.insert(QStringLiteral("gridStep"), parameters.gridStep);
		json.insert(QStringLiteral("projection"), QStringLiteral("minimum Z of Ground points"));
		json.insert(QStringLiteral("interpolateEmptyCells"), parameters.interpolateEmptyCells);
		json.insert(QStringLiteral("interpolation"),
		            parameters.interpolateEmptyCells ? QStringLiteral("Delaunay") : QStringLiteral("none"));
		json.insert(QStringLiteral("maximumInterpolationEdgeLength"),
		            parameters.maximumInterpolationEdgeLength);
		json.insert(QStringLiteral("maximumCellCount"),
		            static_cast<double>(parameters.maximumCellCount));
		json.insert(QStringLiteral("hagSampling"),
		            QStringLiteral("strict bilinear; NODATA propagation; no extrapolation"));
		return json;
	}

	class StreamingStatistics
	{
	public:
		void add(double value)
		{
			if (!std::isfinite(value))
			{
				++m_result.nodataCount;
				return;
			}

			++m_result.validCount;
			if (m_result.validCount == 1)
			{
				m_result.minimum = value;
				m_result.maximum = value;
			}
			else
			{
				m_result.minimum = std::min(m_result.minimum, value);
				m_result.maximum = std::max(m_result.maximum, value);
			}

			const double delta = value - m_mean;
			m_mean += delta / static_cast<double>(m_result.validCount);
			m_m2 += delta * (value - m_mean);
		}

		alis::TerrainValueStatistics finish()
		{
			if (m_result.validCount == 0)
			{
				m_result.minimum = nanValue();
				m_result.maximum = nanValue();
				m_result.mean = nanValue();
				m_result.standardDeviation = nanValue();
				return m_result;
			}

			m_result.mean = m_mean;
			m_result.standardDeviation =
				std::sqrt(std::max(0.0, m_m2 / static_cast<double>(m_result.validCount)));
			return m_result;
		}

	private:
		alis::TerrainValueStatistics m_result;
		double m_mean = 0.0;
		double m_m2 = 0.0;
	};

	std::unique_ptr<ccPointCloud> makeTemporaryGroundCloud(const ccPointCloud& source,
	                                                       const std::vector<bool>& isGround,
	                                                       std::uint64_t groundPointCount)
	{
		std::unique_ptr<ccPointCloud> ground(new ccPointCloud(QStringLiteral("qAL Ground working subset")));
		if (groundPointCount > static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max())
		    || !ground->reserveThePointsTable(static_cast<unsigned>(groundPointCount)))
		{
			return nullptr;
		}

		for (unsigned index = 0; index < source.size(); ++index)
		{
			if (isGround[index])
			{
				ground->addPoint(*source.getPoint(index));
			}
		}

		ground->copyGlobalShiftAndScale(source);
		ground->setDisplay(source.getDisplay());
		return ground;
	}

	bool flattenGrid(const ccRasterGrid& source,
	                 const ccPointCloud& sourceCloud,
	                 alis::TerrainGrid& destination,
	                 alis::TerrainStatistics& statistics)
	{
		const std::uint64_t cellCount =
			static_cast<std::uint64_t>(source.width) * static_cast<std::uint64_t>(source.height);
		if (cellCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		{
			return false;
		}

		destination.width = source.width;
		destination.height = source.height;
		destination.step = source.gridStep;
		destination.minimumCenterX = source.minCorner.x;
		destination.minimumCenterY = source.minCorner.y;
		destination.sourceEntityUid = static_cast<std::uint64_t>(sourceCloud.getUniqueID());
		destination.sourcePointCount = static_cast<std::uint64_t>(sourceCloud.size());
		destination.sourceGlobalShiftX = sourceCloud.getGlobalShift().x;
		destination.sourceGlobalShiftY = sourceCloud.getGlobalShift().y;
		destination.sourceGlobalShiftZ = sourceCloud.getGlobalShift().z;
		destination.sourceGlobalScale = sourceCloud.getGlobalScale();

		try
		{
			destination.heightsLocal.assign(static_cast<std::size_t>(cellCount), nanValue());
			destination.cellStates.assign(static_cast<std::size_t>(cellCount),
			                              alis::TerrainCellState::NoData);
		}
		catch (const std::bad_alloc&)
		{
			return false;
		}

		StreamingStatistics dtmStatistics;
		std::size_t flatIndex = 0;
		for (unsigned row = 0; row < source.height; ++row)
		{
			const ccRasterGrid::Row& sourceRow = source.rows[row];
			for (unsigned column = 0; column < source.width; ++column, ++flatIndex)
			{
				const ccRasterCell& cell = sourceRow[column];
				if (!std::isfinite(cell.h))
				{
					++statistics.nodataCellCount;
					continue;
				}

				destination.heightsLocal[flatIndex] = cell.h;
				if (cell.nbPoints != 0)
				{
					destination.cellStates[flatIndex] = alis::TerrainCellState::ObservedGround;
					++statistics.observedCellCount;
				}
				else
				{
					destination.cellStates[flatIndex] = alis::TerrainCellState::Interpolated;
					++statistics.interpolatedCellCount;
				}

				const double globalElevation =
					cell.h / destination.sourceGlobalScale - destination.sourceGlobalShiftZ;
				dtmStatistics.add(globalElevation);
			}
		}

		statistics.totalCellCount = cellCount;
		statistics.dtmElevation = dtmStatistics.finish();
		// Grid NODATA belongs to grid statistics, not to the valid-value stream above.
		statistics.dtmElevation.nodataCount = statistics.nodataCellCount;
		return destination.isValid();
	}

	std::unique_ptr<ccPointCloud> makeDtmCloud(const ccRasterGrid& grid,
	                                           ccPointCloud& temporaryGroundCloud,
	                                           const ccPointCloud& source,
	                                           const ccBBox& box,
	                                           ccProgressDialog* progressDialog)
	{
		const std::vector<ccRasterGrid::ExportableFields> noExportedStatistics;
		std::unique_ptr<ccPointCloud> cloud(
			grid.convertToCloud(false,
			                    false,
			                    noExportedStatistics,
			                    false,
			                    false,
			                    false,
			                    false,
			                    &temporaryGroundCloud,
			                    2,
			                    box,
			                    0.0,
			                    true,
			                    false,
			                    progressDialog));
		if (!cloud)
		{
			return nullptr;
		}

		cloud->setName(QString::fromLatin1(DTM_CLOUD_NAME));
		cloud->copyGlobalShiftAndScale(source);
		cloud->setDisplay(source.getDisplay());
		cloud->setEnabled(true);
		cloud->setVisible(true);

		const int elevationIndex = cloud->addScalarField(DTM_ELEVATION_FIELD_NAME);
		if (elevationIndex < 0)
		{
			return nullptr;
		}
		ccScalarField* elevation = static_cast<ccScalarField*>(cloud->getScalarField(elevationIndex));
		if (!elevation)
		{
			return nullptr;
		}

		const double scale = source.getGlobalScale();
		const double shiftZ = source.getGlobalShift().z;
		for (unsigned index = 0; index < cloud->size(); ++index)
		{
			const double globalElevation =
				static_cast<double>(cloud->getPoint(index)->z) / scale - shiftZ;
			elevation->setValue(index, static_cast<ScalarType>(globalElevation));
		}
		elevation->computeMinAndMax();
		cloud->setCurrentDisplayedScalarField(elevationIndex);
		cloud->showSF(true);
		cloud->showColors(false);

		cloud->setMetaData(QStringLiteral("ALiS.role"), QStringLiteral("DTM"));
		cloud->setMetaData(QStringLiteral("ALiS.sourceEntityUid"),
		                   QVariant::fromValue<qulonglong>(source.getUniqueID()));
		cloud->setMetaData(QStringLiteral("ALiS.gridStepLocal"), grid.gridStep);
		cloud->setMetaData(QStringLiteral("ALiS.projection"), QStringLiteral("minimum-ground-Z"));
		return cloud;
	}
}

namespace alis
{
	TerrainResult::TerrainResult() = default;
	TerrainResult::~TerrainResult() = default;
	TerrainResult::TerrainResult(TerrainResult&&) noexcept = default;
	TerrainResult& TerrainResult::operator=(TerrainResult&&) noexcept = default;

	bool TerrainGrid::isValid() const
	{
		if (width == 0 || height == 0 || !finitePositive(step) || !finitePositive(sourceGlobalScale))
		{
			return false;
		}

		const std::uint64_t expected =
			static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
		return expected <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
		       && heightsLocal.size() == static_cast<std::size_t>(expected)
		       && cellStates.size() == static_cast<std::size_t>(expected)
		       && std::isfinite(minimumCenterX)
		       && std::isfinite(minimumCenterY);
	}

	double TerrainGrid::bilinearHeightLocal(double xLocal, double yLocal) const
	{
		if (!isValid() || width < 2 || height < 2
		    || !std::isfinite(xLocal) || !std::isfinite(yLocal))
		{
			return nanValue();
		}

		const double gridX = (xLocal - minimumCenterX) / step;
		const double gridY = (yLocal - minimumCenterY) / step;
		const double maximumX = static_cast<double>(width - 1);
		const double maximumY = static_cast<double>(height - 1);
		if (gridX < 0.0 || gridY < 0.0 || gridX > maximumX || gridY > maximumY)
		{
			return nanValue();
		}

		// At the top/right grid centre, use the preceding cell with weight 1. This
		// remains interpolation within the grid and never clamps an outside value.
		const std::size_t x0 = std::min<std::size_t>(
			static_cast<std::size_t>(std::floor(gridX)), static_cast<std::size_t>(width - 2));
		const std::size_t y0 = std::min<std::size_t>(
			static_cast<std::size_t>(std::floor(gridY)), static_cast<std::size_t>(height - 2));
		const std::size_t x1 = x0 + 1;
		const std::size_t y1 = y0 + 1;

		const double q00 = heightsLocal[y0 * width + x0];
		const double q10 = heightsLocal[y0 * width + x1];
		const double q01 = heightsLocal[y1 * width + x0];
		const double q11 = heightsLocal[y1 * width + x1];
		if (!std::isfinite(q00) || !std::isfinite(q10)
		    || !std::isfinite(q01) || !std::isfinite(q11))
		{
			return nanValue();
		}

		const double fractionX = gridX - static_cast<double>(x0);
		const double fractionY = gridY - static_cast<double>(y0);
		const double lower = (1.0 - fractionX) * q00 + fractionX * q10;
		const double upper = (1.0 - fractionX) * q01 + fractionX * q11;
		return (1.0 - fractionY) * lower + fractionY * upper;
	}

	QString TerrainEngine::algorithmId() const
	{
		return QStringLiteral("terrain.ccrastergrid.minimum.v2.13.2");
	}

	TerrainValidation TerrainEngine::validate(const ccPointCloud& source,
	                                           const std::vector<bool>& isGround,
	                                           const TerrainParameters& parameters) const
	{
		TerrainValidation validation;
		if (source.size() == 0)
		{
			validation.message = QStringLiteral("The source cloud is empty.");
			return validation;
		}
		if (isGround.size() != static_cast<std::size_t>(source.size()))
		{
			validation.message = QStringLiteral("The Ground mask is not aligned with the source cloud.");
			return validation;
		}
		if (!finitePositive(parameters.gridStep))
		{
			validation.message = QStringLiteral("DTM grid step must be finite and greater than zero.");
			return validation;
		}
		if (!std::isfinite(parameters.maximumInterpolationEdgeLength)
		    || parameters.maximumInterpolationEdgeLength < 0.0)
		{
			validation.message = QStringLiteral("Maximum Delaunay edge length must be finite and non-negative.");
			return validation;
		}
		if (parameters.maximumCellCount == 0)
		{
			validation.message = QStringLiteral("Maximum cell count must be greater than zero.");
			return validation;
		}
		if (!finitePositive(source.getGlobalScale()))
		{
			validation.message = QStringLiteral("The source cloud has an invalid Global Scale.");
			return validation;
		}

		const ccBBox box = localBoundingBox(source);
		if (!box.isValid())
		{
			validation.message = QStringLiteral("The source cloud bounding box is invalid.");
			return validation;
		}

		for (unsigned index = 0; index < source.size(); ++index)
		{
			if (!isGround[index])
			{
				continue;
			}

			const CCVector3* point = source.getPoint(index);
			if (!point
			    || !std::isfinite(static_cast<double>(point->x))
			    || !std::isfinite(static_cast<double>(point->y))
			    || !std::isfinite(static_cast<double>(point->z)))
			{
				validation.message = QStringLiteral("Ground point %1 has non-finite coordinates.").arg(index);
				return validation;
			}
			++validation.groundPointCount;
		}
		if (validation.groundPointCount == 0)
		{
			validation.message = QStringLiteral("The Ground mask contains no Ground points.");
			return validation;
		}

		unsigned width = 0;
		unsigned height = 0;
		if (!ccRasterGrid::ComputeGridSize(2, box, parameters.gridStep, width, height))
		{
			validation.message = QStringLiteral("CloudCompare could not compute a valid XY raster grid.");
			return validation;
		}
		validation.estimatedGridWidth = width;
		validation.estimatedGridHeight = height;
		if (width != 0
		    && static_cast<std::uint64_t>(height) >
		           std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(width))
		{
			validation.message = QStringLiteral("The requested DTM grid dimensions overflow their storage type.");
			return validation;
		}
		validation.estimatedCellCount =
			static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
		if (validation.estimatedCellCount > parameters.maximumCellCount)
		{
			validation.message =
				QStringLiteral("The requested DTM has %1 cells, above the configured safety limit of %2.")
					.arg(validation.estimatedCellCount)
					.arg(parameters.maximumCellCount);
			return validation;
		}
		if (validation.estimatedCellCount >
		    static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max()))
		{
			validation.message = QStringLiteral("The requested DTM exceeds ccRasterGrid's addressable cell count.");
			return validation;
		}

		validation.valid = true;
		validation.message = QStringLiteral("Terrain parameters and Ground mask are valid.");
		return validation;
	}

	TerrainResult TerrainEngine::run(const ccPointCloud& source,
	                                 const std::vector<bool>& isGround,
	                                 const TerrainParameters& parameters,
	                                 const TerrainContext& context) const
	{
		TerrainResult result;
		result.provenance.algorithmId = algorithmId();
		result.provenance.implementation = QString::fromLatin1(TERRAIN_IMPLEMENTATION);
		result.provenance.sourceEntityUid = static_cast<std::uint64_t>(source.getUniqueID());
		result.provenance.sourcePointCount = static_cast<std::uint64_t>(source.size());
		result.provenance.sourceGlobalScale = source.getGlobalScale();
		result.provenance.effectiveParameters = parametersToJson(parameters);

		const TerrainValidation validation = validate(source, isGround, parameters);
		result.provenance.effectiveParameters.insert(
			QStringLiteral("groundPointCount"), static_cast<double>(validation.groundPointCount));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("gridWidth"), static_cast<double>(validation.estimatedGridWidth));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("gridHeight"), static_cast<double>(validation.estimatedGridHeight));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("cellCount"), static_cast<double>(validation.estimatedCellCount));
		if (!validation.valid)
		{
			result.status = TerrainStatus::InvalidInput;
			result.message = validation.message;
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		QElapsedTimer timer;
		timer.start();
		result.statistics.sourcePointCount = static_cast<std::uint64_t>(source.size());
		result.statistics.groundPointCount = validation.groundPointCount;
		logMessage(
			context.app,
			QStringLiteral("Building %1 x %2 minimum-ground DTM from %3 Ground points (step %4 local units).")
				.arg(validation.estimatedGridWidth)
				.arg(validation.estimatedGridHeight)
				.arg(validation.groundPointCount)
				.arg(parameters.gridStep, 0, 'g', 12),
			ccMainAppInterface::STD_CONSOLE_MESSAGE);

		std::unique_ptr<ccPointCloud> temporaryGround;
		try
		{
			temporaryGround = makeTemporaryGroundCloud(source, isGround, validation.groundPointCount);
		}
		catch (const std::bad_alloc&)
		{
			// Handled uniformly below.
		}
		if (!temporaryGround)
		{
			result.status = TerrainStatus::OutOfMemory;
			result.message = QStringLiteral("Not enough memory for the temporary point-only Ground working cloud.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		const ccBBox box = localBoundingBox(source);
		ccRasterGrid rasterGrid;
		if (!rasterGrid.init(static_cast<unsigned>(validation.estimatedGridWidth),
		                     static_cast<unsigned>(validation.estimatedGridHeight),
		                     parameters.gridStep,
		                     box.minCorner().toDouble()))
		{
			result.status = TerrainStatus::OutOfMemory;
			result.message = QStringLiteral("Not enough memory to allocate the CloudCompare raster grid.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		ccRasterGrid::DelaunayInterpolationParams delaunayParameters;
		delaunayParameters.maxEdgeLength = parameters.maximumInterpolationEdgeLength;
		const ccRasterGrid::InterpolationType interpolation =
			parameters.interpolateEmptyCells
				? ccRasterGrid::InterpolationType::DELAUNAY
				: ccRasterGrid::InterpolationType::NONE;
		void* interpolationParameters =
			parameters.interpolateEmptyCells ? static_cast<void*>(&delaunayParameters) : nullptr;
		if (!rasterGrid.fillWith(temporaryGround.get(),
		                         2,
		                         ccRasterGrid::PROJ_MINIMUM_VALUE,
		                         interpolation,
		                         interpolationParameters,
		                         ccRasterGrid::INVALID_PROJECTION_TYPE,
		                         context.progressDialog,
		                         -1))
		{
			result.status = TerrainStatus::RasterizationFailedOrCancelled;
			result.message = QStringLiteral("CloudCompare DTM rasterization failed or was cancelled.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::WRN_CONSOLE_MESSAGE);
			return result;
		}
		if (!rasterGrid.isValid() || rasterGrid.validCellCount == 0)
		{
			result.status = TerrainStatus::InvalidOutput;
			result.message = QStringLiteral("CloudCompare returned a DTM grid without valid cells.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		try
		{
			if (!flattenGrid(rasterGrid, source, result.grid, result.statistics))
			{
				result.status = TerrainStatus::OutOfMemory;
				result.message = QStringLiteral("Not enough memory to persist the DTM grid in the session model.");
				result.provenance.elapsedMilliseconds = timer.elapsed();
				logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
				return result;
			}
		}
		catch (const std::bad_alloc&)
		{
			result.status = TerrainStatus::OutOfMemory;
			result.message = QStringLiteral("Not enough memory to persist the DTM grid in the session model.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		try
		{
			result.dtmCloud = makeDtmCloud(
				rasterGrid, *temporaryGround, source, box, context.progressDialog);
		}
		catch (const std::bad_alloc&)
		{
			result.dtmCloud.reset();
		}
		if (!result.dtmCloud)
		{
			result.status = TerrainStatus::DtmCloudCreationFailedOrCancelled;
			result.message = QStringLiteral("The qAL DTM cloud could not be created or the export was cancelled.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		try
		{
			result.heightAboveGround.assign(static_cast<std::size_t>(source.size()), nanValue());
		}
		catch (const std::bad_alloc&)
		{
			result.status = TerrainStatus::OutOfMemory;
			result.message = QStringLiteral("Not enough memory for the point-index-aligned HAG values.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		StreamingStatistics hagStatistics;
		const double globalScale = source.getGlobalScale();
		for (unsigned index = 0; index < source.size(); ++index)
		{
			const CCVector3* point = source.getPoint(index);
			double hag = nanValue();
			if (point
			    && std::isfinite(static_cast<double>(point->x))
			    && std::isfinite(static_cast<double>(point->y))
			    && std::isfinite(static_cast<double>(point->z)))
			{
				const double terrainHeight = result.grid.bilinearHeightLocal(point->x, point->y);
				if (std::isfinite(terrainHeight))
				{
					// Translation cancels in a height difference; Global Scale does not.
					hag = (static_cast<double>(point->z) - terrainHeight) / globalScale;
					if (hag < 0.0)
					{
						++result.statistics.negativeHeightAboveGroundCount;
					}
				}
			}

			result.heightAboveGround[index] = hag;
			hagStatistics.add(hag);
		}
		result.statistics.heightAboveGround = hagStatistics.finish();

		result.provenance.elapsedMilliseconds = timer.elapsed();
		result.provenance.effectiveParameters.insert(
			QStringLiteral("observedCellCount"), static_cast<double>(result.statistics.observedCellCount));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("interpolatedCellCount"), static_cast<double>(result.statistics.interpolatedCellCount));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("nodataCellCount"), static_cast<double>(result.statistics.nodataCellCount));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("validHagCount"),
			static_cast<double>(result.statistics.heightAboveGround.validCount));
		result.status = TerrainStatus::Success;
		result.message = QStringLiteral("qAL DTM and point-index-aligned HAG were created successfully.");
		logMessage(
			context.app,
			QStringLiteral("Completed in %1 ms: %2 observed cells, %3 interpolated, %4 NODATA; HAG valid for %5/%6 points.")
				.arg(result.provenance.elapsedMilliseconds)
				.arg(result.statistics.observedCellCount)
				.arg(result.statistics.interpolatedCellCount)
				.arg(result.statistics.nodataCellCount)
				.arg(result.statistics.heightAboveGround.validCount)
				.arg(result.statistics.sourcePointCount),
			ccMainAppInterface::STD_CONSOLE_MESSAGE);
		return result;
	}
}
