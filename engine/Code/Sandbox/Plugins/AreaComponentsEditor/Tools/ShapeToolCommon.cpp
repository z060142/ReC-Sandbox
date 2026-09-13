// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IObjectManager.h>
#include <Viewport.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <Objects/BaseObject.h>
#include <Objects/DisplayContext.h>

#include <CryEntitySystem/IEntity.h>
#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/IObject.h>
#include <CrySchematyc/IObjectProperties.h>
#include <CrySchematyc/Utils/ClassProperties.h>
#include <CrySchematyc/Reflection/TypeDesc.h>

#include <IShapeComponent.h>

using Cry::AreaComponents::IShapeComponent;
using Cry::AreaComponents::IShapeComponentEdit;

namespace
{
//! Two clicks closer together than this make one point, not two.
const float kDrawMinPointSpacing = 0.01f;
//! Largest number of points drawn in one frame while a shape is being drawn.
const int kDrawContourPoints = 1024;
}

namespace AreaShapeTools
{

CBaseObject* FindObject(const CryGUID& objectGuid)
{
	if (objectGuid == CryGUID::Null())
		return nullptr;

	IObjectManager* pObjectManager = GetIEditor()->GetObjectManager();
	return pObjectManager != nullptr ? pObjectManager->FindObject(objectGuid) : nullptr;
}

IShapeComponent* AsShape(IEntityComponent* pComponent)
{
	if (pComponent == nullptr)
		return nullptr;

	// Only cast what really is one: the reflected base list is the cross-DLL proof that this
	// component implements the contract the tools are written against.
	if (pComponent->GetClassDesc().FindBaseByTypeID(Schematyc::GetTypeDesc<IShapeComponent>().GetGUID()) == nullptr)
		return nullptr;

	return static_cast<IShapeComponent*>(pComponent);
}

IShapeComponent* FindShapeOnObject(CBaseObject* pObject, CryGUID* pComponentGuidOut)
{
	IEntity* pEntity = pObject != nullptr ? pObject->GetIEntity() : nullptr;
	if (pEntity == nullptr)
		return nullptr;

	IEntityComponent* pComponent = pEntity->QueryComponentByInterfaceID(Schematyc::GetTypeDesc<IShapeComponent>().GetGUID());
	IShapeComponent*  pShape = AsShape(pComponent);

	if (pShape != nullptr && pComponentGuidOut != nullptr)
		*pComponentGuidOut = pComponent->GetGUID();

	return pShape;
}

void SyncEditedComponent(CBaseObject* pObject, IEntityComponent* pComponent)
{
	if (pComponent == nullptr)
		return;

	// The editor's save snapshot for a Schematyc entity is taken during the inspector's input
	// pass (EntityObject.cpp, CreateComponentWidget). A viewport tool writes outside that pass, so
	// the snapshot has to be refreshed by hand or the edit is lost on save, on undo and on game
	// mode. A legitimate no-op for an entity without a Schematyc object: the level then writes the
	// live component.
	if (IEntity* pEntity = pComponent->GetEntity())
	{
		if (Schematyc::IObject* pSchematycObject = pEntity->GetSchematycObject())
		{
			if (Schematyc::IObjectPropertiesPtr pProperties = pSchematycObject->GetObjectProperties())
			{
				if (Schematyc::CClassProperties* pClassProperties = pProperties->GetComponentProperties(pComponent->GetGUID()))
				{
					pClassProperties->SetOverridePolicy(Schematyc::EOverridePolicy::Override);
					pClassProperties->Read(pComponent->GetClassDesc(), pComponent);
				}
			}
		}
	}

	SEntityEvent event(ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED);
	pComponent->SendEvent(event);

	if (pObject != nullptr)
	{
		pObject->SetModified(false, false);

		// And the bounds. CEntityObject::SetModified -> CalcBBox reads the ENTITY's own local
		// bounds, which a shape component does not contribute to (it owns no render slot), so
		// CalcBBox sees no change and never reaches its InvalidateWorldBox(). The editor's cached
		// world box would then keep the size the shape had when it was last selected: F frames
		// the old volume and rubber-band selection uses the old box until something else
		// invalidates it.
		//
		// InvalidateTM(0) is the public route to that cache (CBaseObject::InvalidateWorldBox is
		// protected, and this is a plugin): it clears m_bWorldBoxValid, and CEntityObject's
		// override re-pushes the entity's world transform, which is a no-op here because the
		// transform did not change. Legacy does the same thing one level down - CAreaBox calls
		// InvalidateWorldBox() itself after a size change (AreaBox.cpp:523).
		pObject->InvalidateTM(0);

		pObject->UpdatePrefab();
	}
}

// ---------------------------------------------------------------------------
// The draw gesture
// ---------------------------------------------------------------------------

bool PickDrawPoint(CViewport* pView, CPoint& point, Vec3& worldPosOut)
{
	if (pView == nullptr)
		return false;

	// The pick CShapeObject makes while it is being drawn: cast to terrain and geometry, with no
	// axis constraint, so every point lands on what the user is pointing at (ShapeObject.cpp:1174).
	worldPosOut = pView->MapViewToCP(point, CLevelEditorSharedState::Axis::None, true, 0.0f);
	worldPosOut = pView->SnapToGrid(worldPosOut);
	return worldPosOut.IsValid();
}

bool AppendDrawPoint(IShapeComponent* pShape, const Vec3& worldPos)
{
	IShapeComponentEdit* pEdit = pShape != nullptr ? pShape->GetEditInterface() : nullptr;
	if (pEdit == nullptr)
		return false;

	// The points are the shape's own, so the world position has to come back into its space - the
	// component's world transform, which is the entity's while the component has no transform.
	const Matrix34 shapeTM = pShape->GetWorldTransformMatrix();

	const int count = pEdit->GetPointCount();
	if (count > 0)
	{
		const Vec3 lastWorld = shapeTM.TransformPoint(pEdit->GetPoint(count - 1));
		if (lastWorld.GetDistance(worldPos) < kDrawMinPointSpacing)
			return false;
	}

	Matrix34 invShapeTM = shapeTM;
	invShapeTM.Invert();

	pEdit->InsertPoint(-1, invShapeTM.TransformPoint(worldPos));

	return pEdit->GetPointCount() > count;
}

void DisplayDrawInProgress(SDisplayContext& dc, IShapeComponent* pShape, bool bCursorValid, const Vec3& cursorWorldPos)
{
	if (pShape == nullptr)
		return;

	// The buffer is on our stack, never a container handed to the other DLL (heap rule,
	// IShapeComponent.h).
	Vec3      points[kDrawContourPoints];
	const int count = min(pShape->GetContour(points, kDrawContourPoints, true), kDrawContourPoints);
	if (count <= 0)
		return;

	// Yellow is what legacy draws the edge being placed in (ShapeObject.cpp:1322-1330).
	dc.SetColor(ColorB(255, 255, 0, 255));

	for (int i = 0; i + 1 < count; ++i)
	{
		dc.DrawLine(points[i], points[i + 1]);
	}

	if (bCursorValid)
	{
		// The rubber band: the edge the next click would commit, and the edge that would close
		// the contour once it has one.
		dc.DrawLine(points[count - 1], cursorWorldPos);

		if (count >= 2)
		{
			dc.SetColor(ColorB(255, 255, 0, 128));
			dc.DrawLine(cursorWorldPos, points[0]);
		}
	}
}

} // namespace AreaShapeTools
