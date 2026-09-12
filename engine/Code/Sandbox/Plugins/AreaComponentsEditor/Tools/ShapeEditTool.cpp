// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeEditTool.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <IObjectManager.h>
#include <IUndoManager.h>
#include <IDisplayViewport.h>
#include <Viewport.h>
#include <Gizmos/IGizmoManager.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <Objects/BaseObject.h>
#include <Objects/DisplayContext.h>
#include <Preferences/SnappingPreferences.h>
#include <Util/Math.h>

#include <CryEntitySystem/IEntity.h>
#include <CryEntitySystem/IEntitySystem.h>
#include <CryPhysics/IPhysics.h>

#include <IShapeComponent.h>

#include <QtCore/Qt>

using Cry::AreaComponents::IShapeComponent;
using Cry::AreaComponents::IShapeComponentEdit;

namespace
{
//! How close, in the same units CShapeObject uses, the cursor must come to a handle or an edge to
//! grab it (SHAPE_CLOSE_DISTANCE, ShapeObject.cpp:54).
const float kShapeCloseDistance = 0.8f;
//! Half-edge of a point handle, in screen-constant units (CShapeObject draws its points this way).
const float kPointHandleSize = 0.8f;
//! How far along a view ray the editor pretends the ray ends (RAY_DISTANCE in EditorQt).
const float kRayDistance = 100000.0f;
//! Stack buffer the tool reads a contour into. Sampled splines get truncated for drawing, which is
//! a visual limit and never a correctness one; the alternative - a container the engine module
//! fills - is forbidden, see the heap rule in IShapeComponent.h.
const int kMaxContourPoints = 256;
//! Largest number of editable points the tool will handle in one frame.
const int kMaxEditPoints = 1024;

//! Distance between the view ray (given as two points) and the segment pi-pj, plus the point on
//! the ray. CShapeObject::RayToLineDistance, ShapeObject.cpp:2505, unchanged.
bool RayToLineDistance(const Vec3& rayLineP1, const Vec3& rayLineP2, const Vec3& pi, const Vec3& pj,
                       float& distance, Vec3& intPnt)
{
	Vec3  pa, pb;
	float ua, ub;
	if (!LineLineIntersect(pi, pj, rayLineP1, rayLineP2, pa, pb, ua, ub))
		return false;

	if (ub < 0.0f)
		return false; // behind the ray origin

	if (ua < 0.0f)
		distance = PointToLineDistance(rayLineP1, rayLineP2, pi, intPnt);
	else if (ua > 1.0f)
		distance = PointToLineDistance(rayLineP1, rayLineP2, pj, intPnt);
	else
	{
		intPnt = rayLineP1 + ub * (rayLineP2 - rayLineP1);
		distance = (pb - pa).GetLength();
	}

	return true;
}
}

// Same registration shape CryDesigner uses (DesignerEditor.cpp): a class desc in the
// ESYSTEM_CLASS_EDITTOOL category, dropped into the editor's class factory by the auto-register
// helper when the plugin DLL is loaded.
class CShapeEditTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaShapeEdit"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CShapeEditTool); }
};

REGISTER_CLASS_DESC(CShapeEditTool_ClassDesc);

IMPLEMENT_DYNCREATE(CShapeEditTool, CEditTool);

CShapeEditTool::CShapeEditTool()
{
	m_pManipulator = GetIEditor()->GetGizmoManager()->AddManipulator(this);
	m_pManipulator->signalBeginDrag.Connect(this, &CShapeEditTool::OnManipulatorBeginDrag);
	m_pManipulator->signalDragging.Connect(this, &CShapeEditTool::OnManipulatorDrag);
	m_pManipulator->signalEndDrag.Connect(this, &CShapeEditTool::OnManipulatorEndDrag);
}

