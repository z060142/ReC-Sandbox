// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Utils/SharedString.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CrySerialization/IArchive.h>

#include <vector>

namespace Cry
{
namespace AreaComponents
{

//! How many of the entity's links this area is currently attached to. DERIVED state, not authored:
//! the component recomputes it whenever it re-syncs, and it exists only so that the inspector can
//! show "how many entities will get this area's events" without the user having to count the rows
//! in the entity's own Links section.
//!
//! It is therefore written in EDIT mode only and never saved - a save or a property round trip
//! writes nothing here and recomputes it on the next sync instead.
struct SAreaLinkStatus
{
	static void ReflectType(Schematyc::CTypeDesc<SAreaLinkStatus>& desc)
	{
		desc.SetGUID("{B4E27C09-8A16-4D53-9C70-1F5D3E8A4610}"_cry_guid);
		desc.SetLabel("Area Links");
	}

	bool operator==(const SAreaLinkStatus& other) const { return count == other.count && unresolved == other.unresolved; }
	bool operator!=(const SAreaLinkStatus& other) const { return !(*this == other); }

	int count = 0;
	int unresolved = 0;
};

//! Read-only row, edit mode only.
//!
//! NOTE, and the reason this takes no label from the caller: PropertyRow stores the label as a bare
//! const char* and never copies it (PropertyRow.h:352, PropertyRow::setLabel, PropertyRow.cpp:415),
//! so a label built into a local string and passed as .c_str() is a dangling pointer the moment the
//! function returns - it renders as intermittent garbage or as nothing at all. Every label here is
//! a string literal.
bool Serialize(Serialization::IArchive& archive, SAreaLinkStatus& value, const char* szName, const char* szLabel);

//! "Area": the semantics that used to be baked into every legacy area object - an id, a group, a
//! priority, a near band, an inner fade and a list of linked entities - as one function component
//! that takes its VOLUME from whatever shape component sits on the same entity (decision 01: one
//! shape per entity, function components bind by rule).
//!
//! Underneath it owns a hidden legacy IEntityAreaComponent (research/07 option 1a). That is
//! deliberate and is the whole reason the six area events, the exclusive-group fade, the broad-
//! phase grid, ExitAllAreas on teardown and every existing consumer that looks an area up with
//! GetProxy(ENTITY_PROXY_AREA) - particles, the physics spline area, the audio components - keep
//! working without a single line of theirs changing. The hidden proxy is flagged NoSave, so the
//! level never contains two copies of the geometry and the privileged <Area> loader hook never
//! re-creates it.
//!
//! It also ATTACHES ITS OWN ENTITY to that area (CArea::AddEntity(own id), the BoidsProxy
//! precedent). That is what lets the stock audio Environment and Area components work when they
//! are added to this same entity: they never look for a shape, they only require that their entity
//! is in some area's attached list. The self-move trap that comes with it is fixed in the engine -
//! see the IsSelfHostedArea guard in EntityAudioProxy.cpp and DefaultComponents/Audio/AreaComponent.cpp.
class CAreaFunctionComponent final
	: public IEntityComponent
	, public IShapeListener
{
public:
	CAreaFunctionComponent() = default;
	virtual ~CAreaFunctionComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CAreaFunctionComponent>& desc)
	{
		// NOT in the 6C3A81D5-...-9620x series: that one belongs to the SHAPE kinds, and stage 3's
		// gravity volume took ...96210 there. A duplicate element GUID does not just lose one
		// component - CEnvRegistry::RegisterPackageElements fails and the WHOLE AreaComponents
		// package is released, so every shape kind disappears with it. Function components of this
		// module use the B4E27C09-8A16-4D53-9C70-1F5D3E8A46xx series instead.
		desc.SetGUID("{B4E27C09-8A16-4D53-9C70-1F5D3E8A4620}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Area");
		desc.SetDescription("Turns the shape on this entity into an area: it sends enter/leave/fade events to the entities linked below.");

		// Singleton, but NOT incompatible with anything: an entity has one area, and any shape
		// kind may carry it. The shape it binds to is found by rule, not declared here.
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		desc.AddMember(&CAreaFunctionComponent::m_areaId, 'arid', "AreaId", "Area Id", "Id handed to the linked entities with every area event", 0);
		desc.AddMember(&CAreaFunctionComponent::m_group, 'grup', "Group", "Group", "Areas sharing a group are exclusive: an entity is only ever in the innermost one. -1 means no group.", -1);
		desc.AddMember(&CAreaFunctionComponent::m_priority, 'prio', "Priority", "Priority", "Which area of the same group wins where they overlap. Higher wins.", 0);
		desc.AddMember(&CAreaFunctionComponent::m_fadeDistance, 'fade', "FadeDistance", "Fade Distance", "Width in metres of the near band outside the shape, in which entities get near/fade events. 0 disables it.", 0.0f);
		desc.AddMember(&CAreaFunctionComponent::m_innerFadeDistance, 'ifad', "InnerFadeDistance", "Inner Fade Distance", "Width in metres inside the shape over which a lower-priority area of the same group fades out.", 0.0f);
		desc.AddMember(&CAreaFunctionComponent::m_linkName, 'lnkn', "LinkName", "Link Name",
		               "Which of the entity's links this area sends its events to. Empty means every link. "
		               "Link entities with Sandbox's own Link tool - this component does not keep a list of its own.", "");
		desc.AddMember(&CAreaFunctionComponent::m_linkStatus, 'lnks', "Links", "Links", "How many linked entities this area is currently attached to", SAreaLinkStatus());
	}

