// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "PointSelection.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
namespace alis {
inline bool pointSelectionDialog(const ccPointCloud& cloud,QJsonArray& rules,bool& workOnly,bool& restore,QWidget* parent){
    QDialog dialog(parent);dialog.setWindowTitle("ALiS — Point filters / working selection");dialog.resize(780,480);auto* layout=new QVBoxLayout(&dialog);
    auto* help=new QLabel("Rows combine with AND. Class codes within one row combine with OR. Example: Working classes 2,6,128,129; HAG interval 0;2. Filtering only hides points; it never deletes or exports them. Colour saturation remains independent.");help->setWordWrap(true);layout->addWidget(help);
    auto* table=new QTableWidget(0,3);table->setObjectName("ALiS.Selection.Rules");table->setHorizontalHeaderLabels({"Scalar field","Condition","Values"});table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table);
    auto add=[&](const QJsonObject& rule){int row=table->rowCount();table->insertRow(row);auto* field=new QComboBox;field->setMinimumContentsLength(12);field->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);for(unsigned i=0;i<cloud.getNumberOfScalarFields();++i)field->addItem(QString::fromUtf8(cloud.getScalarField(i)->getName()));field->setCurrentText(rule.value("field").toString());auto* op=new QComboBox;
        for(auto item:QList<QPair<QString,QString>>{{"Inclusive interval","range"},{"Class codes (OR)","codes"},{"=","="},{"≠","!="},{">",">"},{"≥",">="},{"<","<"},{"≤","<="},{"Finite values","finite"},{"Missing values","missing"}})op->addItem(item.first,item.second);int index=op->findData(rule.value("op").toString());op->setCurrentIndex(index<0?0:index);auto* value=new QLineEdit(rule.value("text").toString());value->setPlaceholderText("Interval: 0;2 / classes: 2,6,128,129");table->setCellWidget(row,0,field);table->setCellWidget(row,1,op);table->setCellWidget(row,2,value);};
    for(const auto& rule:rules)add(rule.toObject());if(rules.isEmpty())add({});
    auto* addButton=new QPushButton("Add condition (AND)");layout->addWidget(addButton);QObject::connect(addButton,&QPushButton::clicked,&dialog,[&](){add({});});auto* remove=new QPushButton("Remove selected condition");layout->addWidget(remove);QObject::connect(remove,&QPushButton::clicked,&dialog,[&](){if(table->currentRow()>=0)table->removeRow(table->currentRow());});
    auto* work=new QCheckBox("Work ONLY on these points — not just display");work->setObjectName("ALiS.Selection.WorkOnly");work->setChecked(workOnly);layout->addWidget(work);
    auto* safety=new QLabel("Working selection is frozen when Apply is pressed. Supported: manual edits, Apply predictions, automatic class proposals, DTM/DFM and in-memory feature queries. Neighbourhood features retain full-cloud context. Other full-cloud tools refuse to run until this option is disabled. Undo restores its original transaction, independent of the current selection. Selection is not saved across reopening the BIN.");safety->setWordWrap(true);layout->addWidget(safety);
    auto* apply=new QPushButton("Apply point filter");apply->setObjectName("ALiS.Selection.Apply");layout->addWidget(apply);auto* clear=new QPushButton("Restore visibility before this filter / disable working selection");layout->addWidget(clear);auto* cancel=new QPushButton("Cancel");layout->addWidget(cancel);
    restore=false;QObject::connect(apply,&QPushButton::clicked,&dialog,[&](){QJsonArray parsed;for(int row=0;row<table->rowCount();++row){QJsonObject rule;QString error;if(!parsePointSelectionRule(static_cast<QComboBox*>(table->cellWidget(row,0))->currentText(),static_cast<QComboBox*>(table->cellWidget(row,1))->currentData().toString(),static_cast<QLineEdit*>(table->cellWidget(row,2))->text(),rule,error)){QMessageBox::warning(&dialog,"Point filter",error);return;}parsed.append(rule);}if(parsed.isEmpty()){QMessageBox::warning(&dialog,"Point filter","Add a condition or use Restore visibility.");return;}rules=parsed;workOnly=work->isChecked();dialog.accept();});
    QObject::connect(clear,&QPushButton::clicked,&dialog,[&](){restore=true;dialog.accept();});QObject::connect(cancel,&QPushButton::clicked,&dialog,&QDialog::reject);return dialog.exec()==QDialog::Accepted;
}
}
