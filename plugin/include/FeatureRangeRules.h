// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
namespace alis {
inline bool withinFeatureRange(double value,double low,double high){
    return std::isfinite(value)&&std::isfinite(low)&&std::isfinite(high)&&low<=high&&value>=low&&value<=high;
}
}
