// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeDragCreateTool.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <Viewport.h>
#include <Objects/BaseObject.h>
#include <Objects/DisplayContext.h>

#include <IShapeComponent.h>

using Cry::AreaComponents::IShapeComponent;
using Cry::AreaComponents::IShapeComponentEdit;

namespace
{
//! Largest number of contour points the tool draws in one frame (heap rule: a stack buffer, never
//! a container the engine module fills - see IShapeComponent.h).
const int kMaxDrawPoints = 256;
}

const float CShapeDragCreateTool::kMinDragDistance = 0.05f;

const float CShapeBoxCreateTool::kDefaultHeight = 2.0f;
const float CShapeBoxCreateTool::kDefaultHalfSize = 1.0f;

const float CShapeSphereCreateTool::kDefaultRadius = 1.0f;

IMPLEMENT_DYNAMIC(CShapeDragCreateTool, CShapeCreateTool)

// ---------------------------------------------------------------------------
// Registration - one tool class per object class, because the Create panel gives a custom
// creation tool no user data (report 02, "Box creation left as a single click").
// ---------------------------------------------------------------------------

class CShapeBoxCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaShapeBoxCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CShapeBoxCreateTool); }
};

REGISTER_CLASS_DESC(CShapeBoxCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CShapeBoxCreateTool, CShapeDragCreateTool);

class CShapeSphereCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaShapeSphereCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CShapeSphereCreateTool); }
};

REGISTER_CLASS_DESC(CShapeSphereCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CShapeSphereCreateTool, CShapeDragCreateTool);

// ---------------------------------------------------------------------------
// The gesture
// ---------------------------------------------------------------------------

Matrix34 CShapeDragCreateTool::GetShapeWorldTM() const
{
	if (IShapeComponent* pShape = AreaShapeTools::AsShape(ResolveComponent()))
	{
		return pShape->GetWorldTransformMatrix();
	}

	if (CBaseObject* pObject = ResolveObject())
	{
		return pObject->GetWorldTM();
	}

	return Matrix34(IDENTITY);
}

bool CShapeDragCreateTool::MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags)
{
	if (pView == nullptr)
		return false;

	if (event != eMouseMove && event != eMouseLDown && event != eMouseLUp)
		return false;

	Vec3 worldPos;
	if (!PickWorldPoint(pView, point, worldPos))
		return false;

	m_cursorWorldPos = worldPos;
	m_bCursorValid = true;

	switch (event)
	{
	case eMouseLDown:
		{
			if (m_bCreating)
				break; // a second press during the same gesture changes nothing

			if (!StartCreation(worldPos))
				return true;

			m_anchorWorld = worldPos;
			m_bDragging = true;

			// The whole drag is ONE gesture for the shape: listeners hear about it once, at the
			// end, instead of once per mouse move.
			if (IShapeComponentEdit* pEdit = ResolveShapeEdit())
			{
				pEdit->BeginEdit();
			}

			// A press that is released without moving still has to produce a usable shape.
			SizeToDrag(m_anchorWorld, worldPos, true);
			break;
		}

	case eMouseMove:
		{
			if (!m_bDragging)
				break;

			const bool bDefaultSize = m_anchorWorld.GetDistance(worldPos) < kMinDragDistance;
			SizeToDrag(m_anchorWorld, worldPos, bDefaultSize);

			pView->SetCurrentCursor(STD_CURSOR_MOVE);
			break;
		}

	case eMouseLUp:
		{
			if (!m_bDragging)
				break;

			m_bDragging = false;

			const bool bDefaultSize = m_anchorWorld.GetDistance(worldPos) < kMinDragDistance;
			SizeToDrag(m_anchorWorld, worldPos, bDefaultSize);

			if (IShapeComponentEdit* pEdit = ResolveShapeEdit())
			{
				pEdit->EndEdit();
			}

			pView->ResetCursor();

			// Accepts the one undo transaction StartCreation opened, selects the entity and
			// leaves the tool - all of it the polygon tool's code, unchanged.
			FinishCreation();
			break;
		}

	default:
		break;
	}

	return true;
}

