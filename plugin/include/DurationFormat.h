// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <algorithm>
#include <cmath>
#include <limits>

namespace alis {
// Duration, not wall-clock time: hours deliberately do not wrap after 24.
// Display precision is one minute; estimation uncertainty is shown separately.
inline QString formatDurationHoursMinutes(double seconds) {
    if(!std::isfinite(seconds))return "-- h -- min";
    const double rounded=std::floor(std::max(0.,seconds)/60.+.5);
    if(rounded>=double(std::numeric_limits<quint64>::max()))return "-- h -- min";
    const auto minutes=quint64(rounded);
    return QString("%1 h %2 min").arg(minutes/60,2,10,QChar('0')).arg(minutes%60,2,10,QChar('0'));
}
}
