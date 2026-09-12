// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"
#include "ShapeKinds.h"
#include "ShapeEditorActions.h"
#include "SplineMath.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CrySerialization/IArchive.h>
#include <CrySerialization/Decorators/ActionButton.h>
#include <CrySchematyc/ResourceTypes.h>

#include <functional>
#include <vector>

namespace Cry
{
namespace AreaComponents
{

//! How the spline builds the normal that orients whatever rides on it - a road's banking, a
//! distributed mesh's rotation.
enum class ESplineFrameMode
{
	//! CSplineObject's rule: Vec3(0,0,1).Cross(tangent), recomputed at every point. Identical to
	//! legacy, and the default, so existing content is untouched. It degenerates wherever the
	//! tangent approaches world up - see SplineMath::PropagateNormal for what that does.
	LegacyUp = 0,
	//! A rotation-minimizing frame carried along the curve by double reflection. No preferred up
	//! vector, so nothing flips through a vertical segment or a tight turn.
	ParallelTransport,
};

inline void ReflectType(Schematyc::CTypeDesc<ESplineFrameMode>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B22}"_cry_guid);
	desc.SetLabel("Frame Mode");
	desc.SetDescription("How the spline builds the normal that orients what rides on it");
	desc.SetDefaultValue(ESplineFrameMode::LegacyUp);
	desc.AddConstant(ESplineFrameMode::LegacyUp, "LegacyUp", "Legacy Up");
	desc.AddConstant(ESplineFrameMode::ParallelTransport, "ParallelTransport", "Parallel Transport");
}

//! One authored point of a spline shape, in the component's local space.
//!
//! It is CSplinePoint (SplineObject.h:9-23) minus the two Bezier handles: those are DERIVED from
//! the neighbouring positions and never authored (SplineMath.h), so storing them would only give
//! the file a second, staler copy of the same information. CGravityVolumeObject already ships that
//! way - it writes Pos/Angle/Width/IsDefaultWidth and has Back/Forw commented out of its
//! serializer (GravityVolumeObject.cpp:810-811, 835-836).
//!
//! `obstructSound` is the polygon's per-edge flag in the same place for the same reason (decision
//! 03): one point, one flag, and a topology edit can never desync a parallel array.
struct SSplinePoint
{
	static void ReflectType(Schematyc::CTypeDesc<SSplinePoint>& desc)
	{
		desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B20}"_cry_guid);
		desc.SetLabel("Spline Point");
	}

	void Serialize(Serialization::IArchive& archive)
	{
		archive(pos, "pos", "Position");
		archive.doc("Position of the point in the shape's local space.");

		archive(angle, "angle", "Angle");
		archive.doc("Roll about the tangent in degrees - what banks a road and rolls a distributed instance.");

		archive(defaultWidth, "defaultWidth", "Default Width");
		archive.doc("Use the consumer's own width here instead of the Width below.");

		if (!defaultWidth)
		{
			archive(width, "width", "Width");
			archive.doc("Width of the spline at this point, in metres.");
		}

		archive(obstructSound, "obstructSound", "Obstruct Sound");
		archive.doc("Whether the segment that starts at this point obstructs sound.");
	}

	bool operator==(const SSplinePoint& other) const
	{
		return pos == other.pos && width == other.width && angle == other.angle &&
		       defaultWidth == other.defaultWidth && obstructSound == other.obstructSound;
	}

	bool operator!=(const SSplinePoint& other) const { return !(*this == other); }

	Vec3  pos = ZERO;
	float width = 0.0f;
	float angle = 0.0f;
	bool  defaultWidth = true;
	bool  obstructSound = false;
};

//! The reflected wrapper a std::vector member needs: a GUID, an operator== (the game-mode property
//! round trip compares whole members) and a free Serialize. research/04 section 5.3.
struct SSplinePoints
{
	static void ReflectType(Schematyc::CTypeDesc<SSplinePoints>& desc)
	{
		desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B21}"_cry_guid);
		desc.SetLabel("Spline Points");
	}

	bool operator==(const SSplinePoints& other) const { return points == other.points; }
	bool operator!=(const SSplinePoints& other) const { return !(*this == other); }

	std::vector<SSplinePoint> points;
};