CShapeEditTool::~CShapeEditTool()
{
	// A gesture that was still recording when the tool went away must not leave the undo manager
	// half open - the same guard CEditShapeTool's destructor has (ShapeObject.cpp:143-144).
	if (m_gestureOpen && GetIEditor()->GetIUndoManager()->IsUndoRecording())
	{
		GetIEditor()->GetIUndoManager()->Cancel();
	}
	m_gestureOpen = false;

	if (m_pManipulator != nullptr)
	{
		m_pManipulator->signalBeginDrag.DisconnectAll();
		m_pManipulator->signalDragging.DisconnectAll();
		m_pManipulator->signalEndDrag.DisconnectAll();
		GetIEditor()->GetGizmoManager()->RemoveManipulator(m_pManipulator);
		m_pManipulator = nullptr;
	}
}

void CShapeEditTool::SetUserData(const char* key, void* userData)
{
	if (userData == nullptr || key == nullptr || strcmp(key, SEntityComponentEditToolTarget::GetUserDataKey()) != 0)
	{
		return;
	}

	const SEntityComponentEditToolTarget* pTarget = static_cast<const SEntityComponentEditToolTarget*>(userData);
	m_objectGuid = pTarget->objectGuid;
	m_componentGuid = pTarget->componentGuid;
	m_selectedPoint = -1;

	// Refuse a payload that does not resolve instead of discovering it one virtual call later: a
	// tool that cannot find its component has nothing to draw and nothing to edit.
	if (ResolveShapeEdit() == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Edit Shape: no editable shape component found for object %s, component %s - the tool will do nothing.",
		           m_objectGuid.ToString().c_str(), m_componentGuid.ToString().c_str());

		m_objectGuid = CryGUID::Null();
		m_componentGuid = CryGUID::Null();
		return;
	}

	if (m_pManipulator != nullptr)
	{
		m_pManipulator->Invalidate();
	}
}

// ---------------------------------------------------------------------------
// Resolving - always from GUIDs, never from a stored pointer
// ---------------------------------------------------------------------------

CBaseObject* CShapeEditTool::ResolveObject() const
{
	return AreaShapeTools::FindObject(m_objectGuid);
}

IEntityComponent* CShapeEditTool::ResolveComponent() const
{
	if (m_componentGuid == CryGUID::Null())
		return nullptr;

	CBaseObject* pObject = ResolveObject();
	IEntity*     pEntity = pObject != nullptr ? pObject->GetIEntity() : nullptr;
	return pEntity != nullptr ? pEntity->GetComponentByGUID(m_componentGuid) : nullptr;
}

IShapeComponent* CShapeEditTool::ResolveShape() const
{
	return AreaShapeTools::AsShape(ResolveComponent());
}

IShapeComponentEdit* CShapeEditTool::ResolveShapeEdit() const
{
	IShapeComponent* pShape = ResolveShape();
	return pShape != nullptr ? pShape->GetEditInterface() : nullptr;
}

Matrix34 CShapeEditTool::GetShapeWorldTM() const
{
	IShapeComponent* pShape = ResolveShape();
	return pShape != nullptr ? pShape->GetWorldTransformMatrix() : Matrix34(IDENTITY);
}

Vec3 CShapeEditTool::GetPointWorldPos(int index) const
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || index < 0 || index >= pEdit->GetPointCount())
		return ZERO;

	return GetShapeWorldTM().TransformPoint(pEdit->GetPoint(index));
}

int CShapeEditTool::GatherPointsWorld(Vec3* pOut, int maxPoints) const
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || pOut == nullptr || maxPoints <= 0)
		return 0;

	const Matrix34 shapeWorldTM = GetShapeWorldTM();
	const int      count = min(pEdit->GetPointCount(), maxPoints);

	for (int i = 0; i < count; ++i)
		pOut[i] = shapeWorldTM.TransformPoint(pEdit->GetPoint(i));

	return count;
}

