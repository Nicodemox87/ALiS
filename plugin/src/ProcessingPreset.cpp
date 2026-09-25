// SPDX-License-Identifier: GPL-2.0-or-later
#include "ProcessingPreset.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>
namespace alis {
namespace {
QJsonArray numbers(const QVector<double>& values) { QJsonArray a; for(double v:values) a.append(v); return a; }
bool number(const QJsonObject& o, const char* key, double& v, double lo, double hi) {
    auto j=o.value(QLatin1String(key)); v=j.toDouble(std::numeric_limits<double>::quiet_NaN());
    return j.isDouble() && std::isfinite(v) && v>=lo && v<=hi;
}
bool sequence(const QJsonValue& value,QVector<double>& out,int limit,bool increasing) {
    if(!value.isArray() || value.toArray().isEmpty() || value.toArray().size()>limit) return false;
    out.clear(); for(auto j:value.toArray()) { double v=j.toDouble(-1); if(!j.isDouble() || !std::isfinite(v) || v<=0 || v>10000 || (increasing && !out.isEmpty() && v<=out.back())) return false; out<<v; } return true;
}
}
QJsonObject processingPresetToJson(const ProcessingPreset& p) {
    QJsonObject g{{"resolution",p.base.clothResolution},{"threshold",p.base.classificationThreshold},
        {"rigidity",p.base.rigidness},{"iterations",p.base.iterations},{"timeStep",p.base.timeStep},{"slope",p.base.slopeProcessing}};
    return {{"schema","qal-processing-preset/1.0"},{"name",p.name},{"algorithm",p.algorithm},{"groundBase",g},
        {"controls",QJsonArray{p.complexity,p.microrelief,p.vegetation}},
        {"pmf",QJsonObject{{"windows",numbers(p.windows)},{"thresholds",numbers(p.thresholds)},{"cellSize",p.cellSize}}},
        {"features",QJsonArray::fromStringList(p.features)},{"radiiMetres",numbers(p.radii)},{"dtmStep",p.dtmStep},
        {"scalePolicy",QJsonObject{{"minimumRadius",p.minimumRadius},{"maximumRadius",p.maximumRadius},{"useValidReturns",p.useReturns},{"targetNeighbours",QJsonArray{16,64,256}}}},
        {"sourceProfile",p.sourceProfile},{"note","Data-only preset. Ground is one cloth/window sequence per run; multiscale refers to feature radii. No automatic mask union. Imported settings are restored exactly, not silently adapted."}};
}
bool processingPresetFromJson(const QJsonObject& j,ProcessingPreset& output,QString& error) {
    ProcessingPreset p; auto fail=[&](const char* why){error=QString::fromLatin1(why);return false;};
    if(j.value("schema").toString()!="qal-processing-preset/1.0") return fail("Unsupported preset schema");
    p.name=j.value("name").toString().trimmed(); if(p.name.isEmpty() || p.name.size()>120) return fail("Preset name must contain 1..120 characters");
    p.algorithm=j.value("algorithm").toString(); if(p.algorithm!="csf.cloudcompare.v2.13.2" && p.algorithm!="pmf.lidr_zhang.fast_raster.v1") return fail("Unknown ground algorithm");
    auto g=j.value("groundBase").toObject(); double rigid=0,iter=0;
    if(!number(g,"resolution",p.base.clothResolution,.001,1000) || !number(g,"threshold",p.base.classificationThreshold,.001,1000)
        || !number(g,"timeStep",p.base.timeStep,.001,10) || !number(g,"rigidity",rigid,1,3) || rigid!=std::floor(rigid)
        || !number(g,"iterations",iter,1,100000) || iter!=std::floor(iter) || !g.value("slope").isBool()) return fail("Invalid CSF parameters");
    p.base.rigidness=int(rigid); p.base.iterations=int(iter); p.base.slopeProcessing=g.value("slope").toBool();
    auto controls=j.value("controls").toArray(); if(controls.size()!=3) return fail("Expected three context controls");
    for(auto v:controls) if(!v.isDouble() || v.toDouble()<0 || v.toDouble()>2 || std::floor(v.toDouble())!=v.toDouble()) return fail("Context levels must be integers 0..2");
    p.complexity=controls[0].toInt();p.microrelief=controls[1].toInt();p.vegetation=controls[2].toInt();
    auto pmf=j.value("pmf").toObject();
    if(!sequence(pmf.value("windows"),p.windows,64,true) || !sequence(pmf.value("thresholds"),p.thresholds,64,false) || p.windows.size()!=p.thresholds.size()
        || !number(pmf,"cellSize",p.cellSize,0,1000)) return fail("Invalid PMF window/threshold/cell sequence");
    if(!sequence(j.value("radiiMetres"),p.radii,32,true) || !number(j,"dtmStep",p.dtmStep,.001,1000)) return fail("Invalid feature radii or DTM step");
    if(!j.value("features").isArray() || j.value("features").toArray().size()>128) return fail("Invalid feature list");
    for(auto f:j.value("features").toArray()) { if(!f.isString() || f.toString().isEmpty() || f.toString().size()>100 || p.features.contains(f.toString())) return fail("Invalid or repeated feature ID"); p.features<<f.toString(); }
    auto policy=j.value("scalePolicy").toObject();
    if(!number(policy,"minimumRadius",p.minimumRadius,.001,1000) || !number(policy,"maximumRadius",p.maximumRadius,p.minimumRadius,1000) || !policy.value("useValidReturns").isBool()) return fail("Invalid adaptive scale bounds");
    if(policy.value("targetNeighbours").toArray()!=QJsonArray{16,64,256}) return fail("Unsupported neighbour targets (expected 16,64,256)");
    p.useReturns=policy.value("useValidReturns").toBool(); p.sourceProfile=j.value("sourceProfile").toObject();
    output=p;error.clear();return true;
}
bool suggestProcessingPreset(ProcessingPreset& p,const QJsonObject& profile,QString& explanation) {
    if(!profile.value("metricConfirmed").toBool()) {explanation="Confirm metric units before adapting a preset.";return false;}
    double spacing=profile.value("nnMedian").toDouble(); double density=profile.value("nnDensity2D").toDouble();
    if(!(spacing>0) || !(density>0) || !std::isfinite(density)) {explanation="Measure nearest-neighbour spacing first.";return false;}
    double fraction=1.; bool returnsUsed=false;
    if(p.useReturns && profile.value("validReturnFraction").toDouble()>=.9) {
        fraction=std::max(.25,std::min(1.,profile.value("lastReturnFraction").toDouble(1.)));
        returnsUsed=true;
    }
    const double supportDensity=density*fraction;
    const double extent=std::min(profile.value("extentX").toDouble(),profile.value("extentY").toDouble());
    const double upper=std::min(p.maximumRadius,extent/4.);
    if(upper<p.minimumRadius) {explanation="Cloud extent is too small for the preset radius bounds.";return false;}
    p.radii.clear();
    for(double neighbours:{16.,64.,256.}) {
        double r=std::max(p.minimumRadius,std::min(upper,std::sqrt(neighbours/(3.141592653589793*supportDensity))));
        r=std::max(p.minimumRadius,std::min(upper,std::round(r*1000)/1000));
        if(p.radii.isEmpty() || r>p.radii.back()) p.radii<<r;
    }
    // Only raise the cloth's support floor on sparse data; never derive Z tolerance from density.
    p.base.clothResolution=std::max(p.base.clothResolution,std::min(8.,4.*spacing));
    p.sourceProfile=profile;
    explanation=QStringLiteral("Heuristic: r=sqrt(k/(pi*rho)), k=16/64/256; rho=%1, support factor=%2; bounds %3..%4 m. %5 Threshold/rigidity/slope unchanged. Total/last-return density is NOT ground density. Review the preview; this is not a calibrated classifier.")
        .arg(density,0,'g',5).arg(fraction,0,'g',4).arg(p.minimumRadius).arg(upper)
        .arg(returnsUsed?QStringLiteral("Valid return metadata used as a bounded support heuristic."):QStringLiteral("Return metadata absent/unreliable: ignored."));
    return true;
}
}
