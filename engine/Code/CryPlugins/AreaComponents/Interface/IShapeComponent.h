// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! The shape contract of the Area modernisation (scene-notes/area/decisions/01-architecture.md).
//!
//! Exactly one shape component lives on an entity (every shape kind is Singleton and declares
//! pairwise Incompatibility with the other kinds). Function components - Area, water, road,
//! gravity volume, distributor - never know which kind they are talking to; they ask the entity
//! for IShapeComponent and work through this header alone.
//!
//! IShapeComponent is a *reflected interface base*, the pattern
//! IEditorEntityComponent uses in CryCommon/CryEntitySystem/IEntityComponent.h: a plain struct
//! deriving from IEntityComponent with a ReflectType carrying a GUID, which implementations pull
//! in with desc.AddBase<IShapeComponent>(). That is what makes
//! IEntity::QueryComponentByInterfaceID() / GetComponent<IShapeComponent>() find an implementation
//! that lives in another DLL - the lookup is by the reflected type GUID, not by RTTI.
//!
//! Memory rule (MEMORY.md, fix bde12cd8): reflected types must never hold raw pointers. Everything
//! stored across a frame here is an EntityId or a value; listeners are held by the implementation
//! as plain observer pointers that the owner un-registers in OnShutDown, never as reflected state.
//!
//! HEAP RULE - no STL container ever crosses this interface (learned the hard way, 2026-09-11).
//! An engine module (CryEngineModule) overrides global new/delete to CryModuleMalloc, i.e. the
//! engine allocator, while a Sandbox editor plugin is compiled with NOT_USE_CRY_MEMORY_MANAGER
//! (Tools/CMake/CommonMacros.cmake, CryEditorPlugin) and so uses the plain CRT heap. A container
//! that the editor declares and the engine fills is therefore allocated on one heap and freed on
//! the other: silent corruption, and a render thread that dies somewhere else entirely. Every
//! method here passes values, references to caller-owned storage, or caller-owned buffers with an
//! explicit capacity. The convenience wrapper below is an inline free function on purpose - it is
//! compiled into the caller, so the vector it grows and frees never leaves that module.

#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CryMath/Cry_Geo.h>
#include <CryMath/Cry_Math.h>

#include <vector>

namespace Cry
{
namespace AreaComponents
{

//! Which geometric family a shape component belongs to. The first wave is Box, Sphere, Polygon
//! (points + height + closed flag) and Spline (Bezier + closed flag); Solid is parked because
//! nothing outside CryDesigner produces a convex hull (research/07 section 6).
enum class EShapeKind
{
	Box = 0,
	Sphere,
	Polygon,
	Spline,
};

inline void ReflectType(Schematyc::CTypeDesc<EShapeKind>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B01}"_cry_guid);
	desc.SetLabel("Shape Kind");
	desc.SetDescription("Which geometric family a shape component belongs to");
	desc.SetDefaultValue(EShapeKind::Box);
	desc.AddConstant(EShapeKind::Box, "Box", "Box");
	desc.AddConstant(EShapeKind::Sphere, "Sphere", "Sphere");
	desc.AddConstant(EShapeKind::Polygon, "Polygon", "Polygon");
	desc.AddConstant(EShapeKind::Spline, "Spline", "Spline");
}

//! How much of a shape changed, so that listeners can pick the cheapest reaction.
//! Transform  - the entity or the shape's offset moved; the hull is unchanged in local space.
//! Geometry   - a dimension or an existing point moved; point count and order are unchanged.
//! Topology   - points were inserted or removed, or the closed flag flipped; rebuild everything.
enum class EShapeChangeReason
{
	Transform = 0,
	Geometry,
	Topology,
};

inline void ReflectType(Schematyc::CTypeDesc<EShapeChangeReason>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B02}"_cry_guid);
	desc.SetLabel("Shape Change Reason");
	desc.SetDescription("How much of a shape changed, so listeners can pick the cheapest reaction");
	desc.SetDefaultValue(EShapeChangeReason::Transform);
	desc.AddConstant(EShapeChangeReason::Transform, "Transform", "Transform");
	desc.AddConstant(EShapeChangeReason::Geometry, "Geometry", "Geometry");
	desc.AddConstant(EShapeChangeReason::Topology, "Topology", "Topology");
}

