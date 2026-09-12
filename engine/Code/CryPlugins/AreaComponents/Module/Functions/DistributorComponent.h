// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"
#include "../../Interface/IDistributorBake.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/ResourceTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CrySerialization/IArchive.h>
#include <CrySerialization/Decorators/ActionButton.h>

#include <CryCore/smartptr.h>
// IStatObj must be COMPLETE here: m_statObjs holds _smart_ptr<IStatObj>, and the defaulted
// destructor instantiates its AddRef/Release in this header.
#include <Cry3DEngine/IStatObj.h>

#include <functional>
#include <vector>

struct IRenderNode;

namespace Cry
{
namespace AreaComponents
{

//! How the instances are spaced along the curve.
enum class EDistributorSpacing
{
	FixedStep = 0, //!< one instance every Step metres - the only mode legacy had (mv_step)
	FixedCount,    //!< exactly Count instances, evenly spread over the usable length
	Density,       //!< Density instances per metre, with optional jitter around the even spacing
	MeshLength,    //!< as many instances as the mesh's own length tiles into the curve
};

inline void ReflectType(Schematyc::CTypeDesc<EDistributorSpacing>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B30}"_cry_guid);
	desc.SetLabel("Spacing Mode");
	desc.SetDescription("How the instances are spaced along the curve");
	desc.SetDefaultValue(EDistributorSpacing::FixedStep);
	desc.AddConstant(EDistributorSpacing::FixedStep, "FixedStep", "Fixed Step");
	desc.AddConstant(EDistributorSpacing::FixedCount, "FixedCount", "Fixed Count");
	desc.AddConstant(EDistributorSpacing::Density, "Density", "Density");
	desc.AddConstant(EDistributorSpacing::MeshLength, "MeshLength", "Mesh Length");
}

//! How an instance is oriented.
enum class EDistributorAlign
{
	FollowTangent = 0, //!< the curve's own frame: tangent, roll-aware normal, their cross product
	WorldUp,           //!< upright, turned to face along the curve on the ground plane
	SurfaceNormal,     //!< upright along the surface under the instance, turned along the curve
	Fixed,             //!< no turning at all; only Z Angle and the jitter apply
	//! Point at the NEXT instance instead of along the tangent. A straight mesh - a fence panel, a
	//! rail, a wall section - is a chord, not a curve, so orienting it by the tangent at its own
	//! sample leaves both of its ends off the line and the chain opens up at every bend. Aiming it
	//! at where the next one starts is what closes it, and with Stretch To Fit it closes exactly.
	Chord,
};

inline void ReflectType(Schematyc::CTypeDesc<EDistributorAlign>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B31}"_cry_guid);
	desc.SetLabel("Alignment");
	desc.SetDescription("How an instance is oriented");
	desc.SetDefaultValue(EDistributorAlign::FollowTangent);
	desc.AddConstant(EDistributorAlign::FollowTangent, "FollowTangent", "Follow Tangent");
	desc.AddConstant(EDistributorAlign::WorldUp, "WorldUp", "World Up");
	desc.AddConstant(EDistributorAlign::SurfaceNormal, "SurfaceNormal", "Surface Normal");
	desc.AddConstant(EDistributorAlign::Fixed, "Fixed", "Fixed");
	desc.AddConstant(EDistributorAlign::Chord, "Chord", "Point To Next");
}

//! Which axis of the MESH points along the curve.
//!
//! +X is the default because it is what the legacy object assumes: CSplineDistributor builds its
//! matrix with SetFromVectors(tangent, normal, tangent x normal, pos) (SplineDistributor.cpp:286-293)
//! and SetFromVectors puts its first argument in COLUMN 0, i.e. on local +X. So the same fence .cgf
//! that lined up under the legacy distributor lines up here with the default untouched.
enum class EDistributorForwardAxis
{
	PlusX = 0,
	PlusY,
	MinusX,
	MinusY,
};

