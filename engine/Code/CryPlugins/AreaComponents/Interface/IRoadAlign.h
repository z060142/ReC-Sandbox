// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! What the editor needs in order to push the terrain up to a road the engine plugin generated -
//! the component form of CRoadObject::AlignHeightMap (RoadObject.cpp:473-667).
//!
//! It is a separate header from IShapeComponent.h for the same reason IDistributorBake.h is: the
//! thing being read is not a shape but a road's centre line, and any component that generates one
//! could implement this and get the button.
//!
//! Why the alignment lives in the EDITOR and not in the component: it rewrites CHeightmap, which is
//! the editor's copy of the terrain, is what the level saves, and is what the terrain undo stack
//! records. Nothing of that is reachable from an engine module, and a component that changed only
//! the engine's terrain would lose the change on the next save.
//!
//! Same two rules as the other interface headers. HEAP RULE: no STL container crosses it - samples
//! are read one at a time into caller-owned storage. MEMORY RULE: nothing reflected holds a raw
//! pointer.
//!
//! ABI: append-only. The editor plugin never links the engine module; this header is the whole
//! contract between them, so new virtuals go at the END and existing ones never change signature.

#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CryMath/Cry_Math.h>

namespace Cry
{
namespace AreaComponents
{

//! One station of the road's centre line, in WORLD space. The road surface at this station runs
//! from `worldPos - 0.5 * width * worldNormal` to `worldPos + 0.5 * width * worldNormal`, which is
//! the pair of vertices the render node was given (CRoadObject::SetRoadSectors,
//! RoadObject.cpp:180-181).
struct SRoadSample
{
	Vec3  worldPos = ZERO;
	//! The road's sideways direction: normalised, already rolled by the spline's per-point Angle.
	Vec3  worldNormal = ZERO;
	//! Full width of the road at this station, in metres.
	float width = 0.0f;
};

//! Implemented by a component that generates a road and is willing to have the terrain pushed onto
//! it. It derives from IEditorActionComponent, not from IEntityComponent, so that an implementation
//! has ONE inheritance chain to IEntityComponent and declares its button through the same generic
//! mechanism every other action uses.
struct IRoadAlignSource : public IEditorActionComponent
{
	static void ReflectType(Schematyc::CTypeDesc<IRoadAlignSource>& desc)
	{
		desc.SetGUID("{8D53C2A7-6F41-4E90-B7A2-039C6E14D8B5}"_cry_guid);
		desc.SetLabel("Road Align Source");
	}

	//! How many centre-line stations the road currently has. 0 is a legitimate answer.
	virtual int   GetRoadSampleCount() const = 0;
	//! Describes station `index` into caller-owned storage. False when the index is out of range.
	virtual bool  GetRoadSample(int index, SRoadSample& out) const = 0;
	//! Width of the band OUTSIDE the road over which the terrain is blended back to what it was, in
	//! metres. CRoadObject calls it mv_borderWidth (RoadObject.h:102).
	virtual float GetRoadBorderWidth() const = 0;
};

} // namespace AreaComponents
} // namespace Cry
