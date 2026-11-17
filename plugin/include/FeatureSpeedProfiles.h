// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace alis {
struct FeatureSpeedProfile {QString id,description;QStringList keys;QVector<double> radii;};
inline FeatureSpeedProfile featureSpeedProfile(int index,const QStringList& originalKeys,QVector<double> originalRadii){
    FeatureSpeedProfile p;p.id="current";p.description="Exact current feature/radius selection. Full-resolution core points and full halo support.";p.keys=originalKeys;p.radii=originalRadii;
    if(index<=0)return p;
    originalRadii.erase(std::remove_if(originalRadii.begin(),originalRadii.end(),[](double r){return !std::isfinite(r)||r<=0;}),originalRadii.end());
    std::sort(originalRadii.begin(),originalRadii.end());originalRadii.erase(std::unique(originalRadii.begin(),originalRadii.end()),originalRadii.end());
    p.id=index==1?"balanced":"fast";p.radii=originalRadii.mid(0,index==1?3:2);p.keys.clear();
    const QStringList names=index==1?QStringList{"neighbor_count","linearity","planarity","sphericity","eigenentropy","surface_variation","verticality","roughness","z_range","z_standard_deviation","z_above_minimum","z_below_maximum"}:QStringList{"neighbor_count","linearity","planarity","surface_variation","verticality","roughness","z_range","z_standard_deviation"};
    for(const auto& name:names)for(double r:p.radii)p.keys<<name+"@"+QString::number(r,'g',12);
    p.description=index==1?"12 descriptors at the 3 smallest selected radii; no quadric curvature fitting or height percentiles.":"8 descriptors at the 2 smallest selected radii; no quadric curvature fitting or height percentiles.";
    p.description+=" All source points and exact neighbours at retained radii are preserved. Fewer/lower scales are NOT equivalent to the complete schema; train/validate a matching model. Classification accuracy is not certified by this speed preset.";
    return p;
}
}