inline void ReflectType(Schematyc::CTypeDesc<EDistributorForwardAxis>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B35}"_cry_guid);
	desc.SetLabel("Forward Axis");
	desc.SetDescription("Which axis of the mesh points along the curve");
	desc.SetDefaultValue(EDistributorForwardAxis::PlusX);
	desc.AddConstant(EDistributorForwardAxis::PlusX, "PlusX", "+X");
	desc.AddConstant(EDistributorForwardAxis::PlusY, "PlusY", "+Y");
	desc.AddConstant(EDistributorForwardAxis::MinusX, "MinusX", "-X");
	desc.AddConstant(EDistributorForwardAxis::MinusY, "MinusY", "-Y");
}

//! The three states of the legacy brush "Hideable" variable (SplineDistributor.cpp:245-248).
enum class EDistributorHideable
{
	Never = 0,
	Hidable,
	HidableSecondary,
};

inline void ReflectType(Schematyc::CTypeDesc<EDistributorHideable>& desc)
{
	desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B32}"_cry_guid);
	desc.SetLabel("Hideable");
	desc.SetDescription("Whether the instances take part in the engine's hide-object passes");
	desc.SetDefaultValue(EDistributorHideable::Never);
	desc.AddConstant(EDistributorHideable::Never, "Never", "Never");
	desc.AddConstant(EDistributorHideable::Hidable, "Hidable", "Hidable");
	desc.AddConstant(EDistributorHideable::HidableSecondary, "HidableSecondary", "Hidable Secondary");
}

//! One mesh the distributor may place, and how often it is picked relative to the others.
//!
//! The resource name is the LAST member on purpose: a resource-name selector that is not last in a
//! reflected struct loses its following siblings in the property tree (the lesson CLAUDE.md records
//! for SCineLensParams::TextureFileName).
struct SDistributorMesh
{
	static void ReflectType(Schematyc::CTypeDesc<SDistributorMesh>& desc)
	{
		desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B33}"_cry_guid);
		desc.SetLabel("Distributor Mesh");
	}

	void Serialize(Serialization::IArchive& archive)
	{
		archive(weight, "weight", "Weight");
		archive.doc("How often this mesh is picked relative to the other entries.");

		archive(mesh, "mesh", "Mesh");
		archive.doc("The .cgf placed along the curve.");
	}

	bool operator==(const SDistributorMesh& other) const { return weight == other.weight && mesh == other.mesh; }
	bool operator!=(const SDistributorMesh& other) const { return !(*this == other); }

	float                   weight = 1.0f;
	Schematyc::GeomFileName mesh;
};

//! The reflected wrapper a std::vector member needs (research/04 section 5.3).
struct SDistributorMeshes
{
	static void ReflectType(Schematyc::CTypeDesc<SDistributorMeshes>& desc)
	{
		desc.SetGUID("{2B5F4C10-6A71-4B2E-9E37-4C1D0A5E7B34}"_cry_guid);
		desc.SetLabel("Distributor Meshes");
	}

	bool operator==(const SDistributorMeshes& other) const { return meshes == other.meshes; }
	bool operator!=(const SDistributorMeshes& other) const { return !(*this == other); }

	std::vector<SDistributorMesh> meshes;
};

inline bool Serialize(Serialization::IArchive& archive, SDistributorMeshes& value, const char* szName, const char* szLabel)
{
	return archive(value.meshes, szName, szLabel);
}

