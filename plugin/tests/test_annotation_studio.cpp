// SPDX-License-Identifier: GPL-2.0-or-later
#include "AnnotationStudio.h"
#include "AnnotationCanvas.h"
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QWheelEvent>
#include <iostream>
#include <stdexcept>

namespace alis
{
	struct AnnotationStudioTestAccess
	{
		static int run()
		{
			int checks = 0;
			auto check = [&](bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); };
			ccPointCloud cloud(QStringLiteral("synthetic full-resolution annotation test"));
			cloud.reserve(250000);
			for (unsigned i = 0; i < 250000; ++i) cloud.addPoint(CCVector3(float(i % 500) * .1f, float(i / 500) * .1f, float(i % 10) * .1f));
			cloud.setGlobalShift(CCVector3d(-440000,-4135000,-500)); cloud.setGlobalScale(2.0);
			AnnotationStudio studio(&cloud);
			check(studio.m_canvases.size() == 4, "four views share one studio");
			check(studio.m_canvases[3]->camera().view == AnnotationView::Orbit3D, "fourth panel starts as 3D");
			check(studio.m_activeCanvas->panelNumber() == 1, "initial active panel is numbered correctly");
			check(std::abs(studio.localCenter() - 24.95) < .003, "global shift/scale round-trip preserves section center");
			studio.m_section->setChecked(false); studio.rebuildPreview();
			check(studio.m_sliceCount == 250000 && studio.m_renderPoints.size() <= 120000, "only rendering is sampled, source scope stays full resolution");
			QPolygonF rectangle; rectangle << QPointF(-1,-1) << QPointF(60,-1) << QPointF(60,60) << QPointF(-1,60);
			studio.selectPolygon(0,1,rectangle);
			check(studio.m_selectedCount == 250000, "rectangle selects ALL original points, not the render sample");
			studio.m_mode->setCurrentIndex(2);
			QPolygonF left; left << QPointF(-1,-1) << QPointF(24.95,-1) << QPointF(24.95,60) << QPointF(-1,60);
			studio.selectPolygon(0,1,left);
			check(studio.m_selectedCount == 125000, "subtract is exact and index-aligned");
			studio.m_mode->setCurrentIndex(1); studio.selectPolygon(0,1,left);
			check(studio.m_selectedCount == 250000, "add merges without duplicate indices");
			studio.m_section->setChecked(true); studio.m_thickness->setValue(1.0); studio.rebuildPreview();
			check(studio.m_selectedCount == 250000, "moving/narrowing a section retains shared selection");
			studio.m_mode->setCurrentIndex(0); studio.selectPolygon(0,1,rectangle);
			check(studio.m_selectedCount == 10000, "one metric metre becomes two local units with global scale 2");
			studio.m_axis->setCurrentIndex(2); studio.rebuildPreview();
			check(studio.m_indexAxis == 2, "changing normal rebuilds the correct coordinate index");
			studio.m_section->setChecked(false); studio.m_wandRadius->setValue(.1); studio.m_wandScalar->setChecked(false);
			studio.selectWand(0);
			check(studio.m_selectedCount > 0 && studio.m_selectedCount < 100, "wand radius selects a bounded full-resolution neighbourhood");
			const int field = cloud.addScalarField("test intensity"); auto* sf = static_cast<ccScalarField*>(cloud.getScalarField(field));
			for (unsigned i = 0; i < cloud.size(); ++i) sf->setValue(i, float(i % 2)); sf->computeMinAndMax();
			cloud.setCurrentDisplayedScalarField(field); cloud.showSF(true);
			studio.m_wandScalar->setChecked(true); studio.m_wandTolerance->setValue(0); studio.selectWand(0);
			bool matching = true; for (unsigned i = 0; i < cloud.size(); ++i) if (studio.m_selected[i] && i % 2) matching = false;
			check(matching && studio.m_selectedCount > 0, "wand applies scalar tolerance to raw point values");
			auto& visibility = cloud.getTheVisibilityArray(); visibility.resize(cloud.size(), CCCoreLib::POINT_VISIBLE);
			for (unsigned i = 0; i < cloud.size(); i += 2) visibility[i] = CCCoreLib::POINT_HIDDEN;
			studio.selectPolygon(0,1,rectangle);
			check(studio.m_selectedCount == 125000, "native host visibility restricts new selection scope");
			visibility.clear(); studio.m_wandScalar->setChecked(false); studio.rebuildPreview();

