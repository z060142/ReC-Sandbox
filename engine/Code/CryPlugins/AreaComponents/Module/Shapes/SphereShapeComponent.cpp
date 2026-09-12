// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "SphereShapeComponent.h"
#include "ShapeDisplay.h"

#include <CryRenderer/IRenderAuxGeom.h>
#include <CryMath/Random.h>

#include <algorithm>

namespace Cry
{
namespace AreaComponents
{

void CSphereShapeComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet - stage 2 exposes IsPointInside / GetRandomPointInside here,
	// for every kind at once.
}

// ---------------------------------------------------------------------------
// Local-to-world: position and orientation from the matrix, size from the scale rule
// ---------------------------------------------------------------------------

Matrix34 CSphereShapeComponent::GetShapeWorldTM() const
{
	Matrix34 tm = GetWorldTransformMatrix();
	tm.OrthonormalizeFast(); // rotation only - the scale is handled by GetWorldRadius()
	tm.SetTranslation(GetWorldCentre());
	return tm;
}

Vec3 CSphereShapeComponent::GetWorldCentre() const
{
	return GetWorldTransformMatrix().TransformPoint(m_offset);
}

float CSphereShapeComponent::GetWorldRadius() const
{
	const Matrix34 worldTM = GetWorldTransformMatrix();

	if (IsNonUniformlyScaled(worldTM) && !m_bNonUniformScaleWarned)
	{
		// Latched: a user dragging a scale gizmo would otherwise fill the console at frame rate.
		m_bNonUniformScaleWarned = true;

		const IEntity* pEntity = GetEntity();
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Shape: Sphere on entity \"%s\" is non-uniformly scaled. A sphere cannot be an ellipsoid, "
		           "so the largest axis scale is used for the whole shape - author the size with the Radius "
		           "property instead of the entity's scale.",
		           pEntity != nullptr ? pEntity->GetName() : "<none>");
	}

	return max(kMinRadius, m_radius * GetLargestAxisScale(worldTM));
}

// ---------------------------------------------------------------------------
// IShapeComponent
// ---------------------------------------------------------------------------

void CSphereShapeComponent::GetLocalAABB(AABB& out) const
{
	// Local space: the authored radius, unscaled. CEntityObject::GetLocalBounds applies the
	// entity transform on top, exactly as it does for the box.
	const Vec3 extent(m_radius, m_radius, m_radius);
	out = AABB(m_offset - extent, m_offset + extent);
}

void CSphereShapeComponent::GetWorldAABB(AABB& out) const
{
	// A sphere's world bounds are axis aligned whatever the rotation is, so this is exact rather
	// than the transformed-AABB approximation a box has to settle for.
	const float radius = GetWorldRadius();
	const Vec3  centre = GetWorldCentre();
	const Vec3  extent(radius, radius, radius);
	out = AABB(centre - extent, centre + extent);
}

bool CSphereShapeComponent::IsPointInside(const Vec3& world) const
{
	const float radius = GetWorldRadius();
	return (world - GetWorldCentre()).GetLengthSquared() <= radius * radius;
}

float CSphereShapeComponent::DistanceToHull(const Vec3& world) const
{
	// The signed distance field of a ball: negative inside, zero on the hull, positive outside.
	return (world - GetWorldCentre()).GetLength() - GetWorldRadius();
}

bool CSphereShapeComponent::IntersectRay(const Ray& ray, float& dist) const
{
	// The quadratic, with a normalised direction so that t comes out in metres.
	const Vec3  direction = ray.direction.GetNormalizedSafe(Vec3(0.0f, 0.0f, 1.0f));
	const Vec3  toCentre = ray.origin - GetWorldCentre();
	const float radius = GetWorldRadius();

	const float b = toCentre.Dot(direction);
	const float c = toCentre.GetLengthSquared() - radius * radius;

	const float discriminant = b * b - c;
	if (discriminant < 0.0f)
		return false;

	const float root = sqrt_tpl(discriminant);
	const float tNear = -b - root;
	const float tFar = -b + root;

	// The nearest hit ahead of the origin. An origin inside the ball gets the exit point, which
	// is what a slab test on the box returns for the same situation.
	if (tNear >= 0.0f)
		dist = tNear;
	else if (tFar >= 0.0f)
		dist = tFar;
	else
		return false;

	return true;
}

