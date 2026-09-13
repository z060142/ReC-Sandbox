// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "RoadAlignTerrainTool.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <IUndoManager.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <Objects/BaseObject.h>
#include <Objects/DisplayContext.h>

#include <Terrain/Heightmap.h>

#include <CryEntitySystem/IEntity.h>
#include <CrySchematyc/Reflection/TypeDesc.h>

#include <IRoadAlign.h>

#include <vector>

using Cry::AreaComponents::IRoadAlignSource;
using Cry::AreaComponents::SRoadSample;

namespace
{

//! Only cast what really is one: the reflected base list is the cross-DLL proof that this component
//! implements the contract, exactly as AreaShapeTools::AsShape does for the shape contract.
IRoadAlignSource* AsRoadAlignSource(IEntityComponent* pComponent)
{
	if (pComponent == nullptr)
		return nullptr;

	if (pComponent->GetClassDesc().FindBaseByTypeID(Schematyc::GetTypeDesc<IRoadAlignSource>().GetGUID()) == nullptr)
		return nullptr;

	return static_cast<IRoadAlignSource*>(pComponent);
}

//! Distance from `point` to the segment [a,b] in the XY plane, and the height the segment has
//! there. Returns the squared planar distance, so the caller can compare without a square root.
float PlanarDistanceSqToSegment(const Vec3& point, const Vec3& a, const Vec3& b, float& heightOut)
{
	const Vec2 p(point.x, point.y);
	const Vec2 p0(a.x, a.y);
	const Vec2 p1(b.x, b.y);

	const Vec2  delta = p1 - p0;
	const float lengthSq = delta.GetLength2();

	float t = 0.0f;
	if (lengthSq > 1e-8f)
		t = clamp_tpl((p - p0).Dot(delta) / lengthSq, 0.0f, 1.0f);

	const Vec2 closest = p0 + delta * t;
	heightOut = a.z + (b.z - a.z) * t;

	return (p - closest).GetLength2();
}

} // namespace

class CRoadAlignTerrainTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaRoadAlignTerrain"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CRoadAlignTerrainTool); }
};

REGISTER_CLASS_DESC(CRoadAlignTerrainTool_ClassDesc);

IMPLEMENT_DYNCREATE(CRoadAlignTerrainTool, CEditTool);

void CRoadAlignTerrainTool::SetUserData(const char* key, void* userData)
{
	if (userData == nullptr || key == nullptr || strcmp(key, SEntityComponentEditToolTarget::GetUserDataKey()) != 0)
	{
		return;
	}

	const SEntityComponentEditToolTarget* pTarget = static_cast<const SEntityComponentEditToolTarget*>(userData);
	m_objectGuid = pTarget->objectGuid;
	m_componentGuid = pTarget->componentGuid;
}

void CRoadAlignTerrainTool::Display(SDisplayContext& dc)
{
	if (m_bDone)
		return;

	m_bDone = true;

	Align();

	// Deletes this tool. Last statement, always.
	GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
}

