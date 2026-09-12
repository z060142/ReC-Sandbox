// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"
#include "ShapeKinds.h"
#include "ShapeEditorActions.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CrySerialization/IArchive.h>

#include <vector>

namespace Cry
{
namespace AreaComponents
{

//! One authored point of a polygon shape, in the component's local space.
//!
//! Per-face sound obstruction lives HERE, on the point, and not on a parallel array beside it
//! (decision 03, "Per-face obstruction"): legacy keeps a std::vector<bool> of numPoints + 2
//! entries next to its point list (ShapeObject.h:246) and has to patch both lists in step on
//! every insert and remove (ShapeObject.cpp:2096-2112). One point, one flag, and a topology edit
//! can no longer desync them. The flag belongs to the edge that STARTS at this point, matching
//! legacy's "Side:%d" ordering; roof and floor are two separate members on the component.
struct SPolygonPoint
{
	static void ReflectType(Schematyc::CTypeDesc<SPolygonPoint>& desc)
	{
		desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B10}"_cry_guid);
		desc.SetLabel("Polygon Point");
	}

	//! The element type of a reflected std::vector is serialized through this member function,
	//! the pattern CAINavigationMarkupShapeComponent::SMarkupShapeProperties uses
	//! (NavigationMarkupShapeComponent.cpp:13-40).
	void Serialize(Serialization::IArchive& archive)
	{
		archive(pos, "pos", "Position");
		archive.doc("Position of the point in the shape's local space.");

		archive(obstructSound, "obstructSound", "Obstruct Sound");
		archive.doc("Whether the wall that starts at this point obstructs sound.");
	}

	bool operator==(const SPolygonPoint& other) const
	{
		return pos == other.pos && obstructSound == other.obstructSound;
	}

	bool operator!=(const SPolygonPoint& other) const { return !(*this == other); }

	Vec3 pos = ZERO;
	bool obstructSound = false;
};

//! The reflected wrapper a std::vector member needs: a GUID, an operator== (the game-mode
//! property round trip compares whole members) and a free Serialize. research/04 section 5.3.
struct SPolygonPoints
{
	static void ReflectType(Schematyc::CTypeDesc<SPolygonPoints>& desc)
	{
		desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B11}"_cry_guid);
		desc.SetLabel("Polygon Points");
	}

	bool operator==(const SPolygonPoints& other) const { return points == other.points; }
	bool operator!=(const SPolygonPoints& other) const { return !(*this == other); }

	std::vector<SPolygonPoint> points;
};

inline bool Serialize(Serialization::IArchive& archive, SPolygonPoints& value, const char* szName, const char* szLabel)
{
	return archive(value.points, szName, szLabel);
}

//! A polygon prism: a point list on the XY plane of the component's local space, extruded upward
//! by Height. Height 0 means "infinite in Z", exactly what a legacy shape with mv_height == 0
//! means to CArea (Area.cpp:442-444 only applies the Z band when m_height > 0). The floor of the
//! band is the lowest point's Z, again as legacy (Area.cpp:2298-2302).
//!
//! This is the component form of EditorQt's CShapeObject, minus everything that was area
//! semantics: id, group, priority, fade and links belong to the Area function component
//! (decision 03), and this component is pure geometry.
class CPolygonShapeComponent final
	: public IShapeComponent
	, public IShapeComponentEdit
#ifndef RELEASE
	, public IEntityComponentPreviewer
