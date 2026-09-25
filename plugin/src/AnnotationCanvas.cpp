// SPDX-License-Identifier: GPL-2.0-or-later
#include "AnnotationCanvas.h"
#include "AnnotationStudio.h"
#include <ccLog.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QVector2D>
#include <QVector3D>
#include <QWheelEvent>
#include <limits>

namespace alis
{
	AnnotationCanvas::AnnotationCanvas(AnnotationStudio* studio, int panel, AnnotationView view)
		: QOpenGLWidget(studio), s(studio), m_panel(panel), m_buffer(QOpenGLBuffer::VertexBuffer)
	{
		QSurfaceFormat format; format.setRenderableType(QSurfaceFormat::OpenGL); format.setVersion(2,1);
		format.setProfile(QSurfaceFormat::NoProfile); format.setDepthBufferSize(24); setFormat(format);
		setMinimumSize(250,170); setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
		setObjectName(QStringLiteral("ALiS.Studio.View.%1").arg(panel));
		setToolTip(QStringLiteral("Left: select; hold Ctrl at the start to ADD. Polygon: double-click to finish. Wheel: zoom. Middle drag: pan. Right drag: rotate in 3D, pan in orthographic views. Esc cancels the outline. Selections include ALL points through the slab, not just the frontmost visible points."));
		m_camera.orient(view, s->m_axis->currentIndex(), s->m_sectionNormal); fit();
	}
	AnnotationCanvas::~AnnotationCanvas()
	{
		if (context()) { makeCurrent(); m_buffer.destroy(); m_program.removeAllShaders(); doneCurrent(); }
	}
	void AnnotationCanvas::fit()
	{
		cancelPath();
		for (int i=0;i<3;++i) m_camera.center[i]=(s->m_min[i]+s->m_max[i])*.5;
		double x=0,y=0;
		for (int bits=0;bits<8;++bits)
		{
			AnnotationVector p; for(int i=0;i<3;++i) p[i]=(bits&(1<<i))?s->m_max[i]:s->m_min[i];
			const auto projected=m_camera.project(p); x=std::max(x,std::abs(projected.first)); y=std::max(y,std::abs(projected.second));
		}
		m_camera.pixelsPerUnit=.88*std::min(std::max(1,width())/std::max(.01,2*x),std::max(1,height()-24)/std::max(.01,2*y));
		m_autoFit=true; invalidate(); s->cameraChanged(this);
	}
	void AnnotationCanvas::setView(AnnotationView view)
	{ cancelPath(); m_camera.orient(view,s->m_axis->currentIndex(),s->m_sectionNormal); s->setActiveCanvas(this); invalidate(); s->cameraChanged(this); }
	void AnnotationCanvas::updateSectionView()
	{
		if (m_camera.view==AnnotationView::SectionFront || m_camera.view==AnnotationView::SectionBack)
		{ cancelPath(); m_camera.orient(m_camera.view,s->m_axis->currentIndex(),s->m_sectionNormal); invalidate(); }
	}
	void AnnotationCanvas::synchronizeFrom(const AnnotationCanvas& source)
	{
		cancelPath(); m_autoFit=false; m_camera.synchronizeFrom(source.m_camera);
		// A 2D source does not determine camera depth; use the slab centre when
		// its viewing normal matches the active section normal.
		// Normalize only the source. Receivers copy the identical result rather than
		// accumulating floating-point corrections along a non-axis-aligned normal.
		if (&source==this && s->m_section->isChecked() && !s->m_placingSection && s->m_indexAxis >= 0 && source.m_camera.view!=AnnotationView::Orbit3D && std::abs(annotationDot(source.m_camera.toward,s->m_sectionNormal))>.999)
		{
			const double offset=s->localCenter()-annotationDot(m_camera.center,s->m_sectionNormal);
			for(int i=0;i<3;++i) m_camera.center[i]+=offset*s->m_sectionNormal[i];
		}
		invalidate();
	}
	void AnnotationCanvas::invalidate() { update(); }
	void AnnotationCanvas::cancelPath() { m_path.clear(); m_drawing=false; update(); }
	QPointF AnnotationCanvas::screen(const AnnotationVector& p) const
	{ const auto q=m_camera.project(p); return {q.first*m_camera.pixelsPerUnit+width()*.5,height()*.5-q.second*m_camera.pixelsPerUnit}; }
	QPointF AnnotationCanvas::plane(QPointF p) const
	{ return {(p.x()-width()*.5)/m_camera.pixelsPerUnit,(height()*.5-p.y())/m_camera.pixelsPerUnit}; }
	void AnnotationCanvas::initializeGL()
	{
		initializeOpenGLFunctions();
		const char* vertex=
			"attribute vec3 position; attribute vec3 colour; varying vec3 pointColour;"
			"uniform vec3 cameraCenter, cameraRight, cameraUp, cameraToward; uniform vec2 screenScale; uniform float depthRange;"
			"void main(){vec3 d=position-cameraCenter; gl_Position=vec4(dot(d,cameraRight)*screenScale.x,dot(d,cameraUp)*screenScale.y,-dot(d,cameraToward)/depthRange,1.0); pointColour=colour;}";
		const char* fragment="varying vec3 pointColour; void main(){gl_FragColor=vec4(pointColour,1.0);}";
		m_glReady=m_program.addShaderFromSourceCode(QOpenGLShader::Vertex,vertex)
			&& m_program.addShaderFromSourceCode(QOpenGLShader::Fragment,fragment) && m_program.link() && m_buffer.create();
		m_uploadedRevision=~quint64(0);
		if(!m_glReady) setToolTip(QStringLiteral("OpenGL viewport initialization failed: ")+m_program.log());
		if(m_panel==1) ccLog::Print(QStringLiteral("[ALiS] Annotation OpenGL renderer: %1; shaders: %2")
			.arg(QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER))))
			.arg(m_glReady ? QStringLiteral("ready") : m_program.log()));
	}
	void AnnotationCanvas::uploadPoints()
	{
		if(m_uploadedRevision==s->m_renderRevision) return;
		struct Vertex {float x,y,z,r,g,b;};
		std::vector<Vertex> vertices; vertices.reserve(s->m_renderPoints.size());
		const AnnotationVector origin{{(s->m_min[0]+s->m_max[0])*.5,(s->m_min[1]+s->m_max[1])*.5,(s->m_min[2]+s->m_max[2])*.5}};
		for(const auto& p:s->m_renderPoints)
		{
			const QRgb c=s->m_selected[p.id]?qRgb(255,223,0):p.colour;
			vertices.push_back({float(p.xyz[0]-origin[0]),float(p.xyz[1]-origin[1]),float(p.xyz[2]-origin[2]),qRed(c)/255.f,qGreen(c)/255.f,qBlue(c)/255.f});
		}
		m_buffer.bind(); m_buffer.allocate(vertices.data(),int(vertices.size()*sizeof(Vertex))); m_buffer.release();
		m_vertexCount=int(vertices.size()); m_uploadedRevision=s->m_renderRevision;
	}
	void AnnotationCanvas::paintGL()
	{
		glClearColor(14.f/255,24.f/255,38.f/255,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
		if(m_glReady)
		{
			uploadPoints(); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDisable(GL_BLEND);
			glPointSize(float(devicePixelRatioF())*1.6f);
			m_program.bind(); m_buffer.bind();
			auto vector=[](const AnnotationVector& v){return QVector3D(float(v[0]),float(v[1]),float(v[2]));};
			AnnotationVector center=m_camera.center; double diagonal=0,offset=0;
			for(int i=0;i<3;++i){center[i]-=(s->m_min[i]+s->m_max[i])*.5; offset+=center[i]*center[i]; diagonal+=std::pow(s->m_max[i]-s->m_min[i],2);}
			m_program.setUniformValue("cameraCenter",vector(center));
			m_program.setUniformValue("cameraRight",vector(m_camera.right)); m_program.setUniformValue("cameraUp",vector(m_camera.up)); m_program.setUniformValue("cameraToward",vector(m_camera.toward));
			m_program.setUniformValue("screenScale",QVector2D(float(2*m_camera.pixelsPerUnit/std::max(1,width())),float(2*m_camera.pixelsPerUnit/std::max(1,height()))));
			m_program.setUniformValue("depthRange",float(std::sqrt(diagonal)*2+std::sqrt(offset)+1));
			m_program.enableAttributeArray("position"); m_program.enableAttributeArray("colour");
			m_program.setAttributeBuffer("position",GL_FLOAT,0,3,6*sizeof(float)); m_program.setAttributeBuffer("colour",GL_FLOAT,3*sizeof(float),3,6*sizeof(float));
			glDrawArrays(GL_POINTS,0,m_vertexCount);
			m_program.disableAttributeArray("position"); m_program.disableAttributeArray("colour"); m_buffer.release(); m_program.release(); glDisable(GL_DEPTH_TEST);
		}
		QPainter painter(this); painter.setPen(QColor(220,230,240));
		if(!m_glReady) painter.drawText(rect(),Qt::AlignCenter,QStringLiteral("OpenGL unavailable — see tooltip"));
		if(s->m_section->isChecked() && !s->m_placingSection)
		{
			// World-space slab guides, projected with the same camera as the points.
			AnnotationCamera section; section.orient(AnnotationView::SectionFront,s->m_indexAxis,s->m_sectionNormal);
			AnnotationVector anchor; double radius2=0;
			for(int i=0;i<3;++i){anchor[i]=(s->m_min[i]+s->m_max[i])*.5;radius2+=std::pow(s->m_max[i]-s->m_min[i],2);}
			const double offset=s->localCenter()-annotationDot(anchor,s->m_sectionNormal), radius=std::max(.01,std::sqrt(radius2)*.5);
			for(int i=0;i<3;++i) anchor[i]+=offset*s->m_sectionNormal[i];
			// Keep the guide out of face-on views where it would cover the points.
			if(std::abs(annotationDot(m_camera.toward,s->m_sectionNormal))<.999)
			{
				for(int side=-1;side<=1;++side)
				{
					QPolygonF guide;
					for(const auto& corner : {QPointF(-1,-1),QPointF(1,-1),QPointF(1,1),QPointF(-1,1)})
					{
						AnnotationVector point; for(int i=0;i<3;++i)point[i]=anchor[i]+radius*(corner.x()*section.right[i]+corner.y()*section.up[i])+side*s->m_thickness->value()*s->m_scale*.5*s->m_sectionNormal[i];
						guide<<screen(point);
					}
					painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor(56,189,248,side==0?210:120),side==0?2:1,side==0?Qt::SolidLine:Qt::DashLine)); painter.drawPolygon(guide);
				}
			}
		}
		painter.setPen(QColor(220,230,240));
		const double metres=70/m_camera.pixelsPerUnit/s->m_scale;
		painter.drawLine(12,height()-17,82,height()-17);
		painter.drawText(88,height()-12,QStringLiteral("%1 m | %2 | Ctrl: add").arg(metres,0,'g',3).arg(m_camera.view==AnnotationView::Orbit3D?QStringLiteral("right: rotate"):QStringLiteral("right: pan")));
		if(!m_path.isEmpty())
		{
			painter.setPen(QPen(QColor(56,189,248),2)); painter.setBrush(QColor(56,189,248,30));
			if(s->m_placingSection) { painter.setBrush(Qt::NoBrush); painter.drawPolyline(m_path); }
			else if(s->m_tool->currentIndex()==0 && m_path.size()>=2) painter.drawRect(QRectF(m_path.front(),m_path.back()).normalized()); else painter.drawPolygon(m_path);
		}
		if(s->m_placingSection) { painter.setPen(QColor(56,189,248)); painter.drawText(12,20,m_camera.view==AnnotationView::Top ? QStringLiteral("Drag section line here — Esc cancels") : QStringLiteral("Draw in Top / XY")); }
		painter.setBrush(Qt::NoBrush); painter.setPen(QPen(s->m_activeCanvas==this?QColor(56,189,248):QColor(60,75,90),2)); painter.drawRect(rect().adjusted(1,1,-1,-1));
	}
	void AnnotationCanvas::resizeGL(int,int) { if(m_autoFit) fit(); }
	void AnnotationCanvas::mousePressEvent(QMouseEvent* e)
	{
		setFocus(); s->setActiveCanvas(this);
		if(e->button()==Qt::RightButton || e->button()==Qt::MiddleButton)
		{
			cancelPath(); m_autoFit=false; m_rotating=e->button()==Qt::RightButton && m_camera.view==AnnotationView::Orbit3D; m_panning=!m_rotating; m_previous=e->pos(); return;
		}
		if(e->button()!=Qt::LeftButton || !s->m_cloud) return;
		if(s->m_placingSection)
		{
			if(m_camera.view!=AnnotationView::Top) return;
			cancelPath(); m_path<<e->localPos()<<e->localPos(); m_drawing=true; update(); return;
		}
		const int tool=s->m_tool->currentIndex();
		if(tool==4)
		{
			cancelPath(); m_autoFit=false; m_rotating=m_camera.view==AnnotationView::Orbit3D; m_panning=!m_rotating; m_previous=e->pos(); return;
		}
		if(!m_drawing || tool!=1) m_gestureMode=annotationSelectionMode(s->m_mode->currentIndex(),e->modifiers().testFlag(Qt::ControlModifier));
		if(tool==3)
		{
			double best=225, bestDepth=-std::numeric_limits<double>::infinity(); unsigned seed=std::numeric_limits<unsigned>::max();
			for(const auto& p:s->m_renderPoints)
			{
				const AnnotationVector point{{p.xyz[0],p.xyz[1],p.xyz[2]}}; const QPointF delta=screen(point)-e->localPos();
				const double distance=delta.x()*delta.x()+delta.y()*delta.y(), depth=annotationDot(point,m_camera.toward);
				if(distance<best-1e-6 || (std::abs(distance-best)<=1e-6 && depth>bestDepth)){best=distance;bestDepth=depth;seed=p.id;}
			}
			if(seed!=std::numeric_limits<unsigned>::max()) s->selectWand(seed,m_gestureMode); return;
		}
		if(tool!=1) m_path.clear(); m_path<<e->localPos(); m_drawing=true; update();
	}
	void AnnotationCanvas::mouseMoveEvent(QMouseEvent* e)
	{
		if(m_panning || m_rotating)
		{
			const QPoint delta=e->pos()-m_previous; m_previous=e->pos();
			if(m_rotating){m_camera.yaw-=delta.x()*.008; m_camera.pitch=std::max(-1.55,std::min(1.55,m_camera.pitch+delta.y()*.008));m_camera.orient(AnnotationView::Orbit3D,s->m_axis->currentIndex());}
			else for(int i=0;i<3;++i)m_camera.center[i]+=(-delta.x()*m_camera.right[i]+delta.y()*m_camera.up[i])/m_camera.pixelsPerUnit;
			invalidate(); s->cameraChanged(this); return;
		}
		if(!m_drawing || !(e->buttons()&Qt::LeftButton))return;
		if(s->m_placingSection || s->m_tool->currentIndex()==0){if(m_path.size()==1)m_path<<e->localPos();else m_path.back()=e->localPos();}
		else if(s->m_tool->currentIndex()==2 && (m_path.back()-e->localPos()).manhattanLength()>=3)m_path<<e->localPos();
		update();
	}
	void AnnotationCanvas::mouseReleaseEvent(QMouseEvent* e)
	{
		if(m_panning || m_rotating){m_panning=false;m_rotating=false;return;}
		if(e->button()==Qt::LeftButton && m_drawing && s->m_placingSection)
		{
			const QPointF start=m_path.front(), end=e->localPos(); cancelPath();
			if(QLineF(start,end).length()<5) { s->m_sectionHint->setText(QStringLiteral("Line too short: drag at least 5 pixels in Top / XY, or Esc to cancel.")); return; }
			auto world=[this](QPointF screenPoint) { const auto q=plane(screenPoint); AnnotationVector result=m_camera.center; for(int i=0;i<3;++i) result[i]+=q.x()*m_camera.right[i]+q.y()*m_camera.up[i]; return result; };
			s->placeSection(world(start),world(end)); return;
		}
		if(e->button()==Qt::LeftButton && m_drawing && s->m_tool->currentIndex()!=1)finishPath();
	}
	void AnnotationCanvas::mouseDoubleClickEvent(QMouseEvent* e)
	{ if(!s->m_placingSection && e->button()==Qt::LeftButton && s->m_tool->currentIndex()==1 && m_drawing)finishPath(); }
	void AnnotationCanvas::keyPressEvent(QKeyEvent* e)
	{ if(e->key()==Qt::Key_Escape){cancelPath();if(s->m_placingSection)s->m_drawSection->setChecked(false);e->accept();}else QOpenGLWidget::keyPressEvent(e); }
	void AnnotationCanvas::wheelEvent(QWheelEvent* e)
	{
		s->setActiveCanvas(this); cancelPath();m_autoFit=false;const QPointF before=plane(e->position());
		m_camera.pixelsPerUnit=std::max(1e-6,std::min(1e7,m_camera.pixelsPerUnit*std::pow(1.2,e->angleDelta().y()/120.0)));
		const QPointF after=plane(e->position());for(int i=0;i<3;++i)m_camera.center[i]+=(before.x()-after.x())*m_camera.right[i]+(before.y()-after.y())*m_camera.up[i];
		invalidate(); s->cameraChanged(this); e->accept();
	}
	void AnnotationCanvas::finishPath()
	{
		QPolygonF polygon;
		if(s->m_tool->currentIndex()==0 && m_path.size()>=2){const QRectF r=QRectF(m_path.front(),m_path.back()).normalized();if(r.width()>=3 && r.height()>=3)polygon<<r.topLeft()<<r.topRight()<<r.bottomRight()<<r.bottomLeft();}
		else polygon=m_path;
		for(QPointF& point:polygon)point=plane(point);
		cancelPath();if(polygon.size()>=3)s->selectProjectedPolygon(m_camera,polygon,m_gestureMode);
	}
}
