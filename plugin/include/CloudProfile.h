// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FeatureEngine.h"
#include <QJsonObject>
#include <QString>
class ccPointCloud;
namespace alis {
QJsonObject cloudProfile(ccPointCloud& cloud,bool metricConfirmed,const SpacingSummary* spacing=nullptr);
QString cloudProfileText(const QJsonObject& profile);
}
