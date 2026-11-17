// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ProgressiveFeatureStore.h"
#include <QSet>
#include <QRegularExpression>
#include <unordered_map>
#include <queue>
#include <array>
#include <climits>

namespace alis {
// Conservative, reviewable rules, NOT a trained classifier or calibrated probabilities.
// Existing multiscale measurements are reused; only previously applied Ground is locked.
struct SemanticParameters {
    double planarFine=.65,planarCoarse=.45,structureSphericity=.12,coarseSphericity=.20;
    double vegetationSphericity=.18,vegetationPlanarityMax=.60;
    double wallNormalMax=.25,roofNormalMin=.35,lowHeight=2.,mediumHeight=5.;
    double minimumWallHeight=2.,minimumWallSpan=2.,minimumRoofWidth=2.;
    int minimumComponentCells=12,minimumWallCells=3,minimumRoofCells=6;
    QJsonObject toJson() const {return {{"planarFine",planarFine},{"planarCoarse",planarCoarse},{"structureSphericity",structureSphericity},{"coarseSphericity",coarseSphericity},{"vegetationSphericity",vegetationSphericity},{"vegetationPlanarityMax",vegetationPlanarityMax},{"wallNormalMax",wallNormalMax},{"roofNormalMin",roofNormalMin},{"lowHeight",lowHeight},{"mediumHeight",mediumHeight},{"minimumWallHeight",minimumWallHeight},{"minimumWallSpan",minimumWallSpan},{"minimumRoofWidth",minimumRoofWidth},{"minimumComponentCells",minimumComponentCells},{"minimumWallCells",minimumWallCells},{"minimumRoofCells",minimumRoofCells}};}
    void validate() const {for(double v:{planarFine,planarCoarse,structureSphericity,coarseSphericity,vegetationSphericity,vegetationPlanarityMax,wallNormalMax,roofNormalMin})checkpointCheck(std::isfinite(v)&&v>=0&&v<=1,"Geometry thresholds must be in 0..1");checkpointCheck(wallNormalMax<roofNormalMin&&std::isfinite(lowHeight)&&std::isfinite(mediumHeight)&&lowHeight>=.15&&mediumHeight>lowHeight&&minimumComponentCells>=1&&minimumWallCells>=1&&minimumRoofCells>=1,"Invalid class height bands, normal separation or component support");}
};
struct SemanticResult {
    std::vector<std::int16_t> labels;
    std::vector<float> scores;
    QJsonObject provenance;
    std::vector<unsigned char> reasons;
};
struct SemanticCell {
    int x,y,z;
    bool operator==(const SemanticCell& b) const { return x==b.x&&y==b.y&&z==b.z; }
};
struct SemanticCellHash { size_t operator()(const SemanticCell& c) const {
    return size_t(c.x)*73856093u ^ size_t(c.y)*19349663u ^ size_t(c.z)*83492791u;
}};
struct SemanticPatch { unsigned wall=0,roof=0; bool visited=false,accepted=false; };
struct SemanticLinearPatch { bool visited=false,accepted=false; double low=1e30,high=-1e30; };
inline SemanticResult classifySemanticGeometry(const ccPointCloud& cloud,
 const std::vector<bool>& ground,const QSet<int>& requested,const std::function<void()>& tick={},const SemanticParameters& parameters=SemanticParameters(),const std::vector<unsigned char>* selection=nullptr) {
    parameters.validate();for(double v:{parameters.minimumWallHeight,parameters.minimumWallSpan,parameters.minimumRoofWidth})checkpointCheck(std::isfinite(v)&&v>=0,"Structure dimensions must be finite and non-negative");checkpointCheck(!selection||selection->size()==cloud.size(),"Classification selection is not point-aligned");
    checkpointCheck(std::isfinite(cloud.getGlobalScale())&&cloud.getGlobalScale()>0,"Invalid coordinate scale");
    checkpointCheck(ground.size()==cloud.size(),"Apply Ground before semantic classification");
    const auto mapping=QJsonDocument::fromJson(cloud.getMetaData("ALiS.featureFields").toByteArray()).object();
    auto field=[&](const QString& key)->const CCCoreLib::ScalarField* {
        auto usable=[&](int index)->const CCCoreLib::ScalarField*{if(index<0)return nullptr;const auto* sf=cloud.getScalarField(index);if(cloud.hasMetaData("ALiS.partialFeature."+QString::fromUtf8(sf->getName())))return nullptr;if(sf->currentSize()!=cloud.size())return nullptr;for(unsigned i=0;i<cloud.size();++i){if(std::isfinite(sf->getValue(i)))return sf;if(tick&&i%65536==0)tick();}return nullptr;};
        const int index=cloud.getScalarFieldIndexByName(mapping.value(key).toString().toUtf8().constData());
        if(const auto* sf=usable(index))return sf;
        // BIN/LAS imports can lose the identity map while retaining the real SFs.
        // Read existing names only; never allocate/recompute fields during resolution.
        const QString descriptor=key.section('@',0,0),radius=key.section('@',1);bool ok=false;const double wanted=radius.toDouble(&ok);if(!ok)return nullptr;
        auto canonical=[](QString value){return value.toLower().remove(' ').remove('_');};
        for(int i=int(cloud.getNumberOfScalarFields())-1;i>=0;--i){const QString name=QString::fromUtf8(cloud.getScalarField(i)->getName());double actual=-1;
            if(name.startsWith("ALiS "+descriptor+"@",Qt::CaseInsensitive))actual=name.section('@',1).section(' ',0,0).toDouble(&ok);
            else {const auto match=QRegularExpression("^qAL_(.+)_R([0-9.eE+-]+)m(?:_.*)?$",QRegularExpression::CaseInsensitiveOption).match(name);if(!match.hasMatch()||canonical(match.captured(1))!=canonical(descriptor))continue;actual=match.captured(2).toDouble(&ok);}
            if(ok&&std::abs(actual-wanted)<1e-9)if(const auto* sf=usable(i))return sf;}return nullptr;
    };
    // Two different physical scales are mandatory. Never silently interpret cluster IDs as classes.
    const auto p2=field("planarity@2"),s2=field("sphericity@2"),n2=field("normal_z_absolute@2");
    const auto p4=field("planarity@4"),s4=field("sphericity@4");
    QStringList missing;if(!p2)missing<<"planarity@2";if(!p4)missing<<"planarity@4";if(!s2)missing<<"sphericity@2";if(!s4)missing<<"sphericity@4";if(!n2)missing<<"normal_z_absolute@2";
    if(!missing.isEmpty())throw std::runtime_error(QString("Required fields missing or entirely empty: %1. Names alone do not mean computed values. Recover saved blocks, or explicitly calculate these scales in Prepare > Features. No feature calculation or new scalar allocation was started.").arg(missing.join(", ")).toStdString());
    auto named=[&](std::initializer_list<const char*> names)->const CCCoreLib::ScalarField* {
        auto normalize=[](QString value){return value.toLower().remove(' ').remove('_');};
        for(auto name:names)for(unsigned i=0;i<cloud.getNumberOfScalarFields();++i)
            if(normalize(QString::fromUtf8(cloud.getScalarField(i)->getName()))==normalize(QString::fromLatin1(name)))return cloud.getScalarField(i);
        return nullptr;
    };
    const auto returns=named({"Return Number","ReturnNumber","Return number"});
    const auto totals=named({"Number of Returns","NumberOfReturns","Number of returns"});
    const auto hag=named({"qAL_HAG"});
    const bool linearRequested=requested.contains(130)||requested.contains(131);
    const auto l2=field("linearity@2"),l4=field("linearity@4"),z2=field("z_range@2"),z4=field("z_range@4");
    checkpointCheck(!linearRequested||(l2&&l4&&z2&&z4&&hag),"Poles/wires require HAG and linearity/z_range at 2 and 4 m");
    auto lastReturn=[&](unsigned i){if(!returns||!totals)return false;const double r=returns->getValue(i),n=totals->getValue(i);return std::isfinite(r)&&std::isfinite(n)&&n>=1&&r>=1&&r==std::floor(r)&&n==std::floor(n)&&r==n;};
    // One-metre physical cells, independent from CloudCompare Global Scale.
    auto cell=[&](unsigned i){const auto& p=*cloud.getPoint(i);const double scale=cloud.getGlobalScale();return SemanticCell{int(std::floor(p.x/scale)),int(std::floor(p.y/scale)),int(std::floor(p.z/scale))};};
    std::unordered_map<SemanticCell,SemanticPatch,SemanticCellHash> patches;
    quint64 groundRejectedReturns=0;
    for(unsigned i=0;i<cloud.size();++i){
        if(ground[i]){if(!lastReturn(i))++groundRejectedReturns;continue;}
        if(!lastReturn(i))continue;
        const double p=p2->getValue(i),s=s2->getValue(i),normal=n2->getValue(i),large=p4->getValue(i);
        if(!(p>=parameters.planarFine&&s<=parameters.structureSphericity&&large>=parameters.planarCoarse&&s4->getValue(i)<=parameters.coarseSphericity&&normal>=0&&normal<=1))continue;
        // Connected roof+wall evidence is required. A local plane or a vertical stem alone is insufficient.
        if(normal<=parameters.wallNormalMax)++patches[cell(i)].wall;
        else if(normal>=parameters.roofNormalMin)++patches[cell(i)].roof;
        checkpointCheck(patches.size()<=2000000,"Geometry patch budget exceeded; run on spatial tiles rather than exhausting RAM");
        if(tick&&i%32768==0)tick();
    }
    for(auto& entry:patches){
        if(entry.second.visited)continue;
        std::vector<SemanticCell> component;std::queue<SemanticCell> todo;todo.push(entry.first);entry.second.visited=true;
        quint64 walls=0,roofs=0;
        SemanticCell wallLow{INT_MAX,INT_MAX,INT_MAX},wallHigh{INT_MIN,INT_MIN,INT_MIN},roofLow=wallLow,roofHigh=wallHigh;
        auto bounds=[](const SemanticCell& k,SemanticCell& low,SemanticCell& high){low.x=std::min(low.x,k.x);low.y=std::min(low.y,k.y);low.z=std::min(low.z,k.z);high.x=std::max(high.x,k.x);high.y=std::max(high.y,k.y);high.z=std::max(high.z,k.z);};
        while(!todo.empty()){
            const auto key=todo.front();todo.pop();component.push_back(key);auto& patch=patches.at(key);
            walls+=patch.wall>=3;roofs+=patch.roof>=3;
            if(patch.wall>=3)bounds(key,wallLow,wallHigh);if(patch.roof>=3)bounds(key,roofLow,roofHigh);
            for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
                auto it=patches.find({key.x+x,key.y+y,key.z+z});if(it!=patches.end()&&!it->second.visited){it->second.visited=true;todo.push(it->first);}
            }
            if(tick&&component.size()%1024==0)tick();
        }
        // A trunk or random leaf planes must not satisfy a wall+roof component merely by count.
        const bool supported=walls>=unsigned(parameters.minimumWallCells)&&roofs>=unsigned(parameters.minimumRoofCells);
        const bool accepted=supported&&component.size()>=unsigned(parameters.minimumComponentCells)&&
            double(wallHigh.z)-wallLow.z>=parameters.minimumWallHeight&&std::max(double(wallHigh.x)-wallLow.x,double(wallHigh.y)-wallLow.y)>=parameters.minimumWallSpan&&
            std::min(double(roofHigh.x)-roofLow.x,double(roofHigh.y)-roofLow.y)>=parameters.minimumRoofWidth;
        for(const auto& key:component)patches.at(key).accepted=accepted;
    }
    // Linear objects need continuity and context; linearity alone also describes tree branches.
    std::unordered_map<SemanticCell,SemanticLinearPatch,SemanticCellHash> wires,poles;
    if(linearRequested){
        for(unsigned i=0;i<cloud.size();++i){
            if(tick&&i%32768==0)tick();if(ground[i])continue;
            const double h=hag->getValue(i);
            if(!(std::isfinite(h)&&h>=.15&&l2->getValue(i)>=.85&&l4->getValue(i)>=.75&&s2->getValue(i)<=.08&&s4->getValue(i)<=.10&&p2->getValue(i)<.25))continue;
            if(h>=2&&z2->getValue(i)>=0&&z2->getValue(i)<=1&&z4->getValue(i)>=0&&z4->getValue(i)<=2){auto& patch=wires[cell(i)];patch.low=std::min(patch.low,h);patch.high=std::max(patch.high,h);}
            else if(z2->getValue(i)>=2.5&&z4->getValue(i)>=3){auto& patch=poles[cell(i)];patch.low=std::min(patch.low,h);patch.high=std::max(patch.high,h);}
            checkpointCheck(wires.size()+poles.size()<=2000000,"Linear-object patch budget exceeded; use spatial tiles");
        }
        auto components=[&](auto& cells,bool wire){for(auto& entry:cells){
            if(entry.second.visited)continue;std::queue<SemanticCell> todo;std::vector<SemanticCell> component;todo.push(entry.first);entry.second.visited=true;
            auto low=entry.first,high=entry.first;double minHeight=1e30,maxHeight=-1e30;
            while(!todo.empty()){
                const auto key=todo.front();todo.pop();component.push_back(key);const auto& patch=cells.at(key);minHeight=std::min(minHeight,patch.low);maxHeight=std::max(maxHeight,patch.high);
                low.x=std::min(low.x,key.x);low.y=std::min(low.y,key.y);low.z=std::min(low.z,key.z);high.x=std::max(high.x,key.x);high.y=std::max(high.y,key.y);high.z=std::max(high.z,key.z);
                for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){auto it=cells.find({key.x+x,key.y+y,key.z+z});if(it!=cells.end()&&!it->second.visited){it->second.visited=true;todo.push(it->first);}}
                if(tick&&component.size()%1024==0)tick();
            }
            const int major=std::max(high.x-low.x,high.y-low.y),minor=std::min(high.x-low.x,high.y-low.y);
            bool accepted=wire?(component.size()>=12&&major>=12&&minor<=std::max(4.,major*.2)&&high.z-low.z<=major*.25+1):
                (component.size()>=4&&high.z-low.z>=3&&high.x-low.x<=1&&high.y-low.y<=1&&minHeight<=.75&&maxHeight>=3);
            if(!wire&&accepted){bool connectedCable=false;for(const auto& key:component)if(key.z>=high.z-1)for(int x=-2;x<=2;++x)for(int y=-2;y<=2;++y)for(int z=-2;z<=2;++z){const auto it=wires.find({key.x+x,key.y+y,key.z+z});if(it!=wires.end()&&it->second.accepted)connectedCable=true;}accepted=connectedCable;}
            for(const auto& key:component)cells.at(key).accepted=accepted;
        }};components(wires,true);components(poles,false);
    }
    SemanticResult result;result.labels.assign(cloud.size(),1);result.scores.assign(cloud.size(),0);
    result.reasons.assign(cloud.size(),0);QJsonObject counts,reasonCounts;quint64 missingHeight=0,unknownReturns=0;
    for(unsigned i=0;i<cloud.size();++i){
        if(selection&&!(*selection)[i]){result.labels[i]=-1;continue;}
        int label=1;float score=0;unsigned char reason=8;
        if(ground[i]){label=2;score=1;reason=1;} // Reuse EXACT Ground, even if return metadata is missing/nonterminal.
        else {
            const auto patch=patches.find(cell(i));
            if(lastReturn(i)&&patch!=patches.end()&&patch->second.accepted&&p2->getValue(i)>=parameters.planarFine&&s2->getValue(i)<=parameters.structureSphericity&&p4->getValue(i)>=parameters.planarCoarse&&s4->getValue(i)<=parameters.coarseSphericity){
                const int surface=n2->getValue(i)<=parameters.wallNormalMax?129:n2->getValue(i)>=parameters.roofNormalMin?128:1;
                if(requested.contains(surface)){label=surface;score=.8f;reason=surface==129?3:2;}else if(surface!=1&&requested.contains(6)){label=6;score=.8f;reason=surface==129?3:2;}else reason=9;
            }
            if(label==1&&linearRequested&&l2->getValue(i)>=.85&&l4->getValue(i)>=.75&&s2->getValue(i)<=.08&&s4->getValue(i)<=.10&&p2->getValue(i)<.25){const auto wire=wires.find(cell(i)),pole=poles.find(cell(i));const double h=hag->getValue(i);if(requested.contains(131)&&std::isfinite(h)&&h>=2&&z2->getValue(i)>=0&&z2->getValue(i)<=1&&z4->getValue(i)>=0&&z4->getValue(i)<=2&&wire!=wires.end()&&wire->second.accepted){label=131;score=.75f;}else if(requested.contains(130)&&std::isfinite(h)&&h>=.15&&z2->getValue(i)>=2.5&&z4->getValue(i)>=3&&pole!=poles.end()&&pole->second.accepted){label=130;score=.75f;}}
            // Spatially dispersed at BOTH scales, not just absence of a plane. Linear trunks/cables remain Other.
            if(label==1&&s2->getValue(i)>=parameters.vegetationSphericity&&s4->getValue(i)>=parameters.vegetationSphericity&&p2->getValue(i)<parameters.vegetationPlanarityMax&&p4->getValue(i)<parameters.vegetationPlanarityMax){
                const double h=hag?hag->getValue(i):std::numeric_limits<double>::quiet_NaN();
                if(std::isfinite(h)&&h>=.15){int vegetation=h<=parameters.lowHeight?3:h<=parameters.mediumHeight?4:5;if(requested.contains(vegetation)){label=vegetation;score=.7f;reason=4;}else reason=9;}
                else {++missingHeight;reason=5;}
            }
            if(label==131)reason=6;else if(label==130)reason=7;
            if(label==1&&reason==8&&p2->getValue(i)>=parameters.planarFine)reason=!lastReturn(i)?10:11;
            if(!lastReturn(i))++unknownReturns;
            if(!std::isfinite(p2->getValue(i))||!std::isfinite(p4->getValue(i))||!std::isfinite(s2->getValue(i))||!std::isfinite(s4->getValue(i))||(!std::isfinite(n2->getValue(i))&&p2->getValue(i)>=parameters.planarFine)){label=1;score=0;reason=12;}
        }
        result.labels[i]=std::int16_t(label);result.scores[i]=score;
        result.reasons[i]=reason;const auto reasonKey=QString::number(reason);reasonCounts[reasonKey]=reasonCounts.value(reasonKey).toDouble()+1;
        const auto key=QString::number(label);counts[key]=counts.value(key).toDouble()+1;
        if(tick&&i%32768==0)tick();
    }
    QJsonArray classes;for(int c:requested)classes.append(c);
    result.provenance={{"algorithm","geometry_semantic_v3_reviewable"},{"requested_classes",classes},{"parameters",parameters.toJson()},{"ground_locked",true},{"reason_counts",reasonCounts},
        {"reason_codes","0 outside working selection; 1 exact applied Ground; 2 connected roof; 3 connected wall; 4 two-scale dispersed vegetation; 5 vegetation missing/invalid HAG; 6 continuous wire; 7 cable-supported pole; 8 ambiguous/insufficient geometry; 9 class not requested; 10 planar but missing/nonterminal return; 11 plane without coherent wall+roof support; 12 missing per-point geometry"},
        {"counts",counts},{"ground_reused",true},{"ground_nonterminal_or_missing_returns",double(groundRejectedReturns)},
        {"non_ground_nonterminal_or_unknown_returns",double(unknownReturns)},{"vegetation_without_valid_hag",double(missingHeight)},
        {"score_calibrated",false},{"manual_labels_used",false},{"feature_scales_m",QJsonArray{2,4}},
        {"building_rule","valid single/last returns, two-scale planar connected wall+roof components"},
        {"custom_class_mapping",QJsonObject{{"128","Roof"},{"129","Vertical wall"},{"130","Utility pole"},{"131","Wire / cable (type not inferred)"}}},
        {"wire_rule","two-scale linear, elevated, continuous span >=12 m; bounded transverse / vertical spread"},
        {"pole_rule","slender vertical continuity >=3 m, near-ground support and accepted cable connection at upper end"},
        {"vegetation_heights_m",QString("0.15..%1 low; >%1..%2 medium; >%2 high; missing height Other").arg(parameters.lowHeight).arg(parameters.mediumHeight)},
        {"limits","Experimental rules; no accuracy claimed. Isolated poles, trunks, short/disconnected cables, towers and ambiguous branches may remain Other; roofs without observed wall support may abstain. Geometry cannot infer electrical function of a cable. Requires visual validation; custom codes128..131 need LAS1.4 point-format6+ or BIN."}};
    return result;
}
}
