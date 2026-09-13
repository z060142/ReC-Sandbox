// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <LevelEditor/Tools/EditTool.h>
#include <Gizmos/ITransformManipulator.h>

#include <CryExtension/CryGUID.h>

class CBaseObject;
struct IEntityComponent;

namespace Cry
{
namespace AreaComponents
{
struct IShapeComponent;
struct IShapeComponentEdit;
}
}

//! The point-editing tool of the Area shape components (decision 02 section 2), registered as
//! "EditTool.AreaShapeEdit" - the name IEditorShapeComponent::GetEditToolClassName() returns, so
//! the "Edit Shape" row of the component inspector starts exactly this tool.
//!
//! It reproduces EditorQt's CEditShapeTool (ShapeObject.cpp:62-470) gesture for gesture: click a
//! point to select it, drag it (free with the mouse, or on an axis with the manipulator),
//! Ctrl+click an edge to insert a point there, double-click or Delete to remove one,
//! Ctrl+Shift+click to snap a point onto terrain or geometry, Esc to leave.
//!
//! It is written against Cry::AreaComponents::IShapeComponentEdit alone and never against a
//! concrete shape class, so the same tool serves Box and Polygon today and Sphere / Spline later.
//! It holds no pointer across a frame: object and component are re-resolved from their GUIDs on
//! every use (MEMORY.md, fix bde12cd8), which is also what makes it survive an undo that
//! re-creates the entity underneath it.
class CShapeEditTool : public CEditTool, public ITransformManipulatorOwner
{
	DECLARE_DYNCREATE(CShapeEditTool)

public:
	CShapeEditTool();

	// CEditTool
	virtual string GetDisplayName() const override { return "Edit Shape"; }
	virtual void   SetUserData(const char* key, void* userData) override;
	virtual void   Display(SDisplayContext& dc) override;
	virtual bool   MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags) override;
	virtual bool   OnKeyDown(CViewport* pView, uint32 nChar, uint32 nRepCnt, uint32 nFlags) override;
	virtual bool   IsNeedMoveTool() override                 { return true; }
	virtual bool   IsNeedToSkipPivotBoxForObjects() override { return true; }
	virtual bool   IsDisplayGrid() override                  { return false; }
	// ~CEditTool

	// ITransformManipulatorOwner
	virtual bool GetManipulatorMatrix(Matrix34& tm) override;
	virtual void GetManipulatorPosition(Vec3& position) override;
	virtual bool IsManipulatorVisible() override;
	// ~ITransformManipulatorOwner

protected:
	virtual ~CShapeEditTool();
	virtual void DeleteThis() override { delete this; }