int CShapeEditTool::GatherEdgePointsWorld(int edgeIndex, Vec3* pOut, int maxPoints) const
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || pOut == nullptr || maxPoints < 2)
		return 0;

	const int count = pEdit->GetPointCount();
	if (edgeIndex < 0 || edgeIndex >= count)
		return 0;

	const int capacity = min(maxPoints, kMaxEdgeSamples);

	Vec3 local[kMaxEdgeSamples];
	int  pointCount = pEdit->GetEdgePoints(edgeIndex, local, capacity);

	if (pointCount < 2)
	{
		// The kind has no curved edges - the default answer - so the edge IS the chord.
		local[0] = pEdit->GetPoint(edgeIndex);
		local[1] = pEdit->GetPoint((edgeIndex + 1) % count);
		pointCount = 2;
	}

	const Matrix34 shapeWorldTM = GetShapeWorldTM();
	for (int i = 0; i < pointCount; ++i)
		pOut[i] = shapeWorldTM.TransformPoint(local[i]);

	return pointCount;
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

int CShapeEditTool::HitTestPoints(CViewport* pView, const Vec3& raySrc, const Vec3& rayDir, float& distanceOut) const
{
	Vec3      points[kMaxEditPoints];
	const int count = GatherPointsWorld(points, kMaxEditPoints);

	int   nearestPoint = -1;
	float nearestRayT = FLT_MAX;

	for (int i = 0; i < count; ++i)
	{
		if (!points[i].IsValid())
			continue;

		const float rayT = max(0.0f, rayDir.Dot(points[i] - raySrc));
		const float distance = (raySrc + rayDir * rayT - points[i]).GetLength();

		const float tolerance = kShapeCloseDistance * pView->GetScreenScaleFactor(points[i]) * 0.01f + pView->GetSelectionTolerance();
		if (distance <= tolerance && rayT < nearestRayT)
		{
			nearestRayT = rayT;
			nearestPoint = i;
		}
	}

	distanceOut = nearestRayT;
	return nearestPoint;
}

bool CShapeEditTool::HitTestEdge(CViewport* pView, const Vec3& raySrc, const Vec3& rayDir, int& indexOut, Vec3& worldPointOut) const
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr)
		return false;

	Vec3      points[kMaxEditPoints];
	const int count = GatherPointsWorld(points, kMaxEditPoints);
	if (count < 2)
		return false;

	const bool bClosed = pEdit->IsContourClosed();
	const Vec3 rayLineP1 = raySrc;
	const Vec3 rayLineP2 = raySrc + rayDir * kRayDistance;

	// CEditShapeTool asks CShapeObject::GetNearestEdge and then compares against the same
	// screen-relative tolerance the point test uses (ShapeObject.cpp:277-290).
	int   bestIndex = -1;
	float minDist = FLT_MAX;
	Vec3  intPnt(ZERO);
	Vec3  bestPnt(ZERO);

	for (int i = 0; i < count; ++i)
	{
		const int j = (i + 1) % count;
		if (!bClosed && j == 0 && i != 0)
			continue;

		// The edge as the KIND describes it: one chord for a polygon, the sampled curve for a
		// spline. Testing the samples is what makes Ctrl+click land the new point on the curve.
		Vec3      edge[kMaxEdgeSamples];
		const int edgePoints = GatherEdgePointsWorld(i, edge, kMaxEdgeSamples);

		for (int k = 0; k + 1 < edgePoints; ++k)
		{
			float d = 0.0f;
			if (!RayToLineDistance(rayLineP1, rayLineP2, edge[k], edge[k + 1], d, intPnt))
				continue;

			if (d < minDist)
			{
				minDist = d;
				bestIndex = i;
				bestPnt = intPnt;
			}
		}
	}

	if (bestIndex < 0)
		return false;

	const float tolerance = kShapeCloseDistance * pView->GetScreenScaleFactor(bestPnt) * 0.01f + pView->GetSelectionTolerance();
	if (minDist > tolerance)
		return false;

	indexOut = bestIndex;
	worldPointOut = bestPnt;
	return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

bool CShapeEditTool::HasLostTarget() const
{
	return m_objectGuid != CryGUID::Null() && ResolveShapeEdit() == nullptr;
}

