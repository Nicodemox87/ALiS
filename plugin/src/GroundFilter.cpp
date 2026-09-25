// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adapter for qCSF, copyright RAMM Laboratory / Beijing Normal University,
// Wuming Zhang, Jianbo Qi, Peng Wan and Hongtao Wang. qCSF source headers
// license the implementation under GPL-2.0-or-later. Scientific reference:
// Zhang et al., Remote Sensing 2016, 8(6), 501, doi:10.3390/rs8060501.

#include "GroundFilter.h"

// CloudCompare qCSF (v2.13.2 baseline)
#include <CSF.h>

// CloudCompare
#include <ccBBox.h>
#include <ccMainAppInterface.h>
#include <ccMesh.h>
#include <ccPointCloud.h>

// Qt
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QThread>

// Standard library
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <new>

namespace
{
	constexpr const char* QCSF_IMPLEMENTATION =
		"CloudCompare/qCSF v2.13.2@49dbbb662f296c7780aae717897c85b3cb3764ed";
	constexpr const char* PMF_IMPLEMENTATION =
		"ALiS fast raster PMF; lidR/Zhang ws-th semantics";
	constexpr double MaxPmfRasterCells = 8000000.0;

	bool finitePositive(double value);

	QJsonObject csfParametersToJson(const alis::GroundFilterParameters& parameters)
	{
		QJsonObject json;
		json.insert(QStringLiteral("coordinateUnits"), QStringLiteral("CloudCompare local units"));
		json.insert(QStringLiteral("smoothSlope"), parameters.smoothSlope);
		json.insert(QStringLiteral("timeStep"), parameters.timeStep);
		json.insert(QStringLiteral("classificationThreshold"), parameters.classificationThreshold);
		json.insert(QStringLiteral("clothResolution"), parameters.clothResolution);
		json.insert(QStringLiteral("rigidness"), parameters.rigidness);
		json.insert(QStringLiteral("iterations"), parameters.iterations);
		json.insert(QStringLiteral("output"), QStringLiteral("point-index-aligned binary mask"));
		return json;
	}

	double effectivePmfCellSize(const alis::GroundFilterParameters& parameters,
	                            double extentX,
	                            double extentY)
	{
		double cellSize = parameters.pmfCellSize;
		if (!finitePositive(cellSize) && !parameters.pmfWindowSizes.empty())
		{
			cellSize = parameters.pmfWindowSizes.front() / 6.0;
		}
		if (!finitePositive(cellSize))
		{
			return 0.0;
		}

		for (int attempt = 0; attempt < 6; ++attempt)
		{
			const double width = std::floor(extentX / cellSize) + 1.0;
			const double height = std::floor(extentY / cellSize) + 1.0;
			const double cells = width * height;
			if (std::isfinite(cells) && cells <= MaxPmfRasterCells)
			{
				return cellSize;
			}
			cellSize *= std::sqrt(cells / MaxPmfRasterCells) * 1.01;
		}
		return cellSize;
	}

	QJsonObject pmfParametersToJson(const alis::GroundFilterParameters& parameters,
	                                double effectiveCellSize)
	{
		QJsonArray windows;
		QJsonArray thresholds;
		for (double value : parameters.pmfWindowSizes) windows.append(value);
		for (double value : parameters.pmfThresholds) thresholds.append(value);

		QJsonObject json;
		json.insert(QStringLiteral("coordinateUnits"), QStringLiteral("CloudCompare local units"));
		json.insert(QStringLiteral("windowSizes"), windows);
		json.insert(QStringLiteral("heightThresholds"), thresholds);
		json.insert(QStringLiteral("requestedCellSize"), parameters.pmfCellSize);
		json.insert(QStringLiteral("effectiveCellSize"), effectiveCellSize);
		QJsonArray cellWidths,footprints;
		for(double ws:parameters.pmfWindowSizes){double cells=2*std::max(1.,std::ceil(ws/(2*effectiveCellSize)))+1;cellWidths.append(cells);footprints.append(cells*effectiveCellSize);}
		json.insert(QStringLiteral("effectiveWindowCells"),cellWidths);
		json.insert(QStringLiteral("effectiveWindowFootprints"),footprints);
		json.insert(QStringLiteral("semantics"), QStringLiteral("lidR/Zhang progressive ws/th"));
		json.insert(QStringLiteral("implementationMode"), QStringLiteral("fast raster surface opening"));
		json.insert(QStringLiteral("output"), QStringLiteral("point-index-aligned binary mask"));
		return json;
	}

