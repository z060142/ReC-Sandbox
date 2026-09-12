// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "AreaFunctionComponent.h"

#include <CryEntitySystem/IEntitySystem.h>
#include <CryCore/StlUtils.h>
#include <CrySerialization/STL.h>
#include <CrySerialization/CryStrings.h>

#include <algorithm>

namespace Cry
{
namespace AreaComponents
{

// ---------------------------------------------------------------------------
// The link list in the inspector
// ---------------------------------------------------------------------------

bool Serialize(Serialization::IArchive& archive, SAreaLinkStatus& value, const char* szName, const char* szLabel)
{
	if (!archive.isEdit())
	{
		// Derived state: nothing to save, nothing to load, nothing to copy through the game-mode
		// property round trip. It is recomputed on the next link sync.
		return true;
	}

	string text;
	if (value.count == 0 && value.unresolved == 0)
	{
		text = "none - link entities to this one with the Link tool";
	}
	else if (value.unresolved == 0)
	{
		text.Format("%d linked", value.count);
	}
	else
	{
		text.Format("%d linked, %d not loaded", value.count, value.unresolved);
	}

	// Both strings passed to the archive are LITERALS. PropertyRow keeps the label as a bare
	// const char* and never copies it (PropertyRow.h:352, PropertyRow::setLabel,
	// PropertyRow.cpp:415), so a label built into a local string and handed over as .c_str() is a
	// dangling pointer the moment this function returns: the row then renders whatever landed in
	// that freed block, which is how an unrelated engine string ("...MergedMesh") turned up in the
	// middle of the Area component's rows. The '!' prefix marks the row read-only.
	archive(text, "linked", "!Linked");
	return true;
}

// ---------------------------------------------------------------------------
// The spawn sink: entities that appear after an area resolved its links
// ---------------------------------------------------------------------------

namespace
{

//! One sink for the whole module, because IEntitySystem::AddSink is a global registration and a
//! sink per area component would make every spawn walk every area twice. It only holds components
//! that still have unresolved link GUIDs, and it is added to the entity system only while that
//! list is not empty - so a level whose links all resolved pays nothing per spawn.
class CAreaLinkSpawnSink final : public IEntitySystemSink
{
public:
	static CAreaLinkSpawnSink& Get()
	{
		static CAreaLinkSpawnSink s_instance;
		return s_instance;
	}

	void Register(CAreaFunctionComponent* pComponent)
	{
		if (pComponent == nullptr || stl::find(m_components, pComponent))
			return;

		m_components.push_back(pComponent);

		if (m_components.size() == 1 && gEnv->pEntitySystem != nullptr)
		{
			gEnv->pEntitySystem->AddSink(this, IEntitySystem::OnSpawn);
		}
	}

	void Unregister(CAreaFunctionComponent* pComponent)
	{
		if (!stl::find_and_erase(m_components, pComponent))
			return;

		if (m_components.empty() && gEnv->pEntitySystem != nullptr)
		{
			gEnv->pEntitySystem->RemoveSink(this);
		}
	}

	// IEntitySystemSink
	virtual bool OnBeforeSpawn(SEntitySpawnParams& params) override { return true; }

	virtual void OnSpawn(IEntity* pEntity, SEntitySpawnParams& params) override
	{
		if (pEntity == nullptr || params.guid.IsNull())
			return;

		// Copy first: a component may unregister itself while being told.
		const std::vector<CAreaFunctionComponent*> components = m_components;
		for (CAreaFunctionComponent* pComponent : components)
		{
			pComponent->OnEntitySpawned(pEntity->GetId(), params.guid);
		}
	}

