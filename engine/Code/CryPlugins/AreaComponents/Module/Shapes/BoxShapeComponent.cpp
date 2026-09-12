// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "BoxShapeComponent.h"
#include "ShapeDisplay.h"

#include <CryRenderer/IRenderAuxGeom.h>
#include <CryMath/Random.h>

#include <algorithm>
#include <limits>

namespace Cry
{
namespace AreaComponents
{

void CBoxShapeComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet - stage 1 exposes IsPointInside / GetRandomPointInside here.
}

Matrix34 CBoxShapeComponent::GetShapeWorldTM() const
{
	return GetWorldTransformMatrix() * Matrix34::CreateTranslationMat(m_offset);
}

void CBoxShapeComponent::GetLocalAABB(AABB& out) const
{
	const Vec3 half = m_size * 0.5f;
	out = AABB(m_offset - half, m_offset + half);
}

void CBoxShapeComponent::GetWorldAABB(AABB& out) const
{
	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetWorldTransformMatrix(), local);
}

bool CBoxShapeComponent::IsPointInside(const Vec3& world) const
{
	const Vec3 local = GetShapeWorldTM().GetInverted().TransformPoint(world);
	const Vec3 half = m_size * 0.5f;
	return fabs_tpl(local.x) <= half.x && fabs_tpl(local.y) <= half.y && fabs_tpl(local.z) <= half.z;
}

float CBoxShapeComponent::DistanceToHull(const Vec3& world) const
{
	// The usual signed box distance field, evaluated in the box's own space: negative inside,
	// zero on the hull, positive outside. This is the primitive the authored fade distances of
	// the Area function component will be built on.
	const Vec3 local = GetShapeWorldTM().GetInverted().TransformPoint(world);
	const Vec3 half = m_size * 0.5f;
	const Vec3 d(fabs_tpl(local.x) - half.x, fabs_tpl(local.y) - half.y, fabs_tpl(local.z) - half.z);

	const Vec3  outside(max(d.x, 0.0f), max(d.y, 0.0f), max(d.z, 0.0f));
	const float inside = min(max(d.x, max(d.y, d.z)), 0.0f);
	return outside.GetLength() + inside;
}

bool CBoxShapeComponent::IntersectRay(const Ray& ray, float& dist) const
{
	// Slab test in the box's own space. The inverse transform is affine, so the ray parameter is
	// preserved and the local direction must NOT be re-normalised - t comes out in world units.
	const Matrix34 invTM = GetShapeWorldTM().GetInverted();
	const Vec3     origin = invTM.TransformPoint(ray.origin);
	const Vec3     dir = invTM.TransformVector(ray.direction);
	const Vec3     half = m_size * 0.5f;

	float tMin = 0.0f;
	float tMax = std::numeric_limits<float>::max();

	for (int axis = 0; axis < 3; ++axis)
	{
		if (fabs_tpl(dir[axis]) < FLT_EPSILON)
		{
			if (fabs_tpl(origin[axis]) > half[axis])
				return false; // parallel to the slab and outside it
			continue;
		}

		const float invDir = 1.0f / dir[axis];
		float       t0 = (-half[axis] - origin[axis]) * invDir;
		float       t1 = (half[axis] - origin[axis]) * invDir;
		if (t0 > t1)
			std::swap(t0, t1);

		tMin = max(tMin, t0);
		tMax = min(tMax, t1);
		if (tMin > tMax)
			return false;
	}

	dist = tMin;
	return true;
}

bool CBoxShapeComponent::GetRandomPointInside(Vec3& out) const
{
	const Vec3 half = m_size * 0.5f;
	const Vec3 local(cry_random(-half.x, half.x), cry_random(-half.y, half.y), cry_random(-half.z, half.z));
	out = GetShapeWorldTM().TransformPoint(local);
	return true;
}

bool CBoxShapeComponent::GetAreaVolume(SShapeAreaVolume& out) const
{
	// A box maps straight onto ENTITY_AREA_TYPE_BOX: local min/max plus the entity's world matrix,
	// which is the one legacy area type whose containment stays correct under a non-uniform scale
	// (CArea keeps the local geometry and inverts the matrix instead of baking world points).
	const Vec3 half = m_size * 0.5f;

	out.form = EAreaVolumeForm::Box;
	out.boxMin = m_offset - half;
	out.boxMax = m_offset + half;

	// Per-side obstruction is not authored on the box shape yet (the polygon carries it per edge);
	// six non-obstructing sides is what a legacy AreaBox has until someone ticks a side.
	for (int i = 0; i < 6; ++i)
		out.boxObstruct[i] = false;

	return true;
}