	bool finitePositive(double value)
	{
		return std::isfinite(value) && value > std::numeric_limits<double>::epsilon();
	}

	void logMessage(ccMainAppInterface* app,
	                const QString& message,
	                ccMainAppInterface::ConsoleMessageLevel level)
	{
		if (app)
		{
			app->dispToConsole(QStringLiteral("[ALiS][Ground][CSF] %1").arg(message), level);
		}
	}

	void logPmfMessage(ccMainAppInterface* app,
	                   const QString& message,
	                   ccMainAppInterface::ConsoleMessageLevel level)
	{
		if (app)
		{
			app->dispToConsole(QStringLiteral("[ALiS][Ground][PMF] %1").arg(message), level);
		}
	}

	void slidingRows(const std::vector<double>& input,
	                 std::size_t width,
	                 std::size_t height,
	                 std::size_t radius,
	                 bool minimum,
	                 std::vector<double>& output)
	{
		const double noData = std::numeric_limits<double>::quiet_NaN();
		output.assign(input.size(), noData);
		for (std::size_t y = 0; y < height; ++y)
		{
			std::deque<std::size_t> queue;
			std::size_t next = 0;
			for (std::size_t x = 0; x < width; ++x)
			{
				const std::size_t end = std::min(width - 1, x + radius);
				while (next <= end)
				{
					const double value = input[y * width + next];
					if (std::isfinite(value))
					{
						while (!queue.empty())
						{
							const double back = input[y * width + queue.back()];
							if (minimum ? value > back : value < back) break;
							queue.pop_back();
						}
						queue.push_back(next);
					}
					++next;
				}

				const std::size_t begin = x > radius ? x - radius : 0;
				while (!queue.empty() && queue.front() < begin) queue.pop_front();
				if (!queue.empty()) output[y * width + x] = input[y * width + queue.front()];
			}
		}
	}

	void slidingColumns(const std::vector<double>& input,
	                    std::size_t width,
	                    std::size_t height,
	                    std::size_t radius,
	                    bool minimum,
	                    std::vector<double>& output)
	{
		const double noData = std::numeric_limits<double>::quiet_NaN();
		output.assign(input.size(), noData);
		for (std::size_t x = 0; x < width; ++x)
		{
			std::deque<std::size_t> queue;
			std::size_t next = 0;
			for (std::size_t y = 0; y < height; ++y)
			{
				const std::size_t end = std::min(height - 1, y + radius);
				while (next <= end)
				{
					const double value = input[next * width + x];
					if (std::isfinite(value))
					{
						while (!queue.empty())
						{
							const double back = input[queue.back() * width + x];
							if (minimum ? value > back : value < back) break;
							queue.pop_back();
						}
						queue.push_back(next);
					}
					++next;
				}

				const std::size_t begin = y > radius ? y - radius : 0;
				while (!queue.empty() && queue.front() < begin) queue.pop_front();
				if (!queue.empty()) output[y * width + x] = input[queue.front() * width + x];
			}
		}
	}

	void morphologicalOpening(const std::vector<double>& surface,
	                          std::size_t width,
	                          std::size_t height,
	                          std::size_t radius,
	                          std::vector<double>& opened)
	{
		std::vector<double> horizontal;
		std::vector<double> eroded;
		std::vector<double> dilatedHorizontal;
		slidingRows(surface, width, height, radius, true, horizontal);
		slidingColumns(horizontal, width, height, radius, true, eroded);
		slidingRows(eroded, width, height, radius, false, dilatedHorizontal);
		slidingColumns(dilatedHorizontal, width, height, radius, false, opened);
	}
}

namespace alis
{
	QString CSFGroundFilter::algorithmId() const
	{
		return QStringLiteral("csf.cloudcompare.v2.13.2");
	}