	virtual bool OnRemove(IEntity* pEntity) override                       { return true; }
	virtual void OnReused(IEntity* pEntity, SEntitySpawnParams& params) override {}
	// ~IEntitySystemSink

private:
	std::vector<CAreaFunctionComponent*> m_components;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void CAreaFunctionComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet.
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

void CAreaFunctionComponent::Initialize()
{
	EnsureBound();
}

void CAreaFunctionComponent::OnShutDown()
{
	UnbindShape();

	if (m_bRegisteredWithSpawnSink)
	{
		CAreaLinkSpawnSink::Get().Unregister(this);
		m_bRegisteredWithSpawnSink = false;
	}

	m_resolvedLinks.clear();
}

Cry::Entity::EventFlags CAreaFunctionComponent::GetEventMask() const
{
	// Deliberately NO ENTITY_EVENT_UPDATE: an idle area costs nothing per frame. The entity's own
	// movement is handled by the legacy proxy's OnMove (the cheap per-kind push), so XFORM is only
	// subscribed to give an unbound component a chance to find a shape that was added after it.
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED
	       | ENTITY_EVENT_XFORM
	       | ENTITY_EVENT_RESET
	       | ENTITY_EVENT_LEVEL_LOADED
	       | ENTITY_EVENT_LINK
	       | ENTITY_EVENT_DELINK
	       | ENTITY_EVENT_START_GAME
	       | ENTITY_EVENT_LAYER_UNHIDE;
}

void CAreaFunctionComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_XFORM:
		// The legacy proxy moves the area itself on this event (MovePoints for a shape, SetMatrix
		// for a box, SetSphere for a sphere - the three cheap paths), and its local geometry is
		// exactly what we pushed, so there is nothing to do here except notice a shape that was
		// added after us. There is no entity event for "a component was added".
		if (!m_bBoundToShape)
		{
			EnsureBound();
		}
		break;

	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		// IEntityAreaComponent::SetPoints() broadcasts this event to the whole entity, so without
		// the guard our own push would come straight back to us.
		if (!m_bPushing)
		{
			EnsureBound();
			PushAreaProperties();
		}
		break;

	case ENTITY_EVENT_RESET:
		// Entering or leaving game mode. The hidden proxy is not UserAdded, so the editor's
		// component cache does not snapshot and restore it - everything has to be re-pushed.
		EnsureBound();
		PushVolume();
		SyncLinks();
		break;

	case ENTITY_EVENT_LEVEL_LOADED:
	case ENTITY_EVENT_START_GAME:
		// The per-entity equivalent of the instant CArea::ResolveEntityIds runs at, and the first
		// moment the entity's own links have been resolved by CEntityLoadManager.
		EnsureBound();
		SyncLinks();
		break;

	case ENTITY_EVENT_LINK:
		SyncLinks();
		break;

	case ENTITY_EVENT_DELINK:
		// Sent while the link is still in the list (Entity.cpp, RemoveEntityLink) - but not in
		// RemoveAllEntityLinks, which unlinks first. Skipping it explicitly is correct either way.
		SyncLinks(reinterpret_cast<const IEntityLink*>(event.nParam[0]));
		break;

	case ENTITY_EVENT_LAYER_UNHIDE:
		// A layer that was not active at load time has just brought its entities in. Legacy links
		// never recover from this (CArea resolves once, guarded by EAreaState::EntityIdsResolved);
		// ours do.
		SyncLinks();
		break;

	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

IShapeComponent* CAreaFunctionComponent::FindShape() const
{
	IEntity* pEntity = GetEntity();
	if (pEntity == nullptr)
		return nullptr;

	IEntityComponent* pComponent = pEntity->QueryComponentByInterfaceID(Schematyc::GetTypeDesc<IShapeComponent>().GetGUID());
	if (pComponent == nullptr)
		return nullptr;

	return static_cast<IShapeComponent*>(pComponent);
}

IEntityAreaComponent* CAreaFunctionComponent::FindAreaProxy() const
{
	if (m_areaProxyGuid.IsNull())
		return nullptr;

	IEntity* pEntity = GetEntity();
	if (pEntity == nullptr)
		return nullptr;

	return static_cast<IEntityAreaComponent*>(pEntity->GetComponentByGUID(m_areaProxyGuid));
}

void CAreaFunctionComponent::EnsureBound()
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
			           "Area on entity \"%s\" has no shape component - add one (Add Component -> Area -> Shape: Box / Sphere / Polygon) "
			           "and the area takes its volume from it.", pEntity->GetName());
		}
		return;
	}

