// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AnnotationGeometry.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QPolygonF>
#include <QImage>

namespace alis
{
	class AnnotationStudio;
	// A lightweight GPU viewport over the shared render sample; selections still
	// project every original candidate point through the SAME camera on the CPU.
	class AnnotationCanvas final : public QOpenGLWidget, protected QOpenGLFunctions_2_1
	{
	public:
		AnnotationCanvas(AnnotationStudio* studio, int panel, AnnotationView view);
		~AnnotationCanvas() override;
		void fit();
		void setView(AnnotationView view);
		void updateSectionView();
		void synchronizeFrom(const AnnotationCanvas& source);
		void invalidate();
		void cancelPath();
		const AnnotationCamera& camera() const { return m_camera; }
		int panelNumber() const { return m_panel; }
	protected:
		void initializeGL() override;
		void paintGL() override;
		void resizeGL(int width, int height) override;
		void mousePressEvent(QMouseEvent*) override;
		void mouseMoveEvent(QMouseEvent*) override;
		void mouseReleaseEvent(QMouseEvent*) override;
		void mouseDoubleClickEvent(QMouseEvent*) override;
		void keyPressEvent(QKeyEvent*) override;
		void wheelEvent(QWheelEvent*) override;
	private:
		friend struct AnnotationStudioTestAccess;
		QPointF screen(const AnnotationVector& p) const;
		QPointF plane(QPointF point) const;
		void finishPath();
		void uploadPoints();
		AnnotationStudio* s;
		int m_panel;
		AnnotationCamera m_camera;
		QOpenGLBuffer m_buffer;
		QOpenGLShaderProgram m_program;
		quint64 m_uploadedRevision = ~quint64(0);
		int m_vertexCount = 0;
		bool m_glReady = false, m_autoFit = true, m_drawing = false, m_panning = false, m_rotating = false;
		int m_gestureMode = 0;
		QPolygonF m_path;
		QPoint m_previous;
	};
}