private:
	void OnManipulatorBeginDrag(IDisplayViewport* pView, ITransformManipulator*, const Vec2i&, int flags);
	void OnManipulatorDrag(IDisplayViewport* pView, ITransformManipulator*, const SDragData& dragData);
	void OnManipulatorEndDrag(IDisplayViewport* pView, ITransformManipulator*);

	CBaseObject*                              ResolveObject() const;
	IEntityComponent*                         ResolveComponent() const;
	Cry::AreaComponents::IShapeComponent*     ResolveShape() const;
	Cry::AreaComponents::IShapeComponentEdit* ResolveShapeEdit() const;

	//! World transform of the shape's local space, identity when the shape cannot be resolved.
	Matrix34 GetShapeWorldTM() const;
	//! World position of an editable point, ZERO when out of range.
	Vec3     GetPointWorldPos(int index) const;
	//! Reads the editable points into a caller-owned buffer, in world space. Returns the count.
	//! Never a container across the DLL boundary (heap rule, IShapeComponent.h).
	int      GatherPointsWorld(Vec3* pOut, int maxPoints) const;

	//! The point under the cursor, or -1. `distanceOut` is the distance along the ray.
	int      HitTestPoints(CViewport* pView, const Vec3& raySrc, const Vec3& rayDir, float& distanceOut) const;
	//! The edge under the cursor. Returns false when nothing is close enough.
	bool     HitTestEdge(CViewport* pView, const Vec3& raySrc, const Vec3& rayDir, int& indexOut, Vec3& worldPointOut) const;
	//! The SHAPE of one edge in world space: the kind's own sampling when it has a curved edge
	//! (IShapeComponentEdit::GetEdgePoints), and the plain chord between the two points otherwise.
	//! Returns how many points were written, at least 2 - which is what lets one hit test and one
	//! highlight serve a straight polygon edge and a Bezier segment without knowing which it has.
	int      GatherEdgePointsWorld(int edgeIndex, Vec3* pOut, int maxPoints) const;

	void     SelectPoint(int index);
	//! Ctrl+click on an edge: one undo step, the new point selected.
	void     InsertPointOnEdge(CViewport* pView, int edgeIndex, const Vec3& worldPoint);
	//! Double-click or Delete: one undo step.
	void     RemoveSelectedPoint();
	//! Ctrl+Shift+click: put the selected point where the cursor hits the world
	//! (CEditShapeTool::SnapSelectedPointToTerrainOrGeometry, ShapeObject.cpp:440-469).
	bool     SnapSelectedPointToTerrainOrGeometry(CViewport* pView, CPoint& point);

	//! True once the tool had a target and that target is gone - the component was removed from
	//! the entity, or the entity itself was deleted, while the tool was up.
	bool     HasLostTarget() const;
	//! Ends the tool without touching the vanished target: cancels a half-open undo transaction,
	//! forgets the GUIDs and pops the tool off the level editor. MUST be the last thing a caller
	//! does, because it deletes this tool.
	void     AbandonLostTarget();

	//! --- The draw flow (backlog B1)
	//!
	//! A shape component added by hand - Add Component -> Area -> Shape: Polygon / Shape: Spline -
	//! arrives with no points at all, so there is nothing for the point tool to edit. Pressing
	//! Edit Shape on such a shape runs the SAME draw gesture the Create panel runs, on the entity
	//! the user already has: the entity's own position is point 1, every click adds the next
	//! point, a double-click or Enter finishes, Esc leaves the shape empty. When it finishes the
	//! tool stays up and simply becomes the point tool again, and the whole draw is one undo step.
	//!
	//! The three steps of the gesture itself - pick, append, draw - are the create tool's, shared
	//! through AreaShapeTools; what is here is only when to start it and when it is done.

	//! Starts the draw on a shape with fewer than two points. Deliberately NOT called from
	//! SetUserData(): that runs while the level editor is still installing the tool, and opening
	//! an undo transaction in there is the trap CShapeRecenterTool already fell into (report 05).
	void     StartDraw();
	//! Ends the draw and returns to point editing. Too few points for the kind
	//! (IShapeComponentEdit::GetMinPointCount) is a cancelled draw, with a warning.
	void     FinishDraw();
	//! Rolls the whole draw back to the empty shape the user pressed Edit Shape on and leaves the
	//! tool. DELETES this tool - callers must return immediately.
	void     CancelDraw();
	bool     DrawMouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags);

	void     BeginGesture(const char* szName);
	void     EndGesture(bool bChanged);
	void     SyncEditedComponent();

	CryGUID m_objectGuid = CryGUID::Null();
	CryGUID m_componentGuid = CryGUID::Null();

	int    m_selectedPoint = -1;
	bool   m_gestureOpen = false;
	//! The target had fewer than two points when the tool was given it: draw first, edit after.
	bool   m_bDrawPending = false;
	bool   m_bDrawing = false;
	//! Cursor position of the last mouse move, drawn as the rubber-band edge while drawing.
	bool   m_bCursorValid = false;
	Vec3   m_cursorWorldPos = ZERO;
	bool   m_modifying = false;
	CPoint m_mouseDownPos = CPoint(0, 0);
	Vec3   m_pointWorldPosAtDragStart = ZERO;

	//! Largest number of samples one edge is described by (a curved edge needs more than two).
	static constexpr int kMaxEdgeSamples = 32;

	//! The edge the cursor is on while Ctrl is held, in world space, for the Display highlight. It
	//! is a polyline, not a segment, so that a curved edge is highlighted along the curve.
	bool m_bHoverEdgeValid = false;
	Vec3 m_hoverEdgePoints[kMaxEdgeSamples];
	int  m_hoverEdgePointCount = 0;

	ITransformManipulator* m_pManipulator = nullptr;
};