	m_bWarnedNoShape = false;

	// The hidden legacy area proxy. It is created once, here, and never again: the point of
	// remembering its GUID is that "is there already an area proxy on this entity?" and "is it
	// OURS?" are two different questions. An entity that already carried one when we arrived is a
	// legacy area object, and taking it over would silently stop its <Area> node being saved.
	if (m_areaProxyGuid.IsNull())
	{
		if (pEntity->GetComponent<IEntityAreaComponent>() != nullptr)
		{
			if (!m_bWarnedLegacyProxy)
			{
				m_bWarnedLegacyProxy = true;
				CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
				           "Area on entity \"%s\" found an existing legacy area proxy and did nothing: that entity is already a legacy "
				           "area object. Put the Area component on an entity of its own.", pEntity->GetName());
			}
			return;
		}

		IEntityAreaComponent* pArea = pEntity->GetOrCreateComponent<IEntityAreaComponent>();
		if (pArea == nullptr)
			return;

		// NoSave keeps the level free of a second copy of the geometry: CEntity::SerializeXML
		// skips NoSave components entirely, so no <Area> node is written and the privileged
		// <Area> hook in EntityLoadManager has nothing to re-create on the next load.
		// FLAG_NOT_SERIALIZE makes the proxy's own LegacySerializeXML early-out as well, which
		// covers the paths that reach it without going through the save loop.
		pArea->GetComponentFlags().Add(EEntityComponentFlags::NoSave);
		pArea->SetFlags(pArea->GetFlags() | IEntityAreaComponent::FLAG_NOT_SERIALIZE);

		m_areaProxyGuid = pArea->GetGUID();
	}

	if (!m_bBoundToShape)
	{
		pShape->AddListener(this);
		m_bBoundToShape = true;

		PushVolume();
		SyncLinks();
	}

	if (!m_bRegisteredWithSpawnSink)
	{
		CAreaLinkSpawnSink::Get().Register(this);
		m_bRegisteredWithSpawnSink = true;
	}
}

void CAreaFunctionComponent::UnbindShape()
{
	if (!m_bBoundToShape)
		return;

	// Re-queried, never cached: if the shape is already gone there is nothing to unregister from.
	if (IShapeComponent* pShape = FindShape())
	{
		pShape->RemoveListener(this);
	}

	m_bBoundToShape = false;
}

void CAreaFunctionComponent::OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason)
{
	if (m_bPushing)
		return;

	switch (reason)
	{
	case EShapeChangeReason::Transform:
		// Handled by the legacy proxy's own OnMove - see ProcessEvent(ENTITY_EVENT_XFORM).
		break;
	case EShapeChangeReason::Geometry:
		PushGeometry();
		break;
	case EShapeChangeReason::Topology:
	default:
		PushVolume();
		break;
	}
}

// ---------------------------------------------------------------------------
// Pushing into the area
// ---------------------------------------------------------------------------

