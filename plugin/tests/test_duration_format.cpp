// SPDX-License-Identifier: GPL-2.0-or-later
#include "DurationFormat.h"
#include "FeatureSpeedProfiles.h"
#include <iostream>
#include <limits>

int main() {
    int checks=0;
    auto check=[&](double seconds,const char* expected){++checks;const auto actual=alis::formatDurationHoursMinutes(seconds);if(actual!=expected){std::cerr<<seconds<<": "<<actual.toStdString()<<" != "<<expected<<'\n';return false;}return true;};
    if(!check(0,"00 h 00 min")||!check(-10,"00 h 00 min")||!check(29,"00 h 00 min")
       ||!check(30,"00 h 01 min")||!check(59,"00 h 01 min")||!check(3599,"01 h 00 min")
       ||!check(353.4*60,"05 h 53 min")||!check(2104.6*60,"35 h 05 min")
       ||!check(86400,"24 h 00 min")||!check(360000,"100 h 00 min")
       ||!check(std::numeric_limits<double>::quiet_NaN(),"-- h -- min")
       ||!check(std::numeric_limits<double>::infinity(),"-- h -- min")
       ||!check(std::numeric_limits<double>::max(),"-- h -- min"))return 1;
    const QStringList original{"planarity@0.5","planarity@1","planarity@2","planarity@4"};
    const QVector<double> radii{4,1,.5,2,1};
    const auto complete=alis::featureSpeedProfile(0,original,radii),balanced=alis::featureSpeedProfile(1,original,radii),fast=alis::featureSpeedProfile(2,original,radii);
    if(complete.keys!=original||complete.radii!=radii||balanced.keys.size()!=36||fast.keys.size()!=16||balanced.radii!=QVector<double>({.5,1,2})||fast.radii!=QVector<double>({.5,1}))return 2;
    for(const auto& key:fast.keys)if(key.contains("curvature")||key.contains("percentile")||key.endsWith("@4"))return 3;
    if(alis::featureSpeedProfile(2,original,{.5}).keys.size()!=8||!alis::featureSpeedProfile(2,original,{}).keys.isEmpty())return 4;
    std::cout<<checks<<" duration checks and 9 speed-profile invariants passed\n";
}
