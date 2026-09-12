// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "TriggerBoundsComponent.h"

#include <CryEntitySystem/IEntitySystem.h>
#include <CryRenderer/IRenderAuxGeom.h>

#include "../Shapes/ShapeDisplay.h"

namespace Cry
{
namespace AreaComponents
{

void CTriggerBoundsComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet.
}

void CTriggerBoundsComponent::Initialize()
{
	EnsureBound();
}

void CTriggerBoundsComponent::OnShutDown()
{
	UnbindShape();
}

Cry::Entity::EventFlags CTriggerBoundsComponent::GetEventMask() const
{
	// No per-frame work: the trigger itself follows the entity (CEntityComponentTriggerBounds
	// moves it on XFORM), so all this component has to hear about is a change of shape.
	// XFORM is subscribed only so that an unbound component can still find a shape that was added
	// after it - there is no entity event for "a component was added".
	// ENTERAREA / LEAVEAREA are subscribed unconditionally rather than only while Log Events is on:
	// the mask is read once when the component is added (and again on a property change), so a mask
	// that depended on a reflected bool would be a second thing to keep in sync for no gain - the
	// handler is two compares when logging is off.
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED | ENTITY_EVENT_XFORM | ENTITY_EVENT_RESET
	       | ENTITY_EVENT_ENTERAREA | ENTITY_EVENT_LEAVEAREA;
}

void CTriggerBoundsComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_XFORM:
		if (!m_bBoundToShape)
		{
			EnsureBound();
		}
		break;

	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
	case ENTITY_EVENT_RESET:
		EnsureBound();
		PushBounds();
		break;

	case ENTITY_EVENT_ENTERAREA:
	case ENTITY_EVENT_LEAVEAREA:
		if (m_bLogEvents)
		{
			// CProximityTriggerSystem's payload, NOT CArea's: nParam[0] is the entity that crossed
			// the box, nParam[1] is always 0, and nParam[2] is the trigger's own entity
			// (ProximityTriggerSystem.cpp, CProximityTriggerSystem::SendEvent). An area puts its
			// AREA ID in nParam[1], which is the payload clash documented in the header - so read
			// nParam[0] and nothing else here.
			const EntityId   crossingId = static_cast<EntityId>(event.nParam[0]);
			const IEntity*   pCrossing = (gEnv->pEntitySystem != nullptr) ? gEnv->pEntitySystem->GetEntity(crossingId) : nullptr;
			const IEntity*   pOwn = GetEntity();

			CryLogAlways("[TriggerBounds] entity '%s': %s by entity %u (%s)",
			             pOwn != nullptr ? pOwn->GetName() : "<none>",
			             event.event == ENTITY_EVENT_ENTERAREA ? "ENTERED" : "LEFT",
			             crossingId,
			             pCrossing != nullptr ? pCrossing->GetName() : "<unknown>");
		}
		break;

	default:
		break;
	}
}

IShapeComponent* CTriggerBoundsComponent::FindShape() const
{
	IEntity* pEntity = GetEntity();
	if (pEntity == nullptr)
		return nullptr;

	IEntityComponent* pComponent = pEntity->QueryComponentByInterfaceID(Schematyc::GetTypeDesc<IShapeComponent>().GetGUID());
	return (pComponent != nullptr) ? static_cast<IShapeComponent*>(pComponent) : nullptr;
}

IEntityTriggerComponent* CTriggerBoundsComponent::FindTrigger() const
{
	IEntity* pEntity = GetEntity();
	if (pEntity == nullptr || m_triggerGuid.IsNull())
		return nullptr;

	return static_cast<IEntityTriggerComponent*>(pEntity->GetComponentByGUID(m_triggerGuid));
}

void CTriggerBoundsComponent::EnsureBound()
{
	IEntity* pEntity = GetEntity();
	if (pEntity == nullptr)
		return;

	IShapeComponent* pShape = FindShape();
	if (pShape == nullptr)
	{
		UnbindShape();

		if (!m_bWarnedNoShape)
		{
			m_bWarnedNoShape = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Trigger Bounds on entity \"%s\" has no shape component - add one (Add Component -> Area) and the trigger "
			           "takes its box from it.", pEntity->GetName());
		}
		return;
	}

	m_bWarnedNoShape = false;

	if (m_triggerGuid.IsNull())
	{
		if (IEntityTriggerComponent* pTrigger = pEntity->GetOrCreateComponent<IEntityTriggerComponent>())
		{
			// Same reasoning as the Area function component's hidden area proxy: the trigger is
			// ours to drive and its box is derived from the shape, so it must not be written to
			// the level a second time.
			pTrigger->GetComponentFlags().Add(EEntityComponentFlags::NoSave);
			m_triggerGuid = pTrigger->GetGUID();
		}
	}

	if (!m_bBoundToShape)
	{
		pShape->AddListener(this);
		m_bBoundToShape = true;
		PushBounds();
	}
}

void CTriggerBoundsComponent::UnbindShape()
{
	if (!m_bBoundToShape)
		return;

	if (IShapeComponent* pShape = FindShape())
	{
		pShape->RemoveListener(this);
	}

	m_bBoundToShape = false;
}

void CTriggerBoundsComponent::OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason)
{
	// A proximity trigger has one AABB and nothing cheaper than replacing it, so every reason -
	// including a pure transform, because the box is expressed in the entity's local space and a
	// shape offset is part of it - takes the same path.
	PushBounds();
}

void CTriggerBoundsComponent::PushBounds()
{
	IShapeComponent*         pShape = FindShape();
	IEntityTriggerComponent* pTrigger = FindTrigger();
	if (pShape == nullptr || pTrigger == nullptr)
		return;

	AABB localBounds(AABB::RESET);
	pShape->GetLocalAABB(localBounds);
	if (localBounds.IsReset())
		return;

	pTrigger->SetTriggerBounds(localBounds);
	pTrigger->ForwardEventsTo(static_cast<EntityId>(m_forwardToEntityId));

	// Without this an entity that is already standing inside the new box never gets its enter
	// event - the trigger only reports crossings.
	pTrigger->InvalidateTrigger();
}

#ifndef RELEASE
void CTriggerBoundsComponent::Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const
{
	IRenderAuxGeom* pAux = gEnv->pAuxGeomRenderer;
	if (pAux == nullptr)
		return;

	// What is drawn is the TRIGGER's box, read back from the trigger itself rather than recomputed
	// from the shape: the point of the outline is to show what the proximity system actually tests,
	// which is an axis-aligned box in entity space with the entity's rotation and scale IGNORED
	// (IEntityTriggerComponent::SetTriggerBounds). On a rotated entity it therefore does not line
	// up with the shape's outline, and that is the truth the user needs to see.
	IEntityTriggerComponent* pTrigger = FindTrigger();
	if (pTrigger == nullptr)
		return;

	AABB localBox(AABB::RESET);
	pTrigger->GetTriggerBounds(localBox);
	if (localBox.IsReset() || localBox.IsEmpty())
		return;

	// Axis aligned in ENTITY space: translation only, no rotation, no scale - the same transform
	// the proximity system applies.
	Matrix34 triggerTM(IDENTITY);
	triggerTM.SetTranslation(entity.GetWorldTM().GetTranslation());

	pAux->DrawAABB(localBox, triggerTM, false, ShapeDisplay::GetHighlightColour(context.bSelected), eBBD_Faceted);
}
#endif

} // namespace AreaComponents
} // namespace Cry