void CAreaFunctionComponent::PushVolume()
{
	IShapeComponent*      pShape = FindShape();
	IEntityAreaComponent* pArea = FindAreaProxy();
	if (pShape == nullptr || pArea == nullptr)
		return;

	SShapeAreaVolume volume;
	if (!pShape->GetAreaVolume(volume))
	{
		if (!m_bWarnedNoVolume)
		{
			m_bWarnedNoVolume = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Area on entity \"%s\": this shape kind has no area volume, so no area was built. A spline is a path, not a "
			           "volume - use the gravity volume or the distributor function on it instead.",
			           GetEntity() != nullptr ? GetEntity()->GetName() : "<none>");
		}
		return;
	}

	m_bWarnedNoVolume = false;
	m_bPushing = true;

	switch (volume.form)
	{
	case EAreaVolumeForm::Box:
		pArea->SetBox(volume.boxMin, volume.boxMax, volume.boxObstruct, 6);
		break;

	case EAreaVolumeForm::Sphere:
		pArea->SetSphere(volume.sphereCentre, volume.sphereRadius);
		break;

	case EAreaVolumeForm::Polygon:
		{
			Vec3 points[kMaxStackPoints];
			int  pointCount = min(pShape->GetContour(points, kMaxStackPoints, false), kMaxStackPoints);

			if (pointCount > 1)
			{
				// n edge flags plus "Roof" and "Floor", which is exactly the layout
				// CArea::SetPoints expects (AreaProxy.cpp packs the same n + 2 array).
				bool obstruction[kMaxStackPoints + 2] = { false };
				pShape->GetContourObstruction(obstruction, pointCount);
				obstruction[pointCount] = volume.obstructRoof;
				obstruction[pointCount + 1] = volume.obstructFloor;

				pArea->SetPoints(points, obstruction, static_cast<size_t>(pointCount), volume.closed, volume.height);
			}
		}
		break;
	}

	m_bPushing = false;

	PushAreaProperties();
	AttachSelf();
}

void CAreaFunctionComponent::PushGeometry()
{
	IShapeComponent*      pShape = FindShape();
	IEntityAreaComponent* pArea = FindAreaProxy();
	if (pShape == nullptr || pArea == nullptr)
		return;

	SShapeAreaVolume volume;
	if (!pShape->GetAreaVolume(volume))
		return;

	m_bPushing = true;

	switch (volume.form)
	{
	case EAreaVolumeForm::Sphere:
		// One field assignment plus a grid update - there is no cheaper path and no need for one.
		pArea->SetSphere(volume.sphereCentre, volume.sphereRadius);
		m_bPushing = false;
		break;

	case EAreaVolumeForm::Polygon:
		{
			Vec3 points[kMaxStackPoints];
			int  pointCount = min(pShape->GetContour(points, kMaxStackPoints, false), kMaxStackPoints);

			if (pointCount > 1 && pointCount == pArea->GetPointsCount())
			{
				// The cheap path: same point count, so the segments are updated in place instead
				// of being released and rebuilt, and the per-entity containment cache survives.
				pArea->MovePoints(points, static_cast<size_t>(pointCount));
				m_bPushing = false;
			}
			else
			{
				m_bPushing = false;
				PushVolume();
			}
		}
		break;

	case EAreaVolumeForm::Box:
	default:
		// A box area is local geometry plus a world matrix, and SetBox is the only route to
		// either through the public interface.
		m_bPushing = false;
		PushVolume();
		break;
	}
}

void CAreaFunctionComponent::PushAreaProperties()
{
	IEntityAreaComponent* pArea = FindAreaProxy();
	if (pArea == nullptr)
		return;

	pArea->SetID(m_areaId);
	pArea->SetGroup(m_group);
	pArea->SetPriority(m_priority);
	pArea->SetInnerFadeDistance(m_innerFadeDistance);

	// The authored near band. Before this existed an area could only have one if an entity with an
	// audio component was linked to it - see IEntityAreaComponent::SetFadeDistance.
	pArea->SetFadeDistance(m_fadeDistance);
}

void CAreaFunctionComponent::AttachSelf()
{
	IEntity*              pEntity = GetEntity();
	IEntityAreaComponent* pArea = FindAreaProxy();
	if (pEntity == nullptr || pArea == nullptr)
		return;

	// The host entity in its own area's attached list. CArea::AddEntity de-duplicates, so calling
	// this on every volume push is safe. This is what makes the stock audio Environment and Area
	// components work when they are added to THIS entity: neither of them ever looks for a shape,
	// they only require that their entity receives the area's events.
	pArea->AddEntity(pEntity->GetId());
}

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

