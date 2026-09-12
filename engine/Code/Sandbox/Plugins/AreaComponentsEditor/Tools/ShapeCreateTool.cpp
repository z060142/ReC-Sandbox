// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeCreateTool.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <IObjectManager.h>
#include <IUndoManager.h>
#include <Viewport.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <Objects/BaseObject.h>
#include <Objects/ClassDesc.h>
#include <Objects/DisplayContext.h>

#include <CryEntitySystem/IEntity.h>

#include <IShapeComponent.h>

#include <QtCore/Qt>

using Cry::AreaComponents::IShapeComponent;
using Cry::AreaComponents::IShapeComponentEdit;

namespace
{
//! Two clicks closer together than this make one point, not two.
const float kMinPointSpacing = 0.01f;
//! Largest number of points drawn in one frame while the shape is being created.
const int kMaxDrawPoints = 1024;
}

//! CAreaFunctionComponent::ReflectType, CryPlugins/AreaComponents/Module/Functions/AreaFunctionComponent.h.
const char* const szAreaFunctionComponentGuid = "B4E27C09-8A16-4D53-9C70-1F5D3E8A4620";

class CShapeCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaShapeCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CShapeCreateTool); }
};

REGISTER_CLASS_DESC(CShapeCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CShapeCreateTool, CEditTool);

//! "Create Object -> Area -> Polygon": the same draw gesture, but the entity ends up with the Area
//! function component beside the shape, because that is what "an area" means to the user. The bare
//! polygon shape stays reachable through Add Component.
class CAreaPolygonCreateTool : public CShapeCreateTool
{
	DECLARE_DYNCREATE(CAreaPolygonCreateTool)

public:
	CAreaPolygonCreateTool() = default;

protected:
	virtual ~CAreaPolygonCreateTool() = default;

	virtual const char* GetFunctionComponentGuid() const override { return szAreaFunctionComponentGuid; }
};

class CAreaPolygonCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaPolygonCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CAreaPolygonCreateTool); }
};

REGISTER_CLASS_DESC(CAreaPolygonCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CAreaPolygonCreateTool, CShapeCreateTool);

CShapeCreateTool::CShapeCreateTool()
{
}

CShapeCreateTool::~CShapeCreateTool()
{
	// Leaving the tool mid-draw must not leave half an entity and an open undo transaction behind.
	if (m_bCreating)
	{
		CancelCreation();
	}
}

// ---------------------------------------------------------------------------
// Resolving - always from GUIDs, never from a stored pointer
// ---------------------------------------------------------------------------

CBaseObject* CShapeCreateTool::ResolveObject() const
{
	return AreaShapeTools::FindObject(m_objectGuid);
}

IEntityComponent* CShapeCreateTool::ResolveComponent() const
{
	CBaseObject* pObject = ResolveObject();
	IEntity*     pEntity = pObject != nullptr ? pObject->GetIEntity() : nullptr;
	if (pEntity == nullptr || m_componentGuid == CryGUID::Null())
		return nullptr;

	return pEntity->GetComponentByGUID(m_componentGuid);
}