	GroundFilterValidation CSFGroundFilter::validate(ccPointCloud& cloud,
	                                                 const GroundFilterParameters& parameters) const
	{
		GroundFilterValidation validation;

		if (cloud.size() == 0)
		{
			validation.message = QStringLiteral("The source cloud is empty.");
			return validation;
		}

		if (!finitePositive(parameters.clothResolution))
		{
			validation.message = QStringLiteral("Cloth resolution must be finite and greater than zero.");
			return validation;
		}

		if (!finitePositive(parameters.classificationThreshold))
		{
			validation.message = QStringLiteral("Classification threshold must be finite and greater than zero.");
			return validation;
		}

		if (!finitePositive(parameters.timeStep))
		{
			validation.message = QStringLiteral("Time step must be finite and greater than zero.");
			return validation;
		}

		if (parameters.rigidness < 1 || parameters.rigidness > 3)
		{
			validation.message = QStringLiteral("Rigidness must be 1 (slope), 2 (relief), or 3 (flat).");
			return validation;
		}

		if (parameters.iterations <= 0)
		{
			validation.message = QStringLiteral("Iteration count must be greater than zero.");
			return validation;
		}

		if (!finitePositive(cloud.getGlobalScale()))
		{
			validation.message = QStringLiteral("The source cloud has an invalid Global Scale.");
			return validation;
		}

		const ccBBox box = cloud.getOwnBB();
		if (!box.isValid())
		{
			validation.message = QStringLiteral("The source cloud bounding box is invalid.");
			return validation;
		}

		const double extentX = static_cast<double>(box.maxCorner().x) - static_cast<double>(box.minCorner().x);
		const double extentY = static_cast<double>(box.maxCorner().y) - static_cast<double>(box.minCorner().y);
		if (!std::isfinite(extentX) || !std::isfinite(extentY) || extentX < 0.0 || extentY < 0.0)
		{
			validation.message = QStringLiteral("The source cloud has an invalid horizontal extent.");
			return validation;
		}

		// Mirrors qCSF's cloth dimensions: floor(extent / resolution) + 2 * clothBuffer,
		// where the fixed clothBuffer is 2 in CloudCompare v2.13.2.
		const double width = std::floor(extentX / parameters.clothResolution) + 4.0;
		const double height = std::floor(extentY / parameters.clothResolution) + 4.0;
		if (!std::isfinite(width) || !std::isfinite(height)
		    || width < 1.0 || height < 1.0
		    || width > static_cast<double>(std::numeric_limits<int>::max())
		    || height > static_cast<double>(std::numeric_limits<int>::max()))
		{
			validation.message = QStringLiteral("The requested resolution produces invalid cloth dimensions.");
			return validation;
		}

		validation.estimatedClothWidth = static_cast<quint64>(width);
		validation.estimatedClothHeight = static_cast<quint64>(height);
		if (validation.estimatedClothWidth != 0
		    && validation.estimatedClothHeight > std::numeric_limits<quint64>::max() / validation.estimatedClothWidth)
		{
			validation.message = QStringLiteral("The requested cloth node count overflows its storage type.");
			return validation;
		}

		validation.estimatedClothNodeCount = validation.estimatedClothWidth * validation.estimatedClothHeight;
		validation.valid = true;
		validation.message = QStringLiteral("Parameters are valid.");
		return validation;
	}

