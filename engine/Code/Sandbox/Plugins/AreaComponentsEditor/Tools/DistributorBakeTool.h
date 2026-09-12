// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <LevelEditor/Tools/EditTool.h>

#include <CryExtension/CryGUID.h>

//! "Bake To Brushes": turn a distributor's live instances into real brush objects, once, in one
//! undo step. Registered as "EditTool.AreaDistributorBake", which is what CDistributorComponent
//! names in its SEditorActionDesc.
//!
//! Decision 04 called this D4 - "an extra button, never the mechanism". The live link to the spline
//! is what makes a distributor worth having; baking is for the moment an author wants to stop
//! generating and start hand-editing. So the bake switches the distributor's own reflected
//! `Enabled` member off rather than deleting the component: the parameters, the seed and the curve
//! all survive, and turning Enabled back on brings the generated instances back.
//!
//! One-shot, like CShapeRecenterTool, and for the same reason: an inspector button is an
//! SEditToolButton and an SEditToolButton starts a CEditTool. The work happens on the first
//! Display() - not in SetUserData(), which runs while the property tree is still installing the
//! tool - and the tool then pops itself off the level editor.
//!
//! WHAT IS CARRIED and what is not. The brushes are created through IEditor::NewObject("Brush",
//! <cgf path>) and placed with CBaseObject::SetWorldTM, i.e. through the generic CBaseObject API
//! only: this plugin deliberately does not link EditorQt, so CBrushObject's own variables - the
//! render-flag block, view-distance and LOD ratios, material layers - are not reachable and the
//! baked brushes get the defaults a hand-placed brush gets. The instance's flags and ratios are
//! read out of the component (SBakeInstance carries them) and reported in the log line, so nothing
//! is silently lost. Linking EditorQt would fix it; that is a bigger decision than this button.
//!
//! The brushes are created FLAT, not inside a group, sharing one name prefix
//! "<entity name>_baked". A CGroup lives in EditorQt and its AddMember contract is not reachable
//! through CBaseObject alone, and a half-working group is worse than none: the single CUndo already
//! makes Ctrl+Z remove every brush and re-enable the distributor in one step, which is what
//! grouping was wanted for.
class CDistributorBakeTool : public CEditTool
{
	DECLARE_DYNCREATE(CDistributorBakeTool)

public:
	CDistributorBakeTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Bake To Brushes"; }
	virtual void   SetUserData(const char* key, void* userData) override;
	virtual void   Display(SDisplayContext& dc) override;
	virtual bool   MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags) override { return false; }
	virtual bool   IsNeedMoveTool() override { return false; }
	// ~CEditTool

protected:
	virtual ~CDistributorBakeTool() = default;
	virtual void DeleteThis() override { delete this; }

private:
	//! The whole action. Returns false when there was nothing to bake, which is not an error.
	bool Bake();

	CryGUID m_objectGuid = CryGUID::Null();
	CryGUID m_componentGuid = CryGUID::Null();

	bool m_bDone = false;

	//! Most instances one bake will turn into objects. A distributor can legitimately generate tens
	//! of thousands; making that many editor objects in one undo step is not a favour to anybody,
	//! so past this it refuses and says so.
	static constexpr int kMaxBakedObjects = 5000;
};