bool CRoadAlignTerrainTool::Align()
{
	CBaseObject* pObject = AreaShapeTools::FindObject(m_objectGuid);
	IEntity*     pEntity = pObject != nullptr ? pObject->GetIEntity() : nullptr;
	if (pEntity == nullptr || m_componentGuid == CryGUID::Null())
		return false;

	IEntityComponent* pComponent = pEntity->GetComponentByGUID(m_componentGuid);
	IRoadAlignSource* pRoad = AsRoadAlignSource(pComponent);
	if (pRoad == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Align Terrain To Road: no road component found - nothing was done.");
		return false;
	}

	const int sampleCount = pRoad->GetRoadSampleCount();
	if (sampleCount < 2)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Align Terrain To Road: this road has no centre line yet - draw the spline first.");
		return false;
	}

	CHeightmap* pHeightmap = GetIEditor()->GetHeightmap();
	if (pHeightmap == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Align Terrain To Road: this level has no terrain.");
		return false;
	}

	const float unitSize = pHeightmap->GetUnitSize();
	if (unitSize <= 0.0f)
		return false;

	const int cellsX = static_cast<int>(pHeightmap->GetWidth());   // heightmap x runs along WORLD Y
	const int cellsY = static_cast<int>(pHeightmap->GetHeight());  // heightmap y runs along WORLD X
	if (cellsX < 2 || cellsY < 2)
		return false;

	// The whole centre line, copied out one sample at a time (heap rule, IRoadAlign.h).
	std::vector<SRoadSample> samples;
	samples.reserve(static_cast<size_t>(sampleCount));
	for (int i = 0; i < sampleCount; ++i)
	{
		SRoadSample sample;
		if (pRoad->GetRoadSample(i, sample))
			samples.push_back(sample);
	}

	if (samples.size() < 2)
		return false;

	const float borderWidth = max(0.0f, pRoad->GetRoadBorderWidth());
	const float halfCell = 0.5f * unitSize;

	// Footprint: every sample's reach, in world metres, then in cells.
	AABB footprint;
	footprint.Reset();

	for (const SRoadSample& sample : samples)
	{
		footprint.Add(sample.worldPos, 0.5f * max(sample.width, 2.0f) + borderWidth + halfCell);
	}

	const int minCellX = clamp_tpl(static_cast<int>(floorf(footprint.min.y / unitSize)) - 1, 0, cellsX - 1);
	const int maxCellX = clamp_tpl(static_cast<int>(ceilf(footprint.max.y / unitSize)) + 1, 0, cellsX - 1);
	const int minCellY = clamp_tpl(static_cast<int>(floorf(footprint.min.x / unitSize)) - 1, 0, cellsY - 1);
	const int maxCellY = clamp_tpl(static_cast<int>(ceilf(footprint.max.x / unitSize)) + 1, 0, cellsY - 1);

	if (minCellX > maxCellX || minCellY > maxCellY)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Align Terrain To Road: the road is entirely outside the terrain - nothing was done.");
		return false;
	}

	IUndoManager* pUndoManager = GetIEditor()->GetIUndoManager();
	const bool    bOwnTransaction = pUndoManager != nullptr && !pUndoManager->IsUndoRecording();
	if (bOwnTransaction)
		pUndoManager->Begin();

	// One undo entry for the whole rectangle, which is the terrain undo CRoadObject uses
	// (RoadObject.cpp:534) - the heightmap records its own old contents.
	pHeightmap->RecordUndo(minCellX, minCellY, maxCellX - minCellX + 1, maxCellY - minCellY + 1);

	const float maxHeight = pHeightmap->GetMaxHeight();
	const int   segmentCount = static_cast<int>(samples.size()) - 1;

	int changedCells = 0;

	for (int cellY = minCellY; cellY <= maxCellY; ++cellY)
	{
		const float worldX = static_cast<float>(cellY) * unitSize;

		for (int cellX = minCellX; cellX <= maxCellX; ++cellX)
		{
			const float worldY = static_cast<float>(cellX) * unitSize;
			const Vec3  cellPos(worldX, worldY, 0.0f);

			// Nearest place on the centre line. The road is a polyline here, which is exactly what
			// the render nodes were built from, so "the road's height" needs no interpolation of a
			// curve that the visible road does not follow either.
			float bestDistanceSq = FLT_MAX;
			float bestHeight = 0.0f;
			float bestWidth = 0.0f;

			for (int s = 0; s < segmentCount; ++s)
			{
				float       height = 0.0f;
				const float distanceSq = PlanarDistanceSqToSegment(cellPos, samples[s].worldPos, samples[s + 1].worldPos, height);

				if (distanceSq < bestDistanceSq)
				{
					bestDistanceSq = distanceSq;
					bestHeight = height;
					// The width of the nearer end. Legacy takes the wider of a segment's two ends
					// (RoadObject.cpp:552-556); this is the same idea without widening the road
					// everywhere a wide point exists.
					bestWidth = max(samples[s].width, samples[s + 1].width);
				}
			}

			const float distance = sqrtf(bestDistanceSq);
			// Legacy never narrows the aligned band below 2 m (RoadObject.cpp:505-506).
			const float halfWidth = 0.5f * max(bestWidth, 2.0f);

			if (distance > halfWidth + borderWidth + halfCell)
				continue;

			float newHeight = bestHeight;

			if (distance > halfWidth + halfCell && borderWidth > 0.0f)
			{
				// Raised cosine over the border band, RoadObject.cpp:631-636: 0 at the road's edge,
				// 1 at the outer edge of the band, blending back to the terrain as it was.
				float k = (distance - (halfWidth + halfCell)) / borderWidth;
				k = 1.0f - (cosf(clamp_tpl(k, 0.0f, 1.0f) * gf_PI) + 1.0f) * 0.5f;

				const float currentHeight = pHeightmap->GetXY(static_cast<uint32>(cellX), static_cast<uint32>(cellY));
				newHeight = k * currentHeight + (1.0f - k) * bestHeight;
			}

			pHeightmap->SetXY(static_cast<uint32>(cellX), static_cast<uint32>(cellY), clamp_tpl(newHeight, 0.0f, maxHeight));
			++changedCells;
		}
	}

	pHeightmap->UpdateEngineTerrain(minCellX, minCellY, maxCellX - minCellX + 1, maxCellY - minCellY + 1,
	                                CHeightmap::ETerrainUpdateType::Elevation);

	if (pUndoManager != nullptr && pUndoManager->IsUndoRecording() && bOwnTransaction)
		pUndoManager->Accept("Align Terrain To Road");

	// The road itself needs no rebuild: CTerrain::SetTerrainElevation collects every eERType_Road
	// node inside the changed rectangle and calls CRoadRenderNode::OnTerrainChanged on it
	// (terran_edit.cpp:407-421), which re-projects the road mesh onto the new heights. The
	// component's nodes are ordinary octree-registered road nodes, so they are in that list.

	CryLog("Align Terrain To Road on entity \"%s\": %d heightmap cells changed over %d x %d cells, "
	       "road %d stations, border width %.2f m. One Ctrl+Z puts the terrain back.",
	       pObject->GetName().c_str(), changedCells,
	       maxCellX - minCellX + 1, maxCellY - minCellY + 1,
	       static_cast<int>(samples.size()), borderWidth);

	return changedCells > 0;
}
