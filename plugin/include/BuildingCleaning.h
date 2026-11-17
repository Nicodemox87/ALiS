// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "SemanticGeometry.h"
#include "ScalarFieldManagement.h"
#include <QCryptographicHash>
namespace alis {
inline QString scalarPayloadHash(const CCCoreLib::ScalarField& field){QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray buffer;buffer.reserve(262144);for(unsigned i=0;i<field.currentSize();++i){const float value=field.getValue(i);buffer.append(reinterpret_cast<const char*>(&value),sizeof(value));if(buffer.size()>=262144){hash.addData(buffer);buffer.clear();}}hash.addData(buffer);return QString::fromLatin1(hash.result().toHex());}
struct BuildingCleaningResult {std::vector<std::int16_t> proposals;std::vector<unsigned char> status;QJsonObject provenance;};
inline BuildingCleaningResult cleanBuildingClasses(const ccPointCloud& cloud,const std::vector<bool>& ground,SemanticParameters parameters,bool currentHag,bool aggressive,bool strict,const std::vector<unsigned char>* selection=nullptr,const std::function<void()>& tick={}){
    const int index=cloud.getScalarFieldIndexByName("qAL_ASPRS_Working");checkpointCheck(index>=0,"Working classification is missing; apply reviewed classes first");const auto* working=cloud.getScalarField(index);
    if(strict){parameters.planarFine=std::max(.75,parameters.planarFine);parameters.planarCoarse=std::max(.60,parameters.planarCoarse);parameters.structureSphericity=std::min(.08,parameters.structureSphericity);parameters.coarseSphericity=std::min(.12,parameters.coarseSphericity);parameters.minimumWallSpan=std::max(3.,parameters.minimumWallSpan);parameters.minimumRoofWidth=std::max(3.,parameters.minimumRoofWidth);}
    auto geometry=classifySemanticGeometry(cloud,ground,QSet<int>{6,3,4,5},tick,parameters,selection);
    const CCCoreLib::ScalarField *returns=nullptr,*total=nullptr;for(unsigned i=0;i<cloud.getNumberOfScalarFields();++i){const auto key=scalarCanonicalName(QString::fromUtf8(cloud.getScalarField(i)->getName()));if(key=="returnnumber")returns=cloud.getScalarField(i);if(key=="numberofreturns")total=cloud.getScalarField(i);}
    BuildingCleaningResult result;result.proposals.assign(cloud.size(),-1);result.status.assign(cloud.size(),0);quint64 targets=0,supported=0,canopy=0,nonterminal=0,weak=0,missing=0,changes=0;
    for(unsigned i=0;i<cloud.size();++i){if(tick&&i%32768==0)tick();if(ground[i]||(selection&&!(*selection)[i]))continue;const double before=working->getValue(i);if(before!=6&&before!=128&&before!=129)continue;++targets;
        const int semantic=geometry.labels[i];const int reason=geometry.reasons[i];const double r=returns?returns->getValue(i):0,n=total?total->getValue(i):0;const bool knownNonterminal=std::isfinite(r)&&std::isfinite(n)&&r>=1&&n>=1&&std::floor(r)==r&&std::floor(n)==n&&r<n;
        if(semantic==6){result.status[i]=1;++supported;}
        else if(semantic==3||semantic==4||semantic==5||reason==5){result.status[i]=2;++canopy;result.proposals[i]=currentHag&&(semantic==3||semantic==4||semantic==5)?std::int16_t(semantic):1;}
        else if(knownNonterminal){result.status[i]=3;++nonterminal;result.proposals[i]=1;}
        else if(reason==12){result.status[i]=5;++missing;}
        else {result.status[i]=4;++weak;if(aggressive)result.proposals[i]=1;}
        if(result.proposals[i]>=0)++changes;
    }
    result.provenance={{"algorithm","building_cleaning_review_v1_experimental"},{"targets",double(targets)},{"supported",double(supported)},{"canopy_suspects",double(canopy)},{"known_nonterminal",double(nonterminal)},{"weak_or_disconnected_review",double(weak)},{"missing_geometry_retained",double(missing)},{"proposed_changes",double(changes)},{"strict",strict},{"aggressive",aggressive},{"HAG_current",currentHag},{"parameters",parameters.toJson()},{"working_sha256",scalarPayloadHash(*working)},
        {"status_legend","0 not targeted; 1 coherent structure retained; 2 dispersed canopy suspect; 3 known nonterminal return; 4 unsupported geometry REVIEW ONLY unless aggressive; 5 missing measurements retained"},
        {"policy","Ground and non-building classes unchanged; no point deletion. Canopy -> height vegetation only with current verified HAG, otherwise Other. Weak planes retained by default. These are reviewable geometric hypotheses, not validated accuracy."}};return result;
}
}
