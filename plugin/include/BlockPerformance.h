// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace alis {
// Scheduling heuristics, not a hard OS memory quota or a promised completion time.
inline std::uint64_t automaticWorkerBudget(std::uint64_t available){
    const double gib=1073741824.;const double reserve=std::max(gib*2,available*.4);
    return std::uint64_t(std::max(0.,std::min(gib*24,std::min(available*.5,double(available)-reserve))));
}
inline int maximumFeatureThreads(int logical){return std::max(1,std::min(32,logical-2));}
inline int recommendedFeatureThreads(int logical){return std::min(24,maximumFeatureThreads(logical));}
inline std::uint64_t boundedManualWorkerBudget(std::uint64_t available,std::uint64_t requested){
    const double reserve=std::max(2147483648.,available*.25);
    return std::uint64_t(std::max(0.,std::min(double(requested),double(available)-reserve)));
}
inline double suggestedGridSide(double points,double area,double halo,double bytesPerPoint,std::uint64_t budget){
    if(points<=0||area<=0||bytesPerPoint<=0||budget<=268435456)return 25;
    const double support=(budget-268435456)/bytesPerPoint*.55;
    const double side=std::sqrt(support/(points/area))-2*halo;
    // Rounded down: a starting map grid only. Actual full-resolution context
    // counts, not this global average, drive the adaptive execution below it.
    return std::max(5.,std::min(100.,std::floor(side/5)*5));
}
inline double blockWorkUnits(double points,double area){return points*(1+points/std::max(1.,area));}
struct BlockTimeEstimator {
    struct Sample{double units,seconds,points;};std::vector<Sample> samples;
    void add(double points,double area,double seconds){if(points>=10000&&seconds>0&&std::isfinite(seconds))samples.push_back({blockWorkUnits(points,area),seconds,points});}
    bool ready()const{return !samples.empty();}
    double remaining(double units)const{double seconds=0,work=0;for(const auto& s:samples){seconds+=s.seconds;work+=s.units;}return work>0?units*seconds/work:0;}
    double lowFactor()const{return samples.size()<3?.4:.6;}
    double highFactor()const{return samples.size()<3?2.5:1.8;}
};
}