IShapeComponentEdit* CShapeCreateTool::ResolveShapeEdit() const
{
	IShapeComponent* pShape = AreaShapeTools::AsShape(ResolveComponent());
	return pShape != nullptr ? pShape->GetEditInterface() : nullptr;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

bool CShapeCreateTool::PickWorldPoint(CViewport* pView, CPoint& point, Vec3& worldPos) const
{
	if (pView == nullptr)
		return false;

	// The pick CShapeObject makes while it is being drawn: cast to terrain and geometry, with no
	// axis constraint, so every point lands on what the user is pointing at (ShapeObject.cpp:1174).
	worldPos = pView->MapViewToCP(point, CLevelEditorSharedState::Axis::None, true, 0.0f);
	worldPos = pView->SnapToGrid(worldPos);
	return worldPos.IsValid();
}

bool CShapeCreateTool::StartCreation(const Vec3& worldPos)
{
	IObjectManager* pObjectManager = GetIEditor()->GetObjectManager();
	IUndoManager*   pUndoManager = GetIEditor()->GetIUndoManager();
	if (pObjectManager == nullptr || pUndoManager == nullptr)
		return false;

	if (!pObjectManager->CanCreateObject())
		return false;

	// One undo step for the whole creation, the way CObjectCreateTool brackets its own
	// (ObjectCreateTool.cpp:218-244): NewObject records CUndoBaseObjectNew inside it.
	pUndoManager->Begin();

	// The class desc's file spec is the shape component's GUID: that is what makes
	// CEntityObjectWithComponent::Init create the component on the new entity
	// (EntityObjectWithComponent.cpp:10-19), and the Create panel passes it the same way.
	CObjectClassDesc* pClassDesc = pObjectManager->FindClass(GetObjectClassName());
	const char*       szFileSpec = pClassDesc != nullptr ? pClassDesc->GetFileSpec() : nullptr;

	CBaseObject* pObject = GetIEditor()->NewObject(GetObjectClassName(), szFileSpec, true);
	if (pObject == nullptr)
	{
		pUndoManager->Cancel();
		return false;
	}

	pObject->SetPos(worldPos);

	m_objectGuid = pObject->GetId();
	m_componentGuid = CryGUID::Null();

	if (AreaShapeTools::FindShapeOnObject(pObject, &m_componentGuid) == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR,
		           "Create Shape: the object class \"%s\" produced no shape component - creation aborted.",
		           GetObjectClassName());

		GetIEditor()->DeleteObject(pObject);
		pUndoManager->Cancel();
		m_objectGuid = CryGUID::Null();
		return false;
	}

	// The preset's function component, if it has one. Added exactly the way
	// CEntityObjectWithComponent adds the shape itself (EntityObjectWithComponent.cpp:22-40) -
	// UserAdded is what makes it serialize and show up in the inspector - and inside the same undo
	// transaction, so one Ctrl+Z removes the whole entity, not half of it.
	if (const char* szFunctionComponentGuid = GetFunctionComponentGuid())
	{
		const CryGUID functionGuid = CryGUID::FromString(szFunctionComponentGuid);

		if (IEntity* pEntity = pObject->GetIEntity())
		{
			if (!functionGuid.IsNull() && pEntity->QueryComponentByInterfaceID(functionGuid) == nullptr)
			{
				if (IEntityComponent* pFunctionComponent = pEntity->CreateComponentByInterfaceID(functionGuid, false))
				{
					pFunctionComponent->GetComponentFlags().Add(EEntityComponentFlags::UserAdded);
				}
				else
				{
					CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
					           "Create Shape: the function component %s could not be added - the shape was created without it.",
					           szFunctionComponentGuid);
				}
			}
		}
	}

	m_bCreating = true;

	SeedShape();

	return true;
}

void CShapeCreateTool::SeedShape()
{
	// The first point is the object's own origin, exactly as legacy starts a shape with
	// InsertPoint(-1, Vec3(0,0,0)) (ShapeObject.cpp:1185-1188).
	if (IShapeComponentEdit* pEdit = ResolveShapeEdit())
	{
		pEdit->InsertPoint(-1, ZERO);
	}
}

void CShapeCreateTool::AppendPoint(const Vec3& worldPos)
{
	CBaseObject*         pObject = ResolveObject();
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	if (pObject == nullptr || pEdit == nullptr)
		return;

	// The points are the shape's own, so the world position has to come back into its space - the
	// component's world transform, which is the entity's while the component has no transform.
	IShapeComponent* pShape = AreaShapeTools::AsShape(ResolveComponent());
	Matrix34         shapeTM = pShape != nullptr ? pShape->GetWorldTransformMatrix() : pObject->GetWorldTM();

	// The click that opens a double click lands on the pixel the double click lands on, so it
	// would append the point the double click is about to finish with. Dropping a point that sits
	// on the previous one is what legacy achieves by popping its trailing temporary point
	// (ShapeObject.cpp:1198-1199) - here it keeps the log free of "too close to another point".
	const int count = pEdit->GetPointCount();
	if (count > 0)
	{
		const Vec3 lastWorld = shapeTM.TransformPoint(pEdit->GetPoint(count - 1));
		if (lastWorld.GetDistance(worldPos) < kMinPointSpacing)
			return;
	}

	Matrix34 invShapeTM = shapeTM;
	invShapeTM.Invert();

	pEdit->InsertPoint(-1, invShapeTM.TransformPoint(worldPos));
}