void CShapeEditTool::AbandonLostTarget()
{
	// A gesture may still be recording against an object that no longer exists; roll it back
	// before anything else touches the undo stack.
	if (m_gestureOpen)
	{
		m_gestureOpen = false;

		IUndoManager* pUndoManager = GetIEditor()->GetIUndoManager();
		if (pUndoManager != nullptr && pUndoManager->IsUndoRecording())
		{
			pUndoManager->Cancel();
		}
	}

	m_modifying = false;
	m_selectedPoint = -1;
	m_bHoverEdgeValid = false;
	m_objectGuid = CryGUID::Null();
	m_componentGuid = CryGUID::Null();

	CryLog("Edit Shape: the shape component being edited is gone - leaving the tool.");

	// Deletes this tool (CEditTool::Release -> DeleteThis). Nothing may touch a member after it,
	// which is why every caller returns immediately. It is the same call FinishCreation() makes
	// from inside CShapeCreateTool::MouseCallback.
	GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
}

void CShapeEditTool::Display(SDisplayContext& dc)
{
	// The component can be removed from the entity, or the entity deleted, while the tool is up -
	// the inspector's "remove component" button is one click away. Display() runs every frame, so
	// it is where that is noticed; MouseCallback and OnKeyDown already refuse to act without a
	// resolved target.
	if (HasLostTarget())
	{
		AbandonLostTarget();
		return; // `this` is gone
	}

	IShapeComponent*     pShape = ResolveShape();
	IShapeComponentEdit* pEdit = pShape != nullptr ? pShape->GetEditInterface() : nullptr;
	if (pEdit == nullptr || dc.view == nullptr)
		return;

	// The hull outline. The component previewer draws it too, but it stops at the entity's
	// selection state, and while the tool is up the outline has to be there whatever happens to
	// the selection.
	//
	// The buffer is on our stack, never a container handed to the other DLL: the engine module and
	// this editor plugin use different heaps (see the heap rule in IShapeComponent.h).
	Vec3      contour[kMaxContourPoints];
	const int contourPoints = min(pShape->GetContour(contour, kMaxContourPoints, true), kMaxContourPoints);
	if (contourPoints >= 2)
	{
		dc.SetColor(ColorB(0, 255, 0, 255));
		dc.DrawPolyLine(contour, contourPoints, pEdit->IsContourClosed());
	}

	// The edge under the cursor while Ctrl is held: the one a click would split. Drawn as the
	// polyline the hit test used, so a curved edge is highlighted along its curve.
	if (m_bHoverEdgeValid && m_hoverEdgePointCount >= 2)
	{
		dc.SetColor(ColorB(255, 255, 0, 255));
		dc.SetLineWidth(3);
		for (int i = 0; i + 1 < m_hoverEdgePointCount; ++i)
			dc.DrawLine(m_hoverEdgePoints[i], m_hoverEdgePoints[i + 1]);
		dc.SetLineWidth(0);
	}

	// Screen-constant point handles, the look CShapeObject::DisplayNormal has
	// (ShapeObject.cpp:1377-1398): depth test off so a handle inside geometry stays grabbable.
	dc.DepthTestOff();

	Vec3      points[kMaxEditPoints];
	const int pointCount = GatherPointsWorld(points, kMaxEditPoints);

	for (int i = 0; i < pointCount; ++i)
	{
		if (!points[i].IsValid())
			continue; // never hand the aux renderer a NaN

		const float size = kPointHandleSize * dc.view->GetScreenScaleFactor(points[i]) * 0.01f;
		if (!NumberValid(size) || size <= 0.0f)
			continue;

		const Vec3 half(size, size, size);

		if (i == m_selectedPoint)
			dc.SetSelectedColor();
		else
			dc.SetColor(ColorB(255, 255, 0, 255));

		dc.DrawWireBox(points[i] - half, points[i] + half);
	}

	dc.DepthTestOn();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool CShapeEditTool::MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags)
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || pView == nullptr)
		return false;

	// Ctrl+Shift+click drops the selected point onto whatever the cursor is over.
	if (event == eMouseLDown && (flags & MK_CONTROL) && (flags & MK_SHIFT))
	{
		return SnapSelectedPointToTerrainOrGeometry(pView, point);
	}

	if (event != eMouseLDown && event != eMouseMove && event != eMouseLUp && event != eMouseLDblClick)
	{
		return false;
	}

	if (event == eMouseLDown)
	{
		m_mouseDownPos = point;
	}

	Vec3 raySrc, rayDir;
	pView->ViewToWorldRay(point, raySrc, rayDir);
	rayDir.Normalize();

	const Matrix34 shapeTM = GetShapeWorldTM();

	// Ctrl (without Shift) means "edit edges": highlight the edge under the cursor, and insert a
	// point into it on click (CEditShapeTool::MouseCallback, ShapeObject.cpp:287-322).
	m_bHoverEdgeValid = false;
	if ((flags & MK_CONTROL) && !m_modifying)
	{
		int  edgeIndex = -1;
		Vec3 edgePoint;
		if (HitTestEdge(pView, raySrc, rayDir, edgeIndex, edgePoint))
		{
			m_hoverEdgePointCount = GatherEdgePointsWorld(edgeIndex, m_hoverEdgePoints, kMaxEdgeSamples);
			m_bHoverEdgeValid = m_hoverEdgePointCount >= 2;

			pView->ResetCursor();

			if (event == eMouseLDown)
			{
				InsertPointOnEdge(pView, edgeIndex, edgePoint);
			}
		}
		return true;
	}

	float     pointDistance = FLT_MAX;
	const int hitPoint = HitTestPoints(pView, raySrc, rayDir, pointDistance);

	if (hitPoint >= 0)
	{
		if (event == eMouseLDown && !m_modifying)
		{
			SelectPoint(hitPoint);

			m_modifying = true;
			m_pointWorldPosAtDragStart = GetPointWorldPos(hitPoint);

			BeginGesture("Move Point");
			pEdit->BeginEdit();

			// Put the construction plane on the point, so a drag maps to the plane the user is
			// looking at rather than to the world origin.
			Matrix34 constructionMatrix(IDENTITY);
			constructionMatrix.SetTranslation(m_pointWorldPosAtDragStart);
			pView->SetConstructionMatrix(constructionMatrix);
		}

		// Delete a point with a double click, but only when the click really hit the point - so
		// that using the move gizmo over a point never deletes it (ShapeObject.cpp:359-372).
		if (event == eMouseLDblClick)
		{
			if (m_modifying)
			{
				if (IShapeComponentEdit* pOpenEdit = ResolveShapeEdit())
					pOpenEdit->EndEdit();
				m_modifying = false;
				EndGesture(false);
			}

			SelectPoint(hitPoint);
			RemoveSelectedPoint();
			return true;
		}

		pView->SetCurrentCursor(STD_CURSOR_HIT);
	}
	else if (event == eMouseLDown && !m_modifying)
	{
		SelectPoint(-1);
		pView->ResetCursor();
	}

	if (m_modifying && event == eMouseMove && m_selectedPoint >= 0)
	{
		// The free drag of the legacy tool (ShapeObject.cpp:398-434): the point follows the cursor
		// on the construction plane, with grid snapping, and rides the terrain when terrain
		// snapping is on. The legacy "+ GetShapeZOffset()" is not reproduced because that offset
		// is 0 for every CShapeObject (m_defaultZOffset, ShapeObject.cpp:880); only GameVolume
		// overrode it, out of a Lua script - see report 02.
		const Vec3 p1 = pView->MapViewToCP(m_mouseDownPos);
		const Vec3 p2 = pView->MapViewToCP(point);
		const Vec3 delta = pView->GetCPVector(p1, p2);

		Vec3 newPos = m_pointWorldPosAtDragStart + delta;
		if (gSnappingPreferences.IsSnapToTerrainEnabled())
		{
			newPos = pView->MapViewToCP(point);
		}

		newPos = pView->SnapToGrid(newPos);

		Matrix34 invShapeTM = shapeTM;
		invShapeTM.Invert();
		pEdit->SetPoint(m_selectedPoint, invShapeTM.TransformPoint(newPos));

		pView->SetCurrentCursor(STD_CURSOR_MOVE);
		return true;
	}

	if (m_modifying && event == eMouseLUp)
	{
		m_modifying = false;
		pEdit->EndEdit();
		EndGesture(true);
		return true;
	}

	return hitPoint >= 0 || m_modifying;
}

