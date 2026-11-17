// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ScalarFieldManagement.h"

namespace alis {
inline bool scalarSearchNeedsStatistics(const QString& field){
    return field=="minimum"||field=="maximum"||field=="mean"||field=="valid"||field=="missing"||field=="missing_percent";
}
inline bool scalarSearchIsText(const QString& field){return field=="name"||field=="status";}
inline bool scalarSearchNumber(QString text,double& value){
    text=text.trimmed();if(text.contains(',')&&!text.contains('.'))text.replace(',','.');
    bool ok=false;value=text.toDouble(&ok);return ok&&std::isfinite(value);
}
inline bool scalarSearchCompare(double actual,const QString& operation,double expected){
    if(!std::isfinite(actual)||!std::isfinite(expected))return false;
    if(operation=="="||operation=="==")return actual==expected;
    if(operation=="!="||operation==QString::fromUtf8("≠"))return actual!=expected;
    if(operation==">")return actual>expected;
    if(operation=="<")return actual<expected;
    if(operation==">="||operation==QString::fromUtf8("≥"))return actual>=expected;
    if(operation=="<="||operation==QString::fromUtf8("≤"))return actual<=expected;
    return false;
}
// A field-level search, not point extraction. Missing statistics never equal zero.
inline bool matchesScalarFieldSearch(const QString& name,const ScalarFieldStats* stats,
 quint64 points,const QString& field,const QString& operation,const QString& query){
    if(query.trimmed().isEmpty())return true;
    if(scalarSearchIsText(field)){
        const QString actual=field=="name"?name:protectedScalarField(name)?QStringLiteral("Protected"):
            !stats?QStringLiteral("Not analysed"):stats->finite==0?QStringLiteral("Empty"):QStringLiteral("Contains values");
        if(operation=="contains")return actual.contains(query,Qt::CaseInsensitive);
        if(operation=="not_contains")return !actual.contains(query,Qt::CaseInsensitive);
        if(operation=="starts")return actual.startsWith(query,Qt::CaseInsensitive);
        if(operation=="ends")return actual.endsWith(query,Qt::CaseInsensitive);
        if(operation=="=")return actual.compare(query,Qt::CaseInsensitive)==0;
        if(operation=="!=")return actual.compare(query,Qt::CaseInsensitive)!=0;
        return false;
    }
    double expected=0;if(!scalarSearchNumber(query,expected))return false;
    if(scalarSearchNeedsStatistics(field)&&!stats)return false;
    double actual=std::numeric_limits<double>::quiet_NaN();
    if(field=="memory")actual=double(points)*sizeof(ScalarType)/1048576.;
    else if(field=="minimum")actual=stats->minimum;
    else if(field=="maximum")actual=stats->maximum;
    else if(field=="mean")actual=stats->mean;
    else if(field=="valid")actual=double(stats->finite);
    else if(field=="missing")actual=double(stats->missing);
    else if(field=="missing_percent")actual=points?100.*double(stats->missing)/double(points):0.;
    return scalarSearchCompare(actual,operation,expected);
}
}
