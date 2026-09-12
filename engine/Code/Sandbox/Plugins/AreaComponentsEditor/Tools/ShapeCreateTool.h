// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <LevelEditor/Tools/EditTool.h>

#include <CryExtension/CryGUID.h>

class CBaseObject;
struct IEntityComponent;

namespace Cry
{
namespace AreaComponents
{
struct IShapeComponentEdit;
}
}

//! "Draw a new shape": the click-click-double-click creation of EditorQt's shape objects
//! (CShapeObject::MouseCreateCallback, ShapeObject.cpp:1155-1237), as an edit tool of this plugin.
//!
//! Registered as "EditTool.AreaShapeCreate", which is what the "Create Object -> Area -> Polygon"
//! class desc returns from GetToolClassName(); the Create panel then starts this tool instead of
//! the generic CObjectCreateTool (ObjectCreateToolPanel.cpp:383-402), exactly as CryDesigner's
//! AreaSolid and ClipVolume entries do.
//!
//! The first click spawns the entity with its polygon shape component at the point under the
//! cursor and makes that the shape's first point; every further click appends a point; a
//! double-click or Enter finishes (the polygon needs at least three points, or it is discarded);
//! Esc cancels and deletes the entity again. The whole creation is one undo step.
class CShapeCreateTool : public CEditTool
{
	DECLARE_DYNCREATE(CShapeCreateTool)

public:
	CShapeCreateTool();

	// CEditTool
	virtual string GetDisplayName() const override { return "Create Shape"; }
	virtual void   Display(SDisplayContext& dc) override;
	virtual bool   MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags) override;
	virtual bool   OnKeyDown(CViewport* pView, uint32 nChar, uint32 nRepCnt, uint32 nFlags) override;
	virtual bool   IsNeedMoveTool() override { return false; }
	// ~CEditTool

protected:
	virtual ~CShapeCreateTool();
	virtual void DeleteThis() override { delete this; }

	//! The object class the tool creates. A subclass changes this to serve another shape kind.
	virtual const char* GetObjectClassName() const { return "AreaShapePolygon"; }

	//! A FUNCTION component the tool adds beside the shape, as a GUID string in the form
	//! CryGUID::ToString() writes. Null (the default) adds nothing, which is what a preset that
	//! means "a bare shape" wants - and what the spline preset wants, because a spline is a path
	//! and has no area volume.
	//!
	//! The Create -> Area -> Box / Sphere / Polygon presets return the Area function component
	//! here, because "an area" is what the user asked for when they picked that menu entry; a
	//! pure shape is still one Add Component away.
	virtual const char* GetFunctionComponentGuid() const { return nullptr; }

	//! Fewest points the finished shape must have, or the draw counts as cancelled. A kind with a
	//! fixed point set (box, sphere) has nothing to count and answers 0.
	virtual int  GetMinPointCount() const { return 3; }

	//! Called right after the entity is spawned, to give the shape its first point. The polygon
	//! starts at the object's own origin, exactly as legacy does
	//! (CShapeObject::MouseCreateCallback, ShapeObject.cpp:1185-1188); a fixed-point kind
	//! overrides this and sizes itself from the drag instead.
	virtual void SeedShape();

	CBaseObject*                              ResolveObject() const;
	IEntityComponent*                         ResolveComponent() const;
	Cry::AreaComponents::IShapeComponentEdit* ResolveShapeEdit() const;

	//! Where the cursor is in the world, picked against terrain and geometry the way legacy picks
	//! the points of a shape it is drawing.
	bool     PickWorldPoint(CViewport* pView, CPoint& point, Vec3& worldPos) const;

	//! Spawns the entity at `worldPos` and makes that its first point. False when the object
	//! could not be created.
	bool     StartCreation(const Vec3& worldPos);
	void     AppendPoint(const Vec3& worldPos);
	//! Ends the creation: keeps the entity and selects it, or deletes it when it has too few
	//! points. Leaves the tool either way.
	void     FinishCreation();
	void     CancelCreation();

	CryGUID m_objectGuid = CryGUID::Null();
	CryGUID m_componentGuid = CryGUID::Null();

	bool m_bCreating = false;
	//! Cursor position of the last mouse move, drawn as the rubber-band edge of the polygon.
	bool m_bCursorValid = false;
	Vec3 m_cursorWorldPos = ZERO;
};
