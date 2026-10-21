// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QStringList>
#include <QVector>
#include <functional>
class ccPointCloud;
namespace alis {
// Native Qt controls inherit the current CloudCompare palette and platform style.
// publish is called only once when a full destination copy is requested; never
// for a block or for current-cloud updates. BIN stores progressive SF metadata.
void showSpatialBlocks(ccPointCloud* cloud,const QStringList& featureKeys,
                       const QVector<double>& radii,QWidget* parent,
                       const std::function<void(ccPointCloud*)>& publish,
                       const std::function<void(ccPointCloud*)>& training);
}
