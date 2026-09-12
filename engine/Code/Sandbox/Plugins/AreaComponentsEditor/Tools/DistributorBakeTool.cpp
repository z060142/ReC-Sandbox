// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "DistributorBakeTool.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <IObjectManager.h>
#include <IUndoManager.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <Objects/BaseObject.h>
#include <Objects/DisplayContext.h>

#include <CryEntitySystem/IEntity.h>
#include <CrySchematyc/Reflection/TypeDesc.h>

#include <IDistributorBake.h>

#include <vector>

using Cry::AreaComponents::IBakeableComponent;
using Cry::AreaComponents::SBakeInstance;

namespace
{

//! Only cast what really is one: the reflected base list is the cross-DLL proof that this component
//! implements the contract, exactly as AreaShapeTools::AsShape does for the shape contract.
IBakeableComponent* AsBakeable(IEntityComponent* pComponent)
{
	if (pComponent == nullptr)
		return nullptr;

	if (pComponent->GetClassDesc().FindBaseByTypeID(Schematyc::GetTypeDesc<IBakeableComponent>().GetGUID()) == nullptr)
		return nullptr;

	return static_cast<IBakeableComponent*>(pComponent);
}

} // namespace

class CDistributorBakeTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaDistributorBake"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CDistributorBakeTool); }
};

REGISTER_CLASS_DESC(CDistributorBakeTool_ClassDesc);

IMPLEMENT_DYNCREATE(CDistributorBakeTool, CEditTool);

void CDistributorBakeTool::SetUserData(const char* key, void* userData)
{
	if (userData == nullptr || key == nullptr || strcmp(key, SEntityComponentEditToolTarget::GetUserDataKey()) != 0)
	{
		return;
	}

	const SEntityComponentEditToolTarget* pTarget = static_cast<const SEntityComponentEditToolTarget*>(userData);
	m_objectGuid = pTarget->objectGuid;
	m_componentGuid = pTarget->componentGuid;
}

void CDistributorBakeTool::Display(SDisplayContext& dc)
{
	if (m_bDone)
		return;

	m_bDone = true;

	Bake();

	// Deletes this tool. Last statement, always.
	GetIEditor()->GetLevelEditorSharedState()->SetEditTool(nullptr);
}

bool CDistributorBakeTool::Bake()
{
	CBaseObject* pObject = AreaShapeTools::FindObject(m_objectGuid);
	IEntity*     pEntity = pObject != nullptr ? pObject->GetIEntity() : nullptr;
	if (pEntity == nullptr || m_componentGuid == CryGUID::Null())
		return false;

	IEntityComponent*   pComponent = pEntity->GetComponentByGUID(m_componentGuid);
	IBakeableComponent* pBakeable = AsBakeable(pComponent);
	if (pBakeable == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Bake To Brushes: no bakeable component found - nothing was done.");
		return false;
	}

	const int instanceCount = pBakeable->GetBakeInstanceCount();
	if (instanceCount <= 0)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Bake To Brushes: this distributor has placed no instances, so there is nothing to bake.");
		return false;
	}

	if (instanceCount > kMaxBakedObjects)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Bake To Brushes: %d instances is more than one bake will turn into editor objects (%d). "
		           "Raise the spacing, or bake a shorter spline.",
		           instanceCount, kMaxBakedObjects);
		return false;
	}

	IObjectManager* pObjectManager = GetIEditor()->GetObjectManager();
	IUndoManager*   pUndoManager = GetIEditor()->GetIUndoManager();
	if (pObjectManager == nullptr || pUndoManager == nullptr || !pObjectManager->CanCreateObject())
		return false;

	const string baseName = string(pObject->GetName()) + "_baked";

	pUndoManager->Begin();

	if (pUndoManager->IsUndoRecording())
	{
		// Taken BEFORE Enabled is switched off, so the undo restores the live distributor. Every
		// NewObject below records its own CUndoBaseObjectNew inside the same transaction, so one
		// Ctrl+Z removes the brushes and brings the instances back together.
		pObject->StoreUndo("Bake To Brushes");
	}

	std::vector<CBaseObject*> created;
	created.reserve(static_cast<size_t>(instanceCount));

	int skipped = 0;

	for (int i = 0; i < instanceCount; ++i)
	{
		SBakeInstance instance;
		if (!pBakeable->GetBakeInstance(i, instance))
			continue;

		if (instance.meshPath[0] == '\0')
		{
			++skipped;
			continue;
		}

		// "Brush" is CBrushObject's class name (BrushObject.h:175) and its file spec is the .cgf,
		// which is exactly what the create panel passes when a brush is dragged into the level.
		CBaseObject* pBrush = GetIEditor()->NewObject("Brush", instance.meshPath);
		if (pBrush == nullptr)
		{
			++skipped;
			continue;
		}

		// One call carries position, rotation and scale, which is all the generic CBaseObject API
		// can carry (see the header for what is deliberately not carried).
		pBrush->SetWorldTM(instance.tm);
		pBrush->SetName(pObjectManager->GenUniqObjectName(baseName));

		created.push_back(pBrush);
	}

	if (created.empty())
	{
		pUndoManager->Cancel();

		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "Bake To Brushes: none of the %d instances could be turned into a brush object - nothing was done.",
		           instanceCount);
		return false;
	}

	// Read before disabling: SetBakeEnabled(false) throws the instance list away.
	SBakeInstance first;
	const bool    bHaveFirst = pBakeable->GetBakeInstance(0, first);

	// The live instances go away, but the distributor keeps its parameters, its seed and its curve:
	// switching Enabled back on regenerates exactly the same layout.
	pBakeable->SetBakeEnabled(false);
	AreaShapeTools::SyncEditedComponent(pObject, pComponent);

	if (pUndoManager->IsUndoRecording())
	{
		pUndoManager->Accept("Bake To Brushes");
	}

	pObjectManager->ClearSelection();
	for (CBaseObject* pBrush : created)
	{
		pObjectManager->SelectObject(pBrush);
	}

	CryLog("Bake To Brushes on entity \"%s\": %d instances -> %d brush objects named \"%s*\", %d skipped for want of a "
	       "mesh. The distributor is now disabled; switch Enabled back on to return to generated instances. "
	       "Render flags 0x%llx, view distance ratio %d and LOD ratio %d were NOT carried - a baked brush gets the "
	       "defaults of a hand-placed one.",
	       pObject->GetName().c_str(), instanceCount, static_cast<int>(created.size()), baseName.c_str(), skipped,
	       bHaveFirst ? first.renderFlags : 0ull,
	       bHaveFirst ? first.viewDistRatio : 0,
	       bHaveFirst ? first.lodRatio : 0);

	return true;
}
