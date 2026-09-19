// Copyright 2026 Crytek GmbH / Crytek Group. All rights reserved.
#include "StdAfx.h"

#include "Terrain/TerrainPlateColourOverlay.h"

#include "Terrain/Heightmap.h"
#include "IEditorImpl.h"

// Header only downcast onto a pure virtual interface, so nothing here needs a symbol out of
// CryDefaultEntities.
#include <DefaultComponents/Geometry/TerrainPlateComponent.h>

#include <CryEntitySystem/IEntitySystem.h>

namespace TerrainPlateColourOverlay
{

namespace
{

using Cry::DefaultComponents::CTerrainPlateComponent;
using Cry::DefaultComponents::ITerrainPlateCommit;

//! Debounce floor: one dirty terrain texture sector costs a whole tile DXT recompress plus an upload.
//! A plate that reports itself settling is held back entirely; this is the backstop for the others.
const uint32 kRepaintFrameInterval = 6;

//! Below this a weight contributes less than half a quantisation step. terrain.stamp_plate_colour uses
//! the same epsilon, which is what keeps the two footprints identical.
const float  kWeightEpsilon = 0.002f;

struct SLivePlate
{
	//! The entity, not the component pointer: ApplyToRow re-resolves it, so a plate deleted between
	//! Update() and the engine's pull cannot dangle.
	EntityId             id = INVALID_ENTITYID;

	// Footprint in world metres, XY only.
	float                worldXMin = 0.f;
	float                worldYMin = 0.f;
	float                worldXMax = 0.f;
	float                worldYMax = 0.f;

	//! The user's optional filter over the sampled diffuse. White by default.
	ColorB               tint = ColorB(255, 255, 255, 255);

	//! Everything the look depends on, folded into one value. On a change the sectors under the OLD and
	//! the NEW footprint are marked dirty. @see MakeLivePlate for what goes into it.
	uint32               signature = 0;
	uint32               lastRepaintFrame = 0;

	//! A drag is in progress. The change detector holds its fire until this clears, so a drag repaints
	//! once, on release.
	bool                 bSettling = false;

	//! The composite order: Bake Priority, then the entity GUID. @see OrderBefore.
	int                  orderPriority = 0;
	CryGUID              guid = CryGUID::Null();
};

//! THE composite order for two plates, and the only one: lower first, so the higher priority plate is
//! blended last and wins where two footprints meet. The GUID tie-break is what makes it reproducible - the
//! entity iterator hands plates back in spawn order, which changes across a save and load. Deliberately
//! the same rule as the height bake (CTerrainPlateComponent::BakesBefore), so colour stacks like relief.
bool OrderBefore(int priorityA, const CryGUID& guidA, int priorityB, const CryGUID& guidB)
{
	if (priorityA != priorityB)
		return priorityA < priorityB;

	return guidA < guidB;
}

std::vector<SLivePlate> g_plates;

//! The export's own snapshot, taken once per export. Separate from g_plates so an export cannot disturb
//! the live preview's change detector (and so the export is unaffected by the debounce, which only exists
//! to keep the editor's frame rate up).
std::vector<SLivePlate> g_exportPlates;

//! The stamp's snapshot: exactly one plate, held only for the duration of terrain.stamp_plate_colour. Its own
//! list so the stamp goes through the same compositing loop as the preview without disturbing it.
std::vector<SLivePlate> g_stampPlates;

uint32 HashCombine(uint32 seed, uint32 value)
{
	// The usual boost mix; any avalanche would do, this is only a change detector.
	return seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2));
}

uint32 HashFloat(uint32 seed, float value)
{
	uint32 bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	return HashCombine(seed, bits);
}

