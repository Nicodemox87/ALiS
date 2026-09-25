// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace alis { namespace vegetation {
// Evidence-based descriptors, but ALiS heuristic thresholds: NOT published
// universal decision boundaries, calibrated probabilities or building labels.
enum Category { Uncertain = 0, Vegetation = 1, OtherSurface = 2, Ground = 3, Excluded = 4 };
struct Observation {
    double count = 0, planarity = 0, scattering = 0, variation = 0, roughnessRatio = 0;
    bool valid(unsigned minimum) const {
        return count >= minimum && std::isfinite(planarity) && std::isfinite(scattering)
            && std::isfinite(variation) && std::isfinite(roughnessRatio);
    }
};
struct Thresholds {
    double planar = .5, scattering = .12, variation = .04, roughnessRatio = .06;
    double nearGround = .25;
    unsigned minimumNeighbors = 12;
    int preset = 0;
};
struct Decision { Category category = Uncertain; double evidence = 0; int reason = 0; };
inline double bounded(double x, double lo, double hi) { return std::max(lo, std::min(hi, x)); }
inline double quantile(std::vector<double> v, double q, double fallback) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x){return !std::isfinite(x);}), v.end());
    if (v.empty()) return fallback;
    std::sort(v.begin(), v.end());
    const double p = (v.size()-1)*q; const auto i = static_cast<std::size_t>(p);
    return v[i]+(p-i)*(v[std::min(i+1,v.size()-1)]-v[i]);
}
inline std::array<double,3> radii(double spacing, double density, double maximum,
                                unsigned target = 16, double fineLimit = .25) {
    // N ~= pi*r^2*density is only a planar-support heuristic. Actual support
    // is checked per point; no claim of measured local surface density.
    if (!std::isfinite(spacing) || spacing <= 0) spacing = .1;
    target=std::max(8u,std::min(128u,target));
    if(!std::isfinite(maximum))maximum=1;
    if(!std::isfinite(fineLimit))fineLimit=.25;
    fineLimit=bounded(fineLimit,.05,.5);
    double r = std::isfinite(density) && density > 0 ? std::sqrt(double(target)/(3.141592653589793*density)) : std::sqrt(double(target))*spacing;
    maximum = bounded(maximum,.2,3.0);
    r = bounded(r,.05,std::min(fineLimit,maximum/4));
    return {{r,2*r,std::min(maximum,4*r)}};
}
inline Thresholds adapt(const std::vector<Observation>& sample, int preset, unsigned minimum) {
    std::vector<double> p,s,v,r;
    for(const auto& o:sample) if(o.valid(minimum)){p.push_back(o.planarity);s.push_back(o.scattering);v.push_back(o.variation);r.push_back(o.roughnessRatio);}
    Thresholds t; t.preset=preset; t.minimumNeighbors=minimum;
    t.planar=bounded(quantile(p,.70,.5),.40,.65);
    t.scattering=bounded(quantile(s,.55,.12),.08,.25);
    t.variation=bounded(quantile(v,.55,.04),.025,.08);
    t.roughnessRatio=bounded(quantile(r,.55,.06),.03,.12);
    const double factor=preset==0?1.15:(preset==2?.85:1.0);
    t.scattering*=factor; t.variation*=factor; t.roughnessRatio*=factor;
    t.nearGround=preset==0?.25:.15;
    return t;
}
inline Decision decide(const std::array<Observation,3>& scales,const Thresholds& t,
                       bool ground,int originalCode,double hag,bool multipleReturn) {
    if(ground) return {Ground,0,1};
    if(originalCode==7 || originalCode==18 || originalCode==9 || originalCode==22) return {Excluded,0,2};
    // Missing HAG does not prevent binary geometry-based separation. Only the
    // optional height subclass needs HAG. Negative HAG is a terrain warning.
    if(std::isfinite(hag) && hag < 0) return {Uncertain,0,3};
    int valid=0,volume=0,planes=0,first=-1;
    bool protect=false;
    double score=0;
    for(int i=0;i<3;++i){const auto& o=scales[i];if(!o.valid(t.minimumNeighbors))continue;
        if(first<0)first=i;
        ++valid;
        const bool planar=o.planarity>=t.planar && o.variation<=.035;
        planes+=planar;
        // A fine planar surface must not become vegetation just because the
        // larger sphere contains leaves above it. Trunks/rocks may be retained.
        if(i==first && planar)protect=true;
        int votes=(o.scattering>=t.scattering)+(o.variation>=t.variation)+(o.roughnessRatio>=t.roughnessRatio);
        const bool volumetric=votes>=2 && o.planarity<t.planar;
        volume+=volumetric;
        score+=votes/3.0;
    }
    if(valid<2 || (t.preset==0 && first!=0))return {Uncertain,0,4};
    score=bounded(score/valid+(multipleReturn?.03:0),0,1);
    if(protect)return {planes>=2?OtherSurface:Uncertain,score,5};
    if(hag<t.nearGround)return {Uncertain,score,6};
    // Require fine AND coarser-scale agreement, not a canopy-column mask.
    const auto& fine=scales[first];
    const bool fineVolume=(fine.scattering>=t.scattering || fine.variation>=t.variation) && fine.planarity<t.planar;
    if(volume>=2 && fineVolume)return {Vegetation,score,7};
    if(planes>=2)return {OtherSurface,score,8};
    return {Uncertain,score,9};
}
}}