int CBoxShapeComponent::GetContour(Vec3* pOutPoints, int maxPoints, bool world) const
{
	// The footprint: the four bottom corners, counter-clockwise seen from above. The closing edge
	// is implicit, the same convention the polygon shape will use. The buffer is the caller's -
	// see the heap rule in IShapeComponent.h - so nothing is allocated here.
	const Vec3 half = m_size * 0.5f;
	const Vec3 corners[4] =
	{
		Vec3(-half.x, -half.y, -half.z),
		Vec3(half.x,  -half.y, -half.z),
		Vec3(half.x,  half.y,  -half.z),
		Vec3(-half.x, half.y,  -half.z),
	};

	const int totalPoints = 4;
	if (pOutPoints == nullptr || maxPoints <= 0)
		return totalPoints;

	const int  writtenPoints = min(maxPoints, totalPoints);
	const Matrix34 tm = GetShapeWorldTM();

	for (int i = 0; i < writtenPoints; ++i)
		pOutPoints[i] = world ? tm.TransformPoint(corners[i]) : (m_offset + corners[i]);

	return totalPoints;
}

bool CBoxShapeComponent::GetEditorLocalBounds(AABB& out) const
{
	// CEntityObject::GetLocalBounds works in the ENTITY's local space, so the component's own
	// transform (identity unless the Transform flag is set) has to be applied here.
	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetTransformMatrix(), local);
	return true;
}

namespace
{

//! Shortest distance between a ray and a segment, plus the ray parameter at the closest point.
//! Both in world space; `ray.direction` is expected to be normalised, so `rayT` is a distance.
bool RayToSegmentDistance(const Ray& ray, const Vec3& a, const Vec3& b, float& distance, float& rayT)
{
	const Vec3  segment = b - a;
	const Vec3  diff = ray.origin - a;
	const float segmentLengthSq = segment.GetLengthSquared();
	if (segmentLengthSq < FLT_EPSILON)
		return false;

	const float dirDotSeg = ray.direction.Dot(segment);
	const float dirDotDiff = ray.direction.Dot(diff);
	const float segDotDiff = segment.Dot(diff);

	// Solve the 2x2 system for the closest pair (rayT along the ray, segT along the segment).
	const float denominator = segmentLengthSq - dirDotSeg * dirDotSeg;

	float segT;
	if (fabs_tpl(denominator) < FLT_EPSILON)
	{
		// Parallel: any point of the segment is as good as the next, take its start.
		rayT = -dirDotDiff;
		segT = 0.0f;
	}
	else
	{
		rayT = (dirDotSeg * segDotDiff - segmentLengthSq * dirDotDiff) / denominator;
		segT = (dirDotSeg * rayT + segDotDiff) / segmentLengthSq;
	}

	segT = clamp_tpl(segT, 0.0f, 1.0f);
	rayT = max(0.0f, ray.direction.Dot((a + segment * segT) - ray.origin));

	const Vec3 pointOnRay = ray.origin + ray.direction * rayT;
	const Vec3 pointOnSegment = a + segment * segT;
	distance = (pointOnRay - pointOnSegment).GetLength();
	return true;
}

} // anonymous namespace

