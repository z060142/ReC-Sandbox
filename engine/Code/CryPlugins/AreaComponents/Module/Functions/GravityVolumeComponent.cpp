// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "GravityVolumeComponent.h"
#include "../Shapes/ShapeDisplay.h"

#include <CryPhysics/IPhysics.h>
#include <CryPhysics/physinterface.h>
#include <CryRenderer/IRenderAuxGeom.h>

namespace Cry
{
namespace AreaComponents
{

namespace
{
//! How many circles the previewer draws around the tube, and how many chords each one has.
const int kGravityPreviewRings = 24;
const int kGravityPreviewRingSegments = 12;
}

void CGravityVolumeComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet: enabling and disabling is the Enabled member, and everything else
	// a script could want is a physics query, not a component call.
}

// ---------------------------------------------------------------------------
// Binding by rule
// ---------------------------------------------------------------------------

ISplineShape* CGravityVolumeComponent::FindSpline() const
{
	if (m_pEntity == nullptr)
		return nullptr;

	IShapeComponent* pShape = m_pEntity->GetComponent<IShapeComponent>();
	return pShape != nullptr ? pShape->GetSpline() : nullptr;
}

ISplineShape* CGravityVolumeComponent::EnsureBound()
{
	IShapeComponent* pShape = (m_pEntity != nullptr) ? m_pEntity->GetComponent<IShapeComponent>() : nullptr;
	ISplineShape*    pSpline = (pShape != nullptr) ? pShape->GetSpline() : nullptr;

	if (pSpline == nullptr)
	{
		Unbind();

		if (!m_bWarnedNoSpline)
		{
			m_bWarnedNoSpline = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Gravity Volume on entity \"%s\": no spline shape on this entity - add \"Shape: Spline\" to give it a volume.",
			           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>");
		}

		return nullptr;
	}

	m_bWarnedNoSpline = false;

	if (m_pBoundShape != pShape)
	{
		Unbind();
		m_pBoundShape = pShape;
		pShape->AddListener(this);
	}

	return pSpline;
}

void CGravityVolumeComponent::Unbind()
{
	if (m_pBoundShape != nullptr)
	{
		m_pBoundShape->RemoveListener(this);
		m_pBoundShape = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Physics
// ---------------------------------------------------------------------------

void CGravityVolumeComponent::Physicalize()
{
	if (m_pEntity == nullptr || gEnv->pPhysicalWorld == nullptr)
		return;

	// Always dephysicalize first: an area's gravity is not reset by re-adding one, which is the
	// comment CAreaComponent::Physicalize carries above the identical line (AreaComponent.cpp:68-72).
	SEntityPhysicalizeParams clearParams;
	clearParams.type = PE_NONE;
	m_pEntity->Physicalize(clearParams);

	if (!m_bEnabled)
		return;

	ISplineShape* pSpline = EnsureBound();
	if (pSpline == nullptr || m_pBoundShape == nullptr)
		return;

	IShapeComponentEdit* pEdit = m_pBoundShape->GetEditInterface();
	if (pEdit == nullptr)
		return;

	const int pointCount = min(pEdit->GetPointCount(), kMaxStackPoints - 1);
	if (pointCount < 2)
		return;

	const Vec3  entityPos = m_pEntity->GetWorldPos();
	const Quat  entityRot = m_pEntity->GetWorldRotation();
	const Vec3  entityScale = m_pEntity->GetScale();
	const float scale = max(0.0001f, entityScale.len() / static_cast<float>(sqrt3));

	const Matrix34 shapeWorldTM = m_pBoundShape->GetWorldTransformMatrix();
	const Quat     invRot = entityRot.GetInverted();

	// A SCRATCH copy, always: AddArea takes a non-const Vec3* and the entity-side path it mirrors
	// rewrites its input in place (PhysicsProxy.cpp:1591-1593). Handing it the shape's own storage
	// would let the spline drift a little further with every rebuild.
	Vec3 points[kMaxStackPoints];
	for (int i = 0; i < pointCount; ++i)
	{
		// Shape-local -> world -> the unscaled entity space AddArea's pos/rot/scale arguments define.
		const Vec3 world = shapeWorldTM.TransformPoint(pEdit->GetPoint(i));
		points[i] = (invRot * (world - entityPos)) / scale;
	}

	int usedPoints = pointCount;
	if (pSpline->IsClosed())
	{
		// The tube has to come back to where it started. CAreaComponent does the same by appending
		// the first point (AreaComponent.cpp:140-146) - except that it has to GUESS closedness from
		// an AABB-centre containment test, because a legacy area shape has no closed flag. Ours does.
		points[usedPoints++] = points[0];
	}

	IPhysicalEntity* pArea = gEnv->pPhysicalWorld->AddArea(points, usedPoints, m_radius, entityPos, entityRot, scale);
	if (pArea == nullptr)
		return;

	m_pEntity->AssignPhysicalEntity(pArea);

	pe_params_area areaParams;
	areaParams.gravity = Vec3(0.0f, 0.0f, m_gravity);
	// A spline area is never uniform - the gravity points at the tube's axis, which is the whole
	// point of a gravity volume. The entity path forces the same thing (PhysicsProxy.cpp:1596-1597).
	areaParams.bUniform = 0;
	areaParams.falloff0 = m_falloff;
	areaParams.damping = m_damping;
	pArea->SetParams(&areaParams);

	// Legacy sets this so that the area can be switched off when the entity stops being rendered
	// (AreaProxy.cpp:169-190). We keep the flag meaningful by only asking for the render event when
	// the user wants that behaviour.
	if (!m_bDontDisableInvisible)
	{
		m_pEntity->SetFlags(m_pEntity->GetFlags() | ENTITY_FLAG_SEND_RENDER_EVENT);
	}
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void CGravityVolumeComponent::Initialize()
{
	Physicalize();
}

void CGravityVolumeComponent::OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason)
{
	// A transform change needs nothing: the area is assigned to the entity, so the entity moves it.
	// Rebuilding here would also mean one AddArea per frame of a drag, and AddArea is not
	// incremental (research/08 section 4). Geometry and topology changes arrive exactly once per
	// edit gesture, because the shape coalesces everything between BeginEdit and EndEdit.
	if (reason == EShapeChangeReason::Transform)
		return;

	Physicalize();
}

void CGravityVolumeComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		Physicalize();
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CGravityVolumeComponent::GetEventMask() const
{
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED;
}

void CGravityVolumeComponent::OnShutDown()
{
	Unbind();

	if (m_pEntity != nullptr)
	{
		SEntityPhysicalizeParams clearParams;
		clearParams.type = PE_NONE;
		m_pEntity->Physicalize(clearParams);
	}
}

// ---------------------------------------------------------------------------
// Previewer
// ---------------------------------------------------------------------------

#ifndef RELEASE
void CGravityVolumeComponent::Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const
{
	IRenderAuxGeom* pAux = gEnv->pAuxGeomRenderer;
	if (pAux == nullptr)
		return;

	ISplineShape* pSpline = FindSpline();
	if (pSpline == nullptr || m_radius <= 0.0f)
		return;

	// The tube, as rings around the curve. Legacy draws the same thing out of its own duplicated
	// spline code (GravityVolumeObject.cpp:1196-1280); this version rides the real curve, so what is
	// drawn and what is physicalized are the same shape - which they are not in legacy, where the
	// display uses a quadratic interpolator and the physics uses the raw control points.
	const ColorB colour = ShapeDisplay::GetOutlineColour(context.bSelected);

	for (int ring = 0; ring <= kGravityPreviewRings; ++ring)
	{
		const float t = static_cast<float>(ring) / static_cast<float>(kGravityPreviewRings);

		const Vec3 centre = pSpline->EvalPos(t);
		Vec3       tangent = pSpline->EvalTangent(t);
		if (tangent.IsZero())
			continue;
		tangent.Normalize();

		Vec3 normal = pSpline->EvalNormal(t);
		if (normal.IsZero())
			normal = tangent.GetOrthogonal().GetNormalized();

		const Vec3 binormal = tangent.Cross(normal).GetNormalized();

		Vec3 previous = centre + normal * m_radius;
		for (int k = 1; k <= kGravityPreviewRingSegments; ++k)
		{
			const float angle = (2.0f * gf_PI * static_cast<float>(k)) / static_cast<float>(kGravityPreviewRingSegments);
			const Vec3  next = centre + (normal * cosf(angle) + binormal * sinf(angle)) * m_radius;

			pAux->DrawLine(previous, colour, next, colour);
			previous = next;
		}
	}
}
#endif

} // namespace AreaComponents
} // namespace Cry
