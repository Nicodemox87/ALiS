// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>
#include <array>

namespace alis
{
	using AnnotationVector = std::array<double, 3>;
	inline double annotationDot(const AnnotationVector& a, const AnnotationVector& b)
	{ return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

	enum class AnnotationView { Top, Bottom, Front, Back, Right, Left, SectionFront, SectionBack, Orbit3D };
	struct AnnotationCamera
	{
		AnnotationVector center{{0,0,0}}, right{{1,0,0}}, up{{0,1,0}}, toward{{0,0,1}};
		double pixelsPerUnit = 1, yaw = -0.7853981633974483, pitch = 0.5235987755982988;
		AnnotationView view = AnnotationView::Top;
		void orient(AnnotationView requested, int sectionAxis, const AnnotationVector& sectionNormal = {{0,1,0}})
		{
			view = requested;
			if (sectionAxis == 3 && (requested == AnnotationView::SectionFront || requested == AnnotationView::SectionBack))
			{
				// Custom vertical plane: Z remains up; front/back are opposite sides.
				const double side = requested == AnnotationView::SectionFront ? 1 : -1;
				right = {{side*sectionNormal[1], -side*sectionNormal[0], 0}};
				up = {{0,0,1}}; toward = {{-side*sectionNormal[0], -side*sectionNormal[1], 0}};
				return;
			}
			if (requested == AnnotationView::SectionFront) requested = sectionAxis == 0 ? AnnotationView::Right : sectionAxis == 1 ? AnnotationView::Front : AnnotationView::Top;
			if (requested == AnnotationView::SectionBack) requested = sectionAxis == 0 ? AnnotationView::Left : sectionAxis == 1 ? AnnotationView::Back : AnnotationView::Bottom;
			switch (requested)
			{
			case AnnotationView::Top: right={{1,0,0}}; up={{0,1,0}}; toward={{0,0,1}}; break;
			case AnnotationView::Bottom: right={{-1,0,0}}; up={{0,1,0}}; toward={{0,0,-1}}; break;
			case AnnotationView::Front: right={{1,0,0}}; up={{0,0,1}}; toward={{0,-1,0}}; break;
			case AnnotationView::Back: right={{-1,0,0}}; up={{0,0,1}}; toward={{0,1,0}}; break;
			case AnnotationView::Right: right={{0,1,0}}; up={{0,0,1}}; toward={{1,0,0}}; break;
			case AnnotationView::Left: right={{0,-1,0}}; up={{0,0,1}}; toward={{-1,0,0}}; break;
			case AnnotationView::Orbit3D:
				right={{-std::sin(yaw),std::cos(yaw),0}};
				up={{-std::cos(yaw)*std::sin(pitch),-std::sin(yaw)*std::sin(pitch),std::cos(pitch)}};
				toward={{std::cos(yaw)*std::cos(pitch),std::sin(yaw)*std::cos(pitch),std::sin(pitch)}}; break;
			default: break;
			}
		}
		std::pair<double,double> project(const AnnotationVector& p) const
		{
			AnnotationVector delta{{p[0]-center[0],p[1]-center[1],p[2]-center[2]}};
			return {annotationDot(delta,right),annotationDot(delta,up)};
		}
		void synchronizeFrom(const AnnotationCamera& source)
		{ center = source.center; pixelsPerUnit = source.pixelsPerUnit; }
	};
	// Ctrl overrides the dropdown for this gesture only. The next gesture starts fresh.
	inline int annotationSelectionMode(int dropdownMode, bool controlHeld)
	{ return controlHeld ? 1 : dropdownMode; }

	// Index only: coordinates and labels remain in the original cloud.
	class SectionIndex
	{
	public:
		template<class Coordinate> void build(unsigned count, double minimum, double maximum, Coordinate coordinate)
		{
			m_min = minimum;
			m_width = std::max(1e-9, (maximum - minimum) / 1024.0);
			m_offsets.assign(1025, 0);
			for (unsigned i = 0; i < count; ++i) ++m_offsets[bin(coordinate(i)) + 1];
			for (std::size_t i = 1; i < m_offsets.size(); ++i) m_offsets[i] += m_offsets[i - 1];
			auto cursor = m_offsets;
			m_ids.resize(count);
			for (unsigned i = 0; i < count; ++i) m_ids[cursor[bin(coordinate(i))]++] = i;
		}
		std::pair<std::size_t, std::size_t> range(double lower, double upper) const
		{
			if (m_offsets.empty() || lower > upper) return {0, 0};
			return {m_offsets[bin(lower)], m_offsets[bin(upper) + 1]};
		}
		const std::vector<unsigned>& ids() const { return m_ids; }
	private:
		std::size_t bin(double value) const
		{
			if (!std::isfinite(value)) return 0;
			return static_cast<std::size_t>(std::max(0.0, std::min(1023.0, std::floor((value - m_min) / m_width))));
		}
		double m_min = 0, m_width = 1;
		std::vector<unsigned> m_ids;
		std::vector<std::size_t> m_offsets;
	};

	using AnnotationPolygon = std::vector<std::pair<double, double>>;
	// Includes boundary points. Coordinates are local, avoiding georeferenced float loss.
	inline bool annotationContains(const AnnotationPolygon& polygon, double x, double y)
	{
		if (polygon.size() < 3 || !std::isfinite(x) || !std::isfinite(y)) return false;
		bool inside = false;
		for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
		{
			const double ax = polygon[j].first, ay = polygon[j].second;
			const double bx = polygon[i].first, by = polygon[i].second;
			const double cross = (x - ax) * (by - ay) - (y - ay) * (bx - ax);
			if (std::abs(cross) <= 1e-9 * std::max(1.0, std::hypot(bx - ax, by - ay))
				&& x >= std::min(ax, bx) - 1e-9 && x <= std::max(ax, bx) + 1e-9
				&& y >= std::min(ay, by) - 1e-9 && y <= std::max(ay, by) + 1e-9) return true;
			if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax) inside = !inside;
		}
		return inside;
	}
}
