// SPDX-License-Identifier: GPL-2.0-or-later
#include "CloudProfile.h"
#include "SessionModel.h"
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QJsonArray>
#include <QRegularExpression>
#include <array>
#include <cmath>
namespace alis {
namespace {
QString normalized(QString s){return s.toLower().remove(QRegularExpression("[^a-z0-9]"));}
const ccScalarField* find(ccPointCloud& c,const QStringList& aliases){
    for(const auto& alias:aliases) for(unsigned i=0;i<c.getNumberOfScalarFields();++i) if(alias==normalized(QString::fromUtf8(c.getScalarFieldName(i)))) return static_cast<ccScalarField*>(c.getScalarField(i)); return nullptr;
}
int code(const ccScalarField* s,unsigned i,int maximum){
    if(!s || i>=s->currentSize()) return -1; double v=s->getValue(i);
    return std::isfinite(v)&&v>=0&&v<=maximum&&v==std::floor(v)?int(v):-1;
}
template<std::size_t N> QJsonObject histogram(const std::array<quint64,N>& a){QJsonObject j;for(std::size_t i=0;i<N;++i)if(a[i])j.insert(QString::number(i),double(a[i]));return j;}
QString counts(const QJsonObject& h,bool classes=false){QStringList a;for(auto i=h.begin();i!=h.end();++i){QString label=i.key();if(classes){auto* entry=findAsprsClass(i.key().toUInt());if(entry)label+=QStringLiteral(" ")+QString::fromLatin1(entry->name);}a<<QStringLiteral("%1: %2").arg(label).arg(i.value().toDouble(),0,'f',0);}return a.isEmpty()?QStringLiteral("not available"):a.join(QStringLiteral("; "));}
}
QJsonObject cloudProfile(ccPointCloud& c,bool metric,const SpacingSummary* spacing){
    QJsonObject p{{"schema","qal-cloud-profile/1.0"},{"name",c.getName()},{"points",double(c.size())},{"metricConfirmed",metric},{"globalScale",c.getGlobalScale()}};
    QJsonArray scalarFields;
    for(unsigned i=0;i<c.getNumberOfScalarFields();++i) scalarFields.append(QString::fromUtf8(c.getScalarFieldName(i)));
    p.insert("scalarFields",scalarFields);
    p.insert("hasColors",c.hasColors());
    auto box=c.getOwnBB(); if(box.isValid() && c.getGlobalScale()>0){double x=(double(box.maxCorner().x)-box.minCorner().x)/c.getGlobalScale(),y=(double(box.maxCorner().y)-box.minCorner().y)/c.getGlobalScale();p.insert("extentX",x);p.insert("extentY",y);p.insert("bboxArea",x*y);if(x*y>0 && c.size()>0){p.insert("bboxDensity",c.size()/(x*y));p.insert("nominalSpacing",std::sqrt(x*y/c.size()));}}
    const auto* original=find(c,{"qalasprsoriginal","classification"});
    const auto* working=find(c,{"qalasprsworking"});
    const auto* rn=find(c,{"returnnumber","returnindex"});
    const auto* nr=find(c,{"numberofreturns","numberofreturn","numberofechoes","numberofechos"});
    std::array<quint64,256> originalCounts{},workingCounts{};std::array<quint64,16> rnCounts{},nrCounts{};
    quint64 originalInvalid=0,valid=0,last=0,multiple=0,invalidRn=0,invalidNr=0;
    for(unsigned i=0;i<c.size();++i){
        int a=code(original,i,255),b=code(working,i,255),r=code(rn,i,15),n=code(nr,i,15);
        if(a>=0)++originalCounts[a];else ++originalInvalid;if(b>=0)++workingCounts[b];
        if(r>=1)++rnCounts[r];else ++invalidRn;if(n>=1)++nrCounts[n];else ++invalidNr;
        if(r>=1&&n>=r){++valid;if(r==n)++last;if(n>1)++multiple;}
    }
    p.insert("originalClasses",histogram(originalCounts));p.insert("workingClasses",histogram(workingCounts));p.insert("invalidOriginalClass",double(originalInvalid));
    p.insert("returnNumberHistogram",histogram(rnCounts));p.insert("numberOfReturnsHistogram",histogram(nrCounts));
    p.insert("returnNumberField",rn?QString::fromUtf8(rn->getName()):QString());p.insert("numberOfReturnsField",nr?QString::fromUtf8(nr->getName()):QString());
    p.insert("invalidReturnNumber",double(invalidRn));p.insert("invalidNumberOfReturns",double(invalidNr));
    p.insert("validReturnFraction",c.size()?double(valid)/c.size():0);p.insert("lastReturnFraction",valid?double(last)/valid:0);p.insert("multipleReturnFraction",valid?double(multiple)/valid:0);
    if(spacing && spacing->valid){p.insert("nnSamples",double(spacing->sampledPoints));p.insert("nnMean",spacing->mean);p.insert("nnMedian",spacing->median);p.insert("nnMinimum",spacing->minimum);p.insert("nnP10",spacing->percentile10);p.insert("nnP90",spacing->percentile90);p.insert("nnDuplicates",double(spacing->duplicateSamples));p.insert("nnDensity2D",spacing->estimatedDensity2D);}
    p.insert("projection",c.getMetaData(QStringLiteral("LAS.projection")).toString());
    return p;
}
QString cloudProfileText(const QJsonObject& p){
    const bool metric=p.value("metricConfirmed").toBool();const QString unit=metric?QStringLiteral("m"):QStringLiteral("source units (not confirmed)");
    QString text=QStringLiteral("%1 points | XY extent %2 x %3 %4\nBBox density: %5 points/%7; nominal spacing: %6 %4 (not uniform ground density).\n")
        .arg(p.value("points").toDouble(),0,'f',0).arg(p.value("extentX").toDouble(),0,'g',7).arg(p.value("extentY").toDouble(),0,'g',7).arg(unit).arg(p.value("bboxDensity").toDouble(),0,'g',7).arg(p.value("nominalSpacing").toDouble(),0,'g',5).arg(metric?QStringLiteral("m²"):QStringLiteral("source units²"));
    if(p.contains("nnMedian"))text+=QStringLiteral("Nearest neighbour, sampled %1: mean %2; median %3; minimum positive %4; P10/P90 %5 / %6 %7. Duplicate samples: %8.\n")
        .arg(p.value("nnSamples").toDouble(),0,'f',0).arg(p.value("nnMean").toDouble(),0,'g',5).arg(p.value("nnMedian").toDouble(),0,'g',5).arg(p.value("nnMinimum").toDouble(),0,'g',5).arg(p.value("nnP10").toDouble(),0,'g',5).arg(p.value("nnP90").toDouble(),0,'g',5).arg(unit).arg(p.value("nnDuplicates").toDouble(),0,'f',0);
    else text+=QStringLiteral("Nearest-neighbour distance: press Analyse spacing (sampled queries against the full cloud, no decimation).\n");
    text+=QStringLiteral("Return number: %1\nEchoes per pulse (Number of Returns): %2\nValid paired return metadata: %3%; last returns among valid: %4%.\nOriginal/imported classes: %5\nWorking classes: %6\nCRS: %7")
        .arg(counts(p.value("returnNumberHistogram").toObject()),counts(p.value("numberOfReturnsHistogram").toObject()))
        .arg(100*p.value("validReturnFraction").toDouble(),0,'f',1).arg(100*p.value("lastReturnFraction").toDouble(),0,'f',1)
        .arg(counts(p.value("originalClasses").toObject(),true),counts(p.value("workingClasses").toObject(),true),p.value("projection").toString().isEmpty()?QStringLiteral("not embedded/exposed; global coordinates preserved, CRS identity unknown"):QStringLiteral("projection metadata present"));
	if(p.value("validReturnFraction").toDouble()<.9)text+=QStringLiteral("\nWARNING: missing/invalid returns are excluded from adaptive suggestions. Last return is not Ground.");
	if(p.value("statisticsStale").toBool())text+=QStringLiteral("\nNOTICE: cached class statistics need refresh (%1). Press Analyse / refresh when you need updated values.").arg(p.value("staleReason").toString(QStringLiteral("relevant cloud attributes changed")));
	else text+=QStringLiteral("\nStatistics are cached for this cloud. Selecting it again does not recalculate them; Analyse / refresh forces a new scan.");
	return text;
}
}
