// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
namespace alis {
struct DfmDecisionRegion {unsigned x0=0,y0=0,x1=0,y1=0;unsigned char preferred=0;};
struct DfmConflictGrid {
    unsigned width=0,height=0;double step=1,originX=0,originY=0,globalScale=1,shiftX=0,shiftY=0,shiftZ=0;
    const std::vector<double>* ground=nullptr;const std::vector<double>* building=nullptr;
    bool minimumRecipe=true;unsigned char fallback=0;
    std::vector<unsigned char> choices;std::vector<DfmDecisionRegion> regions;
    std::uint64_t conflicts=0,regionWork=0;
    bool overlap(std::size_t i) const {return std::isfinite((*ground)[i])&&std::isfinite((*building)[i]);}
    void initialize(){const auto n=std::size_t(width)*height;if(!width||!height||!ground||!building||ground->size()!=n||building->size()!=n||!std::isfinite(step)||step<=0||!std::isfinite(globalScale)||globalScale<=0)throw std::runtime_error("Invalid DFM conflict grid");choices.assign(n,0);conflicts=0;for(std::size_t i=0;i<n;++i)conflicts+=overlap(i);regions.clear();regionWork=0;}
    unsigned char choice(std::size_t i) const {return choices[i]?choices[i]:fallback;}
    void addRegion(DfmDecisionRegion r){if(r.preferred>2||r.x0>r.x1||r.y0>r.y1||r.x1>=width||r.y1>=height)throw std::runtime_error("Conflict review rectangle is outside the grid");const auto area=std::uint64_t(r.x1-r.x0+1)*(r.y1-r.y0+1);if(regions.size()>=1024||regionWork+area>200000000)throw std::runtime_error("Conflict review rectangle budget exceeded; clear redundant rectangles");regionWork+=area;regions.push_back(r);for(unsigned y=r.y0;y<=r.y1;++y)for(unsigned x=r.x0;x<=r.x1;++x){auto i=std::size_t(y)*width+x;if(overlap(i))choices[i]=r.preferred;}}
    void clearRegions(){regions.clear();regionWork=0;std::fill(choices.begin(),choices.end(),0);}
    void undoRegion(){if(regions.empty())return;auto keep=regions;keep.pop_back();clearRegions();for(const auto& r:keep)addRegion(r);}
    QJsonObject toJson() const {QJsonArray saved;for(const auto& r:regions)saved.append(QJsonObject{{"minColumn",int(r.x0)},{"maxColumn",int(r.x1)},{"minRow",int(r.y0)},{"maxRow",int(r.y1)},{"preferred",int(r.preferred)},{"globalMinX",(originX+(double(r.x0)-.5)*step)/globalScale-shiftX},{"globalMaxX",(originX+(double(r.x1)+.5)*step)/globalScale-shiftX},{"globalMinY",(originY+(double(r.y0)-.5)*step)/globalScale-shiftY},{"globalMaxY",(originY+(double(r.y1)+.5)*step)/globalScale-shiftY}});return {{"width",int(width)},{"height",int(height)},{"stepLocal",step},{"originXLocal",originX},{"originYLocal",originY},{"globalScale",globalScale},{"globalShiftX",shiftX},{"globalShiftY",shiftY},{"conflictCells",double(conflicts)},{"unmarkedPreference",int(fallback)},{"regions",saved},{"regionPriority","last rectangle wins; 0 recipe, 1 Ground, 2 chosen Building classes"}};}
};
}
