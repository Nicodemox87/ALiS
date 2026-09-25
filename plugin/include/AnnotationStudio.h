// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AnnotationGeometry.h"
#include <QDialog>
#include <QColor>
#include <QPolygonF>
#include <memory>
#include <functional>
#include <vector>

class ccPointCloud;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace alis
{
	class AnnotationCanvas;
	class AnnotationCloudObserver;
	class AnnotationStudio final : public QDialog
	{
		Q_OBJECT
	public:
		explicit AnnotationStudio(ccPointCloud* cloud, QWidget* parent = nullptr);
		~AnnotationStudio() override;
		void refreshColours();
		void setHistoryAvailable(bool undo, bool redo);
		void detachCloud();
	Q_SIGNALS:
		void labelsRequested(const std::vector<unsigned>& indices, int asprs, int archaeology);
		void historyRequested(bool redo);
	private:
		friend class AnnotationCanvas;
		friend struct AnnotationStudioTestAccess;
		struct RenderPoint { float xyz[3]; QRgb colour; unsigned id; };
		void rebuildIndex();
		void rebuildPreview();
		void schedulePreview();
		void updateCounts();
		bool eligible(unsigned id) const;
		std::pair<std::size_t, std::size_t> candidateRange() const;
		void selectPolygon(int horizontal, int vertical, const QPolygonF& polygon);
		void selectProjectedPolygon(const AnnotationCamera& camera, const QPolygonF& polygon, int selectionMode);
		void selectWand(unsigned seed, int selectionMode = -1);
		void selectMatching(const std::function<bool(unsigned)>& predicate, int selectionMode = -1);
		void setActiveCanvas(AnnotationCanvas* canvas);
		void cameraChanged(AnnotationCanvas* source);
		void synchronizeViews(AnnotationCanvas* source);
		void fitAllViews();
		void setSectionPlacement(bool enabled, bool refresh = true);
		bool placeSection(const AnnotationVector& first, const AnnotationVector& last);
		double sectionCoordinate(unsigned id) const;
		double sectionOrigin() const;
		void updatePositionControls();
		QRgb pointColour(unsigned id) const;
		double localCenter() const;
		ccPointCloud* m_cloud = nullptr;
		unsigned m_sourceCount = 0;
		std::unique_ptr<AnnotationCloudObserver> m_observer;
		SectionIndex m_index;
		std::vector<RenderPoint> m_renderPoints;
		quint64 m_renderRevision = 0;
		std::vector<bool> m_selected;
		std::size_t m_selectedCount = 0, m_sliceCount = 0;
		double m_min[3] = {}, m_max[3] = {};
		double m_scale = 1;
		int m_indexAxis = -1;
		AnnotationVector m_sectionNormal{{0,1,0}};
		double m_sectionMin = 0, m_sectionMax = 0;
		bool m_placingSection = false;
		QPushButton* m_drawSection = nullptr;
		QWidget* m_customControls = nullptr;
		QDoubleSpinBox* m_anchorX = nullptr;
		QDoubleSpinBox* m_anchorY = nullptr;
		QDoubleSpinBox* m_direction = nullptr;
		QLabel* m_positionLabel = nullptr;
		QLabel* m_sectionHint = nullptr;
		bool m_activeSection = true;
		double m_activeCenter = 0, m_activeHalf = 1;
		QCheckBox* m_section = nullptr;
		QComboBox* m_axis = nullptr;
		QDoubleSpinBox* m_thickness = nullptr;
		QDoubleSpinBox* m_center = nullptr;
		QDoubleSpinBox* m_step = nullptr;
		QSlider* m_slider = nullptr;
		QComboBox* m_tool = nullptr;
		QComboBox* m_mode = nullptr;
		QDoubleSpinBox* m_wandRadius = nullptr;
		QDoubleSpinBox* m_wandTolerance = nullptr;
		QCheckBox* m_wandScalar = nullptr;
		QComboBox* m_class = nullptr;
		QPushButton* m_apply = nullptr;
		QPushButton* m_undo = nullptr;
		QPushButton* m_redo = nullptr;
		QLabel* m_counts = nullptr;
		QTimer* m_previewTimer = nullptr;
		std::vector<AnnotationCanvas*> m_canvases;
		AnnotationCanvas* m_activeCanvas = nullptr;
		QLabel* m_activeViewLabel = nullptr;
		QCheckBox* m_liveSync = nullptr;
	};
}