//! Marks every terrain texture sector the box touches, through the editor's own refresh path - the same
//! recipe the texture painter uses for a dab, (iY, iX) axis order included. Calling
//! I3DEngine::SetTerrainSectorTexture directly would be wrong: the engine only arms the dirty flag for a
//! node whose editor texture id is non zero, and CHeightmap::UpdateSectorTexture is what creates it.
void MarkSectorsDirty(float worldXMin, float worldYMin, float worldXMax, float worldYMax)
{
	CHeightmap* pHeightmap = GetIEditorImpl()->GetHeightmap();
	I3DEngine*  p3DEngine = GetIEditorImpl()->Get3DEngine();
	if (pHeightmap == nullptr || p3DEngine == nullptr || p3DEngine->GetITerrain() == nullptr)
		return;

	const float terrainSize = (float)p3DEngine->GetTerrainSize();
	const int   texSectorSize = p3DEngine->GetTerrainTextureNodeSizeMeters();
	if (terrainSize <= 0.f || texSectorSize <= 0)
		return;

	const int sectorCount = (int)terrainSize / texSectorSize;
	if (sectorCount <= 0)
		return;

	const int minSecX = max((int)floorf(worldXMin / (float)texSectorSize), 0);
	const int minSecY = max((int)floorf(worldYMin / (float)texSectorSize), 0);
	const int maxSecX = min((int)ceilf(worldXMax / (float)texSectorSize), sectorCount);
	const int maxSecY = min((int)ceilf(worldYMax / (float)texSectorSize), sectorCount);

	const float normXMin = clamp_tpl(worldXMin / terrainSize, 0.f, 1.f);
	const float normYMin = clamp_tpl(worldYMin / terrainSize, 0.f, 1.f);
	const float normXMax = clamp_tpl(worldXMax / terrainSize, 0.f, 1.f);
	const float normYMax = clamp_tpl(worldYMax / terrainSize, 0.f, 1.f);

	for (int iY = minSecY; iY < maxSecY; ++iY)
	{
		for (int iX = minSecX; iX < maxSecX; ++iX)
		{
			pHeightmap->UpdateSectorTexture(CPoint(iY, iX), normYMin, normXMin, normYMax, normXMax);
		}
	}
}

//! The terrain unit size, or 0 when there is no usable terrain right now.
float GetTerrainUnitSize()
{
	if (gEnv == nullptr || gEnv->pEntitySystem == nullptr || gEnv->p3DEngine == nullptr
	    || gEnv->p3DEngine->GetITerrain() == nullptr)
	{
		return 0.f;
	}

	CHeightmap* pHeightmap = GetIEditorImpl()->GetHeightmap();
	if (pHeightmap == nullptr)
		return 0.f;

	return max(0.f, pHeightmap->GetUnitSize());
}

//! One entity's overlay state, or false when it is not showing plate colour. THE definition of "showing",
//! shared by the live preview, the export and the stamp, so no tier can decide differently.
bool MakeLivePlate(IEntity* pEntity, float unitSize, SLivePlate& out)
{
	if (pEntity == nullptr || unitSize <= 0.f)
		return false;

	// A hidden plate shows nothing, so it colours nothing - in the viewport, the export and the stamp alike.
	if (pEntity->IsHidden())
		return false;

	IEntityComponent* pComponent = pEntity->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
	if (pComponent == nullptr)
		return false;

	ITerrainPlateCommit* pCommit = static_cast<CTerrainPlateComponent*>(pComponent);

	int    rectX1 = 0, rectY1 = 0, rectSize = 0;
	ColorB tint;
	bool   bSettling = false;
	if (!pCommit->GetLiveColourState(rectX1, rectY1, rectSize, tint, bSettling) || rectSize <= 0)
		return false;

	SLivePlate plate;
	plate.id = pEntity->GetId();
	plate.worldXMin = (float)rectX1 * unitSize;
	plate.worldYMin = (float)rectY1 * unitSize;
	plate.worldXMax = (float)(rectX1 + rectSize) * unitSize;
	plate.worldYMax = (float)(rectY1 + rectSize) * unitSize;
	plate.tint = tint;
	plate.orderPriority = pCommit->GetCommitOrderPriority();
	plate.guid = pEntity->GetGuid();

	// A material EDIT (a different texture inside the same material) is not covered by this hash;
	// nudging the plate repaints it.
	uint32 signature = 0x811c9dc5u;
	signature = HashFloat(signature, plate.worldXMin);
	signature = HashFloat(signature, plate.worldYMin);
	signature = HashFloat(signature, plate.worldXMax);
	signature = HashFloat(signature, plate.worldYMax);

	// The plate's own transform, and NOT only the footprint above: GetCommitAppearanceFootprint answers
	// ComputeBakeRect, which is SECTOR aligned (SetTerrainElevation asserts whole sectors), so the rect only
	// changes when the plate crosses a whole terrain sector - tens of metres. The picture inside the rect is
	// drawn from the continuous transform (GetCommitWeightAtWorld / GetCommitAlbedoAtWorld sample the live
	// frame), so a move of a few metres changed everything the user can see while leaving the rect, and with
	// it the old signature, identical: no sector was ever marked dirty and the colour stayed behind at the
	// old position. Every component of the world matrix, so a rotate and a scale count as a move too.
	const Matrix34 worldTM = pEntity->GetWorldTM();
	for (int column = 0; column < 4; ++column)
	{
		const Vec3 axis = worldTM.GetColumn(column);
		signature = HashFloat(signature, axis.x);
		signature = HashFloat(signature, axis.y);
		signature = HashFloat(signature, axis.z);
	}
	signature = HashCombine(signature, ((uint32)tint.r << 16) | ((uint32)tint.g << 8) | (uint32)tint.b);
	// The plate's OWN material, through the interface: the plate is not necessarily slot 0, so the entity's
	// slot 0 override would miss a material change on any other slot.
	signature = HashCombine(signature, (uint32)(uintptr_t)pCommit->GetCommitMaterial());
	plate.signature = signature;
	plate.bSettling = bSettling;

	out = plate;
	return true;
}

