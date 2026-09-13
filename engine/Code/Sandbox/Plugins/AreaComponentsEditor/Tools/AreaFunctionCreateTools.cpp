// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "ShapeCreateTool.h"
#include "SplineCreateTool.h"

#include <IEditorClassFactory.h>

//! The two Create presets stage 4 adds: "Create Object -> Area -> Water" and
//! "Create Object -> Area -> Road". Neither is a new shape kind - water is a polygon with the Water
//! Volume function on it and a road is a spline with the Road function on it - so both are the
//! existing draw tools with one more component added at the end of the gesture, which is exactly
//! what the Box / Sphere / Polygon presets already do with the Area function.
//!
//! They exist because that is where the user looks: the legacy Create panel had
//! Area -> Water Volume and Misc -> Road, and an author who wants water should not have to know
//! that it is "a polygon plus a component".

namespace
{
//! CWaterVolumeComponent::ReflectType, CryPlugins/AreaComponents/Module/Functions/
//! WaterVolumeComponent.h. Written the way CryGUID::ToString() writes it, because that is the form
//! CEntityObjectWithComponent::Init() parses back out of the creation file string.
const char* const szWaterVolumeComponentGuid = "C7B41E58-9D06-4A72-83F5-1B64E29D7C08";

//! CRoadComponent::ReflectType, Module/Functions/RoadComponent.h.
const char* const szRoadComponentGuid = "1E9A63D4-4B57-42C8-9106-7F3D5AC81B62";
}

//! "Create Object -> Area -> Water": the polygon draw gesture, with the Water Volume function
//! beside the shape when it finishes. Four points, not three: CreateArea will take three, but
//! SetAreaPhysicsArea wants more than three (WaterVolumeRenderNode.cpp:440) and so does the legacy
//! object (WaterShapeObject.cpp:288), so a three-point water volume would render and never float
//! anything.
class CAreaWaterCreateTool : public CShapeCreateTool
{
	DECLARE_DYNCREATE(CAreaWaterCreateTool)

public:
	CAreaWaterCreateTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Create Water Volume"; }
	// ~CEditTool

protected:
	virtual ~CAreaWaterCreateTool() = default;

	// CShapeCreateTool
	virtual const char* GetObjectClassName() const override        { return "AreaWater"; }
	virtual const char* GetFunctionComponentGuid() const override  { return szWaterVolumeComponentGuid; }
	virtual int         GetMinPointCount() const override          { return 4; }
	// ~CShapeCreateTool
};

class CAreaWaterCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaWaterCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CAreaWaterCreateTool); }
};

REGISTER_CLASS_DESC(CAreaWaterCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CAreaWaterCreateTool, CShapeCreateTool);

//! "Create Object -> Area -> Road": the spline draw gesture, with the Road function beside the
//! shape when it finishes. Two points are a road, as they are a spline.
class CAreaRoadCreateTool : public CSplineCreateTool
{
	DECLARE_DYNCREATE(CAreaRoadCreateTool)

public:
	CAreaRoadCreateTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Create Road"; }
	// ~CEditTool

protected:
	virtual ~CAreaRoadCreateTool() = default;

	// CShapeCreateTool
	virtual const char* GetObjectClassName() const override       { return "AreaRoad"; }
	virtual const char* GetFunctionComponentGuid() const override { return szRoadComponentGuid; }
	// ~CShapeCreateTool
};

class CAreaRoadCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaRoadCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CAreaRoadCreateTool); }
};

REGISTER_CLASS_DESC(CAreaRoadCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CAreaRoadCreateTool, CSplineCreateTool);
