// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! The inspector buttons a point-list shape kind offers, in one place.
//!
//! A shape gets its buttons through IEditorActionComponent (CryCommon/CryEntitySystem/
//! IEntityComponent.h): the component says how many actions it has and names the CEditTool class
//! each one starts, and CEntityObject::CreateComponentWidget turns each into an SEditToolButton row
//! bound to that component instance. IEditorShapeComponent already implements that interface in
//! terms of the single "Edit Shape" tool a shape names, which is all Box and Sphere need; a kind
//! with a real POINT LIST has a second action, and this header is what keeps Polygon and Spline
//! from spelling it out twice.
//!
//! Why "Recenter Pivot" is an EDITOR action and not, as it briefly was, a reflected ActionButton on
//! the component: moving the pivot means moving the ENTITY, and in Sandbox the editor object owns
//! the transform - CEntityObject pushes its own transform onto the entity and never reads one back
//! (there is no ENTITY_EVENT_XFORM handler in EntityObject.cpp). A component that called
//! IEntity::SetPos would be silently overwritten by the object's next push, and nothing about it
//! would be undoable. The engine-side button compensated by shifting the points and the offset
//! instead, which moved the curve on screen and could not be undone - the bug this replaces. The
//! editor tool does it the only correct way: CBaseObject::SetWorldPos inside one CUndo, with every
//! point rewritten in the new local frame so that nothing moves in the world.

#include <CryEntitySystem/IEntityComponent.h>

namespace Cry
{
namespace AreaComponents
{
namespace ShapeEditorActions
{

//! The point tool of stage 1b, which every shape kind already names.
inline const char* GetEditToolClassName()      { return "EditTool.AreaShapeEdit"; }
inline const char* GetEditToolLabel()          { return "Edit Shape"; }

//! The one-shot pivot action, implemented by CShapeRecenterTool in the editor plugin.
inline const char* GetRecenterToolClassName()  { return "EditTool.AreaShapeRecenter"; }
inline const char* GetRecenterToolLabel()      { return "Recenter Pivot"; }

//! How many actions a kind with an authored point list offers.
inline int GetPointShapeActionCount() { return 2; }

//! Describes action `index` of a kind with an authored point list. The strings are literals with
//! static storage, which is what SEditorActionDesc requires - the property tree keeps the pointers.
inline bool GetPointShapeAction(int index, SEditorActionDesc& out)
{
	switch (index)
	{
	case 0:
		out.szToolClassName = GetEditToolClassName();
		out.szLabel = GetEditToolLabel();
		return true;
	case 1:
		out.szToolClassName = GetRecenterToolClassName();
		out.szLabel = GetRecenterToolLabel();
		return true;
	default:
		return false;
	}
}

} // namespace ShapeEditorActions
} // namespace AreaComponents
} // namespace Cry
