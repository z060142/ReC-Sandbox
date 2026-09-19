// Copyright 2026 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include <CryMath/Cry_Color.h>
#include <CryEntitySystem/IEntityBasicTypes.h>   // EntityId, for the stamp's one plate

struct IEntity;

//! The terrain plate's diffuse as the terrain's macro colour, live and non destructive. The source is
//! ITerrainPlateCommit::GetCommitAlbedoAtWorld, the same call terrain.stamp_plate_colour makes at the
//! same resolution, so Stamp Colour means "make what you are looking at permanent".
//!
//! The engine's editor side macro colour is a PULL: a dirty node makes
//! CTerrainNode::UpdateNodeTextureFromEditorData build its tile row by row out of
//! CHeightmap::GetColorAtPosition. The overlay is applied to that OUTGOING row buffer and CRGBLayer, the
//! level's actual colour, is never written - CRGBLayer::Serialize flushes every dirty tile to disk on
//! every level save, so a write there would become permanent at the next Ctrl+S.
//!
//! Consequences, all wanted: nothing to restore (the atlas is a cache, its truth is CRGBLayer); it
//! survives streaming, because StreamOnComplete re-arms the dirty flag; and it is editor only, because
//! the game loads terraintexture.pak, which is exported from CRGBLayer.
namespace TerrainPlateColourOverlay
{

//! THE composite order for two overlapping plates, and the only one: Bake Priority, then the entity GUID -
//! the same rule the height bake uses, so colour stacks the way relief does. True when pA is blended
//! FIRST, i.e. pB wins where the two meet. The preview and the export sort themselves by it; the stamp
//! walks the selection in it, which is what makes a multi plate stamp come out as the picture showed.
//! Both entities must carry a terrain plate component.
bool CompositesBefore(IEntity* pA, IEntity* pB);

//! THE blend, shared by the live overlay and by terrain.stamp_plate_colour. Both sides to linear, lerp
//! there, back to gamma - the terrain painter's own recipe; blending gamma values directly makes the rim
//! band read too dark.
//! destination and the return value are one terrain colour texel in the RGB layer's packed layout,
//! blue | green << 8 | red << 16. linearAlbedo comes from GetCommitAlbedoAtWorld and is already LINEAR.
//! tint is the user's optional gamma space filter over it, white by default.
uint32 BlendPlateColourIntoTexel(uint32 destination, const ColorF& linearAlbedo, ColorB tint, float weight);

//! Once per editor frame: re-enumerates the plates asking for the overlay and marks the terrain texture
//! sectors under any that appeared, moved, changed or went away. Debounced - a plate that reports itself
//! settling is left alone, and no plate repaints more often than kRepaintFrameInterval.
void Update();

//! True when at least one plate is currently asking for the overlay. Cheap; the stock path must stay
//! byte identical when this is false.
bool IsActive();

//! Composites the live plates over one row of terrain colour texels, in place. Called from
//! CHeightmap::GetColorAtPosition with the row it is about to return.
//! The engine asks for a row along WORLD Y at a fixed world X (CHeightmap's fpx is the RGB layer's, i.e.
//! the world Y axis). worldStepY is the node's own texel size, so ancestor tiles get a wider filter
//! footprint and the rim antialiases at every level.
void ApplyToRow(uint32* pRow, int count, float worldYStart, float worldStepY, float worldX, float rgbTexelSizeMeters);

//! EXPORT. The live overlay above only edits the engine's editor side pull, and the game loads
//! terraintexture.pak, which CGameExporter builds from CRGBLayer - so without this the plate colour never
//! ships. BeginExport takes one snapshot of the plates asking for the overlay (hidden plates and plates
//! with the tick box off are not in it) and ApplyExportToRow composites them into the OUTGOING export tile
//! through the same code path ApplyToRow uses. CRGBLayer is not written and no sector is marked dirty:
//! the editor's own colour is exactly what it was before the export.
void BeginExport();
void EndExport();

//! True while an export snapshot holds at least one plate. The stock export stays byte identical when
//! this is false.
bool IsExportActive();

//! One row of the export tile, in place. Same argument shape as ApplyToRow; texelSizeMeters is the EXPORT
//! tile's own texel size, which is what makes the exported picture agree with the editor preview.
void ApplyExportToRow(uint32* pRow, int count, float worldYStart, float worldStepY, float worldX, float texelSizeMeters);

//! STAMP - terrain.stamp_plate_colour. The same three functions again, over ONE plate, writing into CRGBLayer
//! itself: this is the tier that is permanent. BeginStamp reads that plate's overlay state as it is right now
//! (false when the plate is showing no colour, i.e. there is nothing to make permanent); ApplyStampToRow
//! composites one row of RGB layer texels through the very loop the preview uses and returns how many texels
//! it changed. Fed the RGB layer's own texel size and the tile row's world mapping, the result IS the preview,
//! texel for texel.
bool BeginStamp(EntityId plateId);
void EndStamp();
int  ApplyStampToRow(uint32* pRow, int count, float worldYStart, float worldStepY, float worldX, float texelSizeMeters);

}
