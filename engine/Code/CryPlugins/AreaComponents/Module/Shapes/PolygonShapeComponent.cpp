// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "PolygonShapeComponent.h"
#include "ShapeDisplay.h"
#include "ShapeGeometry.h"

#include <CryRenderer/IRenderAuxGeom.h>
#include <CryMath/Random.h>

#include <algorithm>
#include <limits>

namespace Cry
{
namespace AreaComponents
{

namespace
{
//! Two points closer than this to each other are the same point, the test legacy makes before it
//! accepts an inserted point (ShapeObject.cpp:2069-2081 compares each axis against FLT_EPSILON).
const float kDuplicatePointEpsilon = FLT_EPSILON;
//! How many times GetRandomPointInside draws from the footprint AABB before it gives up. A
//! polygon whose area is a thousandth of its bounding box is pathological; answering false is
//! better than spinning.
const int kRandomPointAttempts = 64;
}

void CPolygonShapeComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet - the same stage as the box: IsPointInside /
	// GetRandomPointInside are exposed once the Area function component needs them.
}

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

Matrix34 CPolygonShapeComponent::GetShapeWorldTM() const
{
	return GetWorldTransformMatrix() * Matrix34::CreateTranslationMat(m_offset);
}

float CPolygonShapeComponent::GetOriginZ() const
{
	if (m_points.points.empty())
		return m_offset.z;

	float originZ = m_points.points[0].pos.z;
	for (const SPolygonPoint& point : m_points.points)
		originZ = min(originZ, point.pos.z);

	return originZ + m_offset.z;
}

bool CPolygonShapeComponent::HasInterior() const
{
	return m_closed && static_cast<int>(m_points.points.size()) >= kMinClosedPoints;
}

int CPolygonShapeComponent::GatherPoints(Vec3* pOut, int maxPoints, bool world) const
{
	if (pOut == nullptr || maxPoints <= 0)
		return 0;

	const int      count = min(static_cast<int>(m_points.points.size()), maxPoints);
	const Matrix34 tm = GetShapeWorldTM();

	for (int i = 0; i < count; ++i)
	{
		const Vec3& pos = m_points.points[i].pos;
		pOut[i] = world ? tm.TransformPoint(pos) : (m_offset + pos);
	}

	return count;
}

bool CPolygonShapeComponent::IsEdgeObstructing(int index) const
{
	if (index < 0 || index >= static_cast<int>(m_points.points.size()))
		return false;

	return m_points.points[index].obstructSound;
}

// ---------------------------------------------------------------------------
// IShapeComponent - geometry
// ---------------------------------------------------------------------------

void CPolygonShapeComponent::GetLocalAABB(AABB& out) const
{
	out.Reset();

	if (m_points.points.empty())
	{
		out = AABB(m_offset, m_offset);
		return;
	}

	for (const SPolygonPoint& point : m_points.points)
		out.Add(m_offset + point.pos);

	if (m_height > 0.0f)
	{
		// The roof sits at the LOWEST point plus the height, the way CArea reads a legacy shape
		// (Area.cpp:2298-2302 with m_origin = min z). A shape whose points climb higher than that
		// keeps its own extent, which is what CShapeObject::CalcBBox does too.
		out.max.z = max(out.max.z, GetOriginZ() + m_height);
	}
}

void CPolygonShapeComponent::GetWorldAABB(AABB& out) const
{
	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetWorldTransformMatrix(), local);
}

bool CPolygonShapeComponent::IsPointInside(const Vec3& world) const
{
	if (!HasInterior())
		return false;

	const Vec3 local = GetShapeWorldTM().GetInverted().TransformPoint(world);

	if (m_height > 0.0f)
	{
		// The band is expressed in shape space, so the origin has to lose the offset it carries
		// for the world-space users.
		const float originZ = GetOriginZ() - m_offset.z;
		if (local.z < originZ || local.z > originZ + m_height)
			return false;
	}

	Vec3      points[kMaxStackPoints];
	const int count = min(static_cast<int>(m_points.points.size()), kMaxStackPoints);
	for (int i = 0; i < count; ++i)
		points[i] = m_points.points[i].pos;

	return ShapeGeo::IsPointInPolygon2D(points, count, local);
}

