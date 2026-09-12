// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "ShapeCreateTool.h"

//! "Drag a new shape out of the ground": the press-drag-release creation of the shape kinds that
//! have no point list - Box and Sphere.
//!
//! Why a tool and not CBaseObject::MouseCreateCallback, which is how legacy objects size
//! themselves while being placed: that virtual belongs to the OBJECT class, and these entities are
//! created as EditorQt's own CEntityObjectWithComponent (the class desc carries the component GUID
//! as its file spec, see Objects/AreaShapeObjectClassDescs.cpp). Subclassing that object class to
//! get the virtual would mean the shape can only ever exist on an entity created as that class,
//! which is exactly what decision 02 rejects (alternative B4). Driving the same gesture from a
//! creation tool keeps the object class stock and the component addable to any entity.
//!
//! The gesture, shared by both kinds:
//!   mouse down  - spawn the entity at the picked point; that point is the anchor
//!   drag        - size the shape from anchor to cursor, live, inside the same undo transaction
//!   mouse up    - finish, select the entity, leave the tool
//!   Esc         - delete the entity again and leave the tool
//! A click with no drag leaves the kind's default size rather than a degenerate shape.
//!
//! Sizing goes through IShapeComponentEdit::SetPoint alone, because both kinds already present
//! their dimensions as a two-point set for the point tool: box = (min corner, max corner),
//! sphere = (centre, radius handle). So the drag reuses exactly the authoring path the "Edit
//! Shape" tool uses, and there is no second way to resize a shape that could disagree with it.
class CShapeDragCreateTool : public CShapeCreateTool
{
	// DYNAMIC, not DYNCREATE: this class is never instantiated by name - only the two concrete
	// kinds below are registered as edit tools - but MFC needs a runtime class here so that
	// IMPLEMENT_DYNCREATE on them has a base to point at.
	DECLARE_DYNAMIC(CShapeDragCreateTool)

public:
	CShapeDragCreateTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Create Shape"; }
	virtual void   Display(SDisplayContext& dc) override;
	virtual bool   MouseCallback(CViewport* pView, EMouseEvent event, CPoint& point, int flags) override;
	// ~CEditTool

protected:
	virtual ~CShapeDragCreateTool() = default;

	// CShapeCreateTool - a fixed point set has nothing to count and nothing to seed.
	virtual int  GetMinPointCount() const override { return 0; }
	virtual void SeedShape() override               {}
	// ~CShapeCreateTool

	//! Size the shape so that it spans from the anchor to `worldPos`. Called on every drag frame
	//! and once at the end; `bDefaultSize` asks for the kind's default instead, which is what a
	//! click with no drag gets.
	virtual void SizeToDrag(const Vec3& anchorWorld, const Vec3& worldPos, bool bDefaultSize) = 0;

	//! Shape-local space of the entity being created. Identity when it cannot be resolved.
	Matrix34 GetShapeWorldTM() const;

	//! Smallest drag, in metres, that counts as a drag rather than a click.
	static const float kMinDragDistance;

	Vec3 m_anchorWorld = ZERO;
	bool m_bDragging = false;
};

//! Create Object -> Area -> Box: the drag rectangle is the box's XY footprint, and the box is
//! extruded upward from the drag plane by a default height.
class CShapeBoxCreateTool : public CShapeDragCreateTool
{
	DECLARE_DYNCREATE(CShapeBoxCreateTool)

public:
	CShapeBoxCreateTool() = default;

protected:
	virtual ~CShapeBoxCreateTool() = default;

	virtual const char* GetObjectClassName() const override        { return "AreaShapeBox"; }
	//! CAreaFunctionComponent - Create -> Area -> Box makes an AREA, not a bare box.
	virtual const char* GetFunctionComponentGuid() const override  { return "B4E27C09-8A16-4D53-9C70-1F5D3E8A4620"; }
	virtual void        SizeToDrag(const Vec3& anchorWorld, const Vec3& worldPos, bool bDefaultSize) override;

private:
	//! How tall a dragged box is. A drag gives two dimensions and a volume needs three; a second,
	//! vertical drag would need a second mouse gesture after the button is already up, which is
	//! not what "drag to size" means to anyone. The box is created sitting ON the drag plane
	//! (0 .. height in local Z) rather than centred on it, because an area box is almost always
	//! meant to cover the ground the user dragged over. Height is then edited like any dimension.
	static const float kDefaultHeight;
	//! Half-extent of the box a click with no drag produces.
	static const float kDefaultHalfSize;
};

//! Create Object -> Area -> Sphere: the drag distance is the radius.
class CShapeSphereCreateTool : public CShapeDragCreateTool
{
	DECLARE_DYNCREATE(CShapeSphereCreateTool)

public:
	CShapeSphereCreateTool() = default;

protected:
	virtual ~CShapeSphereCreateTool() = default;

	virtual const char* GetObjectClassName() const override        { return "AreaShapeSphere"; }
	//! CAreaFunctionComponent - Create -> Area -> Sphere makes an AREA, not a bare sphere.
	virtual const char* GetFunctionComponentGuid() const override  { return "B4E27C09-8A16-4D53-9C70-1F5D3E8A4620"; }
	virtual void        SizeToDrag(const Vec3& anchorWorld, const Vec3& worldPos, bool bDefaultSize) override;

private:
	//! Radius a click with no drag produces - the component's own default.
	static const float kDefaultRadius;
};
