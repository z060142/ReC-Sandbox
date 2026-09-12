// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! Small geometry helpers shared by the shape components.
//!
//! Everything here is a copy of what EditorQt's CShapeObject uses (ShapeObject.cpp:2456-2560 and
//! Sandbox/Plugins/EditorCommon/Util/Math.h), brought into this plugin on purpose: the shape
//! components answer editor questions (EditorHitTest) from an ENGINE module, which must not link
//! - and cannot link - EditorQt or EditorCommon. Keeping the maths identical is what makes a
//! component shape feel exactly like a legacy shape under the cursor.
//!
//! All functions take caller-owned arrays; nothing here allocates (heap rule, IShapeComponent.h).

#include <CryMath/Cry_Math.h>
#include <CryMath/Cry_Geo.h>

namespace Cry
{
namespace AreaComponents
{
namespace ShapeGeo
{

//! How far along a view ray the editor pretends the ray ends (CShapeObject uses RAY_DISTANCE).
constexpr float kRayDistance = 100000.0f;
//! Line-degeneracy epsilon of Util/Math.h (LINE_EPS).
constexpr float kLineEps = 0.0000001f;

//! Distance from p3 to the segment p1-p2, plus the closest point on the segment.
//! EditorCommon/Util/Math.h:63, unchanged.
inline float PointToSegmentDistance(const Vec3& p1, const Vec3& p2, const Vec3& p3, Vec3& intersectPoint)
{
	const Vec3  d = p2 - p1;
	const float lengthSq = d.GetLengthSquared();

	if (lengthSq < 0.00001f)
	{
		intersectPoint = p1;
		return (p3 - p1).GetLength();
	}

	const float u = d.Dot(p3 - p1) / lengthSq;
	if (u < 0.0f)
	{
		intersectPoint = p1;
		return (p3 - p1).GetLength();
	}
	if (u > 1.0f)
	{
		intersectPoint = p2;
		return (p3 - p2).GetLength();
	}

	intersectPoint = p1 + u * d;
	return (p3 - intersectPoint).GetLength();
}

//! Shortest segment between the two lines p1-p2 and p3-p4. EditorCommon/Util/Math.h:104, unchanged.
inline bool LineLineIntersect(const Vec3& p1, const Vec3& p2, const Vec3& p3, const Vec3& p4,
                              Vec3& pa, Vec3& pb, float& mua, float& mub)
{
	const Vec3 p13 = p1 - p3;
	const Vec3 p43 = p4 - p3;
	if (fabs_tpl(p43.x) < kLineEps && fabs_tpl(p43.y) < kLineEps && fabs_tpl(p43.z) < kLineEps)
		return false;

	const Vec3 p21 = p2 - p1;
	if (fabs_tpl(p21.x) < kLineEps && fabs_tpl(p21.y) < kLineEps && fabs_tpl(p21.z) < kLineEps)
		return false;

	const float d1343 = p13.Dot(p43);
	const float d4321 = p43.Dot(p21);
	const float d1321 = p13.Dot(p21);
	const float d4343 = p43.Dot(p43);
	const float d2121 = p21.Dot(p21);

	const float denominator = d2121 * d4343 - d4321 * d4321;
	if (fabs_tpl(denominator) < kLineEps)
		return false;

	const float numerator = d1343 * d4321 - d1321 * d4343;

	mua = numerator / denominator;
	mub = (d1343 + d4321 * mua) / d4343;

	pa = p1 + mua * p21;
	pb = p3 + mub * p43;
	return true;
}

//! Distance between the view ray (given as two points) and the segment pi-pj, plus the point on
//! the ray. CShapeObject::RayToLineDistance, ShapeObject.cpp:2505, unchanged.
inline bool RayToLineDistance(const Vec3& rayLineP1, const Vec3& rayLineP2, const Vec3& pi, const Vec3& pj,
                              float& distance, Vec3& intPnt)
{
	Vec3  pa, pb;
	float ua, ub;
	if (!LineLineIntersect(pi, pj, rayLineP1, rayLineP2, pa, pb, ua, ub))
		return false;

	if (ub < 0.0f)
		return false; // behind the ray origin

	if (ua < 0.0f)
		distance = PointToSegmentDistance(rayLineP1, rayLineP2, pi, intPnt);
	else if (ua > 1.0f)
		distance = PointToSegmentDistance(rayLineP1, rayLineP2, pj, intPnt);
	else
	{
		intPnt = rayLineP1 + ub * (rayLineP2 - rayLineP1);
		distance = (pb - pa).GetLength();
	}

	return true;
}

//! The edge of a world-space contour nearest to a view ray. CShapeObject::GetNearestEdge
//! (ray flavour), ShapeObject.cpp:2531. p1/p2 come back as -1 when nothing was found.
inline void GetNearestEdge(const Vec3* pWorldPoints, int pointCount, bool bClosed,
                           const Vec3& raySrc, const Vec3& rayDir,
                           int& p1, int& p2, float& distance, Vec3& intersectPoint)
{
	p1 = -1;
	p2 = -1;
	distance = FLT_MAX;

	if (pWorldPoints == nullptr || pointCount < 2)
		return;

	const Vec3 rayLineP1 = raySrc;
	const Vec3 rayLineP2 = raySrc + rayDir * kRayDistance;

	float minDist = FLT_MAX;
	Vec3  intPnt;

	for (int i = 0; i < pointCount; ++i)
	{
		const int j = (i + 1) % pointCount;
		if (!bClosed && j == 0 && i != 0)
			continue;

		float d = 0.0f;
		if (!RayToLineDistance(rayLineP1, rayLineP2, pWorldPoints[i], pWorldPoints[j], d, intPnt))
			continue;

		if (d < minDist)
		{
			minDist = d;
			p1 = i;
			p2 = j;
			intersectPoint = intPnt;
		}
	}

	distance = minDist;
}

//! Even-odd point-in-polygon on XY. The contour is treated as closed whatever the closed flag
//! says - an open polyline has no interior, and the callers check that before asking.
inline bool IsPointInPolygon2D(const Vec3* pPoints, int pointCount, const Vec3& point)
{
	if (pPoints == nullptr || pointCount < 3)
		return false;

	bool bInside = false;
	for (int i = 0, j = pointCount - 1; i < pointCount; j = i++)
	{
		const Vec3& a = pPoints[i];
		const Vec3& b = pPoints[j];

		if (((a.y > point.y) != (b.y > point.y)) &&
		    (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x))
		{
			bInside = !bInside;
		}
	}

	return bInside;
}

//! Unsigned distance from a point to the contour's edges, measured on XY only.
inline float DistanceToContour2D(const Vec3* pPoints, int pointCount, bool bClosed, const Vec3& point)
{
	if (pPoints == nullptr || pointCount <= 0)
		return FLT_MAX;

	if (pointCount == 1)
		return Vec2(point.x - pPoints[0].x, point.y - pPoints[0].y).GetLength();

	const Vec3 flatPoint(point.x, point.y, 0.0f);

	float minDist = FLT_MAX;
	for (int i = 0; i < pointCount; ++i)
	{
		const int j = (i + 1) % pointCount;
		if (!bClosed && j == 0 && i != 0)
			continue;

		const Vec3 a(pPoints[i].x, pPoints[i].y, 0.0f);
		const Vec3 b(pPoints[j].x, pPoints[j].y, 0.0f);

		Vec3        closest;
		const float d = PointToSegmentDistance(a, b, flatPoint, closest);
		minDist = min(minDist, d);
	}

	return minDist;
}

//! Ray versus one contour edge treated as an infinite vertical wall, on XY. Returns the ray
//! parameter of the crossing; false when the ray is parallel to the wall or crosses behind it.
inline bool RayToWall2D(const Vec3& origin, const Vec3& dir, const Vec3& a, const Vec3& b, float& t)
{
	const Vec2 rayOrigin(origin.x, origin.y);
	const Vec2 rayDir(dir.x, dir.y);
	const Vec2 segStart(a.x, a.y);
	const Vec2 segDir(b.x - a.x, b.y - a.y);

	const float denominator = rayDir.x * segDir.y - rayDir.y * segDir.x;
	if (fabs_tpl(denominator) < 1e-8f)
		return false;

	const Vec2  diff = segStart - rayOrigin;
	const float rayT = (diff.x * segDir.y - diff.y * segDir.x) / denominator;
	const float segT = (diff.x * rayDir.y - diff.y * rayDir.x) / denominator;

	if (rayT < 0.0f || segT < 0.0f || segT > 1.0f)
		return false;

	t = rayT;
	return true;
}

} // namespace ShapeGeo
} // namespace AreaComponents
} // namespace Cry