bool CAreaFunctionComponent::HasUnresolvedLinks() const
{
	return m_unresolvedLinkCount > 0;
}

void CAreaFunctionComponent::SyncLinks(const IEntityLink* pIgnoredLink)
{
	IEntity*              pEntity = GetEntity();
	IEntityAreaComponent* pArea = FindAreaProxy();
	if (pEntity == nullptr || pArea == nullptr)
		return;

	// The link list is the ENTITY's, authored with Sandbox's own Link tool and serialized by
	// EditorQt (CEntityObject::UpdateIEntityLinks -> IEntity::AddEntityLink, and the <EntityLinks>
	// node the level writes). This component keeps no list of its own: that is the wheel the
	// engine already turns, and it is the same list every other consumer of entity links reads.
	const char* const szWantedName = m_linkName.c_str();
	const bool        bEveryLink = (szWantedName == nullptr || szWantedName[0] == '\0');

	std::vector<EntityId> wanted;
	int                   unresolvedCount = 0;

	for (const IEntityLink* pLink = pEntity->GetEntityLinks(); pLink != nullptr; pLink = pLink->next)
	{
		if (pLink == pIgnoredLink)
			continue; // about to be deleted - see the note on the declaration

		if (!bEveryLink && strcmp(pLink->name.c_str(), szWantedName) != 0)
			continue;

		EntityId id = pLink->entityId;

		// A link written by the editor always carries both halves, but the id half is only filled
		// in once the target exists: a link into a layer that was not loaded arrives with 0 and
		// has to be resolved from the GUID, the way CEntityLoadManager resolves its own queued
		// links (EntityLoadManager.cpp, OnBatchCreationCompleted).
		if (id == INVALID_ENTITYID && !pLink->entityGuid.IsNull() && gEnv->pEntitySystem != nullptr)
		{
			id = gEnv->pEntitySystem->FindEntityByGuid(pLink->entityGuid);
		}

		if (id == INVALID_ENTITYID)
		{
			++unresolvedCount;
			continue;
		}

		if (id == pEntity->GetId())
			continue; // the host is attached by AttachSelf(), and must not be detached with a link

		if (!stl::find(wanted, id))
		{
			wanted.push_back(id);
		}
	}

	// Detach what is no longer linked, attach what is new. Never twice: AddEntity de-duplicates
	// inside CArea as well, but doing it here keeps the ATTACH_THIS events down to the real
	// changes - and the editor re-pushes the WHOLE link list on every edit
	// (CEntityObject::UpdateIEntityLinks removes all and re-adds), so this runs in bursts.
	for (const EntityId id : m_resolvedLinks)
	{
		if (!stl::find(wanted, id))
		{
			pArea->RemoveEntity(id);
		}
	}

	for (const EntityId id : wanted)
	{
		if (!stl::find(m_resolvedLinks, id))
		{
			pArea->AddEntity(id);
		}
	}

	m_resolvedLinks = wanted;
	m_unresolvedLinkCount = unresolvedCount;

	m_linkStatus.count = static_cast<int>(wanted.size());
	m_linkStatus.unresolved = unresolvedCount;
}

void CAreaFunctionComponent::OnEntitySpawned(EntityId spawnedId, const CryGUID& spawnedGuid)
{
	if (m_unresolvedLinkCount <= 0 || spawnedId == INVALID_ENTITYID || spawnedGuid.IsNull())
		return;

	IEntity* pEntity = GetEntity();
	if (pEntity == nullptr)
		return;

	// Only pay for a full re-sync when the entity that just appeared is actually one of ours.
	for (const IEntityLink* pLink = pEntity->GetEntityLinks(); pLink != nullptr; pLink = pLink->next)
	{
		if (pLink->entityGuid == spawnedGuid)
		{
			SyncLinks();
			return;
		}
	}
}

} // namespace AreaComponents
} // namespace Cry