	GroundFilterResult CSFGroundFilter::run(ccPointCloud& cloud,
	                                        const GroundFilterParameters& parameters,
	                                        const GroundFilterContext& context) const
	{
		GroundFilterResult result;
		result.provenance.algorithmId = algorithmId();
		result.provenance.implementation = QString::fromLatin1(QCSF_IMPLEMENTATION);
		result.provenance.sourceEntityUid = static_cast<quint64>(cloud.getUniqueID());
		result.provenance.sourcePointCount = static_cast<quint64>(cloud.size());
		result.provenance.sourceGlobalScale = cloud.getGlobalScale();
		result.provenance.effectiveParameters = csfParametersToJson(parameters);

		const GroundFilterValidation validation = validate(cloud, parameters);
		result.provenance.effectiveParameters.insert(
			QStringLiteral("estimatedClothWidth"),
			static_cast<double>(validation.estimatedClothWidth));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("estimatedClothHeight"),
			static_cast<double>(validation.estimatedClothHeight));
		result.provenance.effectiveParameters.insert(
			QStringLiteral("estimatedClothNodeCount"),
			static_cast<double>(validation.estimatedClothNodeCount));
		if (!validation.valid)
		{
			result.status = GroundFilterStatus::InvalidInput;
			result.message = validation.message;
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		QApplication* application = qobject_cast<QApplication*>(QApplication::instance());
		if (!application || application->thread() != QThread::currentThread())
		{
			result.status = GroundFilterStatus::InvalidInput;
			result.message = QStringLiteral("qCSF must run on the CloudCompare GUI thread.");
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		QElapsedTimer timer;
		timer.start();
		logMessage(
			context.app,
			QStringLiteral("Starting on %1 points with resolution %2 and threshold %3 (local units).")
				.arg(cloud.size())
				.arg(parameters.clothResolution, 0, 'g', 12)
				.arg(parameters.classificationThreshold, 0, 'g', 12),
			ccMainAppInterface::STD_CONSOLE_MESSAGE);

		wl::PointCloud csfCloud;
		try
		{
			csfCloud.resize(static_cast<std::size_t>(cloud.size()));
			for (unsigned i = 0; i < cloud.size(); ++i)
			{
				const CCVector3* point = cloud.getPoint(i);
				if (!point
				    || !std::isfinite(static_cast<double>(point->x))
				    || !std::isfinite(static_cast<double>(point->y))
				    || !std::isfinite(static_cast<double>(point->z)))
				{
					result.status = GroundFilterStatus::InvalidInput;
					result.message = QStringLiteral("Source point %1 has non-finite coordinates.").arg(i);
					result.provenance.elapsedMilliseconds = timer.elapsed();
					logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
					return result;
				}

				// Exact point-order-preserving coordinate transform used by qCSF's
				// ccPointCloud shortcut. No source point is reordered or removed.
				wl::Point& destination = csfCloud[i];
				destination.x = point->x;
				destination.y = -point->z;
				destination.z = point->y;
			}
		}
		catch (const std::bad_alloc&)
		{
			result.status = GroundFilterStatus::OutOfMemory;
			result.message = QStringLiteral("Not enough memory to create the temporary qCSF input buffer.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		CSF::Parameters csfParameters;
		csfParameters.smoothSlope = parameters.smoothSlope;
		csfParameters.time_step = parameters.timeStep;
		csfParameters.class_threshold = parameters.classificationThreshold;
		csfParameters.cloth_resolution = parameters.clothResolution;
		csfParameters.rigidness = parameters.rigidness;
		csfParameters.iterations = parameters.iterations;

		ccMesh* rawClothMesh = nullptr;
		bool applied = false;
		try
		{
			applied = CSF::Apply(csfCloud,
			                     csfParameters,
			                     result.isGround,
			                     false,
			                     rawClothMesh,
			                     context.app,
			                     context.parentWidget);
		}
		catch (const std::bad_alloc&)
		{
			delete rawClothMesh;
			result.isGround.clear();
			result.status = GroundFilterStatus::OutOfMemory;
			result.message = QStringLiteral("qCSF exhausted available memory.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}
		catch (...)
		{
			delete rawClothMesh;
			result.isGround.clear();
			result.status = GroundFilterStatus::AlgorithmFailedOrCancelled;
			result.message = QStringLiteral("qCSF terminated with an unexpected error.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		// Export was explicitly disabled; guard against a future implementation
		// unexpectedly returning a debug cloth mesh.
		std::unique_ptr<ccMesh> clothMeshGuard(rawClothMesh);
		result.provenance.elapsedMilliseconds = timer.elapsed();

		if (!applied)
		{
			result.isGround.clear();
			result.status = GroundFilterStatus::AlgorithmFailedOrCancelled;
			result.message = QStringLiteral("qCSF failed or was cancelled; no mask was accepted.");
			logMessage(context.app, result.message, ccMainAppInterface::WRN_CONSOLE_MESSAGE);
			return result;
		}

		if (result.isGround.size() != static_cast<std::size_t>(cloud.size()))
		{
			result.isGround.clear();
			result.status = GroundFilterStatus::InvalidOutput;
			result.message = QStringLiteral("qCSF returned a mask that is not aligned with the source cloud.");
			logMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		for (bool ground : result.isGround)
		{
			if (ground)
			{
				++result.groundPointCount;
			}
		}
		result.offGroundPointCount = result.provenance.sourcePointCount - result.groundPointCount;
		result.status = GroundFilterStatus::Success;
		result.message = QStringLiteral("qCSF produced an index-aligned Ground/Non-ground mask.");
		logMessage(
			context.app,
			QStringLiteral("Completed in %1 ms: %2 Ground, %3 Non-ground.")
				.arg(result.provenance.elapsedMilliseconds)
				.arg(result.groundPointCount)
				.arg(result.offGroundPointCount),
			ccMainAppInterface::STD_CONSOLE_MESSAGE);
		return result;
	}

	QString PMFGroundFilter::algorithmId() const
	{
		return QStringLiteral("pmf.lidr_zhang.fast_raster.v1");
	}

	GroundFilterValidation PMFGroundFilter::validate(ccPointCloud& cloud,
	                                                 const GroundFilterParameters& parameters) const
	{
		GroundFilterValidation validation;
		if(!std::isfinite(parameters.pmfCellSize)||parameters.pmfCellSize<0||!finitePositive(cloud.getGlobalScale()))
		{
			validation.message=QStringLiteral("PMF cell must be finite and >= 0 (0 = auto); Global Scale must be positive.");return validation;
		}
		if (cloud.size() == 0)
		{
			validation.message = QStringLiteral("The source cloud is empty.");
			return validation;
		}
		if (parameters.pmfWindowSizes.empty())
		{
			validation.message = QStringLiteral("PMF needs at least one window size.");
			return validation;
		}
		if (parameters.pmfWindowSizes.size() != parameters.pmfThresholds.size())
		{
			validation.message = QStringLiteral("PMF window and threshold sequences must have the same length.");
			return validation;
		}
		if (parameters.pmfWindowSizes.size() > 64)
		{
			validation.message = QStringLiteral("PMF accepts at most 64 progressive steps.");
			return validation;
		}
		for (std::size_t i = 0; i < parameters.pmfWindowSizes.size(); ++i)
		{
			if (!finitePositive(parameters.pmfWindowSizes[i]) || !finitePositive(parameters.pmfThresholds[i]))
			{
				validation.message = QStringLiteral("Every PMF window and threshold must be finite and greater than zero.");
				return validation;
			}
			if (i > 0 && parameters.pmfWindowSizes[i] <= parameters.pmfWindowSizes[i - 1])
			{
				validation.message = QStringLiteral("PMF window sizes must be strictly increasing.");
				return validation;
			}
		}

		const ccBBox box = cloud.getOwnBB();
		if (!box.isValid())
		{
			validation.message = QStringLiteral("The source cloud bounding box is invalid.");
			return validation;
		}
		const double extentX = static_cast<double>(box.maxCorner().x) - box.minCorner().x;
		const double extentY = static_cast<double>(box.maxCorner().y) - box.minCorner().y;
		const double cellSize = effectivePmfCellSize(parameters, extentX, extentY);
		if (!finitePositive(cellSize))
		{
			validation.message = QStringLiteral("PMF could not determine a valid raster cell size.");
			return validation;
		}

		const double width = std::floor(extentX / cellSize) + 1.0;
		const double height = std::floor(extentY / cellSize) + 1.0;
		const double cells = width * height;
		if (!std::isfinite(cells) || width < 1.0 || height < 1.0 || cells > MaxPmfRasterCells)
		{
			validation.message = QStringLiteral("PMF raster dimensions exceed the safe memory limit.");
			return validation;
		}

		validation.estimatedRasterWidth = static_cast<quint64>(width);
		validation.estimatedRasterHeight = static_cast<quint64>(height);
		validation.estimatedRasterCellCount = static_cast<quint64>(cells);
		validation.valid = true;
		validation.message = QStringLiteral("Parameters are valid.");
		return validation;
	}

	GroundFilterResult PMFGroundFilter::run(ccPointCloud& cloud,
	                                        const GroundFilterParameters& parameters,
	                                        const GroundFilterContext& context) const
	{
		GroundFilterResult result;
		result.provenance.algorithmId = algorithmId();
		result.provenance.implementation = QString::fromLatin1(PMF_IMPLEMENTATION);
		result.provenance.sourceEntityUid = static_cast<quint64>(cloud.getUniqueID());
		result.provenance.sourcePointCount = static_cast<quint64>(cloud.size());
		result.provenance.sourceGlobalScale = cloud.getGlobalScale();

		const GroundFilterValidation validation = validate(cloud, parameters);
		if (!validation.valid)
		{
			result.status = GroundFilterStatus::InvalidInput;
			result.message = validation.message;
			logPmfMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		const ccBBox box = cloud.getOwnBB();
		const double minimumX = box.minCorner().x;
		const double minimumY = box.minCorner().y;
		const double extentX = static_cast<double>(box.maxCorner().x) - minimumX;
		const double extentY = static_cast<double>(box.maxCorner().y) - minimumY;
		const double cellSize = effectivePmfCellSize(parameters, extentX, extentY);
		const std::size_t width = static_cast<std::size_t>(validation.estimatedRasterWidth);
		const std::size_t height = static_cast<std::size_t>(validation.estimatedRasterHeight);
		const std::size_t cellCount = width * height;
		result.provenance.effectiveParameters = pmfParametersToJson(parameters, cellSize);
		result.provenance.effectiveParameters.insert(QStringLiteral("rasterWidth"), static_cast<double>(width));
		result.provenance.effectiveParameters.insert(QStringLiteral("rasterHeight"), static_cast<double>(height));
		result.provenance.effectiveParameters.insert(QStringLiteral("rasterCellCount"), static_cast<double>(cellCount));

		QElapsedTimer timer;
		timer.start();
		logPmfMessage(context.app,
		              QStringLiteral("Starting %1 progressive step(s) on %2 points; raster %3 x %4, cell %5.")
		                  .arg(parameters.pmfWindowSizes.size())
		                  .arg(cloud.size())
		                  .arg(width)
		                  .arg(height)
		                  .arg(cellSize, 0, 'g', 8),
		              ccMainAppInterface::STD_CONSOLE_MESSAGE);

		try
		{
			const double noData = std::numeric_limits<double>::quiet_NaN();
			std::vector<double> currentZ(cloud.size());
			std::vector<std::size_t> pointCells(cloud.size());
			result.isGround.assign(cloud.size(), true);
			for (unsigned i = 0; i < cloud.size(); ++i)
			{
				const CCVector3* point = cloud.getPoint(i);
				if (!point || !std::isfinite(point->x) || !std::isfinite(point->y) || !std::isfinite(point->z))
				{
					result.status = GroundFilterStatus::InvalidInput;
					result.message = QStringLiteral("Source point %1 has non-finite coordinates.").arg(i);
					result.isGround.clear();
					return result;
				}
				currentZ[i] = point->z;
				const std::size_t x = std::min(width - 1, static_cast<std::size_t>(std::floor((point->x - minimumX) / cellSize)));
				const std::size_t y = std::min(height - 1, static_cast<std::size_t>(std::floor((point->y - minimumY) / cellSize)));
				pointCells[i] = y * width + x;
			}

			std::vector<double> surface(cellCount, noData);
			std::vector<double> opened;
			for (std::size_t step = 0; step < parameters.pmfWindowSizes.size(); ++step)
			{
				std::fill(surface.begin(), surface.end(), noData);
				for (std::size_t i = 0; i < currentZ.size(); ++i)
				{
					if (!result.isGround[i]) continue;
					double& cell = surface[pointCells[i]];
					if (!std::isfinite(cell) || currentZ[i] < cell) cell = currentZ[i];
				}

				const std::size_t radius = std::max<std::size_t>(
					1,
					static_cast<std::size_t>(std::ceil(parameters.pmfWindowSizes[step] / (2.0 * cellSize))));
				morphologicalOpening(surface, width, height, radius, opened);
				for (std::size_t i = 0; i < currentZ.size(); ++i)
				{
					if (!result.isGround[i]) continue;
					const double reference = opened[pointCells[i]];
					if (!std::isfinite(reference)
					    || currentZ[i] - reference >= parameters.pmfThresholds[step])
					{
						result.isGround[i] = false;
					}
					else
					{
						currentZ[i] = reference;
					}
				}
			}
		}
		catch (const std::bad_alloc&)
		{
			result.isGround.clear();
			result.status = GroundFilterStatus::OutOfMemory;
			result.message = QStringLiteral("PMF exhausted available memory.");
			result.provenance.elapsedMilliseconds = timer.elapsed();
			logPmfMessage(context.app, result.message, ccMainAppInterface::ERR_CONSOLE_MESSAGE);
			return result;
		}

		result.provenance.elapsedMilliseconds = timer.elapsed();
		for (bool ground : result.isGround) if (ground) ++result.groundPointCount;
		result.offGroundPointCount = result.provenance.sourcePointCount - result.groundPointCount;
		result.status = GroundFilterStatus::Success;
		result.message = QStringLiteral("PMF produced an index-aligned Ground/Non-ground mask.");
		logPmfMessage(context.app,
		              QStringLiteral("Completed in %1 ms: %2 Ground, %3 Non-ground.")
		                  .arg(result.provenance.elapsedMilliseconds)
		                  .arg(result.groundPointCount)
		                  .arg(result.offGroundPointCount),
		              ccMainAppInterface::STD_CONSOLE_MESSAGE);
		return result;
	}
}
