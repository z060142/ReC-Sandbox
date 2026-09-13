// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"

#include <vector>

namespace Cry
{
namespace AreaComponents
{

//! One station of a ribbon laid along a spline: the point on the curve, the sideways direction
//! there, the width there, and how far along the curve it is. A sector is the quad between two
//! consecutive stations.
//!
//! `left` is `worldPos + 0.5 * width * worldNormal` and `right` is `worldPos - 0.5 * width *
//! worldNormal`, the same way round as CRoadObject::SetRoadSectors (RoadObject.cpp:180-181), which
//! is also the order CRoadRenderNode::GetClipPlanes assumes when it builds a trapezoid's four clip
//! planes out of (l0, r0, l1, r1) (RoadRenderNode.cpp:647-650).
struct SSplineStation
{
	Vec3  worldPos = ZERO;
	Vec3  worldNormal = ZERO;
	float width = 0.0f;
	//! Arc length from the start of the curve, in world metres. The texture coordinate is this
	//! divided by the consumer's tile length.
	float distance = 0.0f;
};

namespace SplineSectors
{

//! CRoadObject::SetRoadSectors' walk (RoadObject.cpp:158-231), line for line, against the shape
//! contract instead of against CSplineObject.
//!
//! It matters that this is per SEGMENT and not per metre. Legacy asks each Bezier segment for its
//! length, turns that into `int((length + 0.5) / step)` sectors - never fewer than one - and then
//! evaluates position, normal and width at the SAME segment-local parameter. Position and frame
//! therefore always belong to the same place on the curve. Walking by arc length instead and
//! evaluating the frame at `t = distance / totalLength` samples two different points on a Bezier
//! curve, which shears the ribbon wherever the control points are unevenly spaced - the defect that
//! made the first road component look unlike a real road.
//!
//! Only the LAST segment emits its closing station; every other stops one short, so two segments
//! meet at exactly one station and there are no duplicates (RoadObject.cpp:172-174).
//!
//! Returns the number of stations written, which is one more than the number of sectors. Fewer than
//! two means there is no ribbon.
inline int BuildStations(const ISplineShape& spline, float step, float defaultWidth, int maxStations,
                         std::vector<SSplineStation>& out)
{
	out.clear();

	if (maxStations < 2)
		return 0;

	const float clampedStep = clamp_tpl(step, 0.25f, 10.0f);
	const int   segmentCount = spline.GetSegmentCount();

	float travelled = 0.0f;

	const auto addStation = [&](float t, float distance)
	{
		SSplineStation station;
		station.worldPos = spline.EvalPos(t);

		Vec3 normal = spline.EvalNormal(t);
		if (normal.IsZero())
		{
			// A degenerate frame - a vertical tangent under the legacy up rule, or a zero-length
			// segment. Any perpendicular beats a zero-width ribbon.
			Vec3 tangent = spline.EvalTangent(t);
			normal = tangent.IsZero() ? Vec3(0.0f, 1.0f, 0.0f) : tangent.GetOrthogonal();
		}

		station.worldNormal = normal.GetNormalizedSafe(Vec3(0.0f, 1.0f, 0.0f));
		station.width = max(0.01f, spline.GetLocalWidth(t, defaultWidth));
		station.distance = distance;

		out.push_back(station);
	};

	if (segmentCount > 0)
	{
		for (int segment = 0; segment < segmentCount; ++segment)
		{
			const float segmentLength = spline.GetSegmentLength(segment, 1.0f);

			// CRoadObject::GetRoadSectorCount, RoadObject.cpp:233-239, including the +0.5 bias and
			// the "a segment is never fewer than one sector" floor.
			int sectorsInSegment = static_cast<int>((segmentLength + 0.5f) / clampedStep);
			if (sectorsInSegment <= 0)
				sectorsInSegment = 1;

			for (int k = 0; k <= sectorsInSegment; ++k)
			{
				if (segment != segmentCount - 1 && k == sectorsInSegment)
					break;

				if (static_cast<int>(out.size()) >= maxStations)
					return static_cast<int>(out.size());

				const float segmentT = static_cast<float>(k) / static_cast<float>(sectorsInSegment);
				addStation(spline.SegmentToParam(segment, segmentT),
				           travelled + spline.GetSegmentLength(segment, segmentT));
			}

			travelled += segmentLength;
		}
	}
	else
	{
		// A spline kind that does not answer the segment queries (the append-only defaults in
		// ISplineShape). Walk the whole curve uniformly in PARAMETER - not in arc length - so that
		// position, normal and width still come from one and the same point.
		const float totalLength = spline.TotalLength();
		if (totalLength <= 0.01f)
			return 0;

		const int sectors = clamp_tpl(static_cast<int>(totalLength / clampedStep + 0.5f), 1, maxStations - 1);

		for (int k = 0; k <= sectors; ++k)
		{
			const float t = static_cast<float>(k) / static_cast<float>(sectors);
			addStation(t, t * totalLength);
		}
	}

	return static_cast<int>(out.size());
}

} // namespace SplineSectors
} // namespace AreaComponents
} // namespace Cry