bool CSphereShapeComponent::GetRandomPointInside(Vec3& out) const
{
	// Uniform in the VOLUME, not in the radius: a direction on the unit sphere scaled by
	// r * cbrt(u). Sampling the radius linearly would crowd the points around the centre.
	const float radius = GetWorldRadius();

	Vec3 direction(cry_random(-1.0f, 1.0f), cry_random(-1.0f, 1.0f), cry_random(-1.0f, 1.0f));
	const float lengthSq = direction.GetLengthSquared();
	if (lengthSq < kEpsilon)
		direction = Vec3(0.0f, 0.0f, 1.0f);
	else
		direction /= sqrt_tpl(lengthSq);

	const float u = cry_random(0.0f, 1.0f);
	out = GetWorldCentre() + direction * (radius * pow_tpl(u, 1.0f / 3.0f));
	return true;
}

bool CSphereShapeComponent::GetAreaVolume(SShapeAreaVolume& out) const
{
	out.form = EAreaVolumeForm::Sphere;

	// The centre is local - IEntityAreaComponent::SetSphere() puts it through the entity's world
	// matrix itself. The RADIUS is the world radius, not the authored one: legacy never scales a
	// sphere area's radius (AreaProxy.cpp), so a scaled legacy sphere area and its drawn shape
	// disagree. Feeding the world radius is what keeps the area exactly the ball this component
	// draws, tests and reports distances against under any uniform scale, and it applies the same
	// largest-axis rule the rest of this component uses for a non-uniform one.
	out.sphereCentre = m_offset;
	out.sphereRadius = GetWorldRadius();

	return true;
}

int CSphereShapeComponent::GetContour(Vec3* pOutPoints, int maxPoints, bool world) const
{
	// A sphere has no footprint in the sense a polygon has one, and the contract owes every
	// consumer SOMETHING closed that encloses the shape on a plane. The convention chosen here,
	// and the one function components and the editor tool may rely on:
	//
	//     GetContour() is a kCircleSegments-segment circle of the shape's radius, on the LOCAL XY
	//     plane through the centre - i.e. the sphere's equator in its own space.
	//
	// It is the great circle a user would draw if asked to trace the shape on the ground, it
	// matches what Box returns (its bottom quad, also a closed footprint) and it degenerates
	// correctly: sampling it gives the outline the point tool draws. The closing edge is implicit,
	// the same convention every other kind uses. The buffer is the CALLER's (heap rule,
	// IShapeComponent.h) - nothing is allocated here.
	const int totalPoints = kCircleSegments;
	if (pOutPoints == nullptr || maxPoints <= 0)
		return totalPoints;

	const int      writtenPoints = min(maxPoints, totalPoints);
	const Matrix34 tm = GetShapeWorldTM();
	const float    radius = world ? GetWorldRadius() : m_radius;

	for (int i = 0; i < writtenPoints; ++i)
	{
		const float angle = (gf_PI2 * i) / totalPoints;
		const Vec3  local(cos_tpl(angle) * radius, sin_tpl(angle) * radius, 0.0f);

		pOutPoints[i] = world ? tm.TransformPoint(local) : (m_offset + local);
	}

	return totalPoints;
}

// ---------------------------------------------------------------------------
// IEditorShapeComponent
// ---------------------------------------------------------------------------

bool CSphereShapeComponent::GetEditorLocalBounds(AABB& out) const
{
	// CEntityObject::GetLocalBounds works in the ENTITY's local space, so the component's own
	// transform (identity unless the Transform flag is set) has to be applied here.
	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetTransformMatrix(), local);
	return true;
}

bool CSphereShapeComponent::EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const
{
	// CAreaSphere::HitTest (AreaSphere.cpp:86-104) picks the SURFACE, not the volume: the distance
	// from the centre to the ray has to land inside a band around the radius. Deliberately kept:
	// the previewer draws three wire circles and nothing else, and a solid test would make a big
	// sphere swallow every click inside it - the same reasoning the polygon's hit test follows.
	const Vec3  centre = GetWorldCentre();
	const float radius = GetWorldRadius();

	const Vec3  direction = worldRay.direction.GetNormalizedSafe(Vec3(0.0f, 0.0f, 1.0f));
	const Vec3  toCentre = centre - worldRay.origin;
	const float distanceToAxis = direction.Cross(toCentre).GetLength();

	if (distanceToAxis > radius + tolerance || distanceToAxis < radius - tolerance)
		return false;

	// Legacy reports the distance to the centre, which is what orders this hit against the other
	// objects under the cursor.
	distOut = toCentre.GetLength();
	return true;
}