//! Places meshes along the entity's spline shape: the component form of CSplineDistributor
//! (SplineDistributor.cpp), and the case that motivated the whole spline family - "the distributor
//! could do far more as a function component on a spline than locked inside one object".
//!
//! BACKEND (decision 04, D1). The instances are render nodes this component owns, created as
//! eERType_MovableBrush and marked with SetOwnerEntity(). That combination is what keeps them out
//! of the level's octree export: COctreeNode::SaveObjects skips every node with an owner entity
//! (ObjectsTree_Serialize.cpp:295, :321), and only plain eERType_Brush is a serialized type in the
//! first place (:398). So the level file stores the PARAMETERS - a handful of floats and a seed -
//! and the instances are rebuilt from them, instead of thousands of brushes being written into the
//! octree the way the legacy object's nodes are. It is the same node class and the same two calls an
//! entity slot makes for a static mesh (EntitySlot.cpp:135-145), which is the trodden path here.
//!
//! DETERMINISM. Everything random - which mesh, the jitter, the skipping - is drawn from a stream
//! seeded by Seed and the instance index, so the same parameters give the same placement on every
//! machine and after every reload. Nothing random is ever stored.
//!
//! COST WHEN IDLE is zero: there is no update event. The instances are rebuilt on a shape change
//! (already coalesced to one notification per edit gesture by the shape), on a property change, and
//! transform-only on a move.
class CDistributorComponent final
	: public IBakeableComponent
	, public IShapeListener
{
public:
	CDistributorComponent() = default;
	virtual ~CDistributorComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CDistributorComponent>& desc)
	{
		// Its own hipart, distinct from every shape kind's AND from the gravity volume's - see the
		// comment on CGravityVolumeComponent::ReflectType. A distributor and a gravity volume can
		// share one entity, so sharing a hipart would blank that entity's inspector.
		desc.SetGUID("{4F82D6B1-7C93-4A15-B2E6-58D0913FC7A2}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Distributor");
		desc.SetDescription("Places meshes along the spline shape on this entity.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		// The reflected interface bases: IBakeableComponent is how the editor's bake tool finds this
		// component across the DLL boundary, and IEditorActionComponent is what puts its button in
		// the inspector (CEntityObject::CreateComponentWidget looks the base up by type GUID).
		desc.AddBase<IBakeableComponent>();
		desc.AddBase<IEditorActionComponent>();

		desc.AddMember(&CDistributorComponent::m_bEnabled, 'enbl', "Enabled", "Enabled", "Whether any instances are placed at all", true);
		desc.AddMember(&CDistributorComponent::m_seed, 'seed', "Seed", "Seed", "Seed of everything random here. The same seed always gives the same placement.", 1u);

		desc.AddMember(&CDistributorComponent::m_spacingMode, 'spmd', "SpacingMode", "Spacing Mode", "How the instances are spaced along the curve", EDistributorSpacing::FixedStep);
		desc.AddMember(&CDistributorComponent::m_step, 'step', "Step", "Step", "Distance between instances in metres (Fixed Step)", 4.0f);
		desc.AddMember(&CDistributorComponent::m_count, 'cont', "Count", "Count", "How many instances to place (Fixed Count)", 10);
		desc.AddMember(&CDistributorComponent::m_density, 'dens', "Density", "Density", "Instances per metre (Density)", 0.25f);
		desc.AddMember(&CDistributorComponent::m_spacingJitter, 'sjit', "SpacingJitter", "Spacing Jitter", "How far along the curve an instance may wander from its even position, as a fraction of the spacing", 0.0f);

		desc.AddMember(&CDistributorComponent::m_trimStart, 'trms', "TrimStart", "Trim Start", "Metres of curve left empty at the start", 0.0f);
		desc.AddMember(&CDistributorComponent::m_trimEnd, 'trme', "TrimEnd", "Trim End", "Metres of curve left empty at the end", 0.0f);
		desc.AddMember(&CDistributorComponent::m_skipProbability, 'skip', "SkipProbability", "Skip Probability", "Chance (0..1) that any one instance is left out, for a broken-up line", 0.0f);

		desc.AddMember(&CDistributorComponent::m_alignMode, 'algn', "Alignment", "Alignment", "How an instance is oriented", EDistributorAlign::FollowTangent);
		desc.AddMember(&CDistributorComponent::m_forwardAxis, 'fwax', "ForwardAxis", "Forward Axis", "Which axis of the mesh points along the curve. +X is what the legacy spline distributor assumes.", EDistributorForwardAxis::PlusX);
		desc.AddMember(&CDistributorComponent::m_bStretchToFit, 'strf', "StretchToFit", "Stretch To Fit", "In Point To Next alignment, stretch each instance along its forward axis until it reaches the next one, so sections meet with no gap.", false);
		desc.AddMember(&CDistributorComponent::m_bPivotAtStart, 'pvst', "PivotAtStart", "Pivot At Start", "Place the START face of the mesh's bounding box on the sample point instead of the mesh's own pivot. On, this is what makes a chain of sections start where it should.", true);
		desc.AddMember(&CDistributorComponent::m_zAngle, 'zang', "ZAngle", "Z Angle", "A constant turn about the instance's own up axis, in degrees", 0.0f);
		desc.AddMember(&CDistributorComponent::m_rotationJitter, 'rjit', "RotationJitter", "Rotation Jitter", "Random rotation about each axis, in degrees, plus or minus", Vec3(0.0f, 0.0f, 0.0f));

		desc.AddMember(&CDistributorComponent::m_scaleMin, 'scmn', "ScaleMin", "Scale Min", "Smallest instance scale", 1.0f);
		desc.AddMember(&CDistributorComponent::m_scaleMax, 'scmx', "ScaleMax", "Scale Max", "Largest instance scale", 1.0f);
		desc.AddMember(&CDistributorComponent::m_scaleRampStart, 'scrs', "ScaleRampStart", "Scale Ramp Start", "Extra scale multiplier at the start of the curve", 1.0f);
		desc.AddMember(&CDistributorComponent::m_scaleRampEnd, 'scre', "ScaleRampEnd", "Scale Ramp End", "Extra scale multiplier at the end of the curve", 1.0f);
		desc.AddMember(&CDistributorComponent::m_bWidthScales, 'wscl', "WidthScales", "Width Scales", "Multiply the instance scale by the spline's own width at that point", false);
		desc.AddMember(&CDistributorComponent::m_defaultWidth, 'wdef', "DefaultWidth", "Default Width", "The width a point flagged \"default width\" contributes", 1.0f);

		desc.AddMember(&CDistributorComponent::m_offsetMin, 'ofmn', "OffsetMin", "Offset Min", "Smallest offset from the curve, in the instance's own frame", Vec3(0.0f, 0.0f, 0.0f));
		desc.AddMember(&CDistributorComponent::m_offsetMax, 'ofmx', "OffsetMax", "Offset Max", "Largest offset from the curve, in the instance's own frame", Vec3(0.0f, 0.0f, 0.0f));
		desc.AddMember(&CDistributorComponent::m_bSnapToTerrain, 'snap', "SnapToTerrain", "Snap To Terrain", "Drop every instance onto the terrain height under it", false);

		desc.AddMember(&CDistributorComponent::m_bOutdoorOnly, 'outd', "OutdoorOnly", "Outdoor Only", "Brush flag: render outdoors only", false);
		desc.AddMember(&CDistributorComponent::m_bCastShadows, 'shdw', "CastShadows", "Cast Shadows", "Brush flag: cast shadow maps", true);
		desc.AddMember(&CDistributorComponent::m_bRainOccluder, 'rain', "RainOccluder", "Rain Occluder", "Brush flag: occlude rain", false);
		desc.AddMember(&CDistributorComponent::m_bRegisterByBBox, 'rbbx', "RegisterByBBox", "Register By BBox", "Brush flag: register in the octree by bounding box", false);
		desc.AddMember(&CDistributorComponent::m_hideable, 'hide', "Hideable", "Hideable", "Brush flag: take part in the hide-object passes", EDistributorHideable::Never);
		desc.AddMember(&CDistributorComponent::m_bExcludeFromTriangulation, 'xtri', "ExcludeFromTriangulation", "Exclude From Triangulation", "Brush flag: keep out of the AI navigation triangulation", false);
		desc.AddMember(&CDistributorComponent::m_bNoDecals, 'ndec', "NoDecals", "No Decals", "Brush flag: receive no decal-node decals", false);
		desc.AddMember(&CDistributorComponent::m_bRecvWind, 'wind', "ReceiveWind", "Receive Wind", "Brush flag: bend in the wind", false);
		desc.AddMember(&CDistributorComponent::m_bGoodOccluder, 'occl', "GoodOccluder", "Good Occluder", "Brush flag: use as an occluder", false);
		desc.AddMember(&CDistributorComponent::m_viewDistRatio, 'vdst', "ViewDistRatio", "View Distance Ratio", "Brush view distance ratio, 0..255", 100);
		desc.AddMember(&CDistributorComponent::m_lodRatio, 'lodr', "LodRatio", "LOD Ratio", "Brush LOD ratio, 0..255", 100);

		// Resource-name members last (see SDistributorMesh above).
		desc.AddMember(&CDistributorComponent::m_meshes, 'mesh', "Meshes", "Meshes", "The meshes placed along the curve, with their relative weights", SDistributorMeshes());
		desc.AddMember(&CDistributorComponent::m_endCapMesh, 'ecap', "EndCapMesh", "End Cap Mesh", "An optional mesh placed once at each end of the curve", Schematyc::GeomFileName());
	}

	// IShapeListener
	virtual void OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason) override;
	// ~IShapeListener

	// IEditorActionComponent - one button, "Bake To Brushes", which starts the editor plugin's
	// one-shot bake tool for this component instance.
	virtual int  GetEditorActionCount() const override { return 1; }
	virtual bool GetEditorAction(int index, SEditorActionDesc& out) const override;
	// ~IEditorActionComponent

	// IBakeableComponent
	virtual int  GetBakeInstanceCount() const override;
	virtual bool GetBakeInstance(int index, SBakeInstance& out) const override;
	virtual bool IsBakeEnabled() const override { return m_bEnabled; }
	virtual void SetBakeEnabled(bool bEnabled) override;
	// ~IBakeableComponent

