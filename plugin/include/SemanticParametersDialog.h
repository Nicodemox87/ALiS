// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "SemanticGeometry.h"
#include <QDialog>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QVBoxLayout>
namespace alis {
inline SemanticParameters semanticParametersFromJson(const QJsonObject& json){
    SemanticParameters p;
    p.planarFine=json.value("planarFine").toDouble(p.planarFine);p.planarCoarse=json.value("planarCoarse").toDouble(p.planarCoarse);
    p.structureSphericity=json.value("structureSphericity").toDouble(p.structureSphericity);p.coarseSphericity=json.value("coarseSphericity").toDouble(p.coarseSphericity);
    p.vegetationSphericity=json.value("vegetationSphericity").toDouble(p.vegetationSphericity);p.vegetationPlanarityMax=json.value("vegetationPlanarityMax").toDouble(p.vegetationPlanarityMax);
    p.wallNormalMax=json.value("wallNormalMax").toDouble(p.wallNormalMax);p.roofNormalMin=json.value("roofNormalMin").toDouble(p.roofNormalMin);
    p.lowHeight=json.value("lowHeight").toDouble(p.lowHeight);p.mediumHeight=json.value("mediumHeight").toDouble(p.mediumHeight);
    p.minimumWallHeight=json.value("minimumWallHeight").toDouble(p.minimumWallHeight);p.minimumWallSpan=json.value("minimumWallSpan").toDouble(p.minimumWallSpan);p.minimumRoofWidth=json.value("minimumRoofWidth").toDouble(p.minimumRoofWidth);
    p.minimumComponentCells=json.value("minimumComponentCells").toInt(p.minimumComponentCells);p.minimumWallCells=json.value("minimumWallCells").toInt(p.minimumWallCells);p.minimumRoofCells=json.value("minimumRoofCells").toInt(p.minimumRoofCells);return p;
}
inline bool semanticParametersDialog(SemanticParameters& parameters,QWidget* parent){
    QDialog dialog(parent);dialog.setWindowTitle("ALiS — Explain / tune automatic classes");dialog.resize(680,780);auto* root=new QVBoxLayout(&dialog);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);auto* content=new QWidget;auto* form=new QFormLayout(content);scroll->setWidget(content);root->addWidget(scroll);form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* note=new QLabel("Ground is ALWAYS copied from the applied Ground/Working state, never inferred again, including poles/wires runs. Structure rules require single/last returns, planar support at both scales and connected roof + wall patches. Vegetation requires dispersed geometry at both scales and valid current HAG. Lower vegetation sphericity accepts less volumetric foliage but can also accept clutter. Higher structural planarity / larger support rejects more trees but may miss roofs. Scores are rule strengths, NOT probabilities. Review Derived and qAL_AutomaticReason before Apply; Working remains unchanged until Apply.");note->setWordWrap(true);note->setMaximumWidth(620);form->addRow(note);
    QList<QPair<QDoubleSpinBox*,double*>> numbers;QList<QPair<QSpinBox*,int*>> counts;auto p=parameters;
    auto real=[&](const QString& label,double& value,double maximum=1.){auto* spin=new QDoubleSpinBox;spin->setRange(0,maximum);spin->setDecimals(3);spin->setSingleStep(maximum==1?.025:.25);spin->setValue(value);form->addRow(label,spin);numbers<<qMakePair(spin,&value);};
    real("Structures: minimum planarity at 2 m",p.planarFine);real("Structures: minimum planarity at 4 m",p.planarCoarse);real("Structures: maximum sphericity at 2 m",p.structureSphericity);real("Structures: maximum sphericity at 4 m",p.coarseSphericity);
    real("Vegetation: minimum sphericity BOTH scales",p.vegetationSphericity);real("Vegetation: maximum planarity BOTH scales",p.vegetationPlanarityMax);real("Wall: maximum absolute normal Z",p.wallNormalMax);real("Roof: minimum absolute normal Z",p.roofNormalMin);real("Low vegetation upper HAG (m)",p.lowHeight,100);real("Medium vegetation upper HAG (m)",p.mediumHeight,100);
    real("Wall: minimum supported vertical extent (m)",p.minimumWallHeight,100);real("Wall: minimum supported lateral extent (m)",p.minimumWallSpan,100);real("Roof: minimum width on BOTH horizontal axes (m)",p.minimumRoofWidth,100);
    auto count=[&](const QString& label,int& value){auto* spin=new QSpinBox;spin->setRange(1,100000);spin->setValue(value);form->addRow(label,spin);counts<<qMakePair(spin,&value);};count("Minimum connected 1 m cells",p.minimumComponentCells);count("Minimum supported wall cells",p.minimumWallCells);count("Minimum supported roof cells",p.minimumRoofCells);
    auto* reset=new QPushButton("Restore conservative defaults");form->addRow(reset);QObject::connect(reset,&QPushButton::clicked,&dialog,[&](){p=SemanticParameters();for(auto pair:numbers)pair.first->setValue(*pair.second);for(auto pair:counts)pair.first->setValue(*pair.second);});
    auto* apply=new QPushButton("Save parameters — do not classify yet");root->addWidget(apply);QObject::connect(apply,&QPushButton::clicked,&dialog,[&](){for(auto pair:numbers)*pair.second=pair.first->value();for(auto pair:counts)*pair.second=pair.first->value();try{p.validate();parameters=p;dialog.accept();}catch(const std::exception& e){QMessageBox::warning(&dialog,"Parameters",e.what());}});auto* cancel=new QPushButton("Cancel");root->addWidget(cancel);QObject::connect(cancel,&QPushButton::clicked,&dialog,&QDialog::reject);return dialog.exec()==QDialog::Accepted;
}
}
