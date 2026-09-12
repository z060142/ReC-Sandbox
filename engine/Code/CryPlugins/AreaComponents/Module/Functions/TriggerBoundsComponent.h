// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>

namespace Cry
{
namespace AreaComponents
{

//! "Trigger Bounds": the second consumer of the shape contract, and the one that proves the
//! contract is not secretly the Area component's private interface - it needs no engine change at
//! all and touches no CArea.
//!
//! It feeds the stock IEntityTriggerComponent, which is a CProximityTriggerSystem trigger, not an
//! area: it fires ENTITY_EVENT_ENTERAREA / LEAVEAREA for any entity crossing an axis-aligned box
//! in the entity's LOCAL space, with the entity's rotation and scale ignored (the documented
//! contract of SetTriggerBounds). So this component takes the local AABB of whatever shape sits on
//! the same entity and hands it over, on bind and on every geometry change, followed by
//! InvalidateTrigger() so that entities already standing inside are caught.
//!
//! What it deliberately does NOT do is pretend to be an area. The proximity system's payload is
//! incompatible with CArea's - it puts the triggering entity in nParam[1], where an area puts its
//! id - so an entity should carry either this or the Area function component, not both. That is a
//! documented sharp edge of the engine, not something this component can fix.
class CTriggerBoundsComponent final
	: public IEntityComponent
	, public IShapeListener
#ifndef RELEASE
	, public IEntityComponentPreviewer
#endif
{
public:
	CTriggerBoundsComponent() = default;
	virtual ~CTriggerBoundsComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CTriggerBoundsComponent>& desc)
	{
		// GUID rule for every component in this folder, and the two reasons for it:
		//
		// 1. The GUID must be unique across the whole module. A duplicate does not lose one
		//    component: CEnvRegistry::RegisterPackageElements fails and RegisterPackage then
		//    releases and erases the ENTIRE package, so every shape kind disappears with it.
		// 2. The HIGH HALF must be unique too, among component types that can sit on ONE entity.
		//    CEntityObject::CreateComponentWidgets keys each component's inspector tree by the
		//    class-desc GUID, and stock 5.7.1 uses only GUID::hipart for that key;
		//    CInspectorWidgetCreator reads a repeated key as one widget shared by several selected
		//    objects and erases it, so two such components blank the whole inspector. Our EditorQt
		//    now folds both halves, but a component we ship must not depend on that fix being
		//    present, so each function component here gets a GUID series of its own rather than a
		//    neighbouring number in someone else's.
		//
		// Hence not {B4E27C09-8A16-4D53-...}, which is the Area function component's series.
		desc.SetGUID("{7D5C9E42-3B18-4A6F-8C21-05E7B9D34611}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Trigger Bounds");
		desc.SetDescription("Feeds the shape on this entity to the entity's proximity trigger, so entities crossing it fire enter/leave events.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		desc.AddMember(&CTriggerBoundsComponent::m_forwardToEntityId, 'fwrd', "ForwardTo", "Forward Events To",
		               "Entity id that should receive the enter/leave events instead of this entity. 0 keeps them here.", 0);
		desc.AddMember(&CTriggerBoundsComponent::m_bLogEvents, 'logv', "LogEvents", "Log Events",
		               "Print one console line whenever an entity enters or leaves the trigger. The trigger raises entity "
		               "events and nothing in the editor displays them, so this is how you see it working.", false);
	}

	// IShapeListener
	virtual void OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason) override;
	// ~IShapeListener

#ifndef RELEASE
	// IEntityComponentPreviewer - draws the trigger's own box, which is NOT the shape's outline:
	// a proximity trigger is an axis-aligned box in entity space with rotation and scale ignored
	// (IEntityTriggerComponent::SetTriggerBounds), so a rotated shape and its trigger disagree and
	// the user should be able to see that they do.
	virtual IEntityComponentPreviewer* GetPreviewer() override { return this; }

	virtual void SerializeProperties(Serialization::IArchive& archive) override {}
	virtual void Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const override;
	// ~IEntityComponentPreviewer
#endif

protected:
	// IEntityComponent
	virtual void                    Initialize() override;
	virtual void                    OnShutDown() override;
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	// ~IEntityComponent

private:
	IShapeComponent*         FindShape() const;
	IEntityTriggerComponent* FindTrigger() const;

	void EnsureBound();
	void UnbindShape();
	void PushBounds();

	int     m_forwardToEntityId = 0;
	bool    m_bLogEvents = false;

	CryGUID m_triggerGuid = CryGUID::Null();
	bool    m_bBoundToShape = false;
	bool    m_bWarnedNoShape = false;
};

} // namespace AreaComponents
} // namespace Cry