bool CShapeEditTool::OnKeyDown(CViewport* pView, uint32 nChar, uint32 nRepCnt, uint32 nFlags)
{
	if (nChar == Qt::Key_Escape)
	{
		GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
		return true;
	}

	if (nChar == Qt::Key_Delete)
	{
		if (!m_modifying)
		{
			RemoveSelectedPoint();
		}
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// Point operations - one undo step each
// ---------------------------------------------------------------------------

void CShapeEditTool::SelectPoint(int index)
{
	if (index == m_selectedPoint)
		return;

	m_selectedPoint = index;
	if (m_pManipulator != nullptr)
		m_pManipulator->Invalidate();
}

void CShapeEditTool::InsertPointOnEdge(CViewport* pView, int edgeIndex, const Vec3& worldPoint)
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || edgeIndex < 0)
		return;

	Matrix34 invShapeTM = GetShapeWorldTM();
	invShapeTM.Invert();

	BeginGesture("Insert Point");

	// Insert BEFORE the edge's far end, so the new point lands between the two points the user
	// clicked between. On the closing edge that far end is point 0, and inserting before point 0
	// would move the new point to the front of the list - append instead, which is what legacy
	// means by "if last edge, insert at end" (ShapeObject.cpp:302-303).
	const int count = pEdit->GetPointCount();
	const int insertBefore = ((edgeIndex + 1) % count == 0) ? count : (edgeIndex + 1);

	const int newIndex = pEdit->InsertPoint(insertBefore, invShapeTM.TransformPoint(worldPoint));
	if (newIndex < 0)
	{
		// A kind with a fixed point set (the box) refuses; nothing changed.
		EndGesture(false);
		return;
	}

	SelectPoint(newIndex);
	EndGesture(true);

	// Set the construction plane on the new point, so that the drag that usually follows works
	// on the plane the user is looking at.
	if (pView != nullptr)
	{
		Matrix34 constructionMatrix(IDENTITY);
		constructionMatrix.SetTranslation(GetPointWorldPos(newIndex));
		pView->SetConstructionMatrix(constructionMatrix);
	}
}

