// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <LevelEditor/Tools/EditTool.h>

#include <CryExtension/CryGUID.h>

//! "Recenter Pivot": move the entity to the centroid of its shape's points without moving the
//! shape, in one undo step. Registered as "EditTool.AreaShapeRecenter", which is what the Polygon
//! and Spline components name in their second SEditorActionDesc.
//!
//! It is a ONE-SHOT action wearing a tool's clothes, because a button in the component inspector is
//! an SEditToolButton and an SEditToolButton starts a CEditTool - that is the whole mechanism
//! stage 2 generalised. So the tool does its work on the first Display() and then pops itself off
//! the level editor; it never handles a mouse event.
//!
//! Why Display() and not SetUserData(): SetUserData runs while the property tree is still
//! installing the tool, so ending it there would leave the caller holding a deleted object.
//! Display() is the first callback that runs with the tool fully installed - the same place
//! CShapeEditTool notices a lost target and leaves.
//!
//! What it does, and why this is the only correct way to move a pivot:
//!   * remember every point's WORLD position;
//!   * CBaseObject::SetWorldPos() to the centroid - the editor object owns the transform in
//!     Sandbox, and CEntityObject pushes it onto the entity (nothing reads a transform back off
//!     the entity, which is why the engine-side version of this button could never work);
//!   * rewrite every point in the new local frame from the remembered world positions, so nothing
//!     moves on screen;
//!   * the 1a sync, then accept the undo - so Ctrl+Z restores both halves together.
class CShapeRecenterTool : public CEditTool
{
	DECLARE_DYNCREATE(CShapeRecenterTool)

public:
	CShapeRecenterTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Recenter Pivot"; }
	virtual void   SetUserData(const char* key, void* userData) override;
	virtual void   Display(SDisplayContext& dc) override;
	virtual bool   MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags) override { return false; }
	virtual bool   IsNeedMoveTool() override { return false; }
	// ~CEditTool

protected:
	virtual ~CShapeRecenterTool() = default;
	virtual void DeleteThis() override { delete this; }

private:
	//! The whole action. Returns false when there was nothing to do, which is not an error.
	bool RecenterPivot();

	CryGUID m_objectGuid = CryGUID::Null();
	CryGUID m_componentGuid = CryGUID::Null();

	//! The action runs once; Display() is called every frame.
	bool m_bDone = false;

	//! Most points any one recenter will move. A shape with more is refused rather than truncated:
	//! silently leaving the tail behind would tear the shape apart.
	static constexpr int kMaxPoints = 4096;
};