void CShapeDragCreateTool::Display(SDisplayContext& dc)
{
	if (!m_bCreating)
		return;

	IShapeComponent* pShape = AreaShapeTools::AsShape(ResolveComponent());
	if (pShape == nullptr)
		return;

	// The component previewer already draws the shape itself every frame - the entity exists from
	// the first press. What is added here is the contour in the "being created" yellow legacy uses
	// for the part of a shape the mouse is still moving (ShapeObject.cpp:1322-1330), so that the
	// shape under the cursor reads as unfinished.
	Vec3      points[kMaxDrawPoints];
	const int count = min(pShape->GetContour(points, kMaxDrawPoints, true), kMaxDrawPoints);
	if (count < 2)
		return;

	dc.SetColor(ColorB(255, 255, 0, 255));
	dc.DrawPolyLine(points, count, true);
}

// ---------------------------------------------------------------------------
// Box: the drag rectangle is the footprint
// ---------------------------------------------------------------------------

void CShapeBoxCreateTool::SizeToDrag(const Vec3& anchorWorld, const Vec3& worldPos, bool bDefaultSize)
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr)
		return;

	Matrix34 invShapeTM = GetShapeWorldTM();
	invShapeTM.Invert();

	// Both ends of the drag in the shape's own space. The entity was spawned AT the anchor, so the
	// anchor is the local origin unless the object class rotated it.
	const Vec3 localAnchor = invShapeTM.TransformPoint(anchorWorld);

	// The box stands ON the drag plane rather than straddling it - see kDefaultHeight in the
	// header for why the third dimension is a default and not a second gesture.
	Vec3 localMin;
	Vec3 localMax;

	if (bDefaultSize)
	{
		// A click with no drag is centred on the pick point, which is what a single-click
		// placement gave before this tool existed.
		localMin = Vec3(localAnchor.x - kDefaultHalfSize, localAnchor.y - kDefaultHalfSize, localAnchor.z);
		localMax = Vec3(localAnchor.x + kDefaultHalfSize, localAnchor.y + kDefaultHalfSize, localAnchor.z + kDefaultHeight);
	}
	else
	{
		// A real drag spans exactly the rectangle the user dragged over.
		const Vec3 localCursor = invShapeTM.TransformPoint(worldPos);

		localMin = Vec3(min(localAnchor.x, localCursor.x), min(localAnchor.y, localCursor.y), localAnchor.z);
		localMax = Vec3(max(localAnchor.x, localCursor.x), max(localAnchor.y, localCursor.y), localAnchor.z + kDefaultHeight);
	}

	// Point 0 is the minimum corner, point 1 the maximum one - the two-point set
	// CBoxShapeComponent presents to the point tool.
	//
	// Three calls, in this order, and not two. Each SetPoint keeps the OTHER corner where it is
	// and refuses to let the box collapse (CBoxShapeComponent::SetPoint pushes the dragged side
	// back off the fixed one by kMinSize). A drag that lands entirely on one side of the box's
	// current position therefore gets its first corner clamped against the stale opposite corner.
	// Setting max, then min, then max again always converges: after the second call the fixed
	// corner is already the one the user dragged to, so the third call is unclamped.
	pEdit->SetPoint(1, localMax);
	pEdit->SetPoint(0, localMin);
	pEdit->SetPoint(1, localMax);
}

// ---------------------------------------------------------------------------
// Sphere: the drag distance is the radius
// ---------------------------------------------------------------------------

void CShapeSphereCreateTool::SizeToDrag(const Vec3& anchorWorld, const Vec3& worldPos, bool bDefaultSize)
{
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pEdit == nullptr)
		return;

	Matrix34 invShapeTM = GetShapeWorldTM();
	invShapeTM.Invert();

	const Vec3  localCentre = invShapeTM.TransformPoint(anchorWorld);
	const float radius = bDefaultSize ? kDefaultRadius : localCentre.GetDistance(invShapeTM.TransformPoint(worldPos));

	// Point 0 is the centre, point 1 the radius handle on local +X - the two-point set
	// CSphereShapeComponent presents to the point tool.
	pEdit->SetPoint(0, localCentre);
	pEdit->SetPoint(1, localCentre + Vec3(radius, 0.0f, 0.0f));
}