//! Told whenever the shape it watches changes. Implemented by function components (Area, water,
//! ...) and by the editor tools. Never stored in reflected state - the listener list is runtime
//! only, and every listener must RemoveListener before it dies.
struct IShapeListener
{
	virtual ~IShapeListener() = default;

	virtual void OnShapeChanged(struct IShapeComponent& shape, EShapeChangeReason reason) = 0;
};

struct ISplineShape;
struct IShapeComponentEdit;

//! Which legacy CArea volume a shape kind maps onto. It is deliberately NOT EShapeKind: several
//! kinds could map onto one volume (a rounded box is still a box to CArea) and a kind may map onto
//! none at all, which is what GetAreaVolume() returning false means.
enum class EAreaVolumeForm
{
	Box = 0,
	Sphere,
	Polygon,
};

//! Everything the Area function component needs in order to build a legacy CArea out of a shape,
//! in the shape's LOCAL space with the component's own offset already applied - i.e. exactly the
//! space IEntityAreaComponent::SetBox / SetSphere / SetPoints expect.
//!
//! It is a flat POD on purpose. No container crosses this interface (heap rule at the top of this
//! header), so the polygon's point list is not in here: the caller reads it with GetContour(false)
//! into its own buffer and its per-edge obstruction flags with GetContourObstruction().
//!
//! Filling it is the KIND's job, which is what keeps function components free of downcasts: the
//! Area component never learns whether it is talking to a box, a sphere or a polygon.
struct SShapeAreaVolume
{
	EAreaVolumeForm form = EAreaVolumeForm::Box;

	//! Box: the two extreme corners in the shape's local space, and per-side sound obstruction in
	//! the order CArea::SetSoundObstructionOnAreaFace() indexes box sides (0..5).
	Vec3  boxMin = ZERO;
	Vec3  boxMax = ZERO;
	bool  boxObstruct[6] = { false, false, false, false, false, false };

	//! Sphere: centre in the shape's local space, radius in metres.
	Vec3  sphereCentre = ZERO;
	float sphereRadius = 0.0f;

	//! Polygon: the scalars beside the contour. Height 0 means "infinite in Z", which is what a
	//! legacy shape with mv_height == 0 means to CArea.
	float height = 0.0f;
	bool  closed = true;
	bool  obstructRoof = false;
	bool  obstructFloor = false;
};

//! The contract every shape kind implements. All queries take and return WORLD space unless the
//! name says otherwise; the implementation owns the local-to-world transform (entity transform
//! composed with the component's own offset), so callers never repeat that maths.
//!
//! It derives from IEditorShapeComponent (CryCommon/CryEntitySystem/IEntityComponent.h) rather
//! than from IEntityComponent directly, for two reasons. First, a shape is exactly the thing that
//! contract describes - geometry the editor should be able to frame, click and edit - so every
//! shape kind owes the editor those four answers anyway. Second, a second IEntityComponent base
//! beside IShapeComponent would make the entity-component base ambiguous and break the
//! static_cast<> that IEntity::GetAllComponents<>() performs on the query result; one single
//! inheritance chain keeps that cast correct.
struct IShapeComponent : public IEditorShapeComponent
{
	static void ReflectType(Schematyc::CTypeDesc<IShapeComponent>& desc)
	{
		desc.SetGUID("{9F1C7A34-5D28-41B6-8E02-7A63C4D9F100}"_cry_guid);
		desc.SetLabel("Shape");
	}

	//! Which family this is, for the few consumers that must special-case (the editor tool picker).
	virtual EShapeKind GetKind() const = 0;

	//! Bounds in the component's own local space (offset already applied), for previewers and
	//! for CEntityObject::GetLocalBounds.
	virtual void       GetLocalAABB(AABB& out) const = 0;
	//! Bounds in world space, for broad-phase culling and viewport framing.
	virtual void       GetWorldAABB(AABB& out) const = 0;

	//! Containment test for a world-space point. Open shapes (an unclosed spline) answer false.
	virtual bool       IsPointInside(const Vec3& world) const = 0;
	//! Distance from a world-space point to the hull. Zero on the hull, negative inside for
	//! closed shapes, positive outside. This is what fade distances are built on.
	virtual float      DistanceToHull(const Vec3& world) const = 0;
	//! Nearest intersection of a world-space ray with the hull. Returns false when there is none;
	//! on success `dist` is the distance from ray.origin along a normalised ray.direction.
	virtual bool       IntersectRay(const Ray& ray, float& dist) const = 0;

