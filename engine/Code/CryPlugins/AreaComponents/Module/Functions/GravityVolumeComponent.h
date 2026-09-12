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

//! A gravity tube along the entity's spline shape: the component form of CGravityVolumeObject
//! (GravityVolumeObject.cpp), minus the 1378 lines of duplicated spline code it carried.
//!
//! It binds BY RULE, not by a reference field: the volume is the spline shape on the same entity
//! (decision 01), found with GetComponent<IShapeComponent>() and GetSpline(). That is the pattern
//! Cry::DefaultComponents::CAreaComponent::EType::Spline already ships (AreaComponent.cpp:132-151)
//! and the one O3DE calls a required service. A latched warning names the entity when there is no
//! spline to bind to.
//!
//! Physics is built DIRECTLY - IPhysicalWorld::AddArea + pe_params_area - rather than through
//! IEntityAreaComponent::SetGravityVolume (decision 04, option G2). Two reasons, both from
//! research/08 section 4: SetGravityVolume only STORES (AreaProxy.cpp:713-727) and the physics is
//! created in OnEnable(), which nothing calls when the shape changes, so a legacy gravity volume
//! silently keeps its old tube until a visibility toggle or a level reload; and the legacy XML form
//! never serialized Falloff or Damping (AreaProxy.cpp:516-527), which quietly discards authored
//! values. Both bugs are gone here: every parameter is a reflected member, and the tube is rebuilt
//! on every shape change.
class CGravityVolumeComponent final
	: public IEntityComponent
	, public IShapeListener
#ifndef RELEASE
	, public IEntityComponentPreviewer
#endif
{
public:
	CGravityVolumeComponent() = default;
	virtual ~CGravityVolumeComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CGravityVolumeComponent>& desc)
	{
		// The HIPART of a component's GUID has to be unique among the components that can sit on
		// ONE entity. CEntityObject::CreateComponentWidgets keys each component's property tree by
		// GetClassDesc().GetGUID().hipart + instanceIndex (EntityObject.cpp:1107), so two different
		// component classes whose GUIDs differ only in the LOPART collide on that key and the whole
		// entity's inspector goes blank. The shape kinds may all share the 6C3A81D5-0F47-4E9A
		// hipart because they are mutually incompatible - never two on one entity - but a function
		// component sits BESIDE a shape and beside the other functions, so it needs its own.
		desc.SetGUID("{9A31F4C7-5E28-4B60-8D19-2C7B6E05A340}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Gravity Volume");
		desc.SetDescription("Turns the spline shape on this entity into a tube of physics gravity.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		desc.AddMember(&CGravityVolumeComponent::m_bEnabled, 'enbl', "Enabled", "Enabled", "Whether the gravity area exists at all", true);
		desc.AddMember(&CGravityVolumeComponent::m_radius, 'radi', "Radius", "Radius", "Radius of the tube around the spline, in metres", 4.0f);
		desc.AddMember(&CGravityVolumeComponent::m_gravity, 'grav', "Gravity", "Gravity", "Strength of the gravity inside the tube, in m/s^2 along the tube's own up axis. Legacy's default is 10.", 10.0f);
		desc.AddMember(&CGravityVolumeComponent::m_falloff, 'falo', "Falloff", "Falloff", "Parametric distance (0..1) from the centre at which the gravity starts falling off", 0.8f);
		desc.AddMember(&CGravityVolumeComponent::m_damping, 'damp', "Damping", "Damping", "Uniform damping applied to bodies inside the tube", 1.0f);
		desc.AddMember(&CGravityVolumeComponent::m_bDontDisableInvisible, 'ddiv', "DontDisableInvisible", "Don't Disable Invisible", "Keep the area active while the entity is not rendered", false);
	}

	// IShapeListener
	virtual void OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason) override;
	// ~IShapeListener

protected:
	// IEntityComponent
	virtual void                    Initialize() override;
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void                    OnShutDown() override;
	// ~IEntityComponent

#ifndef RELEASE
	// IEntityComponentPreviewer
	virtual IEntityComponentPreviewer* GetPreviewer() override { return this; }

	virtual void SerializeProperties(Serialization::IArchive& archive) override {}
	virtual void Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const override;
	// ~IEntityComponentPreviewer
#endif

private:
	//! Finds the sibling spline shape and subscribes to it. Returns it, or nullptr with a latched
	//! warning. Called before every rebuild, because a function component can be added to the entity
	//! before its shape is.
	ISplineShape* EnsureBound();
	//! The sibling spline shape without binding or warning - what a const query uses.
	ISplineShape* FindSpline() const;

	void          Unbind();

	//! Destroys the area and builds it again from the current points and parameters.
	void          Physicalize();

	//! Largest number of control points one rebuild copies onto the stack.
	static constexpr int kMaxStackPoints = 1024;

	bool  m_bEnabled = true;
	float m_radius = 4.0f;
	float m_gravity = 10.0f;
	float m_falloff = 0.8f;
	float m_damping = 1.0f;
	bool  m_bDontDisableInvisible = false;

	//! Runtime only, never reflected (MEMORY.md): the shape we are subscribed to, so that the
	//! listener can be removed again from exactly the shape it was added to even if the entity has
	//! meanwhile grown a different one.
	IShapeComponent* m_pBoundShape = nullptr;

	//! One warning per missing binding, released again when a shape appears: a component in an
	//! entity being assembled would otherwise warn on every property edit.
	bool m_bWarnedNoSpline = false;
};

} // namespace AreaComponents
} // namespace Cry
