// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! What the editor needs in order to turn a component's generated instances into real, editable
//! editor objects - decision 04's D4, "bake to brushes as an extra button, never the mechanism".
//!
//! It is a separate header from IShapeComponent.h because the thing being baked is not a shape:
//! any component that generates placed geometry from parameters can implement this and get the
//! button. Today that is the distributor.
//!
//! Same two rules as the shape contract. HEAP RULE: no STL container crosses this interface - the
//! editor plugin and the engine module use different allocators, so the instance description is a
//! flat POD the caller owns, filled one at a time. MEMORY RULE: nothing reflected holds a raw
//! pointer.
//!
//! ABI: append-only. The editor plugin never links this module; this header is the whole contract
//! between them, so new virtuals go at the END and existing ones never change signature.

#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CryMath/Cry_Math.h>

namespace Cry
{
namespace AreaComponents
{

//! One generated instance, in the form the editor needs to build a brush object out of it.
//! A plain POD with a fixed-size path: the editor fills its own copy and never sees our strings.
struct SBakeInstance
{
	//! World transform - rotation, scale and position as the generator placed it.
	Matrix34 tm = Matrix34(IDENTITY);
	//! Path of the .cgf, null-terminated. Empty means "this instance has no mesh", which a caller
	//! should skip rather than treat as an error.
	char     meshPath[256] = { 0 };
	//! The engine render flags the live instance carries (ERF_*), so a baked brush can be given the
	//! same ones where the editor object exposes them.
	uint64   renderFlags = 0;
	int      viewDistRatio = 100;
	int      lodRatio = 100;
};

//! Implemented by a component whose generated instances can be turned into editor objects.
//!
//! It derives from IEditorActionComponent, not from IEntityComponent, so that an implementation has
//! ONE inheritance chain to IEntityComponent (a second base would make the entity-component base
//! ambiguous and break the static_cast that GetAllComponents<>() performs) and so that a bakeable
//! component declares its button through the same generic mechanism every other action uses.
struct IBakeableComponent : public IEditorActionComponent
{
	static void ReflectType(Schematyc::CTypeDesc<IBakeableComponent>& desc)
	{
		desc.SetGUID("{7E4C1A96-3B85-4D27-91F0-6D2A8C45E713}"_cry_guid);
		desc.SetLabel("Bakeable");
	}

	//! How many instances would be baked right now. 0 is a legitimate answer.
	virtual int  GetBakeInstanceCount() const = 0;
	//! Describes instance `index` into caller-owned storage. False when the index is out of range.
	virtual bool GetBakeInstance(int index, SBakeInstance& out) const = 0;

	//! Whether the component is currently generating instances at all. Baking switches this off, so
	//! that the baked objects do not sit inside a live copy of themselves; it is the component's own
	//! reflected "Enabled" member, so an undo of the bake restores it with the entity's snapshot.
	virtual bool IsBakeEnabled() const = 0;
	virtual void SetBakeEnabled(bool bEnabled) = 0;
};

} // namespace AreaComponents
} // namespace Cry
