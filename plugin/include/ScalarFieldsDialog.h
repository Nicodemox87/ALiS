// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ScalarFieldSearch.h"
#include <QApplication>
#include <QDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QProgressDialog>
#include <QComboBox>
#include <QSignalBlocker>
#include <QHash>
namespace alis {
inline QJsonArray manageScalarFieldsDialog(ccPointCloud& cloud,QWidget* parent){
    QDialog dialog(parent);dialog.setWindowTitle("ALiS — Scalar fields / memory");dialog.setObjectName("ALiS.ScalarFields.Manager");dialog.resize(900,650);auto* layout=new QVBoxLayout(&dialog);
    auto* note=new QLabel("Names can exist before computation: a preallocated all-NaN column still occupies RAM and BIN space. Analyse columns before deciding. Removal affects this loaded cloud only: save a new BIN to keep it. Ground/classes/return metadata are protected.");note->setWordWrap(true);layout->addWidget(note);
    auto* search=new QGridLayout;layout->addLayout(search);
    auto* searchField=new QComboBox;searchField->setObjectName("ALiS.ScalarFields.SearchField");
    for(const auto& item:QList<QPair<QString,QString>>{{"Name / radius / job","name"},{"Minimum value","minimum"},{"Maximum value","maximum"},{"Mean value","mean"},{"Valid value count","valid"},{"Missing value count","missing"},{"Missing values (%)","missing_percent"},{"Memory (MiB)","memory"},{"Status","status"}})searchField->addItem(item.first,item.second);
    auto* operation=new QComboBox;operation->setObjectName("ALiS.ScalarFields.SearchOperator");
    auto* filter=new QLineEdit;filter->setObjectName("ALiS.ScalarFields.SearchValue");filter->setPlaceholderText("Field name, radius or job token…");
    auto* reset=new QPushButton("Reset");
    search->addWidget(new QLabel("Search in"),0,0);search->addWidget(new QLabel("Operator"),0,1);search->addWidget(new QLabel("Text / number"),0,2);
    search->addWidget(searchField,1,0);search->addWidget(operation,1,1);search->addWidget(filter,1,2);search->addWidget(reset,1,3);search->setColumnStretch(2,1);
    auto* searchInfo=new QLabel("Value/count filters require Analyse first. This searches scalar-field statistics, not individual points. Unknown statistics are not zero.");searchInfo->setWordWrap(true);layout->addWidget(searchInfo);
    auto* table=new QTableWidget(0,8);table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->setHorizontalHeaderLabels({"Remove / field","Memory (MiB)","Valid values","Missing values","Status","Minimum","Maximum","Mean"});table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);table->setSelectionBehavior(QAbstractItemView::SelectRows);layout->addWidget(table);
    auto* summary=new QLabel;summary->setWordWrap(true);layout->addWidget(summary);auto* actions=new QGridLayout;layout->addLayout(actions);
    auto* analyse=new QPushButton("Analyse checked / selected fields");auto* empty=new QPushButton("Check analysed empty fields");auto* remove=new QPushButton("Remove checked fields…");auto* close=new QPushButton("Close");auto* all=new QPushButton("Select all filtered fields");all->setObjectName("ALiS.ScalarFields.SelectAll");auto* none=new QPushButton("Deselect all filtered fields");none->setObjectName("ALiS.ScalarFields.DeselectAll");actions->addWidget(all,0,0);actions->addWidget(none,0,1);actions->addWidget(empty,0,2);actions->addWidget(analyse,1,0);actions->addWidget(remove,1,1);actions->addWidget(close,1,2);
    QObject::connect(all,&QPushButton::clicked,&dialog,[&](){for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)&&!protectedScalarField(table->item(row,0)->text()))table->item(row,0)->setCheckState(Qt::Checked);});
    QObject::connect(none,&QPushButton::clicked,&dialog,[&](){for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)&&!protectedScalarField(table->item(row,0)->text()))table->item(row,0)->setCheckState(Qt::Unchecked);});
    QJsonArray changes;QHash<QString,ScalarFieldStats> statistics;
    std::function<void()> applyFilter;
    auto renderStats=[&](int row,const ScalarFieldStats& result){
        table->item(row,2)->setText(QString::number(result.finite));table->item(row,3)->setText(QString::number(result.missing));
        table->item(row,4)->setText((protectedScalarField(table->item(row,0)->text())?QStringLiteral("Protected / "):QString())+(result.finite==0?QStringLiteral("EMPTY / no valid values"):QStringLiteral("Contains values")));table->item(row,4)->setData(Qt::UserRole,result.finite==0);
        const double values[]={result.minimum,result.maximum,result.mean};for(int i=0;i<3;++i){table->item(row,5+i)->setText(std::isfinite(values[i])?QString::number(values[i],'g',12):QStringLiteral("No finite values"));table->item(row,5+i)->setToolTip(std::isfinite(values[i])?QString::number(values[i],'g',17):QString());}
    };
    auto refresh=[&](){table->setRowCount(int(cloud.getNumberOfScalarFields()));for(int row=0;row<table->rowCount();++row){const QString name=QString::fromUtf8(cloud.getScalarField(row)->getName());const bool locked=protectedScalarField(name);auto* item=new QTableWidgetItem(name);item->setToolTip(name);item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|(locked?Qt::NoItemFlags:Qt::ItemIsUserCheckable));if(!locked)item->setCheckState(Qt::Unchecked);table->setItem(row,0,item);table->setItem(row,1,new QTableWidgetItem(QString("%1").arg(double(cloud.size())*sizeof(ScalarType)/1048576.,0,'f',1)));for(int col=2;col<8;++col)table->setItem(row,col,new QTableWidgetItem("Not analysed"));table->item(row,4)->setText(locked?"Protected":"Not analysed");if(statistics.contains(name))renderStats(row,statistics.value(name));}summary->setText(QString("%1 fields; scalar arrays alone: %2 GiB. Analysis reads every point; it never creates features.").arg(cloud.getNumberOfScalarFields()).arg(double(cloud.size())*cloud.getNumberOfScalarFields()*sizeof(ScalarType)/1073741824.,0,'f',2));if(applyFilter)applyFilter();};
    applyFilter=[&](){const QString field=searchField->currentData().toString();double number=0;const bool valid=filter->text().trimmed().isEmpty()||scalarSearchIsText(field)||scalarSearchNumber(filter->text(),number);int shown=0,unknown=0;
        for(int row=0;row<table->rowCount();++row){const auto name=table->item(row,0)->text();auto it=statistics.constFind(name);const auto* stats=it==statistics.cend()?nullptr:&it.value();const bool visible=valid&&matchesScalarFieldSearch(name,stats,cloud.size(),field,operation->currentData().toString(),filter->text());table->setRowHidden(row,!visible);shown+=visible;if(!stats&&scalarSearchNeedsStatistics(field))++unknown;if(!visible&&table->item(row,0)->checkState()==Qt::Checked)table->item(row,0)->setCheckState(Qt::Unchecked);}
        filter->setStyleSheet(valid?QString():QStringLiteral("QLineEdit { border:1px solid #dc2626; }"));
        searchInfo->setText(!valid?QStringLiteral("Enter a finite number, for example 0, 0.5 or 0,5. NaN/Inf are not search thresholds."):QString("%1 / %2 fields match. %3\nSearch concerns field statistics, not point filtering. Changing the search deselects hidden rows.").arg(shown).arg(table->rowCount()).arg(unknown?QString("%1 fields are not analysed: reset the search and Analyse them before numeric comparisons.").arg(unknown):QString("Only analysed statistics are used; unknown values never equal zero.")));
    };
    auto operators=[&](){const QSignalBlocker blocker(operation);const QString previous=operation->currentData().toString();operation->clear();
        if(scalarSearchIsText(searchField->currentData().toString())){operation->addItem("Contains","contains");operation->addItem("Does not contain","not_contains");operation->addItem("Starts with","starts");operation->addItem("Ends with","ends");}
        operation->addItem("=","=");operation->addItem(QString::fromUtf8("≠"),"!=");if(!scalarSearchIsText(searchField->currentData().toString())){operation->addItem(">",">");operation->addItem(QString::fromUtf8("≥"),">=");operation->addItem("<","<");operation->addItem(QString::fromUtf8("≤"),"<=");}
        const int index=operation->findData(previous);operation->setCurrentIndex(index<0?0:index);filter->setPlaceholderText(scalarSearchIsText(searchField->currentData().toString())?"Text, name, radius, job or status…":"Number (decimal comma or dot)…");
    };operators();refresh();
    QObject::connect(filter,&QLineEdit::textChanged,&dialog,[&](const QString&){applyFilter();});
    QObject::connect(searchField,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int){operators();applyFilter();});
    QObject::connect(operation,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int){applyFilter();});
    QObject::connect(reset,&QPushButton::clicked,&dialog,[&](){filter->clear();searchField->setCurrentIndex(0);operation->setCurrentIndex(0);applyFilter();});
    QObject::connect(analyse,&QPushButton::clicked,&dialog,[&](){QVector<int> rows;for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)&&(table->item(row,0)->checkState()==Qt::Checked||table->item(row,0)->isSelected()))rows<<row;
        if(rows.isEmpty()){QMessageBox::information(&dialog,"Analyse fields","Reset the search, then select or check the fields to analyse. No values were calculated or changed.");return;}
        QProgressDialog progress("Reading counts, minimum, maximum and mean…","Cancel",0,rows.size(),&dialog);progress.setWindowModality(Qt::ApplicationModal);progress.setMinimumDuration(0);progress.show();int done=0;
        try{for(int row:rows){auto result=inspectScalarField(*cloud.getScalarField(row),[&](){QApplication::processEvents();checkpointCheck(!progress.wasCanceled(),"Analysis canceled; no fields removed");});statistics.insert(table->item(row,0)->text(),result);renderStats(row,result);progress.setValue(++done);}}catch(const std::exception& e){QMessageBox::information(&dialog,"Analysis",e.what());}applyFilter();
    });
    QObject::connect(empty,&QPushButton::clicked,&dialog,[&](){for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)&&!protectedScalarField(table->item(row,0)->text())&&table->item(row,4)->data(Qt::UserRole).toBool())table->item(row,0)->setCheckState(Qt::Checked);});
    QObject::connect(remove,&QPushButton::clicked,&dialog,[&](){QStringList selected;for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)&&table->item(row,0)->checkState()==Qt::Checked)selected<<table->item(row,0)->text();if(selected.isEmpty())return;
        QMessageBox confirm(QMessageBox::Warning,"Remove loaded scalar fields?",QString("Remove %1 checked fields in the current filter (%2 GiB)?\nThis has no Undo here. Reopen the saved BIN to restore them.\nAffected feature jobs are detached, not falsely marked complete. Checkpoint files remain available for recovery.\nHidden rows are NOT removed.\nSave a NEW BIN afterwards.").arg(selected.size()).arg(double(cloud.size())*selected.size()*4/1073741824.,0,'f',2),QMessageBox::Yes|QMessageBox::Cancel,&dialog);confirm.setDetailedText(selected.join('\n'));confirm.setDefaultButton(QMessageBox::Cancel);if(confirm.exec()!=QMessageBox::Yes)return;
        try{changes.append(removeSelectedScalarFields(cloud,selected));refresh();filter->clear();}catch(const std::exception& e){QMessageBox::critical(&dialog,"Removal stopped",e.what());}
    });QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);dialog.exec();return changes;
}
}