float CPolygonShapeComponent::DistanceToHull(const Vec3& world) const
{
	if (m_points.points.empty())
		return FLT_MAX;

	const Vec3 local = GetShapeWorldTM().GetInverted().TransformPoint(world);

	Vec3      points[kMaxStackPoints];
	const int count = min(static_cast<int>(m_points.points.size()), kMaxStackPoints);
	for (int i = 0; i < count; ++i)
		points[i] = m_points.points[i].pos;

	// The usual prism distance field, in two parts: a signed distance to the footprint on XY and
	// a signed distance to the height band on Z, combined the way a signed box field combines its
	// axes - positive parts outside, the largest negative part inside.
	float distance2D = ShapeGeo::DistanceToContour2D(points, count, m_closed, local);
	if (HasInterior() && ShapeGeo::IsPointInPolygon2D(points, count, local))
		distance2D = -distance2D;

	if (m_height <= 0.0f)
		return distance2D; // infinite in Z: the footprint IS the hull

	const float originZ = GetOriginZ() - m_offset.z;
	const float distanceZ = max(originZ - local.z, local.z - (originZ + m_height));

	const float outside = Vec2(max(distance2D, 0.0f), max(distanceZ, 0.0f)).GetLength();
	const float inside = min(max(distance2D, distanceZ), 0.0f);
	return outside + inside;
}

bool CPolygonShapeComponent::IntersectRay(const Ray& ray, float& dist) const
{
	const int count = min(static_cast<int>(m_points.points.size()), kMaxStackPoints);
	if (count < 2)
		return false;

	// The inverse transform is affine, so the ray parameter survives it and the local direction
	// must not be re-normalised: t comes out in the caller's units.
	const Matrix34 invTM = GetShapeWorldTM().GetInverted();
	const Vec3     origin = invTM.TransformPoint(ray.origin);
	const Vec3     dir = invTM.TransformVector(ray.direction);

	Vec3 points[kMaxStackPoints];
	for (int i = 0; i < count; ++i)
		points[i] = m_points.points[i].pos;

	const float originZ = GetOriginZ() - m_offset.z;
	const float roofZ = originZ + m_height;

	float best = FLT_MAX;

	// The walls.
	for (int i = 0; i < count; ++i)
	{
		const int j = (i + 1) % count;
		if (!m_closed && j == 0 && i != 0)
			continue;

		float t;
		if (!ShapeGeo::RayToWall2D(origin, dir, points[i], points[j], t))
			continue;

		if (m_height > 0.0f)
		{
			const float hitZ = origin.z + dir.z * t;
			if (hitZ < originZ || hitZ > roofZ)
				continue;
		}

		best = min(best, t);
	}

	// The caps, which only exist on a closed shape with a finite height.
	if (m_height > 0.0f && HasInterior() && fabs_tpl(dir.z) > FLT_EPSILON)
	{
		const float capZ[2] = { originZ, roofZ };
		for (const float z : capZ)
		{
			const float t = (z - origin.z) / dir.z;
			if (t < 0.0f || t >= best)
				continue;

			const Vec3 hit = origin + dir * t;
			if (ShapeGeo::IsPointInPolygon2D(points, count, hit))
				best = t;
		}
	}

	if (best == FLT_MAX)
		return false;

	dist = best;
	return true;
}

bool CPolygonShapeComponent::GetRandomPointInside(Vec3& out) const
{
	if (!HasInterior())
		return false;

	Vec3      points[kMaxStackPoints];
	const int count = min(static_cast<int>(m_points.points.size()), kMaxStackPoints);

	AABB footprint;
	footprint.Reset();
	for (int i = 0; i < count; ++i)
	{
		points[i] = m_points.points[i].pos;
		footprint.Add(points[i]);
	}

	const float originZ = GetOriginZ() - m_offset.z;
	const float minZ = (m_height > 0.0f) ? originZ : footprint.min.z;
	const float maxZ = (m_height > 0.0f) ? (originZ + m_height) : footprint.max.z;

	// Rejection sampling in the footprint AABB: uniform inside the polygon by construction, and
	// the only method that stays correct for a concave footprint without triangulating it.
	for (int attempt = 0; attempt < kRandomPointAttempts; ++attempt)
	{
		const Vec3 candidate(cry_random(footprint.min.x, footprint.max.x),
		                     cry_random(footprint.min.y, footprint.max.y),
		                     cry_random(minZ, maxZ));

		if (!ShapeGeo::IsPointInPolygon2D(points, count, candidate))
			continue;

		out = GetShapeWorldTM().TransformPoint(candidate);
		return true;
	}

	return false;
}

