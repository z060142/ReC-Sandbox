// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <CryExtension/CryGUID.h>

class CBaseObject;
struct IEntity;
struct IEntityComponent;

namespace Cry
{
namespace AreaComponents
{
struct IShapeComponent;
}
}

//! The few things every Area shape tool has to do, in one place: find the shape component on an
//! editor object, and push a viewport edit into the places the editor saves from.
//!
//! Nothing here stores a pointer between calls - the tools resolve object and component from their
//! GUIDs on every use (MEMORY.md, fix bde12cd8), which is also what lets a tool survive an undo
//! that re-creates the entity underneath it.
namespace AreaShapeTools
{

CBaseObject*                          FindObject(const CryGUID& objectGuid);

//! The shape component of an entity, or nullptr. The cast is made only after the reflected base
//! list proves the component really implements the contract - that is the cross-DLL proof.
Cry::AreaComponents::IShapeComponent* AsShape(IEntityComponent* pComponent);

//! The single shape component of an editor object (decision 01: there is at most one).
//! `pComponentGuidOut` receives its instance GUID when it is not null.
Cry::AreaComponents::IShapeComponent* FindShapeOnObject(CBaseObject* pObject, CryGUID* pComponentGuidOut);

//! The sandwich a viewport edit has to repeat by hand, because it happens outside the inspector's
//! serialization pass (research/06 section 5.3): refresh the Schematyc save snapshot, tell the
//! component its properties changed, mark the object modified and update the prefab.
void                                  SyncEditedComponent(CBaseObject* pObject, IEntityComponent* pComponent);

} // namespace AreaShapeTools