	//! A uniformly distributed world-space point inside the shape. Returns false for shapes with
	//! no interior (an open spline). Used by spawners and by the distributor.
	virtual bool       GetRandomPointInside(Vec3& out) const = 0;

	//! The footprint contour: the polygon/box outline, or the sampled spline. `world` selects the
	//! space; the points come out in authoring order and the closing edge is implicit.
	//!
	//! The buffer belongs to the CALLER - see the heap rule at the top of this header. At most
	//! `maxPoints` points are written; the return value is how many points the contour HAS, which
	//! may be larger, so a caller that cannot use a fixed buffer calls it once with
	//! (nullptr, 0) to learn the count and again with a buffer of that size.
	virtual int        GetContour(Vec3* pOutPoints, int maxPoints, bool world) const = 0;

	//! Change notification. AddListener is idempotent; RemoveListener on an unknown listener is a
	//! no-op. The shape holds these as non-owning observers - see the memory rule at the top.
	virtual void       AddListener(IShapeListener* pListener) = 0;
	virtual void       RemoveListener(IShapeListener* pListener) = 0;

	//! The optional halves of the contract. Neither derives from IEntityComponent (that would make
	//! the component base ambiguous), so they are reached through the shape itself: nullptr when
	//! the kind does not offer them (only the spline shape answers GetSpline).
	virtual ISplineShape*        GetSpline()        { return nullptr; }
	virtual IShapeComponentEdit* GetEditInterface() { return nullptr; }

	// --- appended 2026-09-11 with the Area function component (stage 2). Append-only from here
	// on, and never pure: a kind that cannot answer keeps the default and says so by returning
	// false / 0, which is how the Area component knows to skip it with a warning instead of
	// building a broken volume.

	//! Describe this shape as a legacy CArea volume, in the shape's LOCAL space. False means the
	//! kind has no area volume at all (an open spline - a gravity volume is a different function),
	//! and the caller must not build an area from it.
	virtual bool GetAreaVolume(SShapeAreaVolume& out) const { return false; }

	//! Per-EDGE sound obstruction for a polygon-shaped volume, one flag per contour point, for the
	//! wall that STARTS at that point - the same ordering GetContour() uses and the same ordering
	//! CArea::SetSoundObstructionOnAreaFace() indexes shape segments with. Roof and floor are not
	//! in here; they are two scalars on SShapeAreaVolume.
	//!
	//! The buffer belongs to the caller (heap rule at the top of this header). At most `maxFlags`
	//! flags are written; the return value is how many the shape HAS. Kinds without per-edge
	//! obstruction answer 0 and the caller treats every edge as non-obstructing.
	virtual int GetContourObstruction(bool* pOutFlags, int maxFlags) const { return 0; }
};

//! Convenience wrapper around IShapeComponent::GetContour for callers that want a vector. Inline on
//! purpose: it is compiled into the calling module, so the vector is allocated and freed on that
//! module's heap and never crosses the DLL boundary (see the heap rule at the top).
inline int GetShapeContour(const IShapeComponent& shape, std::vector<Vec3>& out, bool world)
{
	out.clear();

	const int pointCount = shape.GetContour(nullptr, 0, world);
	if (pointCount > 0)
	{
		out.resize(static_cast<size_t>(pointCount));
		shape.GetContour(out.data(), pointCount, world);
	}

	return pointCount;
}

//! The spline-only half of the contract. Declared here so consumers (gravity volume, distributor,
//! camera dolly) have one header to include; implemented only by the spline shape, beside
//! IShapeComponent.
//!
//! It deliberately does NOT derive from IEntityComponent and is not reflected: the spline shape
//! already derives from IShapeComponent, and a second IEntityComponent base would make the
//! entity-component base ambiguous. Consumers reach it through IShapeComponent::GetSpline().
struct ISplineShape
{
	virtual ~ISplineShape() = default;

	//! Evaluate at a normalised parameter t in [0,1] along the whole spline, world space.
	virtual Vec3  EvalPos(float t) const = 0;
	virtual Vec3  EvalTangent(float t) const = 0;
	virtual Vec3  EvalNormal(float t) const = 0;
	//! Evaluate by arc length in metres from the start; clamped to [0, TotalLength()].
	virtual Vec3  PosByDistance(float distance) const = 0;
	//! Arc length of the whole spline in metres.
	virtual float TotalLength() const = 0;
	virtual bool  IsClosed() const = 0;

