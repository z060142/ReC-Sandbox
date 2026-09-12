// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeRecenterTool.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <IUndoManager.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <Objects/BaseObject.h>
#include <Objects/DisplayContext.h>

#include <CryEntitySystem/IEntity.h>

#include <IShapeComponent.h>

using Cry::AreaComponents::IShapeComponent;
using Cry::AreaComponents::IShapeComponentEdit;

class CShapeRecenterTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaShapeRecenter"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CShapeRecenterTool); }
};

REGISTER_CLASS_DESC(CShapeRecenterTool_ClassDesc);

IMPLEMENT_DYNCREATE(CShapeRecenterTool, CEditTool);

void CShapeRecenterTool::SetUserData(const char* key, void* userData)
{
	if (userData == nullptr || key == nullptr || strcmp(key, SEntityComponentEditToolTarget::GetUserDataKey()) != 0)
	{
		return;
	}

	const SEntityComponentEditToolTarget* pTarget = static_cast<const SEntityComponentEditToolTarget*>(userData);
	m_objectGuid = pTarget->objectGuid;
	m_componentGuid = pTarget->componentGuid;
}

void CShapeRecenterTool::Display(SDisplayContext& dc)
{
	if (m_bDone)
		return;

	m_bDone = true;

	RecenterPivot();

	// Deletes this tool (CEditTool::Release -> DeleteThis). Nothing may touch a member after it,
	// which is why it is the last statement - the same pattern CShapeEditTool::AbandonLostTarget
	// and CShapeCreateTool::FinishCreation use.
	GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
}

bool CShapeRecenterTool::RecenterPivot()
{
	CBaseObject* pObject = AreaShapeTools::FindObject(m_objectGuid);
	IEntity*     pEntity = pObject != nullptr ? pObject->GetIEntity() : nullptr;
	if (pEntity == nullptr || m_componentGuid == CryGUID::Null())
		return false;

	IEntityComponent* pComponent = pEntity->GetComponentByGUID(m_componentGuid);
	IShapeComponent*  pShape = AreaShapeTools::AsShape(pComponent);
	IShapeComponentEdit* pEdit = pShape != nullptr ? pShape->GetEditInterface() : nullptr;
	if (pEdit == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Recenter Pivot: no editable shape component found - nothing was done.");
		return false;
	}

	const int pointCount = pEdit->GetPointCount();
	if (pointCount <= 0)
		return false;

	if (pointCount > kMaxPoints)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Recenter Pivot: %d points is more than this action moves in one step (%d) - nothing was done.",
		           pointCount, kMaxPoints);
		return false;
	}

	// Where every point is in the world right now. This is the whole trick: the pivot moves, these
	// do not, and the points are rewritten afterwards from these remembered positions.
	//
	// The vector is declared and freed in THIS module, and only Vec3 values cross the interface -
	// the heap rule in IShapeComponent.h forbids handing a container to the engine module.
	std::vector<Vec3> worldPoints(static_cast<size_t>(pointCount));

	const Matrix34 shapeTM = pShape->GetWorldTransformMatrix();
	Vec3           centroid(ZERO);

	for (int i = 0; i < pointCount; ++i)
	{
		worldPoints[i] = shapeTM.TransformPoint(pEdit->GetPoint(i));
		centroid += worldPoints[i];
	}

	centroid /= static_cast<float>(pointCount);

	// Already centred: an undo step that changes nothing is worse than no undo step.
	if (centroid.GetDistance(pObject->GetWorldPos()) < 0.001f)
		return false;

	IUndoManager* pUndoManager = GetIEditor()->GetIUndoManager();
	if (pUndoManager == nullptr)
		return false;

	pUndoManager->Begin();

	if (pUndoManager->IsUndoRecording())
	{
		// One CUndoBaseObject snapshot covers both halves - the object's transform AND the
		// component's serialized points - so Ctrl+Z puts the pivot and the points back together.
		pObject->StoreUndo("Recenter Pivot");
	}

	pObject->SetWorldPos(centroid);

	// The shape's world transform has moved with the entity; rebuild the points in the new frame so
	// that each one lands back on the world position it had a moment ago.
	Matrix34 invShapeTM = pShape->GetWorldTransformMatrix();
	invShapeTM.Invert();

	pEdit->BeginEdit();
	for (int i = 0; i < pointCount; ++i)
	{
		pEdit->SetPoint(i, invShapeTM.TransformPoint(worldPoints[i]));
	}
	pEdit->EndEdit();

	AreaShapeTools::SyncEditedComponent(pObject, pComponent);

	if (pUndoManager->IsUndoRecording())
	{
		pUndoManager->Accept("Recenter Pivot");
	}

	return true;
}
