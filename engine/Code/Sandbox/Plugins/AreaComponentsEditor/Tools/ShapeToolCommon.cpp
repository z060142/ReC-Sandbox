// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeToolCommon.h"

#include <IEditor.h>
#include <IObjectManager.h>
#include <Objects/BaseObject.h>

#include <CryEntitySystem/IEntity.h>
#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/IObject.h>
#include <CrySchematyc/IObjectProperties.h>
#include <CrySchematyc/Utils/ClassProperties.h>
#include <CrySchematyc/Reflection/TypeDesc.h>

#include <IShapeComponent.h>

using Cry::AreaComponents::IShapeComponent;

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

} // namespace AreaShapeTools