//! Collects the plates that currently want the overlay. Once per editor frame, from Update().
void ScanPlates(std::vector<SLivePlate>& out)
{
	out.clear();

	const float unitSize = GetTerrainUnitSize();
	if (unitSize <= 0.f)
		return;

	IEntityItPtr it = gEnv->pEntitySystem->GetEntityIterator();
	if (it == nullptr)
		return;

	it->MoveFirst();
	while (!it->IsEnd())
	{
		SLivePlate plate;
		if (MakeLivePlate(it->Next(), unitSize, plate))
			out.push_back(plate);
	}

	// ApplyPlatesToRow blends in list order, so the list IS the composite order. The iterator hands plates
	// back in spawn order, which is not reproducible across a reload and is not the order the stamp walks
	// the selection in; sorting here is what makes the preview, the export and the stamp agree.
	std::sort(out.begin(), out.end(), [](const SLivePlate& a, const SLivePlate& b)
	{
		return OrderBefore(a.orderPriority, a.guid, b.orderPriority, b.guid);
	});
}

}   // anonymous namespace

bool CompositesBefore(IEntity* pA, IEntity* pB)
{
	if (pA == nullptr || pB == nullptr)
		return pA < pB;

	IEntityComponent* pComponentA = pA->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
	IEntityComponent* pComponentB = pB->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
	if (pComponentA == nullptr || pComponentB == nullptr)
		return pComponentA < pComponentB;

	// Through the INTERFACE, always: the overrides are final, so a call on the concrete type devirtualises
	// and Sandbox has no symbol for it.
	const int priorityA = static_cast<ITerrainPlateCommit*>(static_cast<CTerrainPlateComponent*>(pComponentA))->GetCommitOrderPriority();
	const int priorityB = static_cast<ITerrainPlateCommit*>(static_cast<CTerrainPlateComponent*>(pComponentB))->GetCommitOrderPriority();

	return OrderBefore(priorityA, pA->GetGuid(), priorityB, pB->GetGuid());
}

uint32 BlendPlateColourIntoTexel(uint32 destination, const ColorF& linearAlbedo, ColorB tint, float weight)
{
	const float recip255 = 1.f / 255.f;

	// The albedo is already linear (the sampler converted it where the texture's colour space was known);
	// only the tint arrives in gamma, being a colour picker value.
	ColorF colourSource = linearAlbedo;

	ColorF filter((float)tint.r * recip255, (float)tint.g * recip255, (float)tint.b * recip255);
	filter.srgb2rgb();
	colourSource.r *= filter.r;
	colourSource.g *= filter.g;
	colourSource.b *= filter.b;

	colourSource.clamp(0.f, 1.f);

	ColorF colourDest((float)((destination >> 16) & 0xff) * recip255,
	                  (float)((destination >> 8) & 0xff) * recip255,
	                  (float)(destination & 0xff) * recip255);
	colourDest.srgb2rgb();

	ColorF colourOut = colourSource * weight + colourDest * (1.f - weight);
	colourOut.rgb2srgb();
	colourOut *= 255.f;

	return ((uint32)clamp_tpl((int)colourOut.r, 0, 255) << 16)
	       | ((uint32)clamp_tpl((int)colourOut.g, 0, 255) << 8)
	       | (uint32)clamp_tpl((int)colourOut.b, 0, 255);
}

