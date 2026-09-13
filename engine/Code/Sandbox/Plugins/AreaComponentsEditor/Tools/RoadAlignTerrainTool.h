// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <LevelEditor/Tools/EditTool.h>

#include <CryExtension/CryGUID.h>

//! "Align Terrain To Road": push the heightmap up (or down) to meet the road the Road component
//! generated, once, in one undo step. Registered as "EditTool.AreaRoadAlignTerrain", which is what
//! CRoadComponent names in its SEditorActionDesc.
//!
//! It is the component form of CRoadObject::AlignHeightMap (RoadObject.cpp:473-667), and it has to
//! live in the editor rather than in the component: the thing it rewrites is CHeightmap, the
//! EDITOR's copy of the terrain. That copy is what the level saves, what the terrain undo stack
//! records and what UpdateEngineTerrain pushes into the engine; a component that changed only the
//! engine's terrain would lose the change on the next save.
//!
//! THIS IS WHY THIS PLUGIN NOW LINKS Sandbox. CHeightmap is declared in EditorQt and exported with
//! SANDBOX_API, and IEditor::GetHeightmap() hands it out as an opaque pointer - EditorCommon has no
//! terrain-writing interface at all (Cry3DEngine's IEditorHeightmap is read-only). CryDesigner
//! takes exactly this route for exactly this reason (CryDesigner/CMakeLists.txt links Sandbox), and
//! it is the precedent the brief named. The cost is that this DLL now has to be rebuilt and
//! deployed together with Sandbox.exe rather than on its own.
//!
//! What it does, per heightmap cell inside the road's footprint:
//!  * find the nearest place on the road's centre line and the perpendicular distance to it;
//!  * inside `width/2 + halfCell`, take the road's own height there;
//!  * out to `width/2 + borderWidth + halfCell`, blend back to the terrain's current height with
//!    the raised-cosine falloff legacy uses (RoadObject.cpp:626-637);
//!  * beyond that, leave the cell alone.
//! Legacy searches for the nearest curve parameter with 24 rounds of a golden-section walk
//! (:583-600); this walks the road's own sample polyline instead, which is the same answer without
//! the search, because the component already holds the centre line the render nodes were built
//! from.
//!
//! One-shot, like CShapeRecenterTool and CDistributorBakeTool, and for the same reason: an
//! inspector button is an SEditToolButton and an SEditToolButton starts a CEditTool. The work
//! happens on the first Display() - never in SetUserData(), which runs while the property tree is
//! still installing the tool - and the tool then pops itself off the level editor.
class CRoadAlignTerrainTool : public CEditTool
{
	DECLARE_DYNCREATE(CRoadAlignTerrainTool)

public:
	CRoadAlignTerrainTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Align Terrain To Road"; }
	virtual void   SetUserData(const char* key, void* userData) override;
	virtual void   Display(SDisplayContext& dc) override;
	virtual bool   MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags) override { return false; }
	virtual bool   IsNeedMoveTool() override { return false; }
	// ~CEditTool

protected:
	virtual ~CRoadAlignTerrainTool() = default;
	virtual void DeleteThis() override { delete this; }

private:
	//! The whole action. False when there was nothing to align, which is not an error.
	bool Align();

	CryGUID m_objectGuid = CryGUID::Null();
	CryGUID m_componentGuid = CryGUID::Null();

	bool m_bDone = false;
};