protected:
	// IEntityComponent
	virtual void                    Initialize() override;
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void                    OnShutDown() override;
	// ~IEntityComponent

private:
	//! One placed instance, resolved before any render node is touched.
	struct SInstance
	{
		Matrix34 tm = Matrix34(IDENTITY);
		int      meshIndex = 0; //!< index into m_statObjs; the end caps use kEndCapMeshIndex
	};

	ISplineShape* EnsureBound();
	void          Unbind();

	//! A cheap digest of everything a rebuild depends on. ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED
	//! arrives on EVERY inspector serialization pass, not only when a value really changed, so
	//! without this the component would tear down and rebuild all its render nodes many times a
	//! second while its panel is open - which is both wasteful and a good way to lose a node in the
	//! octree's async register/unregister queue.
	uint64        ComputeBuildSignature(ISplineShape& spline) const;

	//! (Re)loads the stat objects named by the mesh list and the end cap.
	void          ReloadMeshes();

	//! Works out where every instance goes and rewrites the render nodes. `bForce` skips the
	//! signature check, which is what a real geometry change needs (a point can move without
	//! changing the curve's length, and the length is all the signature can see of the shape).
	void          Rebuild(bool bForce = false);
	//! The cheap path for a move: the same instances, new matrices.
	void          RebuildTransformsOnly();

	//! Fills `out` with the instances the current parameters ask for. Caller-owned vector, local to
	//! this module (the heap rule only bites across the DLL boundary). It also records how many
	//! instances the spacing mode ASKED for in m_lastRequestedCount, which the log line compares
	//! against how many were actually placed.
	void          BuildInstances(ISplineShape& spline, std::vector<SInstance>& out) const;

	//! Grows or shrinks the render node list to `count` nodes.
	void          SetNodeCount(int count);
	void          DestroyNodes();

	//! Pushes one instance into one render node. `bOnlyTransform` skips the flag and material block.
	void          ApplyInstance(int index, const SInstance& instance, bool bOnlyTransform);

	//! One line per real rebuild, so that "nothing appeared" can be diagnosed from the console
	//! without a debugger: curve length, mode, spacing, the count asked for and the count placed.
	void          LogRebuild(float curveLength, int requestedCount, int placedCount) const;

	//! Says so, once, when the Meshes list places nothing - including the case where the only mesh
	//! sits in End Cap Mesh, which places one instance at each end and nothing in between.
	void          WarnIfNothingToDistribute();

	//! The brush render flags every instance carries, from the flag block below.
	uint64        BuildRenderFlags() const;

	//! Picks a mesh index from the weighted list, or -1 when nothing is loadable.
	int           PickMesh(float random01) const;

	//! The frame an instance is built in, given the curve's data at that point. `forward` is the
	//! direction the instance's forward axis should end up pointing - the tangent in every mode but
	//! Chord, where it is the direction of the chord to the next instance.
	Matrix33      BuildFrame(const Vec3& position, const Vec3& forward, const Vec3& normal) const;

	//! Rotation that brings the mesh's chosen forward axis onto the frame's forward (+X) axis.
	Matrix33      GetForwardAxisFix() const;

	//! The mesh's own extent along the chosen forward axis: `length` is how long it is, `start` the
	//! coordinate of its start face in mesh local space (what Pivot At Start shifts away). False
	//! when the mesh is not loaded or is degenerate along that axis.
	bool          GetMeshForwardExtent(int meshIndex, float& length, float& start) const;

	//! Builds an instance's full transform from the pieces every alignment mode produces.
	Matrix34      BuildInstanceTransform(const Matrix33& frame, int meshIndex, float uniformScale,
	                                     float forwardStretch, const Vec3& position, const Vec3& offset) const;

	//! The end cap's slot in m_statObjs: it is loaded into the last entry so that one array serves
	//! both, and a negative index can still mean "nothing".
	int           GetEndCapMeshIndex() const;

	static constexpr int   kMaxInstances = 20000;
	//! Ray length used by Surface Normal alignment and Snap To Terrain, in metres.
	static constexpr float kSurfaceRayLength = 200.0f;

	bool                m_bEnabled = true;
	uint32              m_seed = 1;

	EDistributorSpacing m_spacingMode = EDistributorSpacing::FixedStep;
	float               m_step = 4.0f;
	int                 m_count = 10;
	float               m_density = 0.25f;
	float               m_spacingJitter = 0.0f;

	float               m_trimStart = 0.0f;
	float               m_trimEnd = 0.0f;
	float               m_skipProbability = 0.0f;

	EDistributorAlign       m_alignMode = EDistributorAlign::FollowTangent;
	EDistributorForwardAxis m_forwardAxis = EDistributorForwardAxis::PlusX;
	bool                    m_bStretchToFit = false;
	bool                    m_bPivotAtStart = true;
	float               m_zAngle = 0.0f;
	Vec3                m_rotationJitter = ZERO;

	float               m_scaleMin = 1.0f;
	float               m_scaleMax = 1.0f;
	float               m_scaleRampStart = 1.0f;
	float               m_scaleRampEnd = 1.0f;
	bool                m_bWidthScales = false;
	float               m_defaultWidth = 1.0f;

	Vec3                m_offsetMin = ZERO;
	Vec3                m_offsetMax = ZERO;
	bool                m_bSnapToTerrain = false;

	bool                 m_bOutdoorOnly = false;
	bool                 m_bCastShadows = true;
	bool                 m_bRainOccluder = false;
	bool                 m_bRegisterByBBox = false;
	EDistributorHideable m_hideable = EDistributorHideable::Never;
	bool                 m_bExcludeFromTriangulation = false;
	bool                 m_bNoDecals = false;
	bool                 m_bRecvWind = false;
	bool                 m_bGoodOccluder = false;
	int                  m_viewDistRatio = 100;
	int                  m_lodRatio = 100;

	SDistributorMeshes      m_meshes;
	Schematyc::GeomFileName m_endCapMesh;

	//! Runtime only, never reflected (MEMORY.md).
	IShapeComponent*               m_pBoundShape = nullptr;
	std::vector<IRenderNode*>      m_renderNodes;
	//! Whether each node has been handed to I3DEngine::RegisterEntity yet. A node is registered
	//! exactly ONCE, when it first receives geometry: from then on CBrush::SetMatrix re-registers
	//! it itself on every move (Brush.cpp:224-227), and a second RegisterEntity racing that call's
	//! UnRegisterEntityAsJob in the same frame is how a node ends up in no octree node at all.
	std::vector<char>              m_nodeRegistered;
	std::vector<_smart_ptr<IStatObj>> m_statObjs;
	//! The instance list of the last rebuild, so that a move can reuse it without re-deciding
	//! anything random.
	std::vector<SInstance>         m_instances;

	bool   m_bWarnedNoSpline = false;
	//! One warning per "the Meshes list places nothing", released again when it does.
	bool   m_bWarnedNoMesh = false;
	//! Digest of the last rebuild's inputs; 0 means "never built".
	uint64 m_lastBuildSignature = 0;
	//! How many instances the spacing mode asked for in the last BuildInstances(), for the log.
	mutable int m_lastRequestedCount = 0;
};

} // namespace AreaComponents
} // namespace Cry