	// IShapeListener
	virtual void OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason) override;
	// ~IShapeListener

	//! Called by the module-wide spawn sink when an entity appears after this area synced its
	//! links, so that a link whose target was not loaded yet still takes effect. Public because the
	//! sink is not a member.
	void OnEntitySpawned(EntityId spawnedId, const CryGUID& spawnedGuid);
	//! Whether some link of ours still points at an entity that does not exist yet - the spawn sink
	//! only has to visit areas that answer true.
	bool HasUnresolvedLinks() const;

protected:
	// IEntityComponent
	virtual void                    Initialize() override;
	virtual void                    OnShutDown() override;
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	// ~IEntityComponent

private:
	//! The shape component on this entity, or nullptr. Never cached across a frame: a component
	//! pointer is not ours to keep (MEMORY.md), and one query of the entity's component list is
	//! far cheaper than the bugs the alternative buys.
	IShapeComponent*      FindShape() const;
	//! The hidden legacy area proxy, or nullptr when we have not created one yet.
	IEntityAreaComponent* FindAreaProxy() const;

	//! Create the hidden proxy if needed, subscribe to the shape and push everything. Cheap and
	//! idempotent, so it can be called from every event handler - which is how a shape component
	//! added AFTER this one still gets picked up (there is no entity event for "a component was
	//! added").
	void EnsureBound();
	void UnbindShape();

	//! Push the whole volume, the expensive path: SetBox / SetSphere / SetPoints plus the
	//! per-face obstruction. Used on bind, on a topology change and after a game-mode reset.
	void PushVolume();
	//! Push moved points only, the cheap path (IEntityAreaComponent::MovePoints). Falls back to
	//! PushVolume() for kinds and situations the cheap path cannot express.
	void PushGeometry();
	//! Push id / group / priority / fade / inner fade.
	void PushAreaProperties();

	//! Reconcile the proxy's attached list with the ENTITY's own links (IEntity::GetEntityLinks),
	//! filtered by LinkName. Idempotent: an entity that is already attached is never attached
	//! twice, and one that is no longer linked is detached. `pIgnoredLink` is the link that is
	//! about to be deleted when this runs from ENTITY_EVENT_DELINK - that event is sent while the
	//! link is still in the list (Entity.cpp, RemoveEntityLink), but not in RemoveAllEntityLinks,
	//! so it has to be skipped explicitly rather than assumed absent.
	void SyncLinks(const IEntityLink* pIgnoredLink = nullptr);
	//! Attach this entity to its own area, so that sibling function components (the stock audio
	//! Environment and Area components) receive the area's events.
	void AttachSelf();

	static constexpr int kMaxStackPoints = 256;

	// --- reflected state ---
	int        m_areaId = 0;
	int        m_group = -1;
	int        m_priority = 0;
	float           m_fadeDistance = 0.0f;
	float           m_innerFadeDistance = 0.0f;
	Schematyc::CSharedString m_linkName;
	SAreaLinkStatus m_linkStatus;

	// --- runtime state, never reflected ---

	//! GUID of the hidden legacy area proxy we created, so that it can be found again without
	//! holding a pointer to it. Null when we have not created one.
	CryGUID m_areaProxyGuid = CryGUID::Null();
	//! True while we are pushing into the proxy. IEntityAreaComponent::SetPoints() broadcasts
	//! ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED to the WHOLE entity, which the shape component
	//! answers with an OnShapeChanged - straight back into us. One flag breaks the loop.
	bool    m_bPushing = false;
	//! True while we are registered as a listener of this entity's shape.
	bool    m_bBoundToShape = false;
	//! The entity ids currently attached to the area because of the entity's links
	//! (self-attachment is not in here). Runtime only: ids, never pointers.
	std::vector<EntityId> m_resolvedLinks;
	//! How many links pointed at an entity that did not exist at the last sync.
	int     m_unresolvedLinkCount = 0;
	//! True once this component has been registered with the module-wide spawn sink.
	bool    m_bRegisteredWithSpawnSink = false;

	//! Latched warnings - a component with no shape, or with a shape that has no area volume,
	//! must say so once and then be quiet, not once per frame or per property edit.
	bool    m_bWarnedNoShape = false;
	bool    m_bWarnedNoVolume = false;
	bool    m_bWarnedLegacyProxy = false;
};

} // namespace AreaComponents
} // namespace Cry
