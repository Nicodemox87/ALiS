// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alis
{
	enum class SelectionState
	{
		None,
		SinglePointCloud,
		MultipleEntities,
		UnsupportedEntity
	};

	SelectionState validateSelection(std::size_t selectionCount, bool soleEntityIsPointCloud);

	struct Vector3d
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	struct Bounds3d
	{
		bool valid = false;
		Vector3d minimum;
		Vector3d maximum;
	};

	struct Dimensions3d
	{
		bool valid = false;
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	Dimensions3d calculateDimensions(const Bounds3d& bounds);

	struct CloudFacts
	{
		std::string name;
		std::uint64_t pointCount = 0;
		Bounds3d bounds;
		std::vector<std::string> scalarFields;
		bool hasRgb = false;
		bool hasNormals = false;
	};

	struct AuditSummary
	{
		bool empty = true;
		std::size_t scalarFieldCount = 0;
		Dimensions3d dimensions;
	};

	AuditSummary summarizeCloud(const CloudFacts& facts);
}