bool CPolygonShapeComponent::GetAreaVolume(SShapeAreaVolume& out) const
{
	// A closed polygon is ENTITY_AREA_TYPE_SHAPE. An OPEN one has no interior - IsPointInside()
	// already answers false for it, and CArea says the same (an unclosed shape is never inside) -
	// but it is still a legal area: legacy shapes carry the closed flag straight through to
	// CArea::SetPoints(), so the flag travels and the caller does not special-case it.
	if (m_points.points.size() < 2)
		return false;

	out.form = EAreaVolumeForm::Polygon;
	out.height = m_height;
	out.closed = m_closed;
	out.obstructRoof = m_obstructRoof;
	out.obstructFloor = m_obstructFloor;

	return true;
}

int CPolygonShapeComponent::GetContourObstruction(bool* pOutFlags, int maxFlags) const
{
	// One flag per point, for the wall that STARTS at it - the same ordering GetContour() writes
	// its points in, and the ordering CArea indexes shape segments with. The buffer is the
	// caller's (heap rule, IShapeComponent.h): nothing is allocated here.
	const int totalFlags = static_cast<int>(m_points.points.size());
	if (pOutFlags == nullptr || maxFlags <= 0)
		return totalFlags;

	const int writtenFlags = min(maxFlags, totalFlags);
	for (int i = 0; i < writtenFlags; ++i)
		pOutFlags[i] = m_points.points[i].obstructSound;

	return totalFlags;
}

int CPolygonShapeComponent::GetContour(Vec3* pOutPoints, int maxPoints, bool world) const
{
	// The buffer belongs to the caller (heap rule, IShapeComponent.h): nothing is allocated here,
	// and the return value is the full count so that a caller with a short buffer can ask again.
	const int totalPoints = static_cast<int>(m_points.points.size());
	if (pOutPoints == nullptr || maxPoints <= 0)
		return totalPoints;

	GatherPoints(pOutPoints, maxPoints, world);
	return totalPoints;
}

// ---------------------------------------------------------------------------
// IEditorShapeComponent
// ---------------------------------------------------------------------------

bool CPolygonShapeComponent::GetEditorLocalBounds(AABB& out) const
{
	if (m_points.points.empty())
		return false;

	// CEntityObject::GetLocalBounds works in the ENTITY's local space, so the component's own
	// transform (identity unless the Transform flag is set) has to be applied here.
	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetTransformMatrix(), local);
	return true;
}

bool CPolygonShapeComponent::EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const
{
	// CShapeObject::HitTest (ShapeObject.cpp:1085-1150) in component form: a shape is picked by
	// its drawn lines - the footprint, and with a height also the roof ring and the vertical
	// edges - and never by its interior. That is deliberately unlike the box, whose solid slab
	// test is its second chance: a polygon with a large footprint would otherwise swallow every
	// click inside it.
	Vec3      points[kMaxStackPoints];
	const int count = GatherPoints(points, kMaxStackPoints, true);
	if (count < 2)
		return false;

	const Vec3 rayDir = worldRay.direction.GetNormalized();
	const Vec3 rayLineP1 = worldRay.origin;
	const Vec3 rayLineP2 = worldRay.origin + rayDir * ShapeGeo::kRayDistance;

	// The roof offset is a world-space Z lift only while the entity is unrotated, which is what
	// legacy assumes as well (it adds Vec3(0, 0, mv_height) to world points).
	const Vec3 up(0.0f, 0.0f, m_height);

	bool  bHit = false;
	float minDist = FLT_MAX;
	Vec3  intPnt(ZERO);
	Vec3  bestPnt(ZERO);

	for (int i = 0; i < count; ++i)
	{
		const int j = (i + 1) % count;
		const bool bWrapEdge = (j == 0 && i != 0);

		float d = 0.0f;

		if (!(!m_closed && bWrapEdge))
		{
			if (ShapeGeo::RayToLineDistance(rayLineP1, rayLineP2, points[i], points[j], d, intPnt) && d < minDist)
			{
				minDist = d;
				bestPnt = intPnt;
				bHit = true;
			}

			if (m_height > 0.0f &&
			    ShapeGeo::RayToLineDistance(rayLineP1, rayLineP2, points[i] + up, points[j] + up, d, intPnt) && d < minDist)
			{
				minDist = d;
				bestPnt = intPnt;
				bHit = true;
			}
		}

		if (m_height > 0.0f &&
		    ShapeGeo::RayToLineDistance(rayLineP1, rayLineP2, points[i], points[i] + up, d, intPnt) && d < minDist)
		{
			minDist = d;
			bestPnt = intPnt;
			bHit = true;
		}
	}

	if (!bHit || minDist > tolerance)
		return false;

	distOut = worldRay.origin.GetDistance(bestPnt);
	return true;
}