void CShapeEditTool::RemoveSelectedPoint()
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || m_selectedPoint < 0 || m_selectedPoint >= pEdit->GetPointCount())
		return;

	const int countBefore = pEdit->GetPointCount();

	BeginGesture("Remove Point");
	pEdit->RemovePoint(m_selectedPoint);

	// The component is the authority on its own minimum point count and warns when it refuses
	// (a closed polygon never drops below a triangle); an undo step for nothing is worse than none.
	const bool bRemoved = pEdit->GetPointCount() < countBefore;
	if (bRemoved)
	{
		SelectPoint(-1);
	}

	EndGesture(bRemoved);
}

bool CShapeEditTool::SnapSelectedPointToTerrainOrGeometry(CViewport* pView, CPoint& point)
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || m_selectedPoint < 0 || m_selectedPoint >= pEdit->GetPointCount())
		return false;

	Vec3 raySrc, rayDir;
	pView->ViewToWorldRay(point, raySrc, rayDir);
	rayDir.Normalize();

	IPhysicalWorld* pPhysicalWorld = gEnv->pPhysicalWorld;
	if (pPhysicalWorld == nullptr)
		return false;

	// rwi_stop_at_pierceable makes every hit solid whatever the surface type says it is, the same
	// call CEditShapeTool makes (ShapeObject.cpp:455-459).
	ray_hit hit;
	ZeroStruct(hit);
	if (pPhysicalWorld->RayWorldIntersection(raySrc, rayDir * 1000.0f, ent_all, rwi_stop_at_pierceable, &hit, 1) == 0)
		return false;

	Matrix34 invShapeTM = GetShapeWorldTM();
	invShapeTM.Invert();

	BeginGesture("Snap Point");
	pEdit->BeginEdit();
	pEdit->SetPoint(m_selectedPoint, invShapeTM.TransformPoint(hit.pt));
	pEdit->EndEdit();
	EndGesture(true);

	return true;
}

