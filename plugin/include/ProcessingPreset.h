// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "WorkspaceLogic.h"
#include <QJsonObject>
#include <QStringList>
#include <QVector>
namespace alis {
struct ProcessingPreset {
    QString name = QStringLiteral("My multiscale preset");
    QString algorithm = QStringLiteral("csf.cloudcompare.v2.13.2");
    GroundParameters base;
    int complexity = 1, microrelief = 1, vegetation = 1;
    QVector<double> windows = {3,6,12,20}, thresholds = {.5,.8,1.4,2.0};
    double cellSize = .5, dtmStep = .5;
    QStringList features;
    QVector<double> radii = {.25,.5,1};
    double minimumRadius = .1, maximumRadius = 3;
    bool useReturns = true;
    QJsonObject sourceProfile;
};
QJsonObject processingPresetToJson(const ProcessingPreset& preset);
bool processingPresetFromJson(const QJsonObject& json, ProcessingPreset& output, QString& error);
bool suggestProcessingPreset(ProcessingPreset& preset, const QJsonObject& profile, QString& explanation);
}