bool IsActive()
{
	return !g_plates.empty();
}

void Update()
{
	if (gEnv == nullptr)
		return;

	// No terrain: drop everything without marking anything. The next level repaints from its own
	// CRGBLayer, which this module never wrote to.
	if (gEnv->p3DEngine == nullptr || gEnv->p3DEngine->GetITerrain() == nullptr)
	{
		g_plates.clear();
		return;
	}

	// g_plates is not "the plates as they were last seen": it is the state the terrain colour atlas was last
	// PAINTED with. Every branch below that decides not to repaint puts the previous painted entry back, and
	// the comparison is always against that - which is what makes a change impossible to lose, however many
	// frames the debounce holds it for.
	std::vector<SLivePlate> current;
	ScanPlates(current);

	const uint32 frameId = gEnv->nMainFrameID;

	// Plates that appeared, moved or changed.
	for (SLivePlate& plate : current)
	{
		const SLivePlate* pPrevious = nullptr;
		for (const SLivePlate& previous : g_plates)
		{
			if (previous.id == plate.id)
			{
				pPrevious = &previous;
				break;
			}
		}

		if (pPrevious != nullptr && pPrevious->signature == plate.signature)
		{
			plate.lastRepaintFrame = pPrevious->lastRepaintFrame;
			continue;
		}

		// Mid drag: carry the last PAINTED state forward whole, so the change stays pending and the ground
		// repaints once, on release. The whole entry and not a few fields: g_plates is the record of what
		// the terrain atlas is currently showing, and the next frame compares against THAT. A partial copy
		// left the entry describing a state that was never painted (the carried signature said one tint,
		// the entry carried another), and the frame that finally settled could then agree with it and skip
		// the only repaint the move was ever going to get.
		if (pPrevious != nullptr && plate.bSettling)
		{
			plate = *pPrevious;
			continue;
		}

		if (pPrevious != nullptr && (frameId - pPrevious->lastRepaintFrame) < kRepaintFrameInterval)
		{
			// Too soon: carry the PAINTED state forward so the change stays pending. This is the throttle,
			// not a drop.
			plate = *pPrevious;
			continue;
		}

		if (pPrevious != nullptr)
		{
			// A move repaints the ground the plate LEFT as well as the ground it arrived on. pPrevious is the
			// LAST PAINTED footprint, not the last scanned one, so a drag that crossed several positions
			// while settling still hands back every sector it actually put colour on.
			MarkSectorsDirty(pPrevious->worldXMin, pPrevious->worldYMin, pPrevious->worldXMax, pPrevious->worldYMax);
		}

		MarkSectorsDirty(plate.worldXMin, plate.worldYMin, plate.worldXMax, plate.worldYMax);
		plate.lastRepaintFrame = frameId;
	}

	// Plates that went away. Repaint the ground they covered; the pull reads CRGBLayer, which never held
	// their colour, so it comes back exactly as it was.
	for (const SLivePlate& previous : g_plates)
	{
		bool bStillLive = false;
		for (const SLivePlate& plate : current)
		{
			if (plate.id == previous.id)
			{
				bStillLive = true;
				break;
			}
		}

		if (!bStillLive)
			MarkSectorsDirty(previous.worldXMin, previous.worldYMin, previous.worldXMax, previous.worldYMax);
	}

	g_plates.swap(current);
}

