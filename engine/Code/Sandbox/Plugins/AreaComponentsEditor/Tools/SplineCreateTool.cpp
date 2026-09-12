// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "SplineCreateTool.h"

#include <IEditorClassFactory.h>

class CSplineCreateTool_ClassDesc : public IClassDesc
{
	virtual ESystemClassID SystemClassID() override   { return ESYSTEM_CLASS_EDITTOOL; }
	virtual const char*    ClassName() override       { return "EditTool.AreaShapeSplineCreate"; }
	virtual const char*    Category() override        { return "Area"; }
	virtual CRuntimeClass* GetRuntimeClass() override { return RUNTIME_CLASS(CSplineCreateTool); }
};

REGISTER_CLASS_DESC(CSplineCreateTool_ClassDesc);

IMPLEMENT_DYNCREATE(CSplineCreateTool, CShapeCreateTool);
