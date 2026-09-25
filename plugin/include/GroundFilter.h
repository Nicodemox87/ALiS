// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <cstdint>
#include <vector>

class ccMainAppInterface;
class ccPointCloud;
class QWidget;

namespace alis
{
	//! Parameters understood by the CloudCompare v2.13.2 qCSF implementation.
	/** Length values are expressed in the selected cloud's local coordinate units.
	 *  A UI that accepts metres must convert them with ccPointCloud::getGlobalScale()
	 *  before calling a GroundFilter.
	 */
	struct GroundFilterParameters
	{
		bool smoothSlope = false;
		double timeStep = 0.65;
		double classificationThreshold = 0.5;
		double clothResolution = 1.0;
		int rigidness = 2;
		int iterations = 500;
		//! Progressive Morphological Filter window widths and height thresholds.
		//! Both sequences use CloudCompare local coordinate units and must match in size.
		std::vector<double> pmfWindowSizes;
		std::vector<double> pmfThresholds;
		//! Zero selects an automatic raster cell size derived from the first PMF window.
		double pmfCellSize = 0.0;
	};

	struct GroundFilterValidation
	{
		bool valid = false;
		QString message;
		quint64 estimatedClothWidth = 0;
		quint64 estimatedClothHeight = 0;
		quint64 estimatedClothNodeCount = 0;
		quint64 estimatedRasterWidth = 0;
		quint64 estimatedRasterHeight = 0;
		quint64 estimatedRasterCellCount = 0;
	};

	struct GroundFilterContext
	{
		ccMainAppInterface* app = nullptr;
		QWidget* parentWidget = nullptr;
	};

	enum class GroundFilterStatus
	{
		Success,
		InvalidInput,
		OutOfMemory,
		AlgorithmFailedOrCancelled,
		InvalidOutput
	};

	struct GroundFilterProvenance
	{
		QString algorithmId;
		QString implementation;
		quint64 sourceEntityUid = 0;
		quint64 sourcePointCount = 0;
		double sourceGlobalScale = 1.0;
		qint64 elapsedMilliseconds = 0;
		QJsonObject effectiveParameters;
	};

	struct GroundFilterResult
	{
		GroundFilterStatus status = GroundFilterStatus::InvalidInput;
		QString message;
		//! Bit-packed mask aligned one-to-one with the source cloud point indices.
		std::vector<bool> isGround;
		quint64 groundPointCount = 0;
		quint64 offGroundPointCount = 0;
		GroundFilterProvenance provenance;

		bool succeeded() const
		{
			return status == GroundFilterStatus::Success;
		}
	};

	//! Abstract point-index preserving ground filter.
	class GroundFilter
	{
	public:
		virtual ~GroundFilter() = default;

		virtual QString algorithmId() const = 0;
		virtual GroundFilterValidation validate(ccPointCloud& cloud,
		                                       const GroundFilterParameters& parameters) const = 0;
		virtual GroundFilterResult run(ccPointCloud& cloud,
		                               const GroundFilterParameters& parameters,
		                               const GroundFilterContext& context) const = 0;
	};

	//! Adapter for the exact qCSF implementation bundled with CloudCompare v2.13.2.
	/** This adapter calls the low-level wl::PointCloud overload so that qCSF returns
	 *  an index-aligned mask. It deliberately does not call the ccPointCloud overload,
	 *  which would allocate permanent ground and off-ground clones.
	 */
	class CSFGroundFilter final : public GroundFilter
	{
	public:
		QString algorithmId() const override;
		GroundFilterValidation validate(ccPointCloud& cloud,
		                                const GroundFilterParameters& parameters) const override;
		GroundFilterResult run(ccPointCloud& cloud,
		                       const GroundFilterParameters& parameters,
		                       const GroundFilterContext& context) const override;
	};

	//! Progressive Morphological Filter with lidR/Zhang-compatible ws/th semantics.
	/** The implementation uses a point-index-preserving raster surface and fast
	 *  separable morphological opening. It follows the progressive acceptance
	 *  rule used by lidR while avoiding an R runtime dependency.
	 */
	class PMFGroundFilter final : public GroundFilter
	{
	public:
		QString algorithmId() const override;
		GroundFilterValidation validate(ccPointCloud& cloud,
		                                const GroundFilterParameters& parameters) const override;
		GroundFilterResult run(ccPointCloud& cloud,
		                       const GroundFilterParameters& parameters,
		                       const GroundFilterContext& context) const override;
	};
}
