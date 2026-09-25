// SPDX-License-Identifier: GPL-2.0-or-later
#include "AnnotationStudio.h"
#include "AnnotationCanvas.h"
#include "WorkspaceLogic.h"
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <limits>

namespace alis
{
	class AnnotationCloudObserver final : public ccHObject
	{
	public:
		AnnotationCloudObserver(ccPointCloud* cloud, AnnotationStudio* studio) : m_cloud(cloud), m_studio(studio)
		{ cloud->addDependency(this, DP_NOTIFY_OTHER_ON_DELETE | DP_NOTIFY_OTHER_ON_UPDATE); }
		~AnnotationCloudObserver() override { if (m_cloud) m_cloud->removeDependencyWith(this); }
	protected:
		void onDeletionOf(const ccHObject* object) override
		{
			ccHObject::onDeletionOf(object);
			if (object == m_cloud) { m_cloud = nullptr; m_studio->detachCloud(); }
		}
		void onUpdateOf(ccHObject* object) override
		{ if (object == m_cloud) m_studio->detachCloud(); }
	private:
		ccPointCloud* m_cloud;
		AnnotationStudio* m_studio;
	};

	AnnotationStudio::AnnotationStudio(ccPointCloud* cloud, QWidget* parent)
		: QDialog(parent, Qt::Window), m_cloud(cloud), m_sourceCount(cloud->size()), m_selected(cloud->size(), false), m_scale(cloud->getGlobalScale())
	{
		setAttribute(Qt::WA_DeleteOnClose); setObjectName(QStringLiteral("ALiS.AnnotationStudio"));
		setWindowTitle(QStringLiteral("ALiS — Annotation Studio — ") + cloud->getName()); resize(1280, 760);
		setWindowIcon(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/annotator.svg")));
		m_observer.reset(new AnnotationCloudObserver(cloud, this));
		const auto box = cloud->getOwnBB();
		for (int axis = 0; axis < 3; ++axis) { m_min[axis] = box.minCorner().u[axis]; m_max[axis] = box.maxCorner().u[axis]; }
		auto* root = new QVBoxLayout(this);
		auto* note = new QLabel(QStringLiteral("Same original cloud in all views • Yellow = shared selection • Display LOD only: labels use full-resolution points.\nSelections pass through the active slab. Moving the section preserves selected points, including those outside the current slab."));
		note->setWordWrap(true); note->setStyleSheet(QStringLiteral("background:#e0f2fe;color:#0c4a6e;padding:7px;")); root->addWidget(note);
		auto* row = new QHBoxLayout;
		m_section = new QCheckBox(QStringLiteral("Live section")); m_section->setChecked(true); row->addWidget(m_section);
		m_axis = new QComboBox; m_axis->addItems({QStringLiteral("X normal / YZ"), QStringLiteral("Y normal / XZ"), QStringLiteral("Z normal / XY"), QStringLiteral("Custom vertical plane")}); m_axis->setCurrentIndex(1); row->addWidget(m_axis);
		m_drawSection = new QPushButton(QStringLiteral("Draw section (Top)")); m_drawSection->setCheckable(true);
		m_drawSection->setObjectName(QStringLiteral("ALiS.Studio.DrawSection")); row->addWidget(m_drawSection);
		auto spin = [](const QString& name, double value, double minimum, double maximum)
		{ auto* result = new QDoubleSpinBox; result->setObjectName(name); result->setDecimals(3); result->setRange(minimum, maximum); result->setValue(value); result->setKeyboardTracking(false); return result; };
		m_thickness = spin(QStringLiteral("ALiS.Studio.Thickness"), 5, .001, 1000000);
		m_step = spin(QStringLiteral("ALiS.Studio.Step"), 1, .001, 1000000);
		m_center = spin(QStringLiteral("ALiS.Studio.Center"), 0, -1e12, 1e12);
		row->addWidget(new QLabel(QStringLiteral("Thickness (m)"))); row->addWidget(m_thickness);
		row->addWidget(new QLabel(QStringLiteral("Step (m)"))); row->addWidget(m_step);
		root->addLayout(row);
		m_customControls = new QWidget; auto* customRow = new QHBoxLayout(m_customControls); customRow->setContentsMargins(0,0,0,0);
		m_anchorX = spin(QStringLiteral("ALiS.Studio.AnchorX"), (m_min[0]+m_max[0])*.5/m_scale-cloud->getGlobalShift().x, -1e12, 1e12);
		m_anchorY = spin(QStringLiteral("ALiS.Studio.AnchorY"), (m_min[1]+m_max[1])*.5/m_scale-cloud->getGlobalShift().y, -1e12, 1e12);
		m_anchorX->setDecimals(6); m_anchorY->setDecimals(6);
		m_direction = spin(QStringLiteral("ALiS.Studio.Direction"), 0, -180, 180); m_direction->setDecimals(6);
		customRow->addWidget(new QLabel(QStringLiteral("Anchor global X"))); customRow->addWidget(m_anchorX);
		customRow->addWidget(new QLabel(QStringLiteral("Y"))); customRow->addWidget(m_anchorY);
		customRow->addWidget(new QLabel(QStringLiteral("Direction from +X (°)"))); customRow->addWidget(m_direction); customRow->addStretch();
		root->addWidget(m_customControls); m_customControls->hide();
		row = new QHBoxLayout;
		auto* back = new QPushButton(QStringLiteral("◀ Step")); auto* next = new QPushButton(QStringLiteral("Step ▶"));
		m_slider = new QSlider(Qt::Horizontal); m_slider->setRange(0, 10000); m_slider->setObjectName(QStringLiteral("ALiS.Studio.Position"));
		m_positionLabel = new QLabel(QStringLiteral("Global coordinate"));
		row->addWidget(back); row->addWidget(m_slider, 1); row->addWidget(next); row->addWidget(m_positionLabel); row->addWidget(m_center); root->addLayout(row);
		m_sectionHint = new QLabel(QStringLiteral("Use X/Y/Z, or Draw section to position a vertical plane freely on the Top view.")); m_sectionHint->setWordWrap(true); root->addWidget(m_sectionHint);
		m_section->setToolTip(QStringLiteral("Constrain all four views and NEW selections to a slab. Existing selections persist until Clear or Replace. Disable to see/select through the whole cloud. Native CloudCompare hidden points are excluded from new selections."));
		m_axis->setToolTip(QStringLiteral("X/Y/Z retain the standard axis-aligned sections. Custom vertical plane lets you choose any horizontal direction and anchor X/Y, by drawing or entering coordinates. It is vertical, not an arbitrarily tilted 3D plane. Changing orientation rebuilds an index of original point IDs; the cloud is not copied."));
		m_drawSection->setToolTip(QStringLiteral("Click, then drag a line in a Top/XY panel. The whole cloud is shown temporarily so you can locate the plane. The line defines a VERTICAL plane through its midpoint, extended across the cloud (its length does not crop points). Release to apply. Esc or this button again cancels and restores the previous section. Labels and selections are untouched. Blue guides show the centre and slab edges. Use Section front/back in a panel to face the new plane."));
		m_anchorX->setToolTip(QStringLiteral("Global X of a point on the custom vertical plane at offset 0. Global shift/scale are handled automatically. Editing the anchor resets the offset to 0; it does not rebuild the point index."));
		m_anchorY->setToolTip(QStringLiteral("Global Y of the same anchor point. Z is unrestricted because the plane is vertical. Drawing uses the midpoint of your line as anchor."));
		m_direction->setToolTip(QStringLiteral("Direction along the section line in the XY plane: 0° = +X, 90° = +Y, counterclockwise. This is NOT north-clockwise azimuth. Positive Step moves to the LEFT of this direction, normal (-sin angle, cos angle, 0). Editing direction rebuilds the point index once; slider/Step reuse it."));
		m_thickness->setToolTip(QStringLiteral("Total slab thickness in metres, centred on the global coordinate. Start with 1–5 m for structures, then narrow it to separate overlapping surfaces."));
		m_step->setToolTip(QStringLiteral("Movement in metres per Step button. The slider can move continuously; updates are coalesced to keep the UI responsive."));
		m_center->setToolTip(QStringLiteral("For X/Y/Z: original global coordinate on that axis. For Custom: signed perpendicular offset in metres from the anchor, 0 = through the anchor. CloudCompare global shift and scale are accounted for. Moving the plane reuses the index and retains existing selections."));
		row = new QHBoxLayout;
		m_tool = new QComboBox; m_tool->addItems({QStringLiteral("Rectangle"), QStringLiteral("Polygon"), QStringLiteral("Lasso"), QStringLiteral("Wand: radius + SF"), QStringLiteral("Navigate / rotate")});
		m_mode = new QComboBox; m_mode->addItems({QStringLiteral("Replace selection"), QStringLiteral("Add"), QStringLiteral("Subtract")});
		auto* clear = new QPushButton(QStringLiteral("Clear selection")); auto* fit = new QPushButton(QStringLiteral("Fit all views"));
		m_liveSync = new QCheckBox(QStringLiteral("Live sync"));
		m_liveSync->setObjectName(QStringLiteral("ALiS.Studio.LiveSync"));
		const QString syncHelp = QStringLiteral("Align all panels once from THIS panel: copy its 3D centre and scale (pixels/metre), preserving each orientation. For a section-facing source, the hidden depth is the current section centre. For continuous alignment, enable Live sync above.");
		m_liveSync->setToolTip(QStringLiteral("Continuously link the four panels' 3D centre and scale (pixels/metre). Enabling aligns them immediately from the active panel (blue border). Pan, wheel zoom and Fit propagate during the gesture, without waiting for mouse release. Any panel you navigate becomes the source. Top/front/side and 3D orientations remain independent; rotating 3D does not rotate the other panels. Uncheck to navigate independently again. Only cameras are updated: no cloud resampling or label changes."));
		fit->setToolTip(QStringLiteral("With Live sync off: fit each view independently. With Live sync on: fit the active panel and copy its centre/scale to the others; different orientations may show different extents."));
		m_activeViewLabel = new QLabel;
		m_mode->setToolTip(QStringLiteral("Replace starts a new selection. Add accumulates; Subtract removes. Hold Ctrl when starting any selection to temporarily ADD, including across views or sections. Release Ctrl for the next gesture to use the menu mode again."));
		row->addWidget(m_tool); row->addWidget(m_mode); row->addWidget(clear); row->addWidget(fit); row->addWidget(m_liveSync); row->addWidget(m_activeViewLabel); row->addStretch(); root->addLayout(row);
		row = new QHBoxLayout;
		m_wandRadius = spin(QStringLiteral("ALiS.Studio.WandRadius"), 1, .001, 1000000);
		m_wandTolerance = spin(QStringLiteral("ALiS.Studio.WandTolerance"), .1, 0, 1e12);
		m_wandScalar = new QCheckBox(QStringLiteral("Also match displayed SF ±"));
		row->addWidget(new QLabel(QStringLiteral("Wand 3D radius (m)"))); row->addWidget(m_wandRadius); row->addWidget(m_wandScalar); row->addWidget(m_wandTolerance); row->addStretch(); root->addLayout(row);
		m_tool->setToolTip(QStringLiteral("Rectangle: drag. Polygon: click vertices, double-click to finish. Lasso: freehand drag. Ctrl at the start temporarily adds. Wand: click a rendered seed; selects ORIGINAL points within a 3D radius and optional scalar tolerance (not connected region growing). Navigate: left drag rotates 3D or pans 2D, without changing selection. Right drag always rotates 3D/pans 2D; middle drag pans; wheel zooms."));
		m_wandScalar->setToolTip(QStringLiteral("Uses the currently displayed CloudCompare scalar field and its raw numeric units, not palette colours. NaN seeds/values are excluded. If no field is shown the operation is refused."));
		m_wandTolerance->setToolTip(QStringLiteral("Maximum absolute difference from the seed scalar value. For class codes use 0 for exact matching; for HAG use a tolerance in metres. No normal/colour region-growing is implied."));
		auto* split = new QSplitter(Qt::Vertical); root->addWidget(split, 1);
		const AnnotationView defaults[] = {AnnotationView::Top, AnnotationView::Front, AnnotationView::Right, AnnotationView::Orbit3D};
		const QStringList viewNames = {QStringLiteral("Top / XY"), QStringLiteral("Bottom / XY"), QStringLiteral("Front / XZ"), QStringLiteral("Back / XZ"), QStringLiteral("Right / YZ"), QStringLiteral("Left / YZ"), QStringLiteral("Section / front"), QStringLiteral("Section / back"), QStringLiteral("3D / orbit")};
		for (int r = 0; r < 2; ++r)
		{
			auto* columns = new QSplitter(Qt::Horizontal); split->addWidget(columns);
			for (int c = 0; c < 2; ++c)
			{
				const int panel = r * 2 + c + 1;
				auto* container = new QWidget; auto* layout = new QVBoxLayout(container); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(2);
				auto* header = new QHBoxLayout; auto* menu = new QComboBox;
				menu->setObjectName(QStringLiteral("ALiS.Studio.Panel.%1.View").arg(panel));
				for (int i = 0; i < viewNames.size(); ++i) menu->addItem(viewNames[i], i);
				menu->setCurrentIndex(int(defaults[panel-1]));
				menu->setToolTip(QStringLiteral("Independent orientation for this panel. Section front/back follows the live section axis. 3D: right drag rotates, middle drag pans, wheel zooms. All panels share the same original points and selection; the display alone uses LOD."));
				auto* localFit = new QPushButton(QStringLiteral("Fit")); auto* localSync = new QPushButton(QStringLiteral("Sync"));
				localFit->setToolTip(QStringLiteral("Fit this panel to the whole cloud's bounds without changing its orientation. If Live sync is enabled, the other panels follow its centre and scale.")); localSync->setToolTip(syncHelp);
				header->addWidget(menu); header->addStretch(); header->addWidget(new QLabel(QStringLiteral("%1").arg(panel))); header->addWidget(localFit); header->addWidget(localSync); layout->addLayout(header);
				auto* canvas = new AnnotationCanvas(this, panel, defaults[panel-1]); m_canvases.push_back(canvas); layout->addWidget(canvas, 1); columns->addWidget(container);
				connect(menu, qOverload<int>(&QComboBox::currentIndexChanged), this, [canvas, menu](int) { canvas->setView(static_cast<AnnotationView>(menu->currentData().toInt())); });
				connect(localFit, &QPushButton::clicked, this, [this, canvas]() { setActiveCanvas(canvas); canvas->fit(); });
				connect(localSync, &QPushButton::clicked, this, [this, canvas]() { synchronizeViews(canvas); });
			}
			columns->setStretchFactor(0, 1); columns->setStretchFactor(1, 1);
		}
		split->setStretchFactor(0, 1); split->setStretchFactor(1, 1); setActiveCanvas(m_canvases.front());
		row = new QHBoxLayout;
		m_class = new QComboBox;
		for (const auto& cls : asprsClasses()) if (!cls.reserved) m_class->addItem(QStringLiteral("ASPRS %1 — %2").arg(cls.code).arg(QString::fromUtf8(cls.name)), int(cls.code));
		m_class->insertSeparator(m_class->count());
		for (const auto& cls : archaeologyAsprsClasses())
			m_class->addItem(QStringLiteral("qAL Other %1 — %2").arg(cls.code).arg(QString::fromUtf8(cls.name)), int(cls.code));
		m_class->setCurrentIndex(m_class->findData(2));
		m_apply = new QPushButton(QStringLiteral("Assign to selected points")); m_apply->setObjectName(QStringLiteral("ALiS.Studio.Apply"));
		m_undo = new QPushButton(QStringLiteral("Undo")); m_redo = new QPushButton(QStringLiteral("Redo"));
		row->addWidget(m_class, 1); row->addWidget(m_apply); row->addWidget(m_undo); row->addWidget(m_redo); root->addLayout(row);
		m_apply->setToolTip(QStringLiteral("Assigns one ASPRS/LAS class per undoable operation to ALL selected original point IDs, including selections retained outside the current section. Archaeological categories use the ALiS user-defined profile (codes 64–75). The result becomes Manual/Trusted training data. Source LAS is not overwritten: save the cloud in CloudCompare."));
		m_counts = new QLabel; m_counts->setWordWrap(true); root->addWidget(m_counts);
		m_previewTimer = new QTimer(this); m_previewTimer->setSingleShot(true); m_previewTimer->setInterval(40);
		connect(m_previewTimer, &QTimer::timeout, this, &AnnotationStudio::rebuildPreview);
		connect(m_axis, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { rebuildIndex(); });
		connect(m_drawSection, &QPushButton::toggled, this, [this](bool enabled) { setSectionPlacement(enabled); });
		connect(m_direction, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { if (m_axis->currentIndex()==3) rebuildIndex(); });
		for (auto* anchor : {m_anchorX,m_anchorY}) connect(anchor, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double)
		{ if (m_axis->currentIndex()==3) { updatePositionControls(); schedulePreview(); } });
		connect(m_section, &QCheckBox::toggled, this, [this](bool) { schedulePreview(); });
		connect(m_thickness, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { schedulePreview(); });
		connect(m_center, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double)
		{
			const QSignalBlocker blocker(m_slider);
			m_slider->setValue(qRound(10000 * (localCenter() - m_sectionMin) / std::max(1e-9, m_sectionMax - m_sectionMin))); schedulePreview();
		});
		connect(m_slider, &QSlider::valueChanged, this, [this](int value)
		{ if (m_cloud) m_center->setValue((m_sectionMin + (m_sectionMax - m_sectionMin) * value / 10000.0 - sectionOrigin()) / m_scale); });
		connect(back, &QPushButton::clicked, this, [this]() { if (!m_placingSection) m_center->setValue(m_center->value() - m_step->value()); });
		connect(next, &QPushButton::clicked, this, [this]() { if (!m_placingSection) m_center->setValue(m_center->value() + m_step->value()); });
		connect(clear, &QPushButton::clicked, this, [this]() { std::fill(m_selected.begin(), m_selected.end(), false); m_selectedCount = 0; rebuildPreview(); });
		connect(fit, &QPushButton::clicked, this, &AnnotationStudio::fitAllViews);
		connect(m_liveSync, &QCheckBox::toggled, this, [this, fit](bool enabled)
		{
			fit->setText(enabled ? QStringLiteral("Fit active + sync") : QStringLiteral("Fit all views"));
			if (enabled) synchronizeViews(m_activeCanvas);
		});
		connect(m_tool, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int tool)
		{ for (auto* canvas : m_canvases) canvas->cancelPath(); m_wandRadius->setEnabled(tool == 3); m_wandScalar->setEnabled(tool == 3); m_wandTolerance->setEnabled(tool == 3); });
		m_wandRadius->setEnabled(false); m_wandScalar->setEnabled(false); m_wandTolerance->setEnabled(false);
		connect(m_undo, &QPushButton::clicked, this, [this]() { Q_EMIT historyRequested(false); });
		connect(m_redo, &QPushButton::clicked, this, [this]() { Q_EMIT historyRequested(true); });
		connect(m_apply, &QPushButton::clicked, this, [this]()
		{
			if (!m_cloud || !m_selectedCount) return;
			if (QMessageBox::question(this, QStringLiteral("Assign trusted labels"), QStringLiteral("Assign %1 to %2 selected original points?\nThis includes selections retained outside the current section. Undo is available. The source LAS file is not overwritten.").arg(m_class->currentText()).arg(qulonglong(m_selectedCount))) != QMessageBox::Yes) return;
			std::vector<unsigned> ids; ids.reserve(m_selectedCount);
			for (unsigned i = 0; i < m_sourceCount; ++i) if (m_selected[i]) ids.push_back(i);
			const int code = m_class->currentData().toInt(); Q_EMIT labelsRequested(ids, code, -1);
		});
		setHistoryAvailable(false, false); rebuildIndex();
	}

	AnnotationStudio::~AnnotationStudio() = default;
	void AnnotationStudio::setActiveCanvas(AnnotationCanvas* canvas)
	{
		if (!canvas || std::find(m_canvases.begin(), m_canvases.end(), canvas) == m_canvases.end()) return;
		m_activeCanvas = canvas;
		m_activeViewLabel->setText(QStringLiteral("Active: %1").arg(canvas->panelNumber()));
		for (auto* view : m_canvases) view->invalidate();
	}
	void AnnotationStudio::synchronizeViews(AnnotationCanvas* source)
	{
		if (!source || !m_cloud || std::find(m_canvases.begin(), m_canvases.end(), source) == m_canvases.end()) return;
		setActiveCanvas(source);
		// Normalize the source's ambiguous hidden depth before copying it.
		source->synchronizeFrom(*source);
		for (auto* view : m_canvases) if (view != source) view->synchronizeFrom(*source);
	}
	void AnnotationStudio::cameraChanged(AnnotationCanvas* source)
	{
		// Receivers do not publish camera changes: no feedback loop or point scan.
		// QWidget::update coalesces painting while every input event updates cameras.
		if (m_liveSync && m_liveSync->isChecked()) synchronizeViews(source);
	}
	void AnnotationStudio::fitAllViews()
	{
		if (m_liveSync->isChecked() && m_activeCanvas) m_activeCanvas->fit();
		else for (auto* canvas : m_canvases) canvas->fit();
	}
	void AnnotationStudio::detachCloud() { m_cloud = nullptr; if (m_previewTimer) m_previewTimer->stop(); setEnabled(false); close(); }
	void AnnotationStudio::setHistoryAvailable(bool undo, bool redo) { m_undo->setEnabled(undo); m_redo->setEnabled(redo); }
	double AnnotationStudio::sectionOrigin() const
	{
		if (!m_cloud) return 0;
		const auto& shift = m_cloud->getGlobalShift();
		if (m_indexAxis == 3) return ((m_anchorX->value()+shift.x)*m_sectionNormal[0] + (m_anchorY->value()+shift.y)*m_sectionNormal[1])*m_scale;
		return m_indexAxis >= 0 ? shift.u[m_indexAxis]*m_scale : 0;
	}
	double AnnotationStudio::localCenter() const
	{ return m_cloud ? m_center->value()*m_scale + sectionOrigin() : 0; }
	double AnnotationStudio::sectionCoordinate(unsigned id) const
	{
		const auto* p = m_cloud->getPoint(id);
		return m_indexAxis == 3 ? double(p->x)*m_sectionNormal[0] + double(p->y)*m_sectionNormal[1] : p->u[m_indexAxis];
	}
	void AnnotationStudio::updatePositionControls()
	{
		const QSignalBlocker centerBlock(m_center), sliderBlock(m_slider);
		const double origin = sectionOrigin();
		double lower = (m_sectionMin-origin)/m_scale, upper = (m_sectionMax-origin)/m_scale;
		if (m_indexAxis == 3) { lower = std::min(0.0,lower); upper = std::max(0.0,upper); }
		m_center->setRange(lower,upper);
		m_center->setValue(m_indexAxis == 3 ? 0 : (m_sectionMin+m_sectionMax)*.5/m_scale-origin/m_scale);
		m_slider->setValue(qRound(10000*(localCenter()-m_sectionMin)/std::max(1e-9,m_sectionMax-m_sectionMin)));
		m_positionLabel->setText(m_indexAxis == 3 ? QStringLiteral("Offset (m)") : QStringLiteral("Global coordinate"));
	}
	void AnnotationStudio::setSectionPlacement(bool enabled, bool refresh)
	{
		if (!m_cloud) return;
		m_placingSection = enabled;
		for (auto* canvas : m_canvases) canvas->cancelPath();
		m_axis->setEnabled(!enabled); m_customControls->setEnabled(!enabled); m_section->setEnabled(!enabled);
		m_center->setEnabled(!enabled); m_slider->setEnabled(!enabled); m_thickness->setEnabled(!enabled); m_step->setEnabled(!enabled); m_tool->setEnabled(!enabled);
		m_drawSection->setText(enabled ? QStringLiteral("Cancel drawing (Esc)") : QStringLiteral("Draw section (Top)"));
		m_sectionHint->setText(enabled ? QStringLiteral("DRAW: drag a line in Top / XY. Whole-cloud overview; blue line defines a vertical plane. Esc cancels. No labels are changed.")
			: QStringLiteral("X/Y/Z or a custom vertical plane. Section front/back faces the plane; slider/Step move it perpendicular to its direction."));
		if (enabled)
		{
			auto found = std::find_if(m_canvases.begin(),m_canvases.end(),[](AnnotationCanvas* c) { return c->camera().view==AnnotationView::Top; });
			auto* canvas = found==m_canvases.end() ? m_canvases.front() : *found;
			if (found==m_canvases.end()) findChild<QComboBox*>(QStringLiteral("ALiS.Studio.Panel.%1.View").arg(canvas->panelNumber()))->setCurrentIndex(int(AnnotationView::Top));
			// Also cover programmatic camera changes that did not change the menu.
			if (canvas->camera().view!=AnnotationView::Top) canvas->setView(AnnotationView::Top);
			setActiveCanvas(canvas); canvas->setFocus();
		}
		if (refresh) rebuildPreview();
	}
	bool AnnotationStudio::placeSection(const AnnotationVector& first, const AnnotationVector& last)
	{
		if (!m_cloud || !m_placingSection) return false;
		const double dx=last[0]-first[0], dy=last[1]-first[1];
		if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx,dy)<1e-8) return false;
		{
			const QSignalBlocker xBlock(m_anchorX), yBlock(m_anchorY), angleBlock(m_direction), axisBlock(m_axis), drawBlock(m_drawSection), sectionBlock(m_section);
			m_anchorX->setValue((first[0]+last[0])*.5/m_scale-m_cloud->getGlobalShift().x);
			m_anchorY->setValue((first[1]+last[1])*.5/m_scale-m_cloud->getGlobalShift().y);
			m_direction->setValue(std::atan2(dy,dx)*180/3.14159265358979323846);
			m_axis->setCurrentIndex(3); m_drawSection->setChecked(false); m_section->setChecked(true);
		}
		// Leave drawing without a redundant full-cloud preview; one index rebuild.
		m_placingSection = false;
		rebuildIndex();
		if (!m_cloud) return true;
		setSectionPlacement(false, false);
		// Keep the map panel; show the new section's face in a different panel.
		const int target = m_activeCanvas==m_canvases[1] ? 1 : 2;
		findChild<QComboBox*>(QStringLiteral("ALiS.Studio.Panel.%1.View").arg(target))->setCurrentIndex(int(AnnotationView::SectionFront));
		return true;
	}
	void AnnotationStudio::schedulePreview()
	{
		for (auto* canvas : m_canvases) canvas->cancelPath();
		cameraChanged(m_activeCanvas);
		// Throttle, not debounce: continuous slider movement must still render.
		if (!m_previewTimer->isActive()) m_previewTimer->start();
	}
	void AnnotationStudio::rebuildIndex()
	{
		if (!m_cloud) return;
		const int axis = m_axis->currentIndex();
		m_indexAxis = axis;
		m_sectionNormal = {{0,0,0}};
		if (axis == 3)
		{
			const double radians = m_direction->value()*3.14159265358979323846/180;
			m_sectionNormal = {{-std::sin(radians),std::cos(radians),0}};
		}
		else m_sectionNormal[axis] = 1;
		m_sectionMin=0; m_sectionMax=0;
		for (int i=0;i<3;++i)
		{
			m_sectionMin += m_sectionNormal[i]*(m_sectionNormal[i]>=0 ? m_min[i] : m_max[i]);
			m_sectionMax += m_sectionNormal[i]*(m_sectionNormal[i]>=0 ? m_max[i] : m_min[i]);
		}
		setCursor(Qt::WaitCursor);
		try { m_index.build(m_sourceCount, m_sectionMin, m_sectionMax, [this](unsigned i) { return sectionCoordinate(i); }); }
		catch (const std::bad_alloc&) { unsetCursor(); QMessageBox::warning(this, QStringLiteral("Annotation"), QStringLiteral("Not enough memory for the section index. Close other tools or use a smaller cloud.")); detachCloud(); return; }
		m_customControls->setVisible(axis==3);
		for (auto* canvas : m_canvases) canvas->updateSectionView();
		updatePositionControls();
		cameraChanged(m_activeCanvas);
		unsetCursor(); rebuildPreview();
	}
	std::pair<std::size_t, std::size_t> AnnotationStudio::candidateRange() const
	{
		if (!m_section->isChecked() || m_placingSection) return {0, m_index.ids().size()};
		const double half = m_thickness->value() * m_scale * .5;
		return m_index.range(localCenter() - half, localCenter() + half);
	}
	bool AnnotationStudio::eligible(unsigned id) const
	{
		const auto& visibility = m_cloud->getTheVisibilityArray();
		if (visibility.size() == m_sourceCount && visibility[id] != CCCoreLib::POINT_VISIBLE) return false;
		const auto* p = m_cloud->getPoint(id);
		if (!std::isfinite(p->x) || !std::isfinite(p->y) || !std::isfinite(p->z)) return false;
		return !m_activeSection || std::abs(sectionCoordinate(id) - m_activeCenter) <= m_activeHalf;
	}
	QRgb AnnotationStudio::pointColour(unsigned id) const
	{
		if (m_cloud->sfShown())
		{
			const auto* sf = m_cloud->getCurrentDisplayedScalarField(); const auto* c = sf ? sf->getValueColor(id) : nullptr;
			return c ? qRgb(c->r, c->g, c->b) : qRgb(120, 120, 120);
		}
		if (m_cloud->hasColors()) { const auto& c = m_cloud->getPointColor(id); return qRgb(c.r, c.g, c.b); }
		return qRgb(180, 210, 230);
	}
	void AnnotationStudio::rebuildPreview()
	{
		if (!m_cloud || m_cloud->size() != m_sourceCount || m_indexAxis < 0) return;
		m_previewTimer->stop(); m_renderPoints.clear(); m_sliceCount = 0;
		m_activeSection = m_section->isChecked() && !m_placingSection; m_activeCenter = localCenter(); m_activeHalf = m_thickness->value() * m_scale * .5;
		const auto range = candidateRange();
		const std::size_t stride = std::max<std::size_t>(1, (range.second - range.first + 119999) / 120000);
		const std::size_t selectedStride = std::max<std::size_t>(1, (m_selectedCount + 9999) / 10000);
		std::size_t chosen = 0;
		for (std::size_t at = range.first; at < range.second; ++at)
		{
			const unsigned id = m_index.ids()[at]; if (!eligible(id)) continue;
			++m_sliceCount;
			const bool highlight = m_selected[id] && chosen++ % selectedStride == 0;
			if (at % stride != 0 && !highlight) continue;
			const auto* p = m_cloud->getPoint(id); m_renderPoints.push_back({{p->x, p->y, p->z}, pointColour(id), id});
		}
		++m_renderRevision;
		for (auto* canvas : m_canvases) canvas->invalidate(); updateCounts();
	}
	void AnnotationStudio::refreshColours()
	{
		if (!m_cloud) return;
		// A mask may have changed in the host; refresh scope as well as colour.
		schedulePreview();
	}
	void AnnotationStudio::updateCounts()
	{
		m_counts->setText(QStringLiteral("%1 selected (all sections)  |  %2 original points in current slab/scope  |  %3 display samples  |  %4 original total.\nNo source point is decimated. Labels are saved with the cloud via CloudCompare; selection outlines are temporary.")
			.arg(qulonglong(m_selectedCount)).arg(qulonglong(m_sliceCount)).arg(m_renderPoints.size()).arg(m_sourceCount));
		m_apply->setEnabled(m_selectedCount > 0 && !m_placingSection);
	}
	void AnnotationStudio::selectMatching(const std::function<bool(unsigned)>& predicate, int selectionMode)
	{
		if (!m_cloud || m_cloud->size() != m_sourceCount || m_placingSection) return;
		m_activeSection = m_section->isChecked(); m_activeCenter = localCenter(); m_activeHalf = m_thickness->value() * m_scale * .5;
		setCursor(Qt::WaitCursor);
		const int mode = selectionMode >= 0 ? selectionMode : m_mode->currentIndex();
		if (mode == 0) { std::fill(m_selected.begin(), m_selected.end(), false); m_selectedCount = 0; }
		const auto range = candidateRange();
		for (std::size_t at = range.first; at < range.second; ++at)
		{
			const unsigned id = m_index.ids()[at];
			if (!eligible(id) || !predicate(id)) continue;
			const bool value = mode != 2;
			if (m_selected[id] != value) { m_selected[id] = value; if (value) ++m_selectedCount; else --m_selectedCount; }
		}
		unsetCursor(); rebuildPreview();
	}
	void AnnotationStudio::selectPolygon(int horizontal, int vertical, const QPolygonF& polygon)
	{
		AnnotationPolygon points; for (const auto& point : polygon) points.emplace_back(point.x(), point.y());
		const auto bounds = polygon.boundingRect();
		selectMatching([this, horizontal, vertical, &points, bounds](unsigned id)
		{ const auto* p = m_cloud->getPoint(id); return bounds.contains(p->u[horizontal], p->u[vertical]) && annotationContains(points, p->u[horizontal], p->u[vertical]); });
	}
	void AnnotationStudio::selectProjectedPolygon(const AnnotationCamera& camera, const QPolygonF& polygon, int selectionMode)
	{
		AnnotationPolygon points; for (const auto& point : polygon) points.emplace_back(point.x(), point.y());
		const auto bounds = polygon.boundingRect();
		selectMatching([this, camera, &points, bounds](unsigned id)
		{
			const auto* p = m_cloud->getPoint(id); const auto q = camera.project({{p->x, p->y, p->z}});
			return bounds.contains(q.first, q.second) && annotationContains(points, q.first, q.second);
		}, selectionMode);
	}
	void AnnotationStudio::selectWand(unsigned seed, int selectionMode)
	{
		if (!m_cloud || seed >= m_sourceCount) return;
		const auto center = *m_cloud->getPoint(seed); const double radius = m_wandRadius->value() * m_scale;
		const auto* sf = m_wandScalar->isChecked() && m_cloud->sfShown() ? m_cloud->getCurrentDisplayedScalarField() : nullptr;
		if (m_wandScalar->isChecked() && (!sf || !std::isfinite(sf->getValue(seed))))
		{ QMessageBox::information(this, QStringLiteral("Wand"), QStringLiteral("Display a scalar field with a finite seed value in the main colour panel, or disable scalar matching.")); return; }
		const double value = sf ? sf->getValue(seed) : 0, tolerance = m_wandTolerance->value();
		selectMatching([this, center, radius, sf, value, tolerance](unsigned id)
		{
			const auto* p = m_cloud->getPoint(id); const double dx = double(p->x) - center.x, dy = double(p->y) - center.y, dz = double(p->z) - center.z;
			return dx * dx + dy * dy + dz * dz <= radius * radius && (!sf || (std::isfinite(sf->getValue(id)) && std::abs(sf->getValue(id) - value) <= tolerance));
		}, selectionMode);
	}
}
