// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic, non-GUI microbenchmark for the pure numerical building blocks.
// It does not measure CloudCompare octree extraction or the end-to-end FeatureEngine.
#include "WorkspaceLogic.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{
	std::size_t parseCount(const char* value)
	{
		try
		{
			const unsigned long long parsed = std::stoull(value);
			return parsed > 0 ? static_cast<std::size_t>(parsed) : 0;
		}
		catch (...)
		{
			return 0;
		}
	}

	double millisecondsSince(const std::chrono::steady_clock::time_point& start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
}

int main(int argc, char** argv)
{
	const std::size_t pointCount = argc > 1 ? parseCount(argv[1]) : 100000;
	if (!pointCount)
	{
		std::cerr << "Usage: benchmark_workspace_logic [positive-point-count]\n";
		return EXIT_FAILURE;
	}

	// Fixed seed and distributions make runs directly comparable on one build/machine.
	std::mt19937_64 generator(0x514152434841454FULL);
	std::uniform_real_distribution<double> eigen1Distribution(0.01, 5.0);
	std::uniform_real_distribution<double> ratioDistribution(0.0, 1.0);
	std::vector<double> values;
	values.reserve(pointCount);
	for (std::size_t i = 0; i < pointCount; ++i)
	{
		values.push_back(std::sin(static_cast<double>(i) * 0.001) + ratioDistribution(generator));
	}

	volatile double checksum = 0.0; // Prevent the optimizer from removing numerical work.
	auto start = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < pointCount; ++i)
	{
		const double l1 = eigen1Distribution(generator);
		const double l2 = l1 * ratioDistribution(generator);
		const double l3 = l2 * ratioDistribution(generator);
		const alis::Descriptors descriptors =
			alis::descriptorsFromEigenvalues(l1, l2, l3, 32, 0.5);
		checksum += descriptors.planarity + descriptors.linearity + descriptors.surfaceVariation;
	}
	const double descriptorsMs = millisecondsSince(start);

	start = std::chrono::steady_clock::now();
	const alis::SummaryStatistics statistics = alis::summarizeFinite(values);
	checksum += statistics.mean + statistics.standardDeviation;
	const double statisticsMs = millisecondsSince(start);

	const std::size_t gridWidth = 1024;
	const std::size_t gridHeight = 1024;
	std::vector<double> grid(gridWidth * gridHeight);
	for (std::size_t y = 0; y < gridHeight; ++y)
	{
		for (std::size_t x = 0; x < gridWidth; ++x)
		{
			grid[y * gridWidth + x] = 0.01 * static_cast<double>(x) + 0.02 * static_cast<double>(y);
		}
	}
	start = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < pointCount; ++i)
	{
		const double x = 0.5 + static_cast<double>(i % (gridWidth - 1));
		const double y = 0.5 + static_cast<double>((i / (gridWidth - 1)) % (gridHeight - 1));
		checksum += alis::bilinearHeight(grid, gridWidth, gridHeight, 0.0, 0.0, 1.0, x, y);
	}
	const double bilinearMs = millisecondsSince(start);

	std::cout << "points,descriptor_ms,statistics_ms,bilinear_ms,checksum\n";
	std::cout << pointCount << ',' << std::fixed << std::setprecision(3)
		<< descriptorsMs << ',' << statisticsMs << ',' << bilinearMs << ',' << checksum << '\n';
	return EXIT_SUCCESS;
}
