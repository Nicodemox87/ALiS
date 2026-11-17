// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace alis {
// Place only the order statistics required by the native five interpolated
// percentiles. No approximate quantiles and no changes to interpolation.
inline void selectQuantileRanks(std::vector<double>& values) {
    if(values.size()<128){std::sort(values.begin(),values.end());return;}
    std::array<std::size_t,10> ranks{};std::size_t count=0;
    for(double p:{.10,.25,.50,.75,.90}){
        const double position=p*double(values.size()-1);
        ranks[count++]=std::size_t(std::floor(position));
        ranks[count++]=std::size_t(std::ceil(position));
    }
    std::sort(ranks.begin(),ranks.end());count=std::size_t(std::unique(ranks.begin(),ranks.end())-ranks.begin());
    struct Part{std::size_t left,right,first,last;};
    std::array<Part,24> stack{};std::size_t pending=0;stack[pending++]={0,values.size(),0,count};
    while(pending){
        const auto part=stack[--pending];if(part.first>=part.last||part.right-part.left<2)continue;
        const auto middle=part.first+(part.last-part.first)/2;const auto rank=ranks[middle];
        std::nth_element(values.begin()+part.left,values.begin()+rank,values.begin()+part.right);
        if(part.first<middle)stack[pending++]={part.left,rank,part.first,middle};
        if(middle+1<part.last)stack[pending++]={rank+1,part.right,middle+1,part.last};
    }
}
}
