// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"
#include "../../Interface/IRoadAlign.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/ResourceTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>

#include <vector>

struct IRoadRenderNode;
struct IMaterial;

namespace Cry
{
namespace AreaComponents
{

//! A road on the entity's spline shape: the component form of CRoadObject (decision 04, option R1).
//!
//! It binds BY RULE to the one shape component on the entity (decision 01) and needs a spline; any
//! other kind gets a latched warning and no road.
//!
//! The render nodes are IRoadRenderNodes owned DIRECTLY by this component, in chunks of 16 sectors
//! exactly as CRoadObject::UpdateSectors does (MAX_TRAPEZOIDS_IN_CHUNK, RoadObject.cpp:258), and
//! each of them is marked with SetOwnerEntity so that COctreeNode::SaveObjects skips it
//! (ObjectsTree_Serialize.cpp:295) - the engine change this stage added. They are NOT in entity
//! slots, because IRoadRenderNode::SetVertices takes WORLD vertices and the node has no transform
//! of its own to speak of: a road follows the terrain it is draped over, which is a world-space
//! idea. Moving the entity therefore rebuilds, which is what the legacy object does too
//! (CRoadObject::InvalidateTM -> SetRoadSectors, RoadObject.cpp:96-100).
//!
//! Terrain alignment is an EDITOR action, "Align Terrain To Road", declared through
//! IEditorActionComponent and run by CRoadAlignTerrainTool in the editor plugin. It has to be: it
//! rewrites CHeightmap, which is the editor's copy of the terrain and the one the level saves, and
//! no engine module can reach it (see Interface/IRoadAlign.h).
class CRoadComponent final
	: public IRoadAlignSource
	, public IShapeListener
{
public:
	CRoadComponent() = default;
	virtual ~CRoadComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CRoadComponent>& desc)
	{
		// A FRESH, FULL GUID with its own high half - see the note on CWaterVolumeComponent and
		// report 05 section 5c for what a shared hipart does to the inspector.
		desc.SetGUID("{1E9A63D4-4B57-42C8-9106-7F3D5AC81B62}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Road");
		desc.SetDescription("Drapes a road over the terrain along the spline shape on this entity.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		desc.AddBase<IEditorActionComponent>();
		desc.AddBase<IRoadAlignSource>();

		desc.AddMember(&CRoadComponent::m_bEnabled, 'enbl', "Enabled", "Enabled", "Whether the road exists at all", true);
		desc.AddMember(&CRoadComponent::m_material, 'matl', "Material", "Material", "Road material. Empty falls back to the entity's own material.", Schematyc::MaterialFileName());

		desc.AddMember(&CRoadComponent::m_width, 'widt', "Width", "Width", "Width of the road at a point flagged \"default width\", in metres", 4.0f);
		desc.AddMember(&CRoadComponent::m_borderWidth, 'bwid', "BorderWidth", "Border Width", "Width of the band outside the road over which Align Terrain To Road blends the terrain back, in metres", 6.0f);
		desc.AddMember(&CRoadComponent::m_step, 'step', "StepSize", "Step Size", "Distance between sectors along the curve, in metres. Shorter follows the curve better and costs more triangles.", 4.0f);
		desc.AddMember(&CRoadComponent::m_tileLength, 'tile', "TileLength", "Tile Length", "How many metres of road one texture tile covers", 4.0f);

		desc.AddMember(&CRoadComponent::m_sortPriority, 'sort', "SortPriority", "Sort Priority", "Which road wins where two overlap, 0 to 255. Higher draws on top.", 0);
		desc.AddMember(&CRoadComponent::m_viewDistRatio, 'vdrt', "ViewDistRatio", "View Distance Ratio", "View distance ratio of the road render nodes, 0 to 255", 100);
		desc.AddMember(&CRoadComponent::m_bIgnoreTerrainHoles, 'holr', "IgnoreTerrainHoles", "Ignore Terrain Holes", "Keep the road surface across a terrain hole instead of letting the hole cut it", false);
		desc.AddMember(&CRoadComponent::m_bPhysicalize, 'phys', "Physicalize", "Physicalize", "Give the road its own collision surface, so its material's surface type is what things drive on", false);
	}

	// IRoadAlignSource - read by the editor's Align Terrain To Road tool.
	virtual int   GetRoadSampleCount() const override;
	virtual bool  GetRoadSample(int index, SRoadSample& out) const override;
	virtual float GetRoadBorderWidth() const override { return m_borderWidth; }
	// ~IRoadAlignSource

	// IEditorActionComponent
	virtual int  GetEditorActionCount() const override { return 1; }
	virtual bool GetEditorAction(int index, SEditorActionDesc& out) const override;
	// ~IEditorActionComponent

	// IShapeListener
	virtual void OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason) override;
	// ~IShapeListener

protected:
	// IEntityComponent
	virtual void                    Initialize() override;
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void                    OnShutDown() override;
	// ~IEntityComponent

private:
	IShapeComponent* EnsureBound();
	void             Unbind();

	//! Rebuilds only when something a rebuild reads has changed - ENTITY_EVENT_COMPONENT_PROPERTY_-
	//! CHANGED arrives on every inspector serialization pass (report 05 section 5d).
	void RebuildIfNeeded(bool bForce);
	//! Samples the curve into m_samples. False when there is no usable curve.
	bool BuildSamples(ISplineShape& spline);
	//! Turns m_samples into render nodes, 16 sectors per node.
	void BuildNodes();
	void DestroyNodes();

	IMaterial* ResolveMaterial() const;

	uint64 ComputeSignature(IShapeComponent& shape) const;

	//! CRoadObject's chunk size (RoadObject.cpp:258). One render node per 16 trapezoids.
	static constexpr int   kSectorsPerChunk = 16;
	//! Most stations one road builds. Each 16 sectors is a render node.
	static constexpr int   kMaxStations = 4096;
	//! How far the last boundary of a chunk is pushed along the road, to hide the gaps f16 meshes
	//! leave between two chunks (RoadObject.cpp:299-312).
	static constexpr float kChunkOverlap = 0.075f;

	bool                        m_bEnabled = true;
	Schematyc::MaterialFileName m_material;

	float m_width = 4.0f;
	float m_borderWidth = 6.0f;
	float m_step = 4.0f;
	float m_tileLength = 4.0f;

	int   m_sortPriority = 0;
	int   m_viewDistRatio = 100;
	bool  m_bIgnoreTerrainHoles = false;
	bool  m_bPhysicalize = false;

	//! Runtime only, never reflected (MEMORY.md).
	IShapeComponent*             m_pBoundShape = nullptr;
	std::vector<SRoadSample>     m_samples;
	//! Texture coordinate of each station: its arc length divided by Tile Length, which is exactly
	//! what CRoadSector::t0 holds in the legacy object (RoadObject.cpp:195).
	std::vector<float>           m_sampleTex;
	std::vector<IRoadRenderNode*> m_nodes;

	bool   m_bWarnedNoSpline = false;
	uint64 m_lastBuildSignature = 0;
};

} // namespace AreaComponents
} // namespace Cry