// ---------------------------------------------------------------------------
// Manipulator - one gesture, one undo step
// ---------------------------------------------------------------------------

bool CShapeEditTool::GetManipulatorMatrix(Matrix34& tm)
{
	IShapeComponent* pShape = ResolveShape();
	if (pShape == nullptr)
		return false;

	tm = pShape->GetWorldTransformMatrix();
	tm.OrthonormalizeFast();
	return true;
}

void CShapeEditTool::GetManipulatorPosition(Vec3& position)
{
	position = GetPointWorldPos(m_selectedPoint);
}

bool CShapeEditTool::IsManipulatorVisible()
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	return pEdit != nullptr && m_selectedPoint >= 0 && m_selectedPoint < pEdit->GetPointCount();
}

void CShapeEditTool::OnManipulatorBeginDrag(IDisplayViewport* pView, ITransformManipulator*, const Vec2i&, int flags)
{
	if (GetIEditor()->GetLevelEditorSharedState()->GetEditMode() != CLevelEditorSharedState::EditMode::Move)
		return;

	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || m_selectedPoint < 0 || m_selectedPoint >= pEdit->GetPointCount())
		return;

	m_pointWorldPosAtDragStart = GetPointWorldPos(m_selectedPoint);

	Matrix34 constructionMatrix(IDENTITY);
	constructionMatrix.SetTranslation(m_pointWorldPosAtDragStart);
	if (pView != nullptr)
		pView->SetConstructionMatrix(constructionMatrix);

	BeginGesture("Move Point");
	pEdit->BeginEdit();
}

void CShapeEditTool::OnManipulatorDrag(IDisplayViewport*, ITransformManipulator*, const SDragData& dragData)
{
	if (!m_gestureOpen)
		return;

	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr || m_selectedPoint < 0 || m_selectedPoint >= pEdit->GetPointCount())
		return;

	Matrix34 invShapeTM = GetShapeWorldTM();
	invShapeTM.Invert();

	pEdit->SetPoint(m_selectedPoint, invShapeTM.TransformPoint(m_pointWorldPosAtDragStart + dragData.accumulateDelta));
}

void CShapeEditTool::OnManipulatorEndDrag(IDisplayViewport*, ITransformManipulator*)
{
	if (!m_gestureOpen)
		return;

	if (IShapeComponentEdit* pEdit = ResolveShapeEdit())
	{
		pEdit->EndEdit();
	}

	EndGesture(true);
}

// ---------------------------------------------------------------------------
// Undo and the save-snapshot sync
// ---------------------------------------------------------------------------

void CShapeEditTool::BeginGesture(const char* szName)
{
	if (m_gestureOpen)
		return;

	CBaseObject* pObject = ResolveObject();
	if (pObject == nullptr)
		return;

	IUndoManager* pUndoManager = GetIEditor()->GetIUndoManager();
	if (pUndoManager == nullptr)
		return;

	pUndoManager->Begin();
	if (pUndoManager->IsUndoRecording())
	{
		// CUndoBaseObject serializes the whole entity, components included, so one snapshot per
		// gesture is both enough and the most we can afford (research/06 section 5.1).
		pObject->StoreUndo(szName);
	}

	m_gestureOpen = true;
}

void CShapeEditTool::EndGesture(bool bChanged)
{
	if (!m_gestureOpen)
		return;

	m_gestureOpen = false;

	if (bChanged)
	{
		SyncEditedComponent();
	}

	IUndoManager* pUndoManager = GetIEditor()->GetIUndoManager();
	if (pUndoManager != nullptr && pUndoManager->IsUndoRecording())
	{
		if (bChanged)
			pUndoManager->Accept("Edit Shape");
		else
			pUndoManager->Cancel();
	}

	if (m_pManipulator != nullptr)
	{
		m_pManipulator->Invalidate();
	}
}

void CShapeEditTool::SyncEditedComponent()
{
	AreaShapeTools::SyncEditedComponent(ResolveObject(), ResolveComponent());
}