	// --- appended 2026-09-11 with the spline shape (stage 3). Append-only from here on: the
	// editor plugin and future function components see this header as a fixed ABI.

	//! The per-point width at `t`, blended along the segment exactly as CRoadObject::GetLocalWidth
	//! does (RoadObject.cpp:125-151). A point flagged "default width" contributes `defaultWidth`,
	//! which is the consumer's own global width - a road's mv_width, a distributor's scale base.
	virtual float GetLocalWidth(float t, float defaultWidth) const = 0;

	//! The roll-aware normal at `t` in the SHAPE's LOCAL space, honouring the per-point `Angle`
	//! (CSplineObject::GetLocalBezierNormal, SplineObject.cpp:841-885). This is the vector road and
	//! distributor build their instance frames from; EvalNormal() is the same vector in world space.
	virtual Vec3  GetLocalBezierNormal(float t) const = 0;

	//! How the normalised parameter maps onto the point list: segment `indexOut` of the curve and
	//! the local parameter within it. Segment i runs from point i to point i+1, and a closed spline
	//! has one more segment than an open one (the wrap from the last point back to the first).
	virtual void  ParamToSegment(float t, int& indexOut, float& segmentTOut) const = 0;
};

//! The authoring half of the contract, used by the editor plugin's point tools and by nothing
//! else at runtime. Kept separate from IShapeComponent so that a shape kind with no editable
//! points (Box, Sphere edit their dimensions, not a point list) can still opt in with a small
//! fixed point set, and so that game code cannot reach the mutators by accident.
//!
//! BeginEdit / EndEdit bracket one user gesture: the shape suppresses per-point listener
//! notification between them and fires exactly one OnShapeChanged with the coarsest reason seen.
//! All points are in the shape's LOCAL space - the tool does the world conversion it already
//! needs for its manipulator.
//!
//! Like ISplineShape it does not derive from IEntityComponent and is not reflected (decided
//! 2026-09-11: a second IEntityComponent base would be ambiguous); the editor reaches it through
//! IShapeComponent::GetEditInterface(). The editor plugin never links the runtime plugin, so this
//! header is the whole ABI between them: append new virtuals at the end only.
struct IShapeComponentEdit
{
	virtual ~IShapeComponentEdit() = default;

	virtual int  GetPointCount() const = 0;
	virtual Vec3 GetPoint(int index) const = 0;
	virtual void SetPoint(int index, const Vec3& local) = 0;
	//! Insert before `index`; index == GetPointCount() appends. Returns the index actually used.
	virtual int  InsertPoint(int index, const Vec3& local) = 0;
	virtual void RemovePoint(int index) = 0;

	virtual void BeginEdit() = 0;
	virtual void EndEdit() = 0;

	//! Whether the contour the points describe closes back on itself. The point tool needs it to
	//! know which edges exist: it is what tells "insert a point on the edge under the cursor" that
	//! the edge between the last point and the first is real (a closed polygon) or not (an open
	//! polyline). Kinds with a fixed point set answer for their drawn footprint - the box's
	//! footprint is a closed quad - so that the same flag also drives the hull the tool draws.
	virtual bool IsContourClosed() const = 0;

	// --- appended 2026-09-11 with the spline shape (stage 3).

	//! The shape of the edge that STARTS at point `index`, sampled into a caller-owned buffer in
	//! the shape's LOCAL space, first and last sample being the two end points of the edge.
	//!
	//! Returning 0 - what every kind whose edges are straight answers, and therefore the default -
	//! means "this edge is the plain segment between GetPoint(index) and its successor", and the
	//! tool uses the chord. A spline answers with its sampled curve instead, which is what makes
	//! Ctrl+click insert a point ON the curve rather than on the chord under it, and what lets the
	//! tool highlight the real segment the cursor is over.
	//!
	//! The buffer belongs to the caller (heap rule at the top of this header); at most `maxPoints`
	//! samples are written and the return value is how many were written.
	virtual int GetEdgePoints(int index, Vec3* pOut, int maxPoints) const { return 0; }
};

} // namespace AreaComponents
} // namespace Cry