// ---------------------------------------------------------------------------
// IShapeComponentEdit - two handles: the centre, and a radius handle on local +X
// ---------------------------------------------------------------------------

Vec3 CSphereShapeComponent::GetPoint(int index) const
{
	if (index <= 0)
		return m_offset;

	return m_offset + Vec3(m_radius, 0.0f, 0.0f);
}

void CSphereShapeComponent::SetPoint(int index, const Vec3& local)
{
	if (index < 0 || index > 1)
		return;

	if (index == 0)
	{
		// The centre handle moves the whole sphere; the radius handle follows it by construction.
		m_offset = local;
	}
	else
	{
		// The radius handle: the distance from the centre is the radius, whichever direction the
		// user dragged it in. Dragging it through the centre never inverts the shape.
		m_radius = max(kMinRadius, (local - m_offset).GetLength());
	}

	if (m_bInEdit)
		m_bChangedDuringEdit = true;
	else
		NotifyListeners(index == 0 ? EShapeChangeReason::Transform : EShapeChangeReason::Geometry);
}

void CSphereShapeComponent::BeginEdit()
{
	m_bInEdit = true;
	m_bChangedDuringEdit = false;
}

void CSphereShapeComponent::EndEdit()
{
	const bool bChanged = m_bChangedDuringEdit;
	m_bInEdit = false;
	m_bChangedDuringEdit = false;

	if (bChanged)
		NotifyListeners(EShapeChangeReason::Geometry);
}

// ---------------------------------------------------------------------------
// Listeners and events
// ---------------------------------------------------------------------------

void CSphereShapeComponent::AddListener(IShapeListener* pListener)
{
	if (pListener == nullptr)
		return;
	if (std::find(m_listeners.begin(), m_listeners.end(), pListener) == m_listeners.end())
		m_listeners.push_back(pListener);
}

void CSphereShapeComponent::RemoveListener(IShapeListener* pListener)
{
	m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), pListener), m_listeners.end());
}

void CSphereShapeComponent::NotifyListeners(EShapeChangeReason reason)
{
	// Copy first: a listener is allowed to remove itself while being told.
	const std::vector<IShapeListener*> listeners = m_listeners;
	for (IShapeListener* pListener : listeners)
		pListener->OnShapeChanged(*this, reason);
}

void CSphereShapeComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_XFORM:
		// A scale change arrives here too, and the shape it produces may be a different size - so
		// the latch is released, letting the warning speak again for a NEW non-uniform scale.
		m_bNonUniformScaleWarned = false;
		NotifyListeners(EShapeChangeReason::Transform);
		break;
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		NotifyListeners(EShapeChangeReason::Geometry);
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CSphereShapeComponent::GetEventMask() const
{
	return ENTITY_EVENT_XFORM | ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED;
}

void CSphereShapeComponent::OnShutDown()
{
	m_listeners.clear();
}

// ---------------------------------------------------------------------------
// Previewer - the three great circles of CAreaSphere's DrawWireSphere
// ---------------------------------------------------------------------------

#ifndef RELEASE
void CSphereShapeComponent::Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const
{
	IRenderAuxGeom* pAux = gEnv->pAuxGeomRenderer;
	if (pAux == nullptr)
		return;

	const float radius = GetWorldRadius();
	if (radius <= 0.0f)
		return;

	const Matrix34 tm = GetShapeWorldTM();
	const ColorB   colour = ShapeDisplay::GetOutlineColour(context.bSelected);

	// SDisplayContext::DrawWireSphere draws three great circles, one per plane; that is what a
	// legacy CAreaSphere looks like and what makes an unfilled sphere readable from any angle.
	// They are drawn in the shape's own space, so a rotated entity rotates its wireframe - which
	// is invisible on a sphere, and correct the day the kind grows an axis-dependent feature.
	static const int kPlaneAxes[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };

	for (const auto& planeAxes : kPlaneAxes)
	{
		Vec3 previous(ZERO);

		for (int segment = 0; segment <= kCircleSegments; ++segment)
		{
			const float angle = (gf_PI2 * segment) / kCircleSegments;

			Vec3 local(ZERO);
			local[planeAxes[0]] = cos_tpl(angle) * radius;
			local[planeAxes[1]] = sin_tpl(angle) * radius;

			const Vec3 world = tm.TransformPoint(local);

			if (segment > 0)
				pAux->DrawLine(previous, colour, world, colour);

			previous = world;
		}
	}
}
#endif

} // namespace AreaComponents
} // namespace Cry