			// Each camera must be orthonormal; projections and inverse screen mapping
			// use the same basis for rendering and full-resolution selection.
			for (int view = 0; view <= int(AnnotationView::Orbit3D); ++view)
			{
				AnnotationCamera camera; camera.orient(static_cast<AnnotationView>(view), 1);
				check(std::abs(annotationDot(camera.right,camera.up)) < 1e-12 && std::abs(annotationDot(camera.right,camera.toward)) < 1e-12 && std::abs(annotationDot(camera.up,camera.toward)) < 1e-12
					&& std::abs(annotationDot(camera.right,camera.right)-1) < 1e-12 && std::abs(annotationDot(camera.up,camera.up)-1) < 1e-12 && std::abs(annotationDot(camera.toward,camera.toward)-1) < 1e-12, "orthonormal camera basis");
				camera.center={{25,25,.45}};
				QPolygonF region; region << QPointF(-12,-12) << QPointF(3,-12) << QPointF(3,7) << QPointF(-12,7);
				studio.selectProjectedPolygon(camera,region,0);
				std::size_t expected=0; bool sameIds=true;
				for(unsigned i=0;i<cloud.size();++i)
				{
					const auto* p=cloud.getPoint(i); const auto q=camera.project({{p->x,p->y,p->z}});
					const bool inside=q.first>=-12 && q.first<=3 && q.second>=-12 && q.second<=7;
					if(inside) ++expected; if(studio.m_selected[i]!=inside) sameIds=false;
				}
				check(sameIds && studio.m_selectedCount==expected && expected>0, "view selection matches exact projected original IDs");
			}
			AnnotationCamera top,bottom; top.orient(AnnotationView::Top,0); bottom.orient(AnnotationView::Bottom,0);
			check(top.project({{3,7,9}}).first==3 && bottom.project({{3,7,9}}).first==-3, "opposite views mirror horizontal projection");
			for(int mode=0;mode<3;++mode)
				check(annotationSelectionMode(mode,true)==1 && annotationSelectionMode(mode,false)==mode, "Ctrl temporarily adds regardless of menu mode");