#endif
{
public:
	CPolygonShapeComponent() = default;
	virtual ~CPolygonShapeComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CPolygonShapeComponent>& desc)
	{
		desc.SetGUID("{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96202}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Shape: Polygon");
		desc.SetDescription("A polygon shape, optionally extruded in Z. Area functions on the same entity use it as their volume.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		// The reflected interface bases - what makes a cross-DLL GetComponent<IShapeComponent>()
		// and EditorQt's GetAllComponents<IEditorShapeComponent>() find us (IShapeComponent.h).
		desc.AddBase<IShapeComponent>();
		desc.AddBase<IEditorShapeComponent>();

		// One shape per entity (decision 01). One call, every other kind, both directions -
		// see ShapeKinds.h for why this is a list and not twelve hand-written declarations.
		DeclareShapeKindIncompatibilities(desc);

		desc.AddMember(&CPolygonShapeComponent::m_height, 'hght', "Height", "Height", "Extrusion of the polygon in metres. 0 means the shape is infinite in Z.", 0.0f);
		desc.AddMember(&CPolygonShapeComponent::m_closed, 'clsd', "Closed", "Closed", "Whether the last point connects back to the first. An open polygon has no interior.", true);
		desc.AddMember(&CPolygonShapeComponent::m_obstructRoof, 'roof', "ObstructRoof", "Obstruct Roof", "Whether the top cap obstructs sound", false);
		desc.AddMember(&CPolygonShapeComponent::m_obstructFloor, 'flor', "ObstructFloor", "Obstruct Floor", "Whether the bottom cap obstructs sound", false);
		desc.AddMember(&CPolygonShapeComponent::m_offset, 'offs', "Offset", "Offset", "Translation of the whole polygon in the entity's local space", Vec3(0.0f, 0.0f, 0.0f));
		desc.AddMember(&CPolygonShapeComponent::m_points, 'pnts', "Points", "Points", "The polygon's points, in the shape's local space", SPolygonPoints());
	}

	// IShapeComponent
	virtual EShapeKind GetKind() const override { return EShapeKind::Polygon; }

	virtual void  GetLocalAABB(AABB& out) const override;
	virtual void  GetWorldAABB(AABB& out) const override;
	virtual bool  IsPointInside(const Vec3& world) const override;
	virtual float DistanceToHull(const Vec3& world) const override;
	virtual bool  IntersectRay(const Ray& ray, float& dist) const override;
	virtual bool  GetRandomPointInside(Vec3& out) const override;
	virtual int   GetContour(Vec3* pOutPoints, int maxPoints, bool world) const override;
	virtual void  AddListener(IShapeListener* pListener) override;
	virtual void  RemoveListener(IShapeListener* pListener) override;

	virtual IShapeComponentEdit* GetEditInterface() override { return this; }
	virtual bool                 GetAreaVolume(SShapeAreaVolume& out) const override;
	virtual int                  GetContourObstruction(bool* pOutFlags, int maxFlags) const override;
	// ~IShapeComponent

	// IEditorShapeComponent
	virtual bool        GetEditorLocalBounds(AABB& out) const override;
	virtual bool        EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const override;
	virtual const char* GetEditToolClassName() const override { return ShapeEditorActions::GetEditToolClassName(); }
	virtual const char* GetEditToolLabel() const override     { return ShapeEditorActions::GetEditToolLabel(); }

	//! A polygon has an authored point list, so it offers the pivot action beside the point tool
	//! (ShapeEditorActions.h) - the same two buttons the spline shows, from the same place.
	virtual int  GetEditorActionCount() const override { return ShapeEditorActions::GetPointShapeActionCount(); }
	virtual bool GetEditorAction(int index, SEditorActionDesc& out) const override { return ShapeEditorActions::GetPointShapeAction(index, out); }
	// ~IEditorShapeComponent

	// IShapeComponentEdit - the real thing for this kind: a point list the tool edits directly.
	virtual int  GetPointCount() const override;
	virtual Vec3 GetPoint(int index) const override;
	virtual void SetPoint(int index, const Vec3& local) override;
	virtual int  InsertPoint(int index, const Vec3& local) override;
	virtual void RemovePoint(int index) override;
	virtual void BeginEdit() override;
	virtual void EndEdit() override;
	virtual bool IsContourClosed() const override { return m_closed; }
	// ~IShapeComponentEdit

	//! Per-face obstruction, used by the Area function component and by the drawing below. Not
	//! part of IShapeComponentEdit: the point tool needs no access to it, and appending to that
	//! ABI is reserved for things every shape kind can answer.
	bool IsEdgeObstructing(int index) const;
	bool IsRoofObstructing() const  { return m_obstructRoof; }
	bool IsFloorObstructing() const { return m_obstructFloor; }

protected:
	// IEntityComponent
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void                    OnShutDown() override;
	// ~IEntityComponent

#ifndef RELEASE
	// IEntityComponentPreviewer
	virtual IEntityComponentPreviewer* GetPreviewer() override { return this; }

	virtual void SerializeProperties(Serialization::IArchive& archive) override {}
	virtual void Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const override;
	// ~IEntityComponentPreviewer
#endif

private:
	void     NotifyListeners(EShapeChangeReason reason);
	//! Raises the reason of the currently open gesture to the coarsest one seen so far.
	void     RecordChange(EShapeChangeReason reason);

	//! World transform of the shape's own space: the component's world transform with the offset.
	Matrix34 GetShapeWorldTM() const;
	//! Lowest Z of the point list, the floor of the height band (Area.cpp:2298-2302).
	float    GetOriginZ() const;
	//! Whether the shape has an interior at all: closed and at least a triangle.
	bool     HasInterior() const;

	//! Fills a caller-owned buffer with the points in shape space (offset applied) or in world
	//! space. Returns how many were written, never more than maxPoints.
	int      GatherPoints(Vec3* pOut, int maxPoints, bool world) const;

	//! Smallest number of points a closed polygon may be reduced to.
	static constexpr int kMinClosedPoints = 3;
	//! Smallest number of points an open polyline may be reduced to.
	static constexpr int kMinOpenPoints = 2;
	//! Largest point count any one call will process on the stack.
	static constexpr int kMaxStackPoints = 1024;

	SPolygonPoints m_points;
	float          m_height = 0.0f;
	bool           m_closed = true;
	bool           m_obstructRoof = false;
	bool           m_obstructFloor = false;
	Vec3           m_offset = ZERO;

	//! Set between BeginEdit() and EndEdit(): listeners hear one notification per gesture.
	bool               m_bInEdit = false;
	bool               m_bChangedDuringEdit = false;
	EShapeChangeReason m_editReason = EShapeChangeReason::Geometry;

	//! Point count as the last notification saw it, so that an inspector edit can tell a moved
	//! point (Geometry) from an added or removed one (Topology).
	mutable int m_lastNotifiedPointCount = 0;

	//! Runtime only, never reflected: non-owning observers that un-register themselves.
	std::vector<IShapeListener*> m_listeners;
};

} // namespace AreaComponents
} // namespace Cry
