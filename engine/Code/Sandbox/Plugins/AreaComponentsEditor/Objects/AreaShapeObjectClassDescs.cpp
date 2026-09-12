// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"

#include <IEditor.h>
#include <IEditorClassFactory.h>
#include <IObjectEnumerator.h>
#include <Objects/ClassDesc.h>

//! The Create-panel entries of the Area modernisation (decision 02 section 5). They live in this
//! editor plugin, not in EditorQt: a plugin CObjectClassDesc is registered through the deferred
//! REGISTER_CLASS_DESC helper, which is exactly what CryDesigner does for its own object classes
//! (Objects/DesignerObject.cpp:28-35).
//!
//! Creation reuses EditorQt's CEntityObjectWithComponent machinery instead of a new object class:
//! that class already knows how to spawn a plain "Entity" and create one component on it from a
//! GUID passed as the creation "file" string (EntityObjectWithComponent.cpp:83-113). Borrowing its
//! runtime class through the editor's class factory keeps this DLL free of any link dependency on
//! Sandbox, which is what lets it be deployed on its own.
namespace Private_AreaShapeObjectClassDescs
{

//! CBoxShapeComponent::ReflectType, CryPlugins/AreaComponents/Module/Shapes/BoxShapeComponent.h.
//! Written the way CryGUID::ToString() writes it, because that is the form
//! CEntityObjectWithComponent::Init() parses back out of the creation file string.
const char* const szBoxShapeComponentGuid = "6C3A81D5-0F47-4E9A-B4D1-72E5C8A96201";

//! CPolygonShapeComponent::ReflectType, Module/Shapes/PolygonShapeComponent.h.
const char* const szPolygonShapeComponentGuid = "6C3A81D5-0F47-4E9A-B4D1-72E5C8A96202";

//! CSphereShapeComponent::ReflectType, Module/Shapes/SphereShapeComponent.h.
const char* const szSphereShapeComponentGuid = "6C3A81D5-0F47-4E9A-B4D1-72E5C8A96203";

//! CSplineShapeComponent::ReflectType, Module/Shapes/SplineShapeComponent.h.
const char* const szSplineShapeComponentGuid = "6C3A81D5-0F47-4E9A-B4D1-72E5C8A96204";

//! The object class every Area preset is created as. Resolved lazily: EditorQt registers its own
//! class descs before any plugin is loaded, but not necessarily before this plugin's statics run.
CRuntimeClass* GetEntityWithComponentRuntimeClass()
{
	static CRuntimeClass* pRuntimeClass = nullptr;
	if (pRuntimeClass == nullptr)
	{
		if (IClassDesc* pClassDesc = GetIEditor()->GetClassFactory()->FindClass("EntityWithComponent"))
		{
			pRuntimeClass = pClassDesc->GetRuntimeClass();
		}
	}
	return pRuntimeClass;
}

} // namespace Private_AreaShapeObjectClassDescs

