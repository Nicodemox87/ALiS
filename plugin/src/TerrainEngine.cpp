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
#include <QJsonDocument>
#include <QStringList>

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
		json.insert(QStringLiteral("projection"), parameters.surfaceMaximum?(parameters.dfmMinimumMerge?QStringLiteral("MIN Ground without fill + MIN Building without fill; merge aligned cell clouds; final MIN; fill only final voids"):QStringLiteral("minimum Ground Z; maximum Building Z; Building precedence per shared cell")):QStringLiteral("minimum Z of Ground points"));
		json.insert(QStringLiteral("interpolateEmptyCells"), parameters.interpolateEmptyCells);
		json.insert(QStringLiteral("interpolation"),
		            parameters.surfaceMaximum?QStringList{"none","local-mean guarded","IDW guarded","TIN / Delaunay linear guarded","Kriging guarded"}.value(int(parameters.dfmFill.method),"invalid"):parameters.interpolateEmptyCells ? QStringLiteral("Delaunay") : QStringLiteral("none"));
		if(parameters.surfaceMaximum){json["dfmMinimumMerge"]=parameters.dfmMinimumMerge;json["localRadius"]=parameters.dfmFill.radius;json["maximumLocalHeightRange"]=parameters.dfmFill.maximumLocalRange;json["maximumMeanDeviation"]=parameters.dfmFill.maximumMeanDeviation;json["minimumLocalSupport"]=int(parameters.dfmFill.minimumSupport);json["maximumLocalSupport"]=int(parameters.dfmFill.maximumSupport);json["surroundingSupportRequired"]=parameters.dfmFill.requireSurroundingSupport;json["sharedGridOrigin"]=true;}
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
		if(parameters.surfaceMaximum&&(!context.surfaceBuildingMask||context.surfaceBuildingMask->size()!=source.size())){result.status=TerrainStatus::InvalidInput;result.message="DFM requires an aligned Building mask; refusing a maximum-all-points substitute.";return result;}
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
			parameters.interpolateEmptyCells && !parameters.surfaceMaximum
				? ccRasterGrid::InterpolationType::DELAUNAY
				: ccRasterGrid::InterpolationType::NONE;
		void* interpolationParameters =
			parameters.interpolateEmptyCells ? static_cast<void*>(&delaunayParameters) : nullptr;
		if (!rasterGrid.fillWith(temporaryGround.get(),
		                         2,
		                         parameters.surfaceMaximum?ccRasterGrid::PROJ_MAXIMUM_VALUE:ccRasterGrid::PROJ_MINIMUM_VALUE,
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
		std::vector<unsigned char> surfaceSource;
		if(parameters.surfaceMaximum){
			try{
				const std::size_t cells=std::size_t(rasterGrid.width)*rasterGrid.height;
				std::vector<double> minimumGround(cells,nanValue()),maximumBuilding(cells,nanValue());surfaceSource.assign(cells,0);
				for(unsigned i=0;i<source.size();++i)if(isGround[i]){
					const auto& point=*source.getPoint(i);const auto pos=rasterGrid.computeCellPos(point,0,1);if(pos.x<0||pos.y<0||pos.x>=int(rasterGrid.width)||pos.y>=int(rasterGrid.height))continue;
					const auto offset=std::size_t(pos.y)*rasterGrid.width+pos.x;auto& value=(*context.surfaceBuildingMask)[i]?maximumBuilding[offset]:minimumGround[offset];const double z=point.z;
					if(!std::isfinite(value)||((*context.surfaceBuildingMask)[i]&&!parameters.dfmMinimumMerge?z>value:z<value))value=z;
				}
				DfmConflictGrid review;review.width=rasterGrid.width;review.height=rasterGrid.height;review.step=parameters.gridStep;review.originX=rasterGrid.minCorner.x;review.originY=rasterGrid.minCorner.y;review.globalScale=source.getGlobalScale();review.shiftX=source.getGlobalShift().x;review.shiftY=source.getGlobalShift().y;review.minimumRecipe=parameters.dfmMinimumMerge;review.ground=&minimumGround;review.building=&maximumBuilding;review.initialize();
				if(review.conflicts){if(context.conflictReview){if(!context.conflictReview(review)){result.status=TerrainStatus::RasterizationFailedOrCancelled;result.message="DFM overlap review canceled; no definitive surface or interpolation created.";return result;}}else if(parameters.requireConflictReview){result.status=TerrainStatus::InvalidInput;result.message="Ground/Building conflicts require map review before creating the definitive DFM.";return result;}}
				result.provenance.effectiveParameters["overlapReview"]=review.toJson();
				std::uint64_t groundCells=0,buildingCells=0,overlapCells=0,groundWinsOverlap=0;
				for(unsigned row=0;row<rasterGrid.height;++row)for(unsigned col=0;col<rasterGrid.width;++col){const auto offset=std::size_t(row)*rasterGrid.width+col;auto& cell=rasterGrid.rows[row][col];const bool hasBuilding=std::isfinite(maximumBuilding[offset]),hasGround=std::isfinite(minimumGround[offset]);groundCells+=hasGround;buildingCells+=hasBuilding;overlapCells+=hasBuilding&&hasGround;bool building=hasBuilding&&(!parameters.dfmMinimumMerge||!hasGround||maximumBuilding[offset]<minimumGround[offset]);if(hasBuilding&&hasGround){if(review.choice(offset)==1)building=false;else if(review.choice(offset)==2)building=true;}cell.h=building?maximumBuilding[offset]:minimumGround[offset];surfaceSource[offset]=building?2:hasGround?1:0;if(hasBuilding&&hasGround&&!building)++groundWinsOverlap;}
				result.provenance.effectiveParameters["groundStageCells"]=double(groundCells);result.provenance.effectiveParameters["buildingStageCells"]=double(buildingCells);result.provenance.effectiveParameters["overlapCells"]=double(overlapCells);result.provenance.effectiveParameters["groundWinsOverlap"]=double(groundWinsOverlap);
				rasterGrid.updateCellStats();if(parameters.interpolateEmptyCells){
					if(parameters.dfmFill.method==DfmFillMethod::Delaunay&&!rasterGrid.interpolateEmptyCells(parameters.maximumInterpolationEdgeLength*parameters.maximumInterpolationEdgeLength)){result.status=TerrainStatus::RasterizationFailedOrCancelled;result.message="DFM built, but requested Delaunay interpolation failed; no surface committed.";return result;}
					std::vector<double> heights(cells);for(unsigned row=0;row<rasterGrid.height;++row)for(unsigned col=0;col<rasterGrid.width;++col)heights[std::size_t(row)*rasterGrid.width+col]=rasterGrid.rows[row][col].h;
					std::vector<DataPoint> krigingData;std::unique_ptr<Kriging> kriging;std::shared_ptr<OrdinaryKrigeContext> krigingContext;std::function<double(unsigned,unsigned)> krigingCandidate;
					if(parameters.dfmFill.method==DfmFillMethod::Kriging){
						const int knn=parameters.krigingNeighbours;if(knn<3||knn>64||parameters.dfmFill.minimumSupport<unsigned(knn)||parameters.krigingModel<0||parameters.krigingModel>2)throw std::runtime_error("Kriging requires 3..64 neighbours, a valid model and minimum local support >= KNN");
						std::uint64_t voids=0;for(auto code:surfaceSource)voids+=code==0;if(double(voids)*std::pow(double(knn+1),3)>500000000.)throw std::runtime_error("Kriging solve budget exceeded; reduce KNN, use a coarser grid or a smaller working selection");
						double low=std::numeric_limits<double>::infinity(),high=-low;krigingData.reserve(cells-std::size_t(voids));for(unsigned row=0;row<rasterGrid.height;++row)for(unsigned col=0;col<rasterGrid.width;++col){const auto i=std::size_t(row)*rasterGrid.width+col;if(surfaceSource[i]){krigingData.emplace_back(col*parameters.gridStep,row*parameters.gridStep,heights[i]);low=std::min(low,heights[i]);high=std::max(high,heights[i]);}}
						if(krigingData.size()<=std::size_t(knn))throw std::runtime_error("Not enough observed grid cells for requested Kriging KNN");
						if(parameters.krigingAutoParameters&&low==high){krigingCandidate=[low](unsigned,unsigned){return low;};result.provenance.effectiveParameters["krigingConstantSurface"]=true;}
						else{
							kriging.reset(new Kriging(krigingData,RasterParameters(CCVector2d(0,0),parameters.gridStep,rasterGrid.width,rasterGrid.height)));Kriging::KrigeParams effective=parameters.krigingAutoParameters?kriging->computeDefaultParameters():Kriging::KrigeParams(Kriging::Model(parameters.krigingModel),parameters.krigingNugget,parameters.krigingSill,parameters.krigingRange);effective.model=Kriging::Model(parameters.krigingModel);
							if(!std::isfinite(effective.sill)||!std::isfinite(effective.range)||!std::isfinite(effective.nugget)||effective.sill<=0||effective.range<=0||effective.nugget<0||effective.nugget>effective.sill)throw std::runtime_error("Degenerate Kriging variogram; use Local mean or explicit valid variogram parameters. No silent fallback was applied");
							auto* contextPtr=kriging->createOrdinaryKrigeContext(knn);if(!contextPtr)throw std::runtime_error("Kriging context allocation failed");krigingContext=std::shared_ptr<OrdinaryKrigeContext>(contextPtr,[&kriging](OrdinaryKrigeContext* pointer){kriging->releaseOrdinaryKrigeContext(pointer);});krigingCandidate=[&kriging,contextPtr,effective](unsigned col,unsigned row){return kriging->ordinaryKrigeSingleCell(effective,col,row,contextPtr);};
							result.provenance.effectiveParameters["krigingModel"]=int(effective.model);result.provenance.effectiveParameters["krigingSillLocalSquared"]=effective.sill;result.provenance.effectiveParameters["krigingNuggetLocalSquared"]=effective.nugget;result.provenance.effectiveParameters["krigingRangeLocal"]=effective.range;
						}result.provenance.effectiveParameters["krigingKNN"]=knn;result.provenance.effectiveParameters["krigingAutoParameters"]=parameters.krigingAutoParameters;result.provenance.effectiveParameters["krigingVoidOnlyObservedSupport"]=true;
					}
					const auto filled=fillDfmVoids(rasterGrid.width,rasterGrid.height,parameters.gridStep,heights,surfaceSource,parameters.dfmFill,[&](){if(context.progressDialog){context.progressDialog->update(0);if(context.progressDialog->isCancelRequested())throw std::runtime_error("DFM interpolation canceled; no output committed");}},krigingCandidate);
					for(unsigned row=0;row<rasterGrid.height;++row)for(unsigned col=0;col<rasterGrid.width;++col)rasterGrid.rows[row][col].h=heights[std::size_t(row)*rasterGrid.width+col];rasterGrid.updateCellStats();result.provenance.effectiveParameters["filledVoids"]=double(filled.filled);result.provenance.effectiveParameters["insufficientLocalSupport"]=double(filled.insufficient);result.provenance.effectiveParameters["localDiscontinuitiesRejected"]=double(filled.discontinuity);result.provenance.effectiveParameters["meanDeviationRejected"]=double(filled.deviation);result.provenance.effectiveParameters["remainingVoids"]=double(filled.remaining);
				}
			}catch(const std::bad_alloc&){result.status=TerrainStatus::OutOfMemory;result.message="Insufficient memory for class-aware DFM grid; no source data changed.";return result;}
			catch(const std::exception& e){result.status=TerrainStatus::RasterizationFailedOrCancelled;result.message=QString::fromUtf8(e.what());return result;}
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

		if(parameters.surfaceMaximum){
			result.dtmCloud->setName(parameters.dfmMinimumMerge?"qAL DFM — MIN Ground + MIN Buildings / merged MIN":"qAL DFM — min Ground / max Buildings");result.dtmCloud->setMetaData("ALiS.role","DFM");result.dtmCloud->setMetaData("ALiS.projection",parameters.dfmMinimumMerge?"minimum-ground + minimum-building / merged minimum":"minimum-ground / maximum-building; building precedence");result.dtmCloud->setMetaData("ALiS.DFM.parameters",QJsonDocument(result.provenance.effectiveParameters).toJson(QJsonDocument::Compact));
			const int elevation=result.dtmCloud->getScalarFieldIndexByName(DTM_ELEVATION_FIELD_NAME);if(elevation>=0)result.dtmCloud->getScalarField(elevation)->setName("qAL DFM elevation");
			const int sourceIndex=result.dtmCloud->addScalarField("qAL DFM source (1 Ground, 2 Building, 3 interpolated)");if(sourceIndex<0){result.status=TerrainStatus::OutOfMemory;result.message="DFM source-quality field allocation failed; no surface committed.";return result;}
			for(unsigned i=0;i<result.dtmCloud->size();++i){const auto pos=rasterGrid.computeCellPos(*result.dtmCloud->getPoint(i),0,1);const auto code=surfaceSource[std::size_t(pos.y)*rasterGrid.width+pos.x];result.dtmCloud->getScalarField(sourceIndex)->setValue(i,code?code:3);}result.dtmCloud->getScalarField(sourceIndex)->computeMinAndMax();
			result.provenance.algorithmId=parameters.dfmMinimumMerge?"surface.staged.minimum.ground-building.minimum.v1":"surface.hybrid.min-ground.max-building.v1";result.provenance.elapsedMilliseconds=timer.elapsed();result.status=TerrainStatus::Success;result.message="DFM created; DTM/HAG retained unchanged.";return result;
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