//! THE compositing loop, and the only one. The live pull and the exporter both come through here, so the
//! exported terrain texture cannot drift from what the editor showed.
//! Returns how many texels it actually changed, which only the stamp reads (for its log line).
static int ApplyPlatesToRow(const std::vector<SLivePlate>& plates, uint32* pRow, int count,
                      float worldYStart, float worldStepY, float worldX, float rgbTexelSizeMeters)
{
	int blended = 0;

	if (pRow == nullptr || count <= 0 || plates.empty())
		return blended;

	if (gEnv == nullptr || gEnv->pEntitySystem == nullptr)
		return blended;

	const float worldYEnd = worldYStart + worldStepY * (float)(count - 1);
	const float rowYMin = min(worldYStart, worldYEnd);
	const float rowYMax = max(worldYStart, worldYEnd);

	// The rim weight is an area measure and sampleSizeMeters is the area it is measured over. The colour
	// commit measures it over one RGB texel, so this floor makes the leaf tile agree with the commit
	// texel for texel; coarser ancestor tiles keep their own wider footprint and antialias the rim.
	const float sampleSize = max(fabs_tpl(worldStepY), rgbTexelSizeMeters);

	for (const SLivePlate& plate : plates)
	{
		if (worldX < plate.worldXMin || worldX > plate.worldXMax
		    || rowYMax < plate.worldYMin || rowYMin > plate.worldYMax)
		{
			continue;
		}

		// Re-resolve through the entity id: the pull runs inside the engine update, and an entity deleted
		// between Update() and here would leave a dangling interface pointer.
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(plate.id);
		if (pEntity == nullptr)
			continue;

		IEntityComponent* pComponent = pEntity->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
		if (pComponent == nullptr)
			continue;

		ITerrainPlateCommit* pCommit = static_cast<CTerrainPlateComponent*>(pComponent);

		for (int i = 0; i < count; ++i)
		{
			const float worldY = worldYStart + worldStepY * (float)i;
			if (worldY < plate.worldYMin || worldY > plate.worldYMax)
				continue;

			// The same rim weight call terrain.stamp_plate_colour makes, so the two cannot drift.
			const float weight = pCommit->GetCommitWeightAtWorld(worldX, worldY, sampleSize);
			if (weight <= kWeightEpsilon)
				continue;

			// The commit makes the identical call with the identical texel size, so both tiers sample the
			// same mip at the same place. An unsamplable material stamps nothing rather than inventing.
			ColorF albedo;
			if (!pCommit->GetCommitAlbedoAtWorld(worldX, worldY, rgbTexelSizeMeters, albedo))
				continue;

			pRow[i] = BlendPlateColourIntoTexel(pRow[i], albedo, plate.tint, weight);
			++blended;
		}
	}

	return blended;
}

void ApplyToRow(uint32* pRow, int count, float worldYStart, float worldStepY, float worldX, float rgbTexelSizeMeters)
{
	ApplyPlatesToRow(g_plates, pRow, count, worldYStart, worldStepY, worldX, rgbTexelSizeMeters);
}

void BeginExport()
{
	// A fresh scan, not g_plates: the export can run when the live list is stale (or empty, if the overlay
	// never ticked), and it must never inherit the preview's debounce.
	ScanPlates(g_exportPlates);
}

void EndExport()
{
	g_exportPlates.clear();
	g_exportPlates.shrink_to_fit();
}

bool IsExportActive()
{
	return !g_exportPlates.empty();
}

void ApplyExportToRow(uint32* pRow, int count, float worldYStart, float worldStepY, float worldX, float texelSizeMeters)
{
	ApplyPlatesToRow(g_exportPlates, pRow, count, worldYStart, worldStepY, worldX, texelSizeMeters);
}

bool BeginStamp(EntityId plateId)
{
	g_stampPlates.clear();

	// The plate's own state, read now - not g_plates, whose debounce can be a few frames behind what the user
	// is looking at, and which the stamp must not depend on having ticked at all.
	SLivePlate plate;
	if (gEnv == nullptr || gEnv->pEntitySystem == nullptr
	    || !MakeLivePlate(gEnv->pEntitySystem->GetEntity(plateId), GetTerrainUnitSize(), plate))
	{
		return false;
	}

	g_stampPlates.push_back(plate);
	return true;
}

void EndStamp()
{
	g_stampPlates.clear();
	g_stampPlates.shrink_to_fit();
}

int ApplyStampToRow(uint32* pRow, int count, float worldYStart, float worldStepY, float worldX, float texelSizeMeters)
{
	return ApplyPlatesToRow(g_stampPlates, pRow, count, worldYStart, worldStepY, worldX, texelSizeMeters);
}

}
