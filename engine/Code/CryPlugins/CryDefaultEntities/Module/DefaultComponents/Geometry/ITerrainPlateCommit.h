// Copyright 2026 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include <CryExtension/CryGUID.h>
#include <CryMath/Cry_Color.h>

struct IMaterial;

namespace Cry
{
namespace DefaultComponents
{

//! The link the editor side of the terrain plate stamp needs into the plate component.
//!
//! An interface rather than the class because CHeightmap and CRGBLayer are exported from Sandbox.exe, so
//! the stamp can only run inside EditorQt, while CTerrainPlateComponent lives in an engine plugin that
//! Sandbox never links. Every member is pure virtual, so EditorQt needs no link time symbol; it reaches
//! one by including TerrainPlateComponent.h and static_casting (the precedent is EditorParticle with
//! CParticleComponent). A cross cast is not available - RTTI is off for every engine module - and
//! IEntityComponent::QueryInterface is protected. Deliberately not derived from IEntityComponent: the
//! plate already reaches it through CBaseMeshComponent and a second path would make that base ambiguous.
//!
//! Two user operations, two undo groups: Stamp Height (GetCommitFootprint, OnHeightStamped) and Stamp
//! Colour (the appearance queries, OnColourStamped). Each stands alone; neither is a precondition of the
//! other, and neither changes a single setting on the plate: a stamp is a pure, repeatable action.
struct ITerrainPlateCommit
{
	//! Type id of CTerrainPlateComponent, for IEntity::GetComponentByTypeId. Must stay in step with
	//! CTerrainPlateComponent::ReflectType's desc.SetGUID.
	static CryGUID GetComponentTypeId() { return "{3F1D8B4A-6C2E-4E9F-9A21-7B5C0D4E8A13}"_cry_guid; }

	//! Bake Into Terrain, as the panel shows it. Only used to tell "nothing baked to keep" (a no-op that
	//! deserves an explanation) from "armed but the bake is not in the ground" (a warning).
	virtual bool  IsBakeIntoTerrainEnabled() const = 0;

	//! Terrain Colour From Plate, as the panel shows it, including the gate that hides it. False means the
	//! ground under this plate is showing no plate colour, so there is nothing to make permanent.
	virtual bool  IsLiveColourEnabled() const = 0;

	//! The sector aligned square the LIVE bake holds in the engine terrain, in terrain units. The block
	//! covers (outSize + 1)^2 cells - units outX1 .. outX1 + outSize inclusive - which is the range
	//! ITerrain::SetTerrainElevation reads.
	//! False when there is nothing to stamp: the plate holds no live bake in the terrain.
	virtual bool  GetCommitFootprint(int& outX1, int& outY1, int& outSize) const = 0;

	//! The square the COLOUR stamp covers, in terrain units, same convention as GetCommitFootprint.
	//! Derived from the plate's current transform, so it is available with or without a live bake.
	virtual bool  GetCommitAppearanceFootprint(int& outX1, int& outY1, int& outSize) const = 0;

	//! smoothstep(t) * coverage at a world XY - the SAME rim curve the height bake used, so the colour
	//! edge sits exactly on the height edge. 0 outside the footprint.
	//! sampleSizeMeters is the edge of the sample's own footprint. Only the coverage term is an area
	//! measure. 0 or less means a point sample.
	virtual float GetCommitWeightAtWorld(float worldX, float worldY, float sampleSizeMeters) const = 0;

	//! THE colour sampler, shared by the live overlay, the export and the colour stamp, so the three
	//! tiers cannot show different colours.
	//!
	//! Returns the plate material's diffuse texture, sampled bilinearly at the world position projected
	//! top down through the plate's own UV mapping (mirroring and the material's texture modifier
	//! respected), times the material's diffuse colour.
	//!
	//! outLinearColor is LINEAR, not gamma: the CPU copy of an sRGB texture is already linear
	//! (CTexture::PrepareLowResSystemCopy), so the conversion belongs where the texture's colour space is
	//! known, and 8 bits of a linear value would crush the shadows.
	//!
	//! texelSizeMeters only picks the RESOLUTION of the sampled CPU copy (@see GetLowResSystemCopy), so
	//! every consumer must pass the terrain colour texel size, not its own filter footprint, or they
	//! sample different mips and drift apart.
	//!
	//! False when there is no usable projection (no material, no diffuse, or an undecompressable
	//! texture). There is no fallback colour, so false means "leave this texel alone".
	virtual bool  GetCommitAlbedoAtWorld(float worldX, float worldY, float texelSizeMeters,
	                                     ColorF& outLinearColor) const = 0;

	//! The LIVE terrain colour overlay, which is also what Stamp Colour writes. False when the feature is
	//! off or the plate has no footprint on the terrain.
	//!
	//! outSettling is true while a drag is in progress. Every dirty terrain texture sector costs a tile
	//! DXT recompress plus an upload, so the editor holds the repaint back until the plate is dropped.
	virtual bool  GetLiveColourState(int& outX1, int& outY1, int& outSize, ColorB& outTint,
	                                 bool& outSettling) const = 0;

	//! Bake Priority, as the panel shows it. THE composite order for overlapping plates: lower composites
	//! first and shows through, higher composites last and wins the rim, with the entity GUID as the
	//! tie-break - the same rule the height bake uses (@see CTerrainPlateComponent::BakesBefore), so the
	//! colour of two overlapping plates stacks the way their relief does.
	virtual int   GetCommitOrderPriority() const = 0;

	//! The material the plate actually renders with: the override on the plate's OWN slot, then the mesh's.
	//! The plate is not necessarily slot 0, so the editor cannot guess it from the entity. Only ever
	//! compared, never dereferenced - it is the material identity the overlay's change detector hashes.
	virtual const IMaterial* GetCommitMaterial() const = 0;

	//! The editor has written the stamped ground into CHeightmap: adopt it as this plate's baseline. The
	//! plate stays armed and keeps its live footprint - re-applying the bake over the ground it just
	//! stamped is a no-op, and the restore now hands back the stamped ground rather than the old one.
	//! Called INSIDE the editor's undo group, after the heights are written.
	virtual void  OnHeightStamped() = 0;

	//! The editor has written this plate's live colour into CRGBLayer. Nothing on the panel changes; the
	//! plate only records that the ground carries the colour, so the overlay and the export stop blending
	//! it a second time over itself. The picture does not change: the stamp IS the overlay.
	virtual void  OnColourStamped() = 0;

	//! Undo/redo of a colour stamp: put the record back exactly as it was, forced. Not ClearColourStamp,
	//! which ignores a clear in the frame after a stamp (the property tree reports the action button itself
	//! as a property change) - an undo is a deliberate reversal and must always land.
	//! Only the editor's own undo object calls this; nothing else on the plate changes.
	virtual void  SetColourStampRecord(bool bStamped) = 0;

	//! True while the ground under this plate already carries the colour this plate would draw (@see
	//! OnColourStamped). Pressing Stamp Colour again is a silent success: there is nothing left to write.
	virtual bool  IsColourStamped() const = 0;

	//! Somebody wrote heights straight into the engine terrain over the given square (terrain units, same
	//! convention as GetCommitFootprint). A plate whose live bake sits inside it has just been flattened,
	//! so bring its next self repair poll forward instead of waiting out the poll interval.
	virtual void  NudgeBakeAfterTerrainWrite(int rectX1, int rectY1, int rectSize) = 0;

protected:
	~ITerrainPlateCommit() {}
};

}
}