//! "Create Object -> Area -> Box": an entity carrying a CBoxShapeComponent, dragged out of the
//! ground instead of dropped at a default size (stage 1c). The custom tool class name is what
//! makes the Create panel start CShapeBoxCreateTool instead of the generic CObjectCreateTool
//! (ObjectCreateToolPanel.cpp:383-402).
class CAreaShapeBoxClassDesc : public CObjectClassDesc
{
public:
	virtual ObjectType     GetObjectType() override           { return OBJTYPE_ENTITY; }
	virtual const char*    ClassName() override               { return "AreaShapeBox"; }
	virtual const char*    UIName() override                  { return "Box"; }
	virtual const char*    Category() override                { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override         { return Private_AreaShapeObjectClassDescs::GetEntityWithComponentRuntimeClass(); }
	virtual const char*    GetFileSpec() override             { return Private_AreaShapeObjectClassDescs::szBoxShapeComponentGuid; }
	virtual const char*    GetDataFilesFilterString() override { return ""; }
	virtual const char*    GetToolClassName() override        { return "EditTool.AreaShapeBoxCreate"; }

	//! The entry is a single button, not a browsable list, so the Create panel asks
	//! EnumerateObjects() for the buttons it should show.
	virtual bool IsCreatedByListEnumeration() override { return false; }

	virtual void EnumerateObjects(IObjectEnumerator* pEnumerator) override
	{
		pEnumerator->AddEntry("Box", Private_AreaShapeObjectClassDescs::szBoxShapeComponentGuid);
	}
};

REGISTER_CLASS_DESC(CAreaShapeBoxClassDesc);

//! "Create Object -> Area -> Polygon": the same entity carrying a CPolygonShapeComponent, drawn
//! click-click-double-click instead of placed with a single click. The custom tool class name is
//! what makes the Create panel start CShapeCreateTool instead of the generic CObjectCreateTool
//! (ObjectCreateToolPanel.cpp:383-402), the route CryDesigner's AreaSolid entry takes.
class CAreaShapePolygonClassDesc : public CObjectClassDesc
{
public:
	virtual ObjectType     GetObjectType() override            { return OBJTYPE_SHAPE; }
	virtual const char*    ClassName() override                { return "AreaShapePolygon"; }
	virtual const char*    UIName() override                   { return "Polygon"; }
	virtual const char*    Category() override                 { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override          { return Private_AreaShapeObjectClassDescs::GetEntityWithComponentRuntimeClass(); }
	virtual const char*    GetFileSpec() override              { return Private_AreaShapeObjectClassDescs::szPolygonShapeComponentGuid; }
	virtual const char*    GetDataFilesFilterString() override { return ""; }
	virtual const char*    GetToolClassName() override         { return "EditTool.AreaPolygonCreate"; }

	virtual bool IsCreatedByListEnumeration() override { return false; }

	virtual void EnumerateObjects(IObjectEnumerator* pEnumerator) override
	{
		pEnumerator->AddEntry("Polygon", Private_AreaShapeObjectClassDescs::szPolygonShapeComponentGuid);
	}
};

REGISTER_CLASS_DESC(CAreaShapePolygonClassDesc);

//! "Create Object -> Area -> Sphere": an entity carrying a CSphereShapeComponent, dragged out of
//! the ground - the press picks the centre, the drag distance is the radius.
class CAreaShapeSphereClassDesc : public CObjectClassDesc
{
public:
	virtual ObjectType     GetObjectType() override            { return OBJTYPE_ENTITY; }
	virtual const char*    ClassName() override                { return "AreaShapeSphere"; }
	virtual const char*    UIName() override                   { return "Sphere"; }
	virtual const char*    Category() override                 { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override          { return Private_AreaShapeObjectClassDescs::GetEntityWithComponentRuntimeClass(); }
	virtual const char*    GetFileSpec() override              { return Private_AreaShapeObjectClassDescs::szSphereShapeComponentGuid; }
	virtual const char*    GetDataFilesFilterString() override { return ""; }
	virtual const char*    GetToolClassName() override         { return "EditTool.AreaShapeSphereCreate"; }

	virtual bool IsCreatedByListEnumeration() override { return false; }

	virtual void EnumerateObjects(IObjectEnumerator* pEnumerator) override
	{
		pEnumerator->AddEntry("Sphere", Private_AreaShapeObjectClassDescs::szSphereShapeComponentGuid);
	}
};

REGISTER_CLASS_DESC(CAreaShapeSphereClassDesc);

//! "Create Object -> Area -> Spline": an entity carrying a CSplineShapeComponent, drawn
//! click-click-double-click like the polygon - the same gesture CSplineObject::MouseCreateCallback
//! has (SplineObject.cpp:1591-1647) - except that two points are already a spline.
class CAreaShapeSplineClassDesc : public CObjectClassDesc
{
public:
	virtual ObjectType     GetObjectType() override            { return OBJTYPE_SHAPE; }
	virtual const char*    ClassName() override                { return "AreaShapeSpline"; }
	virtual const char*    UIName() override                   { return "Spline"; }
	virtual const char*    Category() override                 { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override          { return Private_AreaShapeObjectClassDescs::GetEntityWithComponentRuntimeClass(); }
	virtual const char*    GetFileSpec() override              { return Private_AreaShapeObjectClassDescs::szSplineShapeComponentGuid; }
	virtual const char*    GetDataFilesFilterString() override { return ""; }
	virtual const char*    GetToolClassName() override         { return "EditTool.AreaShapeSplineCreate"; }

	virtual bool IsCreatedByListEnumeration() override { return false; }

	virtual void EnumerateObjects(IObjectEnumerator* pEnumerator) override
	{
		pEnumerator->AddEntry("Spline", Private_AreaShapeObjectClassDescs::szSplineShapeComponentGuid);
	}
};

REGISTER_CLASS_DESC(CAreaShapeSplineClassDesc);
