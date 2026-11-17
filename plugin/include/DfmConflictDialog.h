// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "DfmConflictGrid.h"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QDialog>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QGridLayout>
#include <QMessageBox>
#include <QFileDialog>
#include <QPainter>
#include <QTimer>
namespace alis {
class DfmConflictView final : public QGraphicsView {
public:
    explicit DfmConflictView(QGraphicsScene* scene,QWidget* parent=nullptr):QGraphicsView(scene,parent){setRenderHint(QPainter::Antialiasing);setTransformationAnchor(AnchorUnderMouse);setMinimumHeight(240);}
    std::function<void(QRectF)> rectangleChosen;
protected:
    void mousePressEvent(QMouseEvent* e)override{if(e->button()==Qt::RightButton){panning=true;last=e->pos();setCursor(Qt::ClosedHandCursor);return;}if(e->button()==Qt::LeftButton){start=mapToScene(e->pos());drawing=true;rubber=scene()->addRect(QRectF(start,start),QPen(Qt::white,0));return;}QGraphicsView::mousePressEvent(e);}
    void mouseMoveEvent(QMouseEvent* e)override{if(panning){const auto delta=e->pos()-last;last=e->pos();horizontalScrollBar()->setValue(horizontalScrollBar()->value()-delta.x());verticalScrollBar()->setValue(verticalScrollBar()->value()-delta.y());return;}if(drawing&&rubber){rubber->setRect(QRectF(start,mapToScene(e->pos())).normalized());return;}QGraphicsView::mouseMoveEvent(e);}
    void mouseReleaseEvent(QMouseEvent* e)override{if(e->button()==Qt::RightButton){panning=false;unsetCursor();return;}if(e->button()==Qt::LeftButton&&drawing){drawing=false;const auto area=QRectF(start,mapToScene(e->pos())).normalized();if(rubber){scene()->removeItem(rubber);delete rubber;rubber=nullptr;}if(rectangleChosen&&area.width()>.1&&area.height()>.1)rectangleChosen(area);return;}QGraphicsView::mouseReleaseEvent(e);}
    void wheelEvent(QWheelEvent* e)override{const double factor=e->angleDelta().y()>0?1.25:.8;const double current=transform().m11();if(current*factor>.00001&&current*factor<200)scale(factor,factor);e->accept();}
private:bool drawing=false,panning=false;QPoint last;QPointF start;QGraphicsRectItem* rubber=nullptr;
};
inline bool dfmConflictDialog(DfmConflictGrid& grid,QWidget* parent){
    QDialog dialog(parent);dialog.setWindowTitle("ALiS — DFM overlap map: choose Ground or Building");dialog.resize(1050,800);auto* root=new QVBoxLayout(&dialog);
    auto* note=new QLabel("Review BEFORE final raster/interpolation. MAGENTA = overlapping Ground/Building cells following recipe; GREEN = chosen Ground; RED = chosen Building; BLUE = Building only; GREY = Ground only; BLACK = no observations. Left-drag rectangles to assign a preference, right-drag to pan, wheel to zoom. Decisions affect overlapping cells only, never source classes. Last rectangle wins. The preview aggregates pixels so even small conflicts stay highlighted.");note->setWordWrap(true);root->addWidget(note);
    auto* row=new QGridLayout;auto* preference=new QComboBox;preference->addItem("Rectangle: keep Ground",1);preference->addItem("Rectangle: keep chosen Building",2);preference->addItem("Rectangle: restore unmarked preference",0);auto* fallback=new QComboBox;fallback->addItem("Unmarked overlaps: use selected recipe",0);fallback->addItem("Unmarked overlaps: keep Ground",1);fallback->addItem("Unmarked overlaps: keep Building",2);fallback->setCurrentIndex(grid.fallback);row->addWidget(preference,0,0);row->addWidget(fallback,0,1);root->addLayout(row);
    auto* scene=new QGraphicsScene(&dialog);auto* view=new DfmConflictView(scene);view->setObjectName("ALiS.DFM.ConflictMap");root->addWidget(view,1);auto* status=new QLabel;status->setWordWrap(true);root->addWidget(status);
    auto* buttons=new QGridLayout;root->addLayout(buttons);auto* undo=new QPushButton("Undo rectangle");auto* clear=new QPushButton("Clear rectangles");auto* fit=new QPushButton("Fit map");auto* exportImage=new QPushButton("Export review PNG…");auto* accept=new QPushButton("Confirm choices and create final DFM");auto* cancel=new QPushButton("Cancel — no DFM created");buttons->addWidget(undo,0,0);buttons->addWidget(clear,0,1);buttons->addWidget(fit,0,2);buttons->addWidget(exportImage,0,3);buttons->addWidget(accept,1,0,1,2);buttons->addWidget(cancel,1,2,1,2);
    QImage preview;auto redraw=[&](){const unsigned w=std::min(1600u,grid.width),h=std::min(1200u,grid.height);std::vector<unsigned char> bins(std::size_t(w)*h,0);std::uint64_t ground=0,building=0,recipe=0;
        for(unsigned y=0;y<grid.height;++y)for(unsigned x=0;x<grid.width;++x){const auto i=std::size_t(y)*grid.width+x;const auto pixel=std::size_t((grid.height-1-y)*std::uint64_t(h)/grid.height)*w+x*std::uint64_t(w)/grid.width;unsigned char flags=0;if(grid.overlap(i)){const auto choice=grid.choice(i);if(choice==1){flags=4;++ground;}else if(choice==2){flags=8;++building;}else {flags=16;++recipe;}}else if(std::isfinite((*grid.building)[i]))flags=2;else if(std::isfinite((*grid.ground)[i]))flags=1;bins[pixel]|=flags;}
        preview=QImage(int(w),int(h),QImage::Format_RGB32);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto flags=bins[std::size_t(y)*w+x];QColor color=flags&16?QColor(230,40,210):(flags&4)&&(flags&8)?QColor(255,210,30):flags&8?QColor(230,65,55):flags&4?QColor(50,205,95):flags&2?QColor(55,130,220):flags&1?QColor(155,155,155):QColor(30,35,45);preview.setPixelColor(int(x),int(y),color);}
        scene->clear();auto* image=scene->addPixmap(QPixmap::fromImage(preview));image->setTransform(QTransform::fromScale(double(grid.width)/w,double(grid.height)/h));scene->setSceneRect(0,0,grid.width,grid.height);for(const auto& r:grid.regions){QPen pen(r.preferred==1?QColor(50,255,100):r.preferred==2?QColor(255,90,70):QColor(255,255,255));pen.setWidth(2);pen.setCosmetic(true);scene->addRect(r.x0,grid.height-1-r.y1,r.x1-r.x0+1,r.y1-r.y0+1,pen);}
        status->setText(QString("%1 overlapping cells: Ground override %2; Building override %3; recipe %4. Rectangles: %5. Shared grid %6 × %7, step %8 m. Remaining recipe cells will use %9.").arg(grid.conflicts).arg(ground).arg(building).arg(recipe).arg(grid.regions.size()).arg(grid.width).arg(grid.height).arg(grid.step/grid.globalScale,0,'g',6).arg(grid.minimumRecipe?"MINIMUM (Ground may hide roofs)":"Building maximum priority"));};
    view->rectangleChosen=[&](QRectF area){area=area.intersected(QRectF(0,0,grid.width,grid.height));if(area.isEmpty())return;DfmDecisionRegion r;r.x0=unsigned(std::max(0,int(std::floor(area.left()))));r.x1=unsigned(std::min(int(grid.width)-1,int(std::ceil(area.right()))-1));r.y0=unsigned(std::max(0,int(grid.height)-int(std::ceil(area.bottom()))));r.y1=unsigned(std::min(int(grid.height)-1,int(grid.height)-1-int(std::floor(area.top()))));r.preferred=static_cast<unsigned char>(preference->currentData().toInt());try{grid.addRegion(r);redraw();}catch(const std::exception& e){QMessageBox::warning(&dialog,"DFM region",e.what());}};
    QObject::connect(fallback,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int){grid.fallback=static_cast<unsigned char>(fallback->currentData().toInt());redraw();});QObject::connect(undo,&QPushButton::clicked,&dialog,[&](){grid.undoRegion();redraw();});QObject::connect(clear,&QPushButton::clicked,&dialog,[&](){grid.clearRegions();redraw();});QObject::connect(fit,&QPushButton::clicked,&dialog,[&](){view->fitInView(scene->sceneRect(),Qt::KeepAspectRatio);});
    QObject::connect(exportImage,&QPushButton::clicked,&dialog,[&](){const auto file=QFileDialog::getSaveFileName(&dialog,"Export DFM review image","F:/2026_PZARMERINA/DFM_overlap_review.png","PNG (*.png)");if(file.isEmpty())return;QImage output(preview.width(),preview.height()+100,QImage::Format_RGB32);output.fill(Qt::white);QPainter painter(&output);scene->render(&painter,QRectF(0,0,preview.width(),preview.height()),scene->sceneRect());painter.setPen(Qt::black);painter.drawText(QRectF(8,preview.height()+4,preview.width()-16,92),Qt::TextWordWrap,"DFM review (NOT final elevations). Magenta recipe; green Ground; red Building; blue Building-only; grey Ground-only.\n"+status->text());painter.end();if(!output.save(file))QMessageBox::warning(&dialog,"Export","Could not write PNG.");});
    QObject::connect(accept,&QPushButton::clicked,&dialog,[&](){if(QMessageBox::question(&dialog,"Confirm overlap decisions",status->text()+"\n\nProceed to final raster and selected interpolation?",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Yes)dialog.accept();});QObject::connect(cancel,&QPushButton::clicked,&dialog,&QDialog::reject);redraw();QTimer::singleShot(0,&dialog,[&](){view->fitInView(scene->sceneRect(),Qt::KeepAspectRatio);});return dialog.exec()==QDialog::Accepted;
}
}
