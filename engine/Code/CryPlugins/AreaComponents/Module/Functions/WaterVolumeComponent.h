// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/ResourceTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>

#include <CryPhysics/physinterface.h>

#include <vector>

struct IWaterVolumeRenderNode;
struct IPhysicalEntity;
struct IMaterial;

namespace Cry
{
namespace AreaComponents
{

//! Water on the entity's shape: the component form of CWaterShapeObject (area water, on a polygon)
//! and of CRiverObject (a river, on a spline), decision 05 option C and decision 04 option V2.
//!
//! It binds BY RULE to the one shape component on the entity (decision 01) and picks its mode from
//! the shape's kind - Polygon means area water, Spline means a river. Any other kind gets a latched
//! warning and no water, because a box or a sphere has no water surface to speak of.
//!
//! AREA WATER is an entity-SLOT render node (decision 05, option C). The component creates an
//! IWaterVolumeRenderNode, calls SetAreaAttachedToEntity() on it and hands it to
//! IEntity::SetSlotRenderNode, so hide, layer hiding, editor selection highlight, the entity
//! material and the world matrix all come from the slot (EntitySlot.cpp:164-231, :271-278) and
//! MOVING THE ENTITY COSTS A MATRIX UPDATE, not a rebuild. The legacy object re-tessellates on
//! every transform invalidation (CShapeObject::InvalidateTM -> UpdateGameArea,
//! ShapeObject.cpp:1044-1053) and CreateArea tessellates 256x (WaterVolumeRenderNode.cpp:262-322),
//! which is why dragging a legacy water volume is what it is.
//! Its vertices are therefore LOCAL, relative to contour point 0, and the slot's local transform
//! carries that point - which reproduces the legacy fog plane exactly (the object builds it from
//! its world Z column through world point 0, WaterShapeObject.cpp:296, and
//! CWaterVolumeRenderNode::SetMatrix builds it from the node matrix' Z column through the node
//! matrix' translation, .cpp:667-676).
//! Physics has to be hand-rolled, because CWaterVolumeRenderNode::Physicalize() early-returns for
//! an attached node (.cpp:840). The recipe is CGameVolume_Water::CreatePhysicsArea
//! (GameVolume_Water.cpp:418-470): SetAndCreatePhysicsArea, then place the area with pe_params_pos
//! and give it a buoyancy plane. Unlike that game object we also push SetPhysParams' density and
//! resistance and the whole pe_params_area wave-sim block, so parity with the Sandbox object is
//! complete (research/09 section 2b table).
//!
//! A RIVER is N nodes, one per sector, exactly as CRiverObject does (RiverObject.cpp:195-271).
//! They are NOT slot-attached and they take WORLD vertices, for one hard reason: a river node's
//! physics area can only be built by CWaterVolumeRenderNode::Physicalize(), because
//! SetAndCreatePhysicsArea goes through SetAreaPhysicsArea, which refuses anything that is not
//! eWVT_Area (.cpp:440-442) - and Physicalize() refuses anything that IS attached (.cpp:840). So a
//! river node is either attached and unphysicalizable, or free and physicalized by the engine. We
//! take the second, which is also what the legacy object does, and pay for it by rebuilding on
//! transform. The nodes are still owned by the entity (SetOwnerEntity), which is what keeps the
//! octree exporter from baking them (ObjectsTree_Serialize.cpp:295).
//!
//! Not reproduced: the legacy CausticTiling load bug (WaterShapeObject.cpp:396 and
//! RiverObject.cpp:327-329 both read CausticTiling into mv_waterCausticIntensity).
class CWaterVolumeComponent final
	: public IEntityComponent
	, public IShapeListener
{
public:
	CWaterVolumeComponent() = default;
	virtual ~CWaterVolumeComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CWaterVolumeComponent>& desc)
	{
		// A FRESH, FULL GUID with its own high half. CEntityObject::CreateComponentWidgets keys each
		// component's property tree by GetClassDesc().GetGUID().hipart + instance index
		// (EntityObject.cpp:1107), so two component classes that share a hipart blank the whole
		// entity's inspector when they sit on one entity (report 05 section 5c). This one shares a
		// hipart with nothing in the module.
		desc.SetGUID("{C7B41E58-9D06-4A72-83F5-1B64E29D7C08}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Water Volume");
		desc.SetDescription("Fills the shape on this entity with water - a polygon becomes a water volume, a spline becomes a river.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		desc.AddMember(&CWaterVolumeComponent::m_bEnabled, 'enbl', "Enabled", "Enabled", "Whether the water exists at all", true);
		desc.AddMember(&CWaterVolumeComponent::m_material, 'matl', "Material", "Material", "Water material. It must use a shader of type Water - anything else is reported once in the console. Empty falls back to the entity's own material.", Schematyc::MaterialFileName());

		desc.AddMember(&CWaterVolumeComponent::m_volumeDepth, 'dpth', "VolumeDepth", "Depth", "How deep the water goes below its surface, in metres. It is also what the physics area is extruded down by.", 10.0f);
		desc.AddMember(&CWaterVolumeComponent::m_streamSpeed, 'strm', "StreamSpeed", "Stream Speed", "Speed of the surface flow. On a river it also becomes the physics flow along the contour.", 0.0f);

		desc.AddMember(&CWaterVolumeComponent::m_fogDensity, 'fogd', "FogDensity", "Fog Density", "Density of the underwater fog. ZERO MEANS THE VOLUME DRAWS NOTHING AT ALL (WaterVolumeRenderNode.cpp:699).", 0.5f);
		desc.AddMember(&CWaterVolumeComponent::m_fogColor, 'fogc', "FogColor", "Fog Color", "Colour of the underwater fog, before the multiplier", ColorF(0.005f, 0.01f, 0.02f));
		desc.AddMember(&CWaterVolumeComponent::m_fogColorMultiplier, 'fogm', "FogColorMultiplier", "Fog Color Multiplier", "Multiplier applied to Fog Color before it reaches the render node", 0.5f);
		desc.AddMember(&CWaterVolumeComponent::m_bFogColorAffectedBySun, 'fogs', "FogColorAffectedBySun", "Fog Color Affected By Sun", "Whether the sun colour tints the fog", true);
		desc.AddMember(&CWaterVolumeComponent::m_fogShadowing, 'fogh', "FogShadowing", "Fog Shadowing", "How much shadows darken the fog, 0 to 1", 0.5f);
		desc.AddMember(&CWaterVolumeComponent::m_bCapFogAtVolumeDepth, 'fogv', "CapFogAtVolumeDepth", "Cap Fog At Volume Depth", "Stop the fog at Depth instead of letting it continue below", false);

		desc.AddMember(&CWaterVolumeComponent::m_uScale, 'uscl', "UScale", "U Scale", "Surface texture scale along U", 1.0f);
		desc.AddMember(&CWaterVolumeComponent::m_vScale, 'vscl', "VScale", "V Scale", "Surface texture scale along V", 1.0f);
		desc.AddMember(&CWaterVolumeComponent::m_viewDistRatio, 'vdrt', "ViewDistRatio", "View Distance Ratio", "View distance ratio of the water render nodes, 0 to 255", 100);

		desc.AddMember(&CWaterVolumeComponent::m_bCaustics, 'caus', "Caustics", "Caustics", "Whether the volume casts water caustics", true);
		desc.AddMember(&CWaterVolumeComponent::m_causticIntensity, 'caui', "CausticIntensity", "Caustic Intensity", "Strength of the caustics", 1.0f);
		desc.AddMember(&CWaterVolumeComponent::m_causticTiling, 'caut', "CausticTiling", "Caustic Tiling", "Tiling of the caustic pattern", 1.0f);
		desc.AddMember(&CWaterVolumeComponent::m_causticHeight, 'cauh', "CausticHeight", "Caustic Height", "How far above the surface the caustics still reach, in metres. The bounding box grows by it.", 0.5f);

		desc.AddMember(&CWaterVolumeComponent::m_waterDensity, 'wdns', "WaterDensity", "Water Density", "Buoyancy density of the water", 1000.0f);
		desc.AddMember(&CWaterVolumeComponent::m_waterResistance, 'wres', "WaterResistance", "Water Resistance", "Buoyancy resistance of the water", 1000.0f);

		// The pe_params_area wave-simulation block, which CGameVolume_Water never had
		// (research/09 section 2b). Defaults are CWaterShapeObject::InitVariables', line for line.
		desc.AddMember(&CWaterVolumeComponent::m_fixedVolume, 'fvol', "FixedVolume", "Fixed Volume", "Advanced: fixed volume of water, 0 for none", 0.0f);
		desc.AddMember(&CWaterVolumeComponent::m_volumeAccuracy, 'vacc', "VolumeAccuracy", "Volume Accuracy", "Advanced: accuracy the fixed volume is maintained to", 0.001f);
		desc.AddMember(&CWaterVolumeComponent::m_borderPad, 'bpad', "ExtrudeBorder", "Extrude Border", "Advanced: how far the simulation border is extruded outwards, in metres", 0.0f);
		desc.AddMember(&CWaterVolumeComponent::m_bConvexBorder, 'bcvx', "ConvexBorder", "Convex Border", "Advanced: treat the border as convex", false);
		desc.AddMember(&CWaterVolumeComponent::m_objVolThreshold, 'ovth', "ObjectSizeLimit", "Object Size Limit", "Advanced: smallest object volume that still displaces water", 0.001f);
		desc.AddMember(&CWaterVolumeComponent::m_waveSimCell, 'wcel', "WaveSimCell", "Wave Sim Cell", "Advanced: cell size of the wave simulation grid, 0 disables the simulation", 0.0f);
		desc.AddMember(&CWaterVolumeComponent::m_waveSpeed, 'wspd', "WaveSpeed", "Wave Speed", "Advanced: propagation speed of the wave simulation", 140.0f);
		desc.AddMember(&CWaterVolumeComponent::m_waveDamping, 'wdmp', "WaveDamping", "Wave Damping", "Advanced: damping at the centre of the wave simulation", 0.2f);
		desc.AddMember(&CWaterVolumeComponent::m_waveTimestep, 'wtst', "WaveTimestep", "Wave Timestep", "Advanced: fixed timestep of the wave simulation", 0.02f);
		desc.AddMember(&CWaterVolumeComponent::m_minWaveVel, 'wmvl', "MinWaveVel", "Min Wave Velocity", "Advanced: velocity below which the wave simulation goes to sleep", 0.01f);
		desc.AddMember(&CWaterVolumeComponent::m_simDepth, 'wdpc', "DepthCells", "Depth Cells", "Advanced: depth of the wave simulation in cells", 8.0f);
		desc.AddMember(&CWaterVolumeComponent::m_heightLimit, 'whlm', "HeightLimit", "Height Limit", "Advanced: largest wave height the simulation allows", 7.0f);
		desc.AddMember(&CWaterVolumeComponent::m_waveResistance, 'wrst', "WaveResistance", "Wave Resistance", "Advanced: resistance the wave simulation applies to bodies", 1.0f);
		desc.AddMember(&CWaterVolumeComponent::m_simAreaGrowth, 'wgrw', "SimAreaGrowth", "Sim Area Growth", "Advanced: how much room the simulation reserves for growth", 0.0f);

		// River only. Width/StepSize/TileLength are CRoadObject::InitBaseVariables' defaults, which
		// CRiverObject inherits verbatim (RoadObject.cpp:41-48).
		desc.AddMember(&CWaterVolumeComponent::m_riverWidth, 'rwid', "RiverWidth", "River Width", "River: width of the water at a point flagged \"default width\", in metres", 4.0f);
		desc.AddMember(&CWaterVolumeComponent::m_riverStep, 'rstp', "RiverStepSize", "River Step Size", "River: distance between sectors along the curve, in metres. Shorter follows the curve better and costs more render nodes.", 4.0f);
		desc.AddMember(&CWaterVolumeComponent::m_riverTileLength, 'rtil', "RiverTileLength", "River Tile Length", "River: how many metres of river one texture tile covers", 4.0f);
	}

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
	//! Which shape kind the component found, and therefore what it builds.
	enum class EMode
	{
		None = 0,
		Area,
		River,
	};

	IShapeComponent* EnsureBound();
	void             Unbind();

	//! Rebuilds only when something a rebuild reads has changed, the way the distributor does it -
	//! ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED arrives on every inspector serialization pass, and a
	//! rebuild here is a 256x tessellation plus a physics area (report 05 section 5d).
	//! Returns whether it actually rebuilt, so that the transform path knows whether it still has
	//! to move the physics area by hand.
	bool  RebuildIfNeeded(bool bForce);
	void  Rebuild();
	bool  BuildArea(IShapeComponent& shape);
	bool  BuildRiver(IShapeComponent& shape, ISplineShape& spline);

	//! Every scalar that is not geometry, onto one node.
	void  PushNodeProperties(IWaterVolumeRenderNode* pNode) const;
	//! The pe_params_area block, assembled from the advanced members.
	void  FillAuxPhysParams(pe_params_area& out) const;

	//! Area mode only: create the physics area for an attached node, which cannot physicalize
	//! itself, and place it. `localVertices` are the same vertices CreateArea was given.
	void  CreateAreaPhysics(IWaterVolumeRenderNode* pNode, const Vec3* pLocalVertices, int vertexCount);
	//! Area mode only: move an existing physics area to the node's current world transform. This is
	//! the whole cost of moving an area water entity.
	void  UpdateAreaPhysicsPlacement();
	void  DestroyAreaPhysics();

	void  DestroyNodes();

	//! The material the nodes should carry: the component's own, or the entity's when it is empty.
	IMaterial* ResolveMaterial() const;
	//! One console line when the material's shader is not eST_Water, as both legacy objects do
	//! (WaterShapeObject.cpp:255-258, RiverObject.cpp:184-189). Latched on the material path, so a
	//! wrong material is reported once and a corrected one re-arms it.
	void  ValidateMaterial(IMaterial* pMaterial);

	//! World transform of the node frame: the entity transform with the contour's first point.
	Matrix34 GetNodeWorldTM() const;

	uint64 ComputeSignature(IShapeComponent& shape, EMode mode) const;

	//! The contour's first point in ENTITY space, which is what m_localOrigin caches and what the
	//! slot's local transform carries. False when the shape has no usable contour.
	//!
	//! It exists because of the pivot-recenter bug: "Recenter Pivot" moves the entity AND rewrites
	//! every local point so that nothing moves in the world, which leaves the world contour
	//! identical and m_localOrigin - a point in ENTITY space - stale. A stale origin translates the
	//! water by exactly the pivot delta while keeping its shape, which is what the user saw.
	bool GetContourOriginInEntitySpace(IShapeComponent& shape, Vec3& out) const;

	//! Largest contour any one rebuild copies onto the stack. A water contour is triangulated and
	//! then tessellated 256x, so a contour anywhere near this is already a mistake.
	static constexpr int kMaxStackPoints = 1024;
	//! Most sectors one river builds. Each is a render node; past this the step size is wrong.
	static constexpr int kMaxRiverSectors = 512;

	bool                        m_bEnabled = true;
	Schematyc::MaterialFileName m_material;

	float m_volumeDepth = 10.0f;
	float m_streamSpeed = 0.0f;

	float  m_fogDensity = 0.5f;
	ColorF m_fogColor = ColorF(0.005f, 0.01f, 0.02f);
	float  m_fogColorMultiplier = 0.5f;
	bool   m_bFogColorAffectedBySun = true;
	float  m_fogShadowing = 0.5f;
	bool   m_bCapFogAtVolumeDepth = false;

	float m_uScale = 1.0f;
	float m_vScale = 1.0f;
	int   m_viewDistRatio = 100;

	bool  m_bCaustics = true;
	float m_causticIntensity = 1.0f;
	float m_causticTiling = 1.0f;
	float m_causticHeight = 0.5f;

	float m_waterDensity = 1000.0f;
	float m_waterResistance = 1000.0f;

	float m_fixedVolume = 0.0f;
	float m_volumeAccuracy = 0.001f;
	float m_borderPad = 0.0f;
	bool  m_bConvexBorder = false;
	float m_objVolThreshold = 0.001f;
	float m_waveSimCell = 0.0f;
	float m_waveSpeed = 140.0f;
	float m_waveDamping = 0.2f;
	float m_waveTimestep = 0.02f;
	float m_minWaveVel = 0.01f;
	float m_simDepth = 8.0f;
	float m_heightLimit = 7.0f;
	float m_waveResistance = 1.0f;
	float m_simAreaGrowth = 0.0f;

	float m_riverWidth = 4.0f;
	float m_riverStep = 4.0f;
	float m_riverTileLength = 4.0f;

	//! Runtime only, never reflected (MEMORY.md).
	IShapeComponent*                     m_pBoundShape = nullptr;
	EMode                                m_mode = EMode::None;
	//! Area mode: one node, owned by the entity SLOT. River mode: N nodes owned directly by this
	//! component. The two never coexist - a mode change destroys everything first.
	std::vector<IWaterVolumeRenderNode*> m_nodes;
	bool                                 m_bNodeInSlot = false;
	//! Area mode only: the hand-rolled physics area, and the local centre the physics gave it,
	//! which is what pe_params_pos has to be recomputed from on every move
	//! (GameVolume_Water.cpp:440-446).
	IPhysicalEntity*                     m_pPhysArea = nullptr;
	Vec3                                 m_physLocalCentre = ZERO;
	//! The contour's first point in ENTITY space: the origin every local vertex is relative to.
	Vec3                                 m_localOrigin = ZERO;

	bool   m_bWarnedNoShape = false;
	bool   m_bWarnedWrongKind = false;
	bool   m_bWarnedMaterial = false;
	bool   m_bWarnedTooFewPoints = false;
	uint64 m_lastBuildSignature = 0;
};

} // namespace AreaComponents
} // namespace Cry