			// Drive real canvas handlers: Ctrl is latched when a gesture starts,
			// survives key release, and never changes the dropdown for the next one.
			auto* canvas=studio.m_canvases[0]; canvas->resize(600,400); canvas->setView(AnnotationView::Top); canvas->fit();
			studio.m_mode->setCurrentIndex(0); studio.m_tool->setCurrentIndex(0);
			auto drag=[&](AnnotationCanvas* target, QPointF start, QPointF end, Qt::KeyboardModifiers modifiers)
			{
				QMouseEvent press(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,modifiers); QApplication::sendEvent(target,&press);
				QMouseEvent move(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(target,&move);
				QMouseEvent release(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier); QApplication::sendEvent(target,&release);
			};
			drag(canvas,canvas->screen({{-1,-1,0}}),canvas->screen({{24.95,60,0}}),Qt::NoModifier);
			check(studio.m_selectedCount==125000, "real rectangle gesture selects first half");
			drag(canvas,canvas->screen({{25,-1,0}}),canvas->screen({{60,60,0}}),Qt::ControlModifier);
			check(studio.m_selectedCount==250000 && studio.m_mode->currentIndex()==0, "Ctrl adds second half without changing Replace menu");
			drag(canvas,canvas->screen({{25,-1,0}}),canvas->screen({{60,60,0}}),Qt::NoModifier);
			check(studio.m_selectedCount==125000, "next non-Ctrl gesture replaces again");
			studio.m_mode->setCurrentIndex(2);
			drag(canvas,canvas->screen({{-1,-1,0}}),canvas->screen({{24.95,60,0}}),Qt::ControlModifier);
			check(studio.m_selectedCount==250000 && studio.m_mode->currentIndex()==2, "Ctrl overrides Subtract for one gesture");
			studio.m_mode->setCurrentIndex(0); studio.m_tool->setCurrentIndex(1);
			QMouseEvent polygonStart(QEvent::MouseButtonPress,QPointF(40,40),Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier); QApplication::sendEvent(canvas,&polygonStart);
			QMouseEvent polygonNext(QEvent::MouseButtonPress,QPointF(60,40),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(canvas,&polygonNext);
			check(canvas->m_gestureMode==1 && canvas->m_drawing, "polygon keeps Ctrl mode from first vertex"); canvas->cancelPath();
			studio.m_tool->setCurrentIndex(3); studio.m_wandRadius->setValue(.1);
			const auto beforeWand=studio.m_selectedCount;
			QMouseEvent wand(QEvent::MouseButtonPress,canvas->screen({{0,0,0}}),Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier); QApplication::sendEvent(canvas,&wand);
			check(studio.m_selectedCount==beforeWand && canvas->m_gestureMode==1, "Ctrl wand adds without clearing shared selection");

			// Per-panel menu, dynamic section side, and sync with orientation retention.
			auto* menu=studio.findChild<QComboBox*>(QStringLiteral("ALiS.Studio.Panel.2.View"));
			check(menu && menu->count()==9, "each panel exposes all nine view choices");
			menu->setCurrentIndex(int(AnnotationView::SectionBack)); studio.m_axis->setCurrentIndex(0);
			check(studio.m_canvases[1]->camera().toward[0]==-1, "section back follows X section normal");
			studio.m_axis->setCurrentIndex(1);
			check(studio.m_canvases[1]->camera().toward[1]==1, "section back follows Y section normal");
			auto* orbit=studio.m_canvases[3]; orbit->m_camera.center={{7,8,9}}; orbit->m_camera.pixelsPerUnit=17;
			const auto orientation=canvas->camera().right; studio.synchronizeViews(orbit);
			bool synced=true; for(auto* view:studio.m_canvases) synced &= view->camera().center==orbit->camera().center && view->camera().pixelsPerUnit==17;
			check(synced && canvas->camera().right==orientation && studio.m_activeCanvas==orbit, "sync copies centre/scale and retains independent orientations");
			studio.m_section->setChecked(true); studio.rebuildPreview();
			auto* front=studio.m_canvases[1]; front->setView(AnnotationView::Front); front->m_camera.center[1]=-100;
			studio.synchronizeViews(front);
			check(std::abs(orbit->camera().center[1]-studio.m_activeCenter)<1e-9 && front->camera().center==orbit->camera().center, "section source sync resolves hidden depth to slab centre");
			const auto previousRight=orbit->camera().right; const auto revision=studio.m_renderRevision;
			QMouseEvent rotateStart(QEvent::MouseButtonPress,QPointF(100,100),Qt::RightButton,Qt::RightButton,Qt::NoModifier); QApplication::sendEvent(orbit,&rotateStart);
			QMouseEvent rotateMove(QEvent::MouseMove,QPointF(140,120),Qt::NoButton,Qt::RightButton,Qt::NoModifier); QApplication::sendEvent(orbit,&rotateMove);
			QMouseEvent rotateEnd(QEvent::MouseButtonRelease,QPointF(140,120),Qt::RightButton,Qt::NoButton,Qt::NoModifier); QApplication::sendEvent(orbit,&rotateEnd);
			check(orbit->camera().right!=previousRight && studio.m_renderRevision==revision && cloud.size()==250000, "3D rotation changes only camera, not data or uploaded sample revision");
			studio.m_tool->setCurrentIndex(4); const auto beforeNavigation=orbit->camera().right; const auto selectedBeforeNavigation=studio.m_selectedCount;
			drag(orbit,QPointF(100,100),QPointF(130,110),Qt::NoModifier);
			check(orbit->camera().right!=beforeNavigation && studio.m_selectedCount==selectedBeforeNavigation && !orbit->m_rotating, "Navigate left-drag rotates without touching selection and stops on release");
			const auto beforePan=canvas->camera().center; drag(canvas,QPointF(100,100),QPointF(120,115),Qt::NoModifier);
			check(canvas->camera().center!=beforePan && canvas->camera().view==AnnotationView::Top && !canvas->m_panning, "Navigate left-drag pans orthographic views");
			studio.m_section->setChecked(false); studio.m_tool->setCurrentIndex(2); studio.m_mode->setCurrentIndex(0);
			studio.selectMatching([](unsigned id){return id==249999;},0);
			canvas->fit();
			const QPointF lassoPoints[]={canvas->screen({{-1,-1,0}}),canvas->screen({{24.95,-1,0}}),canvas->screen({{24.95,60,0}}),canvas->screen({{-1,60,0}})};
			QMouseEvent lassoStart(QEvent::MouseButtonPress,lassoPoints[0],Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier); QApplication::sendEvent(canvas,&lassoStart);
			for(int i=1;i<4;++i){QMouseEvent move(QEvent::MouseMove,lassoPoints[i],Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(canvas,&move);}
			QMouseEvent lassoEnd(QEvent::MouseButtonRelease,lassoPoints[3],Qt::LeftButton,Qt::NoButton,Qt::NoModifier); QApplication::sendEvent(canvas,&lassoEnd);
			check(studio.m_selectedCount==125001 && studio.m_selected[249999], "Ctrl lasso preserves an existing selection outside its outline");

			// Live linking is immediate on mouse MOVE, not just when dragging ends.
			auto* liveSync=studio.findChild<QCheckBox*>(QStringLiteral("ALiS.Studio.LiveSync"));
			check(liveSync && !liveSync->isChecked(), "continuous synchronization is an explicit checkbox, initially off");
			studio.m_tool->setCurrentIndex(4); studio.rebuildPreview(); studio.setActiveCanvas(canvas);
			canvas->m_camera.center={{12,14,.4}}; canvas->m_camera.pixelsPerUnit=9;
			front->m_camera.center={{2,3,4}}; front->m_camera.pixelsPerUnit=3;
			auto camerasLinked=[&]()
			{
				for(auto* view:studio.m_canvases)
					if(view->camera().center!=studio.m_activeCanvas->camera().center || view->camera().pixelsPerUnit!=studio.m_activeCanvas->camera().pixelsPerUnit) return false;
				return true;
			};
			liveSync->setChecked(true);
			check(camerasLinked() && front->camera().pixelsPerUnit==9 && studio.m_activeCanvas==canvas, "enabling aligns immediately from the active panel");
			const auto liveRevision=studio.m_renderRevision; const auto liveSelected=studio.m_selected;
			const auto liveFrontRight=front->camera().right, liveOrbitRight=orbit->camera().right;
			QMouseEvent livePress(QEvent::MouseButtonPress,QPointF(100,100),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(canvas,&livePress);
			for(int n=1;n<=3;++n)
			{
				const auto oldCenter=front->camera().center;
				QMouseEvent liveMove(QEvent::MouseMove,QPointF(100+n*10,100+n*5),Qt::NoButton,Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(canvas,&liveMove);
				check(camerasLinked() && canvas->m_panning && front->camera().center!=oldCenter, "every move propagates before mouse release");
			}
			QMouseEvent liveRelease(QEvent::MouseButtonRelease,QPointF(130,115),Qt::LeftButton,Qt::NoButton,Qt::NoModifier); QApplication::sendEvent(canvas,&liveRelease);
			check(studio.m_renderRevision==liveRevision && studio.m_selected==liveSelected && front->camera().right==liveFrontRight && orbit->camera().right==liveOrbitRight, "live pan does not rescan points, alter labels/selection or rotate other panels");
			const double oldZoom=canvas->camera().pixelsPerUnit;
			QWheelEvent liveWheel(QPointF(90,70),QPointF(90,70),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false); QApplication::sendEvent(orbit,&liveWheel);
			check(camerasLinked() && studio.m_activeCanvas==orbit && canvas->camera().pixelsPerUnit>oldZoom && studio.m_renderRevision==liveRevision, "wheel changes source to any panel and propagates zoom immediately without point work");
			const auto unrotatedTop=canvas->camera().right; const auto orbitBefore=orbit->camera().right;
			drag(orbit,QPointF(100,100),QPointF(120,110),Qt::NoModifier);
			check(camerasLinked() && orbit->camera().right!=orbitBefore && canvas->camera().right==unrotatedTop && front->camera().right==liveFrontRight, "live 3D orbit preserves other orientations");
			canvas->setView(AnnotationView::Bottom);
			check(camerasLinked() && studio.m_activeCanvas==canvas && front->camera().view==AnnotationView::Front, "changing one view keeps linking but not its orientation");
			canvas->fit(); check(camerasLinked(), "local Fit maintains live centre and scale linking");
			studio.setActiveCanvas(front); studio.fitAllViews();
			check(camerasLinked() && studio.m_activeCanvas==front, "Fit all with linking uses the active panel, not the last panel in the loop");
			const auto beforeResize=front->camera(); canvas->resize(750,350); canvas->resizeGL(750,350);
			check(camerasLinked() && canvas->camera().pixelsPerUnit==beforeResize.pixelsPerUnit, "resizing linked panels preserves their common metric scale");
			studio.m_section->setChecked(true); studio.m_axis->setCurrentIndex(1); front->setView(AnnotationView::Front);
			const double centerBefore=front->camera().center[1]; studio.m_center->setValue(studio.m_center->value()+1);
			check(camerasLinked() && front->camera().center[1]>centerBefore && front->camera().center[1]==studio.localCenter(), "live section movement uses current controls before the preview timer fires");
			liveSync->setChecked(false); const auto independentFront=front->camera();
			drag(canvas,QPointF(100,100),QPointF(120,100),Qt::NoModifier); QApplication::sendEvent(canvas,&liveWheel);
			check(front->camera().center==independentFront.center && front->camera().pixelsPerUnit==independentFront.pixelsPerUnit && !camerasLinked(), "disabling restores independent pan and zoom immediately");
			studio.synchronizeViews(canvas);
			check(camerasLinked() && !liveSync->isChecked(), "local one-shot Sync still works without turning continuous mode on");
			check(studio.m_selected==liveSelected && cloud.size()==250000, "all synchronization operations leave original point IDs and selection unchanged");

			// Custom section placement is a camera-space gesture, never a selection.
			const auto savedSelection=studio.m_selected;
			const int savedAxis=studio.m_axis->currentIndex(); const double savedPosition=studio.m_center->value();
			studio.rebuildPreview(); const auto savedCount=studio.m_sliceCount;
			studio.m_drawSection->setChecked(true);
			if (!studio.m_placingSection || studio.m_sliceCount!=cloud.size() || studio.m_activeCanvas->camera().view!=AnnotationView::Top)
				std::cerr << "Placement state: " << studio.m_placingSection << ", scope=" << studio.m_sliceCount << ", total=" << cloud.size() << ", view=" << int(studio.m_activeCanvas->camera().view) << '\n';
			check(studio.m_placingSection && studio.m_sliceCount==cloud.size() && studio.m_activeCanvas->camera().view==AnnotationView::Top, "drawing opens full-resolution scope with a Top overview");
			studio.selectPolygon(0,1,rectangle);
			check(studio.m_selected==savedSelection && !studio.m_apply->isEnabled(), "placement cannot accidentally select or apply labels");
			check(!studio.placeSection({{1,2,0}},{{1,2,0}}) && studio.m_placingSection, "degenerate line rejected without changing section");
			drag(orbit,QPointF(50,50),QPointF(100,100),Qt::NoModifier);
			check(studio.m_placingSection && studio.m_axis->currentIndex()==savedAxis, "drawing in non-Top view cannot create an ambiguous plane");
			drag(canvas,QPointF(50,50),QPointF(51,51),Qt::NoModifier);
			check(studio.m_placingSection && studio.m_axis->currentIndex()==savedAxis, "short click stays in drawing mode without altering the plane");
			QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier); QApplication::sendEvent(canvas,&escape);
			check(!studio.m_placingSection && !studio.m_drawSection->isChecked() && studio.m_axis->currentIndex()==savedAxis && studio.m_center->value()==savedPosition && studio.m_sliceCount==savedCount && studio.m_selected==savedSelection, "Esc restores previous slab, position and selections");
			studio.m_drawSection->setChecked(true); studio.setActiveCanvas(canvas);
			canvas->m_camera.center={{25,25,.45}}; canvas->m_camera.pixelsPerUnit=5;
			const auto beforePlacementRevision=studio.m_renderRevision;
			drag(canvas,canvas->screen({{10,10,0}}),canvas->screen({{40,40,0}}),Qt::ControlModifier);
			check(!studio.m_placingSection && studio.m_axis->currentIndex()==3 && studio.m_section->isChecked() && studio.m_tool->isEnabled(), "Top drag creates custom plane and restores the prior selection tool");
			check(studio.m_renderRevision==beforePlacementRevision+1, "successful placement rebuilds the preview only once");
			check(std::abs(studio.m_direction->value()-45)<1e-6 && std::abs(studio.m_anchorX->value()-440012.5)<1e-6 && std::abs(studio.m_anchorY->value()-4135012.5)<1e-6, "drawn midpoint and angle use global shift/scale correctly");
			check(studio.m_center->value()==0 && std::abs(studio.localCenter())<1e-8 && studio.m_selected==savedSelection, "custom offset starts at anchor and retains all selected original IDs");
			check(front->camera().view==AnnotationView::SectionFront && std::abs(annotationDot(front->camera().toward,studio.m_sectionNormal)+1)<1e-12, "different panel automatically faces the drawn section");
			const auto originalIds=studio.m_index.ids();
			auto exactSlab=[&]()
			{
				std::size_t count=0; bool same=true;
				const double angle=studio.m_direction->value()*3.14159265358979323846/180;
				for(unsigned i=0;i<cloud.size();++i)
				{
					const auto* p=cloud.getPoint(i);
					const double gx=double(p->x)/2+440000, gy=double(p->y)/2+4135000;
					const double d=-(gx-studio.m_anchorX->value())*std::sin(angle)+(gy-studio.m_anchorY->value())*std::cos(angle)-studio.m_center->value();
					const bool expected=std::abs(d)<=studio.m_thickness->value()*.5;
					if(expected)++count; if(studio.m_selected[i]!=expected)same=false;
				}
				return same && count==studio.m_sliceCount && count==studio.m_selectedCount;
			};
			studio.m_thickness->setValue(.723); studio.selectPolygon(0,1,rectangle);
			check(exactSlab(), "rotated slab selection matches independent global-coordinate brute force at full resolution");
			studio.m_center->setValue(1.237); studio.selectPolygon(0,1,rectangle);
			check(exactSlab() && studio.m_index.ids()==originalIds, "metric perpendicular movement reuses the index and selects the exact new slab");
			studio.m_slider->setValue(4000); studio.selectPolygon(0,1,rectangle);
			check(exactSlab() && studio.m_index.ids()==originalIds, "custom slider maps projected bounds to signed offset without reindexing");
			studio.m_anchorY->setValue(studio.m_anchorY->value()+2.123); studio.selectPolygon(0,1,rectangle);
			check(exactSlab() && studio.m_center->value()==0 && studio.m_index.ids()==originalIds, "numeric anchor change resets offset without rebuilding the index");
			studio.m_direction->setValue(-28.317); studio.selectPolygon(0,1,rectangle);
			check(exactSlab() && studio.m_index.ids()!=originalIds, "numeric direction change rebuilds projected index and exact slab");
			front->setView(AnnotationView::SectionBack);
			check(std::abs(annotationDot(front->camera().toward,studio.m_sectionNormal)-1)<1e-12 && std::abs(annotationDot(front->camera().right,front->camera().up))<1e-12 && std::abs(annotationDot(front->camera().right,front->camera().toward))<1e-12, "custom back view is orthonormal and looks from the opposite side");
			studio.setActiveCanvas(front); liveSync->setChecked(true); studio.m_center->setValue(.531);
			check(camerasLinked() && std::abs(annotationDot(front->camera().center,studio.m_sectionNormal)-studio.localCenter())<1e-9, "live sync follows custom section normal immediately, not a coordinate axis");
			liveSync->setChecked(false);
			studio.m_axis->setCurrentIndex(0); studio.rebuildPreview();
			check(studio.m_indexAxis==0 && studio.m_sectionNormal==AnnotationVector({{1,0,0}}) && front->camera().toward[0]==-1 && std::abs(studio.localCenter()-24.95)<.003, "standard X section remains available after drawing a custom one");
			studio.m_axis->setCurrentIndex(3);
			check(std::abs(studio.m_direction->value()+28.317)<1e-6 && studio.m_center->value()==0, "custom anchor and direction survive switching to presets and back");
			studio.m_section->setChecked(false); studio.m_drawSection->setChecked(true); studio.m_drawSection->setChecked(false);
			check(!studio.m_section->isChecked() && !studio.m_placingSection && studio.m_sliceCount==cloud.size(), "cancelling placement also restores an unclipped original scope");
			return checks;
		}
	};
}
int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	try { std::cout << "Annotation Studio tests passed (" << alis::AnnotationStudioTestAccess::run() << " checks)\n"; }
	catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
	return 0;
}
