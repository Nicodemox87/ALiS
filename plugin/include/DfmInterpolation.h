// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>
namespace alis {
enum class DfmFillMethod {None=0,LocalMean=1,InverseDistance=2,Delaunay=3,Kriging=4};
struct DfmFillOptions {
    DfmFillMethod method=DfmFillMethod::None;
    double radius=1.,maximumLocalRange=1.,maximumMeanDeviation=.25; // CloudCompare local units.
    unsigned minimumSupport=4,maximumSupport=16;
    bool requireSurroundingSupport=true;
};
struct DfmFillStatistics {std::uint64_t filled=0,insufficient=0,discontinuity=0,deviation=0,remaining=0;};
// Fill original voids ONLY. Existing estimates never become support for further estimates.
// Delaunay candidates, if requested, are supplied in heights by ccRasterGrid beforehand.
inline DfmFillStatistics fillDfmVoids(unsigned width,unsigned height,double step,std::vector<double>& heights,
 const std::vector<unsigned char>& observed,const DfmFillOptions& p,const std::function<void()>& tick={},const std::function<double(unsigned,unsigned)>& krigingCandidate={}){
    const auto n=std::size_t(width)*height;DfmFillStatistics result;
    if(heights.size()!=n||observed.size()!=n||!std::isfinite(step)||step<=0)throw std::runtime_error("DFM grid alignment is invalid");
    if(p.method==DfmFillMethod::None){for(auto z:heights)result.remaining+=!std::isfinite(z);return result;}
    if(int(p.method)<0||int(p.method)>4||(p.method==DfmFillMethod::Kriging&&!krigingCandidate))throw std::runtime_error("DFM interpolation method or Kriging predictor missing");
    if(!std::isfinite(p.radius)||p.radius<=0||!std::isfinite(p.maximumLocalRange)||p.maximumLocalRange<0||!std::isfinite(p.maximumMeanDeviation)||p.maximumMeanDeviation<0||p.minimumSupport<3||p.maximumSupport<p.minimumSupport||p.maximumSupport>256)throw std::runtime_error("Invalid local DFM interpolation settings");
    const double cells=p.radius/step;if(cells>64)throw std::runtime_error("DFM local radius exceeds 64 cells; reduce radius or increase grid step");const int reach=int(std::ceil(cells));
    std::uint64_t voids=0;for(std::size_t i=0;i<n;++i)voids+=!observed[i];
    if(double(voids)*double(2*reach+1)*double(2*reach+1)>200000000.)throw std::runtime_error("DFM local interpolation search budget exceeded; use a smaller radius, coarser grid or working selection");
    const auto original=heights;struct Neighbour {double d2,z;int dx,dy;std::size_t index;};std::vector<Neighbour> support;support.reserve(std::size_t(2*reach+1)*(2*reach+1));
    for(unsigned row=0;row<height;++row){if(tick)tick();for(unsigned col=0;col<width;++col){const auto i=std::size_t(row)*width+col;if(observed[i])continue;heights[i]=std::numeric_limits<double>::quiet_NaN();
        if(p.method==DfmFillMethod::Delaunay&&!std::isfinite(original[i])){++result.insufficient;continue;}
        support.clear();for(int dy=-reach;dy<=reach;++dy)for(int dx=-reach;dx<=reach;++dx){const int x=int(col)+dx,y=int(row)+dy;if(x<0||y<0||x>=int(width)||y>=int(height)||(dx==0&&dy==0))continue;const double d2=double(dx)*dx+double(dy)*dy;if(d2>cells*cells)continue;const auto j=std::size_t(y)*width+x;if(observed[j]&&std::isfinite(original[j]))support.push_back({d2,original[j],dx,dy,j});}
        if(support.size()<p.minimumSupport){++result.insufficient;continue;}
        std::sort(support.begin(),support.end(),[](const auto& a,const auto& b){return a.d2==b.d2?a.index<b.index:a.d2<b.d2;});if(support.size()>p.maximumSupport)support.resize(p.maximumSupport);
        unsigned quadrants=0;double mean=0,low=support.front().z,high=low,weighted=0,weights=0;
        for(const auto& v:support){mean+=v.z;low=std::min(low,v.z);high=std::max(high,v.z);const double w=1./v.d2;weighted+=w*v.z;weights+=w;if(v.dx<=0&&v.dy<=0)quadrants|=1;if(v.dx>=0&&v.dy<=0)quadrants|=2;if(v.dx<=0&&v.dy>=0)quadrants|=4;if(v.dx>=0&&v.dy>=0)quadrants|=8;}
        if(p.requireSurroundingSupport&&quadrants!=15){++result.insufficient;continue;}
        if(high-low>p.maximumLocalRange){++result.discontinuity;continue;}mean/=double(support.size());
        const double estimate=p.method==DfmFillMethod::LocalMean?mean:p.method==DfmFillMethod::InverseDistance?weighted/weights:p.method==DfmFillMethod::Kriging?krigingCandidate(col,row):original[i];
        if(!std::isfinite(estimate)||std::abs(estimate-mean)>p.maximumMeanDeviation){++result.deviation;continue;}heights[i]=estimate;++result.filled;
    }}for(auto z:heights)result.remaining+=!std::isfinite(z);return result;
}
}
