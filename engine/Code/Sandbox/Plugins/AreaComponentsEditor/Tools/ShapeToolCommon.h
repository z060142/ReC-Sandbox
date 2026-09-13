// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <CryExtension/CryGUID.h>
#include <CryMath/Cry_Math.h>

class CBaseObject;
class CPoint;
class CViewport;
struct IEntity;
struct IEntityComponent;
struct SDisplayContext;

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

// --- The draw gesture, shared by the two tools that perform it (backlog B1).
//
// Drawing a shape is the same three steps wherever it happens: pick the world point under the
// cursor, append it to the shape's point list in the shape's own space, and draw what has been
// committed so far with a rubber band to the cursor. CShapeCreateTool does them on an entity it
// has just spawned, CShapeEditTool does them on an entity the user already has (Edit Shape on a
// shape with no points). Only the first step - where the entity comes from - differs, so only
// that stayed in the create tool.

//! The world point under the cursor, picked the way a shape being drawn picks its points: against
//! terrain and geometry with no axis constraint (CShapeObject::MouseCreateCallback,
//! ShapeObject.cpp:1174), then snapped to the grid. False when the pick is not a usable position.
bool PickDrawPoint(CViewport* pView, CPoint& point, Vec3& worldPosOut);

//! Appends a world-space point to the shape's point list, converted into the shape's own space.
//! A point that lands on the previous one is dropped rather than appended - the click that opens
//! a double click lands on the pixel the double click lands on - which is what legacy achieves by
//! popping its trailing temporary point (ShapeObject.cpp:1198-1199).
//! Returns true when a point was really appended.
bool AppendDrawPoint(Cry::AreaComponents::IShapeComponent* pShape, const Vec3& worldPos);

//! Draws a shape that is being drawn: the committed edges, the rubber band to the cursor, and the
//! faint edge that would close the contour. The yellow legacy draws a shape in progress in
//! (ShapeObject.cpp:1322-1330).
void DisplayDrawInProgress(SDisplayContext& dc, Cry::AreaComponents::IShapeComponent* pShape,
                           bool bCursorValid, const Vec3& cursorWorldPos);

} // namespace AreaShapeTools