inline bool Serialize(Serialization::IArchive& archive, SSplinePoints& value, const char* szName, const char* szLabel)
{
	return archive(value.points, szName, szLabel);
}

//! A Bezier spline: the component form of EditorQt's CSplineObject, with the two things that object
//! never had - a real `Closed` flag (decision 04, C2) and an entity transform that owns the
//! placement, so no point edit ever moves the entity (legacy's CalcBBox re-bases the whole point
//! list on point 0, SplineObject.cpp:1208-1217, which couples every edit to the transform).
//!
//! It is pure geometry. The things that consume a spline - gravity volumes, distributors, roads,
//! rivers - are separate FUNCTION components on the same entity that find it through
//! IShapeComponent::GetSpline() (decision 01).
class CSplineShapeComponent final
	: public IShapeComponent
	, public IShapeComponentEdit
	, public ISplineShape
#ifndef RELEASE
	, public IEntityComponentPreviewer
#endif
{
public:
	CSplineShapeComponent() = default;
	virtual ~CSplineShapeComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CSplineShapeComponent>& desc)
	{
		desc.SetGUID("{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96204}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Shape: Spline");
		desc.SetDescription("A Bezier spline, the path form of a shape. Area functions on the same entity - gravity volume, distributor - use it as their curve.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		desc.AddBase<IShapeComponent>();
		desc.AddBase<IEditorShapeComponent>();

		// One shape per entity (decision 01): every other kind, both directions, from one list.
		DeclareShapeKindIncompatibilities(desc);

		desc.AddMember(&CSplineShapeComponent::m_closed, 'clsd', "Closed", "Closed", "Whether the last point connects back to the first, closing the curve smoothly.", false);
		desc.AddMember(&CSplineShapeComponent::m_offset, 'offs', "Offset", "Offset", "Translation of the whole spline in the entity's local space", Vec3(0.0f, 0.0f, 0.0f));
		desc.AddMember(&CSplineShapeComponent::m_smoothIterations, 'smth', "SmoothIterations", "Smooth Iterations", "Rounds the corners of the control polygon before the curve is fitted, 0 to 4 passes. The authored points stay on the curve and stay editable; 0 is the legacy curve exactly.", 0);
		desc.AddMember(&CSplineShapeComponent::m_tension, 'tens', "Tension", "Tension", "How far the curve bulges between its points, 0 to 1. 1 is the legacy handle length exactly; 0 makes the curve the straight control polygon.", 1.0f);
		desc.AddMember(&CSplineShapeComponent::m_frameMode, 'frmm', "FrameMode", "Frame Mode", "How the normal that orients roads and distributed meshes is built along the curve", ESplineFrameMode::LegacyUp);
		desc.AddMember(&CSplineShapeComponent::m_points, 'pnts', "Points", "Points", "The spline's points, in the shape's local space", SSplinePoints());
	}

	// IShapeComponent
	virtual EShapeKind GetKind() const override { return EShapeKind::Spline; }

	virtual void  GetLocalAABB(AABB& out) const override;
	virtual void  GetWorldAABB(AABB& out) const override;
	virtual bool  IsPointInside(const Vec3& world) const override;
	virtual float DistanceToHull(const Vec3& world) const override;
	virtual bool  IntersectRay(const Ray& ray, float& dist) const override;
	virtual bool  GetRandomPointInside(Vec3& out) const override;
	virtual int   GetContour(Vec3* pOutPoints, int maxPoints, bool world) const override;
	virtual void  AddListener(IShapeListener* pListener) override;
	virtual void  RemoveListener(IShapeListener* pListener) override;

	virtual ISplineShape*        GetSpline() override        { return this; }
	virtual IShapeComponentEdit* GetEditInterface() override { return this; }
	// ~IShapeComponent

	// IEditorShapeComponent
	virtual bool        GetEditorLocalBounds(AABB& out) const override;
	virtual bool        EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const override;
	virtual const char* GetEditToolClassName() const override { return ShapeEditorActions::GetEditToolClassName(); }
	virtual const char* GetEditToolLabel() const override     { return ShapeEditorActions::GetEditToolLabel(); }

	//! A spline has an authored point list, so it offers the pivot action beside the point tool
	//! (ShapeEditorActions.h). Both buttons are bound to THIS component instance by the inspector.
	virtual int  GetEditorActionCount() const override { return ShapeEditorActions::GetPointShapeActionCount(); }
	virtual bool GetEditorAction(int index, SEditorActionDesc& out) const override { return ShapeEditorActions::GetPointShapeAction(index, out); }
	// ~IEditorShapeComponent

	// ISplineShape - t is normalised over the WHOLE curve and maps onto the segments uniformly in
	// parameter, not in arc length; PosByDistance is the arc-length query.
	virtual Vec3  EvalPos(float t) const override;
	virtual Vec3  EvalTangent(float t) const override;
	virtual Vec3  EvalNormal(float t) const override;
	virtual Vec3  PosByDistance(float distance) const override;
	virtual float TotalLength() const override;
	virtual bool  IsClosed() const override { return m_closed; }
	virtual float GetLocalWidth(float t, float defaultWidth) const override;
	virtual Vec3  GetLocalBezierNormal(float t) const override;
	virtual void  ParamToSegment(float t, int& indexOut, float& segmentTOut) const override;
	// ~ISplineShape

	// IShapeComponentEdit - the real thing for this kind: the control points the tool edits.
	virtual int  GetPointCount() const override;
	virtual Vec3 GetPoint(int index) const override;
	virtual void SetPoint(int index, const Vec3& local) override;
	virtual int  InsertPoint(int index, const Vec3& local) override;
	virtual void RemovePoint(int index) override;
	virtual void BeginEdit() override;
	virtual void EndEdit() override;
	virtual bool IsContourClosed() const override { return m_closed; }
	virtual int  GetEdgePoints(int index, Vec3* pOut, int maxPoints) const override;
	// ~IShapeComponentEdit

	//! Per-segment sound obstruction, for the Area function component. Not part of
	//! IShapeComponentEdit: appending to that ABI is reserved for things every kind can answer.
	bool IsSegmentObstructing(int index) const;

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
	//! Everything derived from the point list: the Bezier handles, the same data in world space, and
	//! the arc-length table. Rebuilt lazily, so a const query can ask for it.
	struct SCache
	{
		//! The EVALUATION polygon: the authored points after SmoothIterations passes of subdivision.
		//! With smoothing off it is the authored points themselves. Everything downstream - handles,
		//! samples, lengths, contour, hit test, previewer - works on this and never on m_points, so
		//! smoothing is a pure evaluation transform and the tool keeps editing the real points.
		std::vector<Vec3>                 localPos;
		std::vector<SplineMath::SHandles> localHandles;
		std::vector<Vec3>                 worldPos;
		std::vector<SplineMath::SHandles> worldHandles;
		//! Angle and width per EVALUATION point, subdivided in lockstep with the positions.
		std::vector<float>                evalAngle;
		std::vector<float>                evalWidth;
		//! How many evaluation points one authored point is worth: 2^SmoothIterations. It is what
		//! maps an authored point or edge onto the evaluation polygon exactly.
		int                               evalStride = 1;
		//! Rotation-minimizing normals, one per curve sample, in the shape's local space. Empty
		//! unless FrameMode is ParallelTransport.
		std::vector<Vec3>                 frameNormals;
		//! Arc length of each segment, in WORLD metres, and the distance each segment starts at.
		std::vector<float>                segmentLength;
		std::vector<float>                segmentStart;
		float                             totalLength = 0.0f;
		Matrix34                          worldTM = Matrix34(IDENTITY);
		bool                              bValid = false;
	};

	//! Fills the cache's evaluation polygon and its per-point attributes from the authored points.
	void           BuildEvalPolygon(SCache& cache) const;
	//! Fills cache.frameNormals by carrying one normal along the sampled curve.
	void           BuildFrameNormals(SCache& cache) const;
	//! How many samples one Bezier segment gets. Smoothing multiplies the number of SEGMENTS, so
	//! the samples per segment come down to match and the total stays roughly constant.
	int            GetSamplesPerSegment() const;

	void           InvalidateCache() const { m_cache.bValid = false; }
	const SCache&  GetCache() const;
	int            GetSegmentCount() const;

	//! Evaluation on the cached arrays. `world` picks which of the two copies is used.
	Vec3           SegmentPos(int segment, float t, bool world) const;
	Vec3           SegmentTangent(int segment, float t, bool world) const;

	//! The two end point indices of a segment, wrapped for a closed spline.
	void           SegmentPoints(int segment, int& i0, int& i1) const;

	void           NotifyListeners(EShapeChangeReason reason);
	void           RecordChange(EShapeChangeReason reason);

	//! World transform of the shape's own space: the component's world transform with the offset.
	Matrix34       GetShapeWorldTM() const;

	//! Samples the whole curve into a caller-owned buffer. Returns how many samples the curve HAS,
	//! which may exceed `maxPoints` - the contract of GetContour.
	int            SampleCurve(Vec3* pOut, int maxPoints, bool world) const;
	int            GetSampleCount() const;

	//! Distance from a world point to the sampled curve, and the nearest sample position.
	float          DistanceToCurve(const Vec3& world, Vec3* pNearestOut) const;

	//! How finely one Bezier segment is sampled for drawing, contours and distance queries. Legacy
	//! draws 8 chords per segment (SplineObject.cpp:1266) and hit-tests 6 (:1400); 8 everywhere
	//! keeps the picked curve and the drawn curve the same curve.
	static constexpr int   kSamplesPerSegment = 8;
	//! Smallest number of points a spline may be reduced to (CSplineObject::GetMinPoints, :83).
	static constexpr int   kMinPoints = 2;
	//! Most smoothing passes offered. Each doubles the evaluation polygon, so 4 is 16x - past that
	//! the curve stops changing visibly and only the cost grows.
	static constexpr int   kMaxSmoothIterations = 4;
	//! Largest evaluation polygon any amount of smoothing may produce.
	static constexpr int   kMaxEvalPoints = 8192;
	//! Largest point count any one call will process on the stack.
	static constexpr int   kMaxStackPoints = 1024;
	//! How close a ray must come to the curve for IntersectRay to call it a hit. The contract has no
	//! tolerance parameter and a curve has no thickness, so the shape names one: a quarter of a
	//! metre, which is roughly a character's shoulder. EditorHitTest uses the editor's own tolerance
	//! instead and is unaffected.
	static constexpr float kRayTolerance = 0.25f;

	SSplinePoints    m_points;
	bool             m_closed = false;
	Vec3             m_offset = ZERO;
	int              m_smoothIterations = 0;
	float            m_tension = 1.0f;
	ESplineFrameMode m_frameMode = ESplineFrameMode::LegacyUp;

	//! Set between BeginEdit() and EndEdit(): listeners hear one notification per gesture.
	bool               m_bInEdit = false;
	bool               m_bChangedDuringEdit = false;
	EShapeChangeReason m_editReason = EShapeChangeReason::Geometry;

	//! Point count as the last notification saw it, so an inspector edit can tell a moved point
	//! (Geometry) from an added or removed one (Topology).
	mutable int m_lastNotifiedPointCount = 0;

	mutable SCache m_cache;
	//! Set while GetCache() is filling m_cache. Nothing inside a cache build may read the cache;
	//! this is the belt on top of that rule - see the comment in the arc-length loop.
	mutable bool   m_bBuildingCache = false;

	//! Runtime only, never reflected: non-owning observers that un-register themselves.
	std::vector<IShapeListener*> m_listeners;
};

} // namespace AreaComponents
} // namespace Cry
