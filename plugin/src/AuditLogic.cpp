// SPDX-License-Identifier: GPL-2.0-or-later
#include "AuditLogic.h"

#include <cmath>

namespace alis
{
	SelectionState validateSelection(std::size_t selectionCount, bool soleEntityIsPointCloud)
	{
		if (selectionCount == 0)
		{
			return SelectionState::None;
		}

		if (selectionCount != 1)
		{
			return SelectionState::MultipleEntities;
		}

		return soleEntityIsPointCloud ? SelectionState::SinglePointCloud : SelectionState::UnsupportedEntity;
	}

	Dimensions3d calculateDimensions(const Bounds3d& bounds)
	{
		Dimensions3d result;
		if (!bounds.valid)
		{
			return result;
		}

		const double dx = bounds.maximum.x - bounds.minimum.x;
		const double dy = bounds.maximum.y - bounds.minimum.y;
		const double dz = bounds.maximum.z - bounds.minimum.z;
		if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || dx < 0.0 || dy < 0.0 || dz < 0.0)
		{
			return result;
		}

		result.valid = true;
		result.x = dx;
		result.y = dy;
		result.z = dz;
		return result;
	}

	AuditSummary summarizeCloud(const CloudFacts& facts)
	{
		AuditSummary result;
		result.empty = facts.pointCount == 0;
		result.scalarFieldCount = facts.scalarFields.size();
		result.dimensions = calculateDimensions(facts.bounds);
		return result;
	}
}