// ---------------------------------------------------------------------------
// IShapeComponentEdit
// ---------------------------------------------------------------------------

int CPolygonShapeComponent::GetPointCount() const
{
	return static_cast<int>(m_points.points.size());
}

Vec3 CPolygonShapeComponent::GetPoint(int index) const
{
	if (index < 0 || index >= static_cast<int>(m_points.points.size()))
		return ZERO;

	// Component-local space, offset included - the same convention the box uses, so that the
	// tool's single "transform by the component's world matrix" is right for every kind.
	return m_offset + m_points.points[index].pos;
}

void CPolygonShapeComponent::SetPoint(int index, const Vec3& local)
{
	if (index < 0 || index >= static_cast<int>(m_points.points.size()))
		return;

	m_points.points[index].pos = local - m_offset;
	RecordChange(EShapeChangeReason::Geometry);
}

int CPolygonShapeComponent::InsertPoint(int index, const Vec3& local)
{
	const Vec3 pos = local - m_offset;
	const int  count = static_cast<int>(m_points.points.size());

	// Legacy refuses a point that lands on an existing one and says so, rather than creating a
	// zero-length edge nobody can grab again (ShapeObject.cpp:2064-2083).
	for (int i = 0; i < count; ++i)
	{
		const Vec3 diff = m_points.points[i].pos - pos;
		if (fabs_tpl(diff.x) < kDuplicatePointEpsilon &&
		    fabs_tpl(diff.y) < kDuplicatePointEpsilon &&
		    fabs_tpl(diff.z) < kDuplicatePointEpsilon)
		{
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING, "Shape: Polygon - the point is too close to another point!");
			return i;
		}
	}

	SPolygonPoint point;
	point.pos = pos;
	point.obstructSound = false;

	int newIndex;
	if (index < 0 || index >= count)
	{
		m_points.points.push_back(point);
		newIndex = static_cast<int>(m_points.points.size()) - 1;
	}
	else
	{
		m_points.points.insert(m_points.points.begin() + index, point);
		newIndex = index;
	}

	RecordChange(EShapeChangeReason::Topology);
	return newIndex;
}

void CPolygonShapeComponent::RemovePoint(int index)
{
	const int count = static_cast<int>(m_points.points.size());
	if (index < 0 || index >= count)
		return;

	const int minPoints = m_closed ? kMinClosedPoints : kMinOpenPoints;
	if (count <= minPoints)
	{
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Shape: Polygon - a %s shape needs at least %d points, the point was not removed.",
		           m_closed ? "closed" : "open", minPoints);
		return;
	}

	m_points.points.erase(m_points.points.begin() + index);
	RecordChange(EShapeChangeReason::Topology);
}

void CPolygonShapeComponent::BeginEdit()
{
	m_bInEdit = true;
	m_bChangedDuringEdit = false;
	m_editReason = EShapeChangeReason::Geometry;
}

void CPolygonShapeComponent::EndEdit()
{
	const bool               bChanged = m_bChangedDuringEdit;
	const EShapeChangeReason reason = m_editReason;

	m_bInEdit = false;
	m_bChangedDuringEdit = false;
	m_editReason = EShapeChangeReason::Geometry;

	if (bChanged)
		NotifyListeners(reason);
}

