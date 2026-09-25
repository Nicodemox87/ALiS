// SPDX-License-Identifier: GPL-2.0-or-later
#include "GroundFilter.h"

#include <ccPointCloud.h>

#include <cstdlib>
#include <iostream>

namespace
{
	int checks = 0;
	int failures = 0;

	void check(bool condition, const char* message)
	{
		++checks;
		if (!condition)
		{
			++failures;
			std::cerr << "FAIL: " << message << '\n';
		}
	}
}

int main()
{
	using namespace alis;
	ccPointCloud cloud(QStringLiteral("pmf-fixture"));
	check(cloud.reserve(26), "synthetic cloud allocation succeeds");
	for (int y = 0; y < 5; ++y)
	{
		for (int x = 0; x < 5; ++x)
		{
			cloud.addPoint(CCVector3(static_cast<PointCoordinateType>(x),
			                         static_cast<PointCoordinateType>(y),
			                         0.0f));
		}
	}
	cloud.addPoint(CCVector3(2.0f, 2.0f, 2.0f));

	PMFGroundFilter filter;
	GroundFilterParameters parameters;
	parameters.pmfWindowSizes = {1.5, 3.0};
	parameters.pmfThresholds = {0.20, 0.50};
	const GroundFilterValidation validation = filter.validate(cloud, parameters);
	check(validation.valid, "PMF accepts a valid progressive ws/th sequence");
	check(validation.estimatedRasterCellCount > 0, "PMF reports its bounded working raster");

	GroundFilterContext context;
	const GroundFilterResult result = filter.run(cloud, parameters, context);
	check(result.succeeded(), "PMF completes on the synthetic cloud");
	check(result.isGround.size() == cloud.size(), "PMF mask remains point-index aligned");
	check(result.groundPointCount == 25 && result.offGroundPointCount == 1,
	      "PMF retains the flat surface and rejects the elevated shrub point");
	check(result.isGround.back() == false, "the elevated final point is Non-ground");
	check(result.provenance.algorithmId == filter.algorithmId(), "PMF provenance records the algorithm");

	parameters.pmfThresholds.pop_back();
	check(!filter.validate(cloud, parameters).valid, "PMF rejects mismatched ws/th sequences");

	if (failures)
	{
		std::cerr << failures << " of " << checks << " PMF checks failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "ALiS PMF tests passed (" << checks << " checks)\n";
	return EXIT_SUCCESS;
}