void CShapeCreateTool::FinishCreation()
{
	CBaseObject*         pObject = ResolveObject();
	IShapeComponentEdit* pEdit = ResolveShapeEdit();
	IUndoManager*        pUndoManager = GetIEditor()->GetIUndoManager();

	const int pointCount = pEdit != nullptr ? pEdit->GetPointCount() : 0;
	if (pObject == nullptr || pointCount < GetMinPointCount())
	{
		// Too few points is a cancelled draw, not a shape: legacy returns MOUSECREATE_ABORT and
		// the object is thrown away (ShapeObject.cpp:1194-1205).
		CancelCreation();
		return;
	}

	m_bCreating = false;

	AreaShapeTools::SyncEditedComponent(pObject, ResolveComponent());

	if (pUndoManager != nullptr && pUndoManager->IsUndoRecording())
	{
		pUndoManager->Accept(string("New ") + pObject->GetTypeName());
	}

	if (IObjectManager* pObjectManager = GetIEditor()->GetObjectManager())
	{
		pObjectManager->ClearSelection();
		pObjectManager->SelectObject(pObject);
	}

	m_objectGuid = CryGUID::Null();
	m_componentGuid = CryGUID::Null();

	GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
}

void CShapeCreateTool::CancelCreation()
{
	m_bCreating = false;

	if (CBaseObject* pObject = ResolveObject())
	{
		GetIEditor()->DeleteObject(pObject);
	}

	IUndoManager* pUndoManager = GetIEditor()->GetIUndoManager();
	if (pUndoManager != nullptr && pUndoManager->IsUndoRecording())
	{
		pUndoManager->Cancel();
	}

	m_objectGuid = CryGUID::Null();
	m_componentGuid = CryGUID::Null();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool CShapeCreateTool::MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags)
{
	if (pView == nullptr)
		return false;

	if (event != eMouseMove && event != eMouseLDown && event != eMouseLDblClick)
		return false;

	Vec3 worldPos;
	if (!PickWorldPoint(pView, point, worldPos))
		return false;

	m_cursorWorldPos = worldPos;
	m_bCursorValid = true;

	if (event == eMouseMove)
	{
		return true;
	}

	if (event == eMouseLDblClick)
	{
		// Click-click-double-click: the double click ends the shape. The click that precedes it
		// has already appended its point, so nothing is added here.
		if (m_bCreating)
		{
			FinishCreation();
		}
		return true;
	}

	// eMouseLDown
	if (!m_bCreating)
	{
		StartCreation(worldPos);
	}
	else
	{
		AppendPoint(worldPos);
	}

	return true;
}

bool CShapeCreateTool::OnKeyDown(CViewport* pView, uint32 nChar, uint32 nRepCnt, uint32 nFlags)
{
	if (nChar == Qt::Key_Escape)
	{
		CancelCreation();
		GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
		return true;
	}

	if (nChar == Qt::Key_Return || nChar == Qt::Key_Enter)
	{
		if (m_bCreating)
		{
			FinishCreation();
		}
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void CShapeCreateTool::Display(SDisplayContext& dc)
{
	if (!m_bCreating)
		return;

	IShapeComponent* pShape = AreaShapeTools::AsShape(ResolveComponent());
	if (pShape == nullptr)
		return;

	// The buffer is on our stack, never a container handed to the other DLL (heap rule,
	// IShapeComponent.h).
	Vec3      points[kMaxDrawPoints];
	const int count = min(pShape->GetContour(points, kMaxDrawPoints, true), kMaxDrawPoints);
	if (count <= 0)
		return;

	// Yellow is what legacy draws the edge being placed in (ShapeObject.cpp:1322-1330).
	dc.SetColor(ColorB(255, 255, 0, 255));

	for (int i = 0; i + 1 < count; ++i)
	{
		dc.DrawLine(points[i], points[i + 1]);
	}

	if (m_bCursorValid)
	{
		// The rubber band: the edge the next click would commit, and the edge that would close
		// the polygon once it has one.
		dc.DrawLine(points[count - 1], m_cursorWorldPos);

		if (count >= 2)
		{
			dc.SetColor(ColorB(255, 255, 0, 128));
			dc.DrawLine(m_cursorWorldPos, points[0]);
		}
	}
}
