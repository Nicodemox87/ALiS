// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace alis {
// Half-open cores, except the outside survey edge. Each source row belongs
// to exactly one core; overlapping context must never become training rows.
struct SpatialBlockGrid {
    double x0=0,y0=0,x1=0,y1=0,side=50;
    int cols=0,rows=0;
    SpatialBlockGrid() = default;
    SpatialBlockGrid(double a,double b,double c,double d,double s):x0(a),y0(b),x1(c),y1(d),side(s) {
        if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(c)||!std::isfinite(d)||!std::isfinite(s)||s<=0||c<a||d<b)
            throw std::runtime_error("Invalid block grid bounds or side");
        const double nc=std::max(1.,std::ceil((c-a)/s)),nr=std::max(1.,std::ceil((d-b)/s));
        if(nc*nr>10000)throw std::runtime_error("More than 10,000 blocks: increase block size");
        cols=int(nc);rows=int(nr);
    }
    int size() const {return cols*rows;}
    int cell(double x,double y) const {
        if(!cols||!rows||!std::isfinite(x)||!std::isfinite(y)||x<x0||x>x1||y<y0||y>y1)return -1;
        return std::min(rows-1,int((y-y0)/side))*cols+std::min(cols-1,int((x-x0)/side));
    }
    double left(int i)const{return x0+(i%cols)*side;}
    double bottom(int i)const{return y0+(i/cols)*side;}
    double right(int i)const{return std::min(x1,left(i)+side);}
    double top(int i)const{return std::min(y1,bottom(i)+side);}
    bool context(int i,double x,double y,double halo)const {
        return std::isfinite(x)&&std::isfinite(y)&&x>=left(i)-halo&&x<=right(i)+halo&&y>=bottom(i)-halo&&y<=top(i)+halo;
    }
};
}