bool CBoxShapeComponent::EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const
{
	// Two chances, in the order the user expects. First the twelve wireframe edges, because the
	// previewer draws the box as a wireframe and clicking a drawn line is what "selecting the
	// shape" means; `tolerance` is the world-space width the viewport gave that line. Only then
	// the slab test, so a click inside a large box still selects it.
	const Matrix34 tm = GetShapeWorldTM();
	const Vec3     half = m_size * 0.5f;

	const Vec3 corners[8] =
	{
		tm.TransformPoint(Vec3(-half.x, -half.y, -half.z)),
		tm.TransformPoint(Vec3(half.x,  -half.y, -half.z)),
		tm.TransformPoint(Vec3(half.x,  half.y,  -half.z)),
		tm.TransformPoint(Vec3(-half.x, half.y,  -half.z)),
		tm.TransformPoint(Vec3(-half.x, -half.y, half.z)),
		tm.TransformPoint(Vec3(half.x,  -half.y, half.z)),
		tm.TransformPoint(Vec3(half.x,  half.y,  half.z)),
		tm.TransformPoint(Vec3(-half.x, half.y,  half.z)),
	};

	static const int edges[12][2] =
	{
		{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // bottom
		{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // top
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, // sides
	};

	const Ray normalisedRay(worldRay.origin, worldRay.direction.GetNormalized());

	bool  bHit = false;
	float bestRayT = FLT_MAX;

	for (const auto& edge : edges)
	{
		float distance, rayT;
		if (!RayToSegmentDistance(normalisedRay, corners[edge[0]], corners[edge[1]], distance, rayT))
			continue;

		if (distance <= tolerance && rayT < bestRayT)
		{
			bestRayT = rayT;
			bHit = true;
		}
	}

	if (bHit)
	{
		distOut = bestRayT;
		return true;
	}

	return IntersectRay(normalisedRay, distOut);
}

Vec3 CBoxShapeComponent::GetPoint(int index) const
{
	const Vec3 half = m_size * 0.5f;
	return (index <= 0) ? (m_offset - half) : (m_offset + half);
}

void CBoxShapeComponent::SetPoint(int index, const Vec3& local)
{
	if (index < 0 || index > 1)
		return;

	// The dragged corner moves, the opposite corner stays where it is; size and offset follow.
	Vec3 minCorner = GetPoint(0);
	Vec3 maxCorner = GetPoint(1);

	if (index == 0)
		minCorner = local;
	else
		maxCorner = local;

	for (int axis = 0; axis < 3; ++axis)
	{
		if (maxCorner[axis] - minCorner[axis] < kMinSize)
		{
			// Never let a drag collapse the box: push the dragged side back off the fixed one.
			if (index == 0)
				minCorner[axis] = maxCorner[axis] - kMinSize;
			else
				maxCorner[axis] = minCorner[axis] + kMinSize;
		}
	}

	m_size = maxCorner - minCorner;
	m_offset = (maxCorner + minCorner) * 0.5f;

	if (m_bInEdit)
		m_bChangedDuringEdit = true;
	else
		NotifyListeners(EShapeChangeReason::Geometry);
}

void CBoxShapeComponent::BeginEdit()
{
	m_bInEdit = true;
	m_bChangedDuringEdit = false;
}

void CBoxShapeComponent::EndEdit()
{
	const bool bChanged = m_bChangedDuringEdit;
	m_bInEdit = false;
	m_bChangedDuringEdit = false;

	if (bChanged)
		NotifyListeners(EShapeChangeReason::Geometry);
}

void CBoxShapeComponent::AddListener(IShapeListener* pListener)
{
	if (pListener == nullptr)
		return;
	if (std::find(m_listeners.begin(), m_listeners.end(), pListener) == m_listeners.end())
		m_listeners.push_back(pListener);
}

void CBoxShapeComponent::RemoveListener(IShapeListener* pListener)
{
	m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), pListener), m_listeners.end());
}

void CBoxShapeComponent::NotifyListeners(EShapeChangeReason reason)
{
	// Copy first: a listener is allowed to remove itself while being told.
	const std::vector<IShapeListener*> listeners = m_listeners;
	for (IShapeListener* pListener : listeners)
		pListener->OnShapeChanged(*this, reason);
}

void CBoxShapeComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_XFORM:
		NotifyListeners(EShapeChangeReason::Transform);
		break;
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		NotifyListeners(EShapeChangeReason::Geometry);
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CBoxShapeComponent::GetEventMask() const
{
	return ENTITY_EVENT_XFORM | ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED;
}

void CBoxShapeComponent::OnShutDown()
{
	m_listeners.clear();
}

#ifndef RELEASE
void CBoxShapeComponent::Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const
{
	IRenderAuxGeom* pAux = gEnv->pAuxGeomRenderer;
	if (pAux == nullptr)
		return;

	// Colours follow CShapeObject exactly: its own blue when the entity is not selected, the
	// editor's pulsing selection colour when it is (ShapeDisplay.h).
	const ColorB colour = ShapeDisplay::GetOutlineColour(context.bSelected);

	const Vec3 half = m_size * 0.5f;
	const AABB localBox(-half, half);

	pAux->DrawAABB(localBox, GetShapeWorldTM(), false, colour, eBBD_Faceted);
}
#endif

} // namespace AreaComponents
} // namespace Cry