void CPolygonShapeComponent::RecordChange(EShapeChangeReason reason)
{
	if (!m_bInEdit)
	{
		NotifyListeners(reason);
		return;
	}

	// Coarsest reason wins: a gesture that both moved and inserted points is a topology change.
	if (!m_bChangedDuringEdit || static_cast<int>(reason) > static_cast<int>(m_editReason))
		m_editReason = reason;

	m_bChangedDuringEdit = true;
}

// ---------------------------------------------------------------------------
// Listeners and events
// ---------------------------------------------------------------------------

void CPolygonShapeComponent::AddListener(IShapeListener* pListener)
{
	if (pListener == nullptr)
		return;
	if (std::find(m_listeners.begin(), m_listeners.end(), pListener) == m_listeners.end())
		m_listeners.push_back(pListener);
}

void CPolygonShapeComponent::RemoveListener(IShapeListener* pListener)
{
	m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), pListener), m_listeners.end());
}

void CPolygonShapeComponent::NotifyListeners(EShapeChangeReason reason)
{
	m_lastNotifiedPointCount = static_cast<int>(m_points.points.size());

	// Copy first: a listener is allowed to remove itself while being told.
	const std::vector<IShapeListener*> listeners = m_listeners;
	for (IShapeListener* pListener : listeners)
		pListener->OnShapeChanged(*this, reason);
}

void CPolygonShapeComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_XFORM:
		// The hull is unchanged in local space; only the placement moved.
		NotifyListeners(EShapeChangeReason::Transform);
		break;
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		{
			// An inspector edit can be anything. Rows added to or removed from the point list are
			// a topology change and force a full rebuild downstream; everything else - a point
			// dragged in a numeric row, Height, Closed, the offset - is a cheap geometry change.
			const int pointCount = static_cast<int>(m_points.points.size());
			NotifyListeners(pointCount != m_lastNotifiedPointCount ? EShapeChangeReason::Topology
			                                                       : EShapeChangeReason::Geometry);
		}
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CPolygonShapeComponent::GetEventMask() const
{
	return ENTITY_EVENT_XFORM | ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED;
}

void CPolygonShapeComponent::OnShutDown()
{
	m_listeners.clear();
}

// ---------------------------------------------------------------------------
// Previewer
// ---------------------------------------------------------------------------

#ifndef RELEASE
void CPolygonShapeComponent::Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const
{
	IRenderAuxGeom* pAux = gEnv->pAuxGeomRenderer;
	if (pAux == nullptr)
		return;

	Vec3      points[kMaxStackPoints];
	const int count = GatherPoints(points, kMaxStackPoints, true);
	if (count < 2)
		return;

	// Colours follow CShapeObject::DisplayNormal exactly: its own blue when the entity is not
	// selected, the editor's pulsing selection colour when it is (ShapeDisplay.h). An edge that
	// obstructs sound is drawn in legacy's highlight orange instead, so the obstruction pattern
	// is readable without opening the inspector.
	const ColorB colour = ShapeDisplay::GetOutlineColour(context.bSelected);
	const ColorB obstructColour = ShapeDisplay::GetHighlightColour(context.bSelected);

	const Vec3 up(0.0f, 0.0f, m_height);

	for (int i = 0; i < count; ++i)
	{
		const int j = (i + 1) % count;
		const bool bWrapEdge = (j == 0 && i != 0);
		if (!m_closed && bWrapEdge)
			continue; // an open polyline stops at the last point

		const ColorB edgeColour = IsEdgeObstructing(i) ? obstructColour : colour;

		pAux->DrawLine(points[i], edgeColour, points[j], edgeColour);

		if (m_height > 0.0f)
		{
			// The top ring, and the vertical edge that carries it - the legacy extrusion drawing
			// (ShapeObject.cpp:1336-1360), without the per-vertex-height variant, which no
			// component member exposes.
			pAux->DrawLine(points[i] + up, edgeColour, points[j] + up, edgeColour);
		}
	}

	if (m_height > 0.0f)
	{
		for (int i = 0; i < count; ++i)
			pAux->DrawLine(points[i], colour, points[i] + up, colour);
	}
}
#endif

} // namespace AreaComponents
} // namespace Cry
