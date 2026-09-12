// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! The one place that knows every shape kind's component GUID, and the one helper that turns that
//! list into the pairwise Incompatibility declarations decision 01 asks for.
//!
//! Why a list and not twelve hand-written AddComponentInteraction() calls: "exactly one shape per
//! entity" is enforced by every kind declaring every OTHER kind incompatible, and the check is
//! one-directional - EntityObject.cpp:729 walks the components already present on the entity and
//! asks each of them IsCompatibleWith(candidate). So the pair has to be declared on BOTH sides,
//! which is n*(n-1) declarations for n kinds: 12 for four kinds, 20 for five. Written by hand that
//! is a list that silently rots the first time someone adds a kind and forgets one direction.
//! Here, adding a kind is exactly ONE line in AREA_SHAPE_COMPONENT_LIST below.
//!
//! The GUIDs are spelled out here rather than pulled from the component headers on purpose: every
//! kind would otherwise have to include every other kind's header, which is a cycle. This header
//! includes nothing of the kinds and each kind includes only this.

#include <CryEntitySystem/IEntityComponent.h>
#include <CryExtension/CryGUID.h>

namespace Cry
{
namespace AreaComponents
{

//! Every shape component kind: (name, component GUID). The Spline slot is declared before the
//! kind exists - an Incompatibility with a GUID nothing has registered is inert (IsCompatibleWith
//! only ever compares it against components actually present), so the slot costs nothing and the
//! spline shape will be complete the day its component is written.
#define AREA_SHAPE_COMPONENT_LIST(f)                              \
	f(Box,     "{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96201}"_cry_guid)   \
	f(Polygon, "{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96202}"_cry_guid)   \
	f(Sphere,  "{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96203}"_cry_guid)   \
	f(Spline,  "{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96204}"_cry_guid)

//! The GUID of one kind by name, for the few places that need to name a specific kind
//! (ShapeComponentGuid::Sphere() and so on).
namespace ShapeComponentGuid
{
#define AREA_SHAPE_DECLARE_GUID_ACCESSOR(name, guid) \
	inline const CryGUID& name() { static const CryGUID value = guid; return value; }

AREA_SHAPE_COMPONENT_LIST(AREA_SHAPE_DECLARE_GUID_ACCESSOR)

#undef AREA_SHAPE_DECLARE_GUID_ACCESSOR
} // namespace ShapeComponentGuid

//! The whole list, as a caller-owned view (heap rule: no container crosses a module boundary).
inline const CryGUID* GetShapeComponentGuids(int& countOut)
{
#define AREA_SHAPE_GUID_ENTRY(name, guid) guid,

	static const CryGUID s_guids[] = { AREA_SHAPE_COMPONENT_LIST(AREA_SHAPE_GUID_ENTRY) };

#undef AREA_SHAPE_GUID_ENTRY

	countOut = CRY_ARRAY_COUNT(s_guids);
	return s_guids;
}

//! Declares the calling shape kind incompatible with every other shape kind. Call it from
//! ReflectType AFTER desc.SetGUID(), which is how the helper knows which kind "self" is.
inline void DeclareShapeKindIncompatibilities(CEntityComponentClassDesc& desc)
{
	int            count = 0;
	const CryGUID* pGuids = GetShapeComponentGuids(count);

	const CryGUID self = desc.GetGUID();
	CRY_ASSERT(!self.IsNull(), "DeclareShapeKindIncompatibilities() must be called after SetGUID()");

	for (int i = 0; i < count; ++i)
	{
		if (pGuids[i] != self)
		{
			desc.AddComponentInteraction(SEntityComponentRequirements::EType::Incompatibility, pGuids[i]);
		}
	}
}

} // namespace AreaComponents
} // namespace Cry
