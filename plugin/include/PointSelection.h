// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ScalarFieldSearch.h"
#include <QJsonArray>
#include <QRegularExpression>

namespace alis {
// AND across rows, OR within a class list. Values are source SF values, never colours.
inline bool parsePointSelectionRule(const QString& field,const QString& op,const QString& input,QJsonObject& rule,QString& error){
    rule={{"field",field},{"op",op},{"text",input}};
    if(field.isEmpty()){error="Choose an existing scalar field.";return false;}
    if(op=="finite"||op=="missing")return true;
    if(op=="codes"){
        QJsonArray codes;for(const auto& token:input.split(QRegularExpression("[,;\\s]+"),Qt::SkipEmptyParts)){
            double value;if(!scalarSearchNumber(token,value)||value<0||value>255||std::floor(value)!=value){error="Class codes must be integers in 0..255, separated by commas.";return false;}codes.append(value);
        }if(codes.isEmpty()){error="Choose at least one class code.";return false;}rule["codes"]=codes;return true;
    }
    if(op=="range"){
        const auto parts=input.split(';');double low,high;
        if(parts.size()!=2||!scalarSearchNumber(parts[0],low)||!scalarSearchNumber(parts[1],high)||low>high){error="Inclusive interval: enter minimum;maximum, for example 0.15;2. Decimal commas are allowed.";return false;}
        rule["min"]=low;rule["max"]=high;return true;
    }
    if(!QStringList{"=","!=",">",">=","<","<="}.contains(op)){error="Unsupported point filter operator.";return false;}
    double value;if(!scalarSearchNumber(input,value)){error="Enter a finite number.";return false;}rule["value"]=value;return true;
}
inline bool matchesPointSelection(double value,const QJsonObject& rule){
    const auto op=rule.value("op").toString();if(op=="missing")return !std::isfinite(value);if(!std::isfinite(value))return false;if(op=="finite")return true;
    if(op=="codes"){for(const auto& code:rule.value("codes").toArray())if(value==code.toDouble())return true;return false;}
    if(op=="range")return value>=rule.value("min").toDouble()&&value<=rule.value("max").toDouble();
    return scalarSearchCompare(value,op,rule.value("value").toDouble());
}
inline std::vector<unsigned char> pointSelectionMask(const ccPointCloud& cloud,const QJsonArray& rules,const std::function<void()>& tick={}){
    checkpointCheck(!rules.isEmpty(),"Add at least one point filter rule");
    std::vector<std::pair<const CCCoreLib::ScalarField*,QJsonObject>> resolved;
    for(const auto& value:rules){auto rule=value.toObject();QJsonObject checked;QString error;checkpointCheck(parsePointSelectionRule(rule.value("field").toString(),rule.value("op").toString(),rule.value("text").toString(),checked,error),error.toUtf8().constData());
        int index=cloud.getScalarFieldIndexByName(checked.value("field").toString().toUtf8());checkpointCheck(index>=0,"A filter field is no longer present; rebuild the selection");auto* sf=cloud.getScalarField(index);checkpointCheck(sf->currentSize()==cloud.size(),"Filter field is not point-aligned");resolved.emplace_back(sf,checked);
    }
    std::vector<unsigned char> mask(cloud.size(),1);for(unsigned i=0;i<cloud.size();++i){for(const auto& rule:resolved)if(!matchesPointSelection(rule.first->getValue(i),rule.second)){mask[i]=0;break;}if(tick&&i%65536==0)tick();}return mask;
}
}
