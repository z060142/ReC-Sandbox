// Copyright 2026 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include "BaseMeshComponent.h"
#include "ITerrainPlateCommit.h"

#include <CrySchematyc/ResourceTypes.h>
#include <CrySchematyc/Utils/SharedString.h>
#include <CrySerialization/Decorators/ResourceFilePath.h>
#include <CrySerialization/Decorators/ActionButton.h>

#include <functional>
#include <vector>

class CPlugin_CryDefaultEntities;
//! CryPhysics' registered geometry handle; only passed around here, so a forward declaration is enough.
struct phys_geometry;

namespace Serialization
{
//! File picker for the heightmap property: a plain file dialog, since .r16 / .raw / .pgm are not texture assets.
inline ResourceFilePath TerrainPlateHeightmapPath(string& path)
{
	return ResourceFilePath(path, "Heightmap (r16, raw, pgm, bmp)|*.r16;*.raw;*.pgm;*.bmp");
}
}

namespace Schematyc
{
typedef SerializationUtils::SResourceNameSerializer<&Serialization::TerrainPlateHeightmapPath> TerrainHeightmapFileName;

inline void ReflectType(CTypeDesc<TerrainHeightmapFileName>& desc)
{
	desc.SetGUID("{7C1F5A08-2E6B-4F0D-9B54-6F0B2A9D3C41}"_cry_guid);
	desc.SetLabel("TerrainHeightmapFileName");
	desc.SetDescription("Path to a heightmap file (r16, raw, pgm, bmp)");
}
}

namespace Cry
{
namespace DefaultComponents
{

//! A height field sampled on the CPU, normalised to [0,1]. Row major, row 0 is the +Y edge of the
//! plate and column 0 the -X edge - the orientation the Sandbox heightmap importer ends up with.
struct STerrainPlateHeightField
{
	int                m_width = 0;
	int                m_height = 0;
	std::vector<float> m_values;

	bool IsValid() const { return m_width > 1 && m_height > 1 && (int)m_values.size() == m_width * m_height; }
	void Clear()         { m_width = 0; m_height = 0; m_values.clear(); }

	//! Nearest sample, clamped to the border.
	float At(int x, int y) const
	{
		x = clamp_tpl(x, 0, m_width - 1);
		y = clamp_tpl(y, 0, m_height - 1);
		return m_values[(size_t)y * m_width + x];
	}

	//! u/v in [0,1]; v = 0 is row 0, i.e. the +Y edge of the plate.
	float SampleBilinear(float u, float v) const;
};

//! CPU loaders for the two plate sources. All I/O through ICryPak; CImageEx is 8 bit and editor only.
namespace TerrainPlateLoader
{
//! Dispatches on the extension: .r16/.raw (headerless 16 bit LE, square), .pgm (binary P5), .bmp.
bool LoadHeightmapFile(const char* szPath, STerrainPlateHeightField& out);

//! Uncompressed DDS only (R8, R16, R16F, R32F, RGBA8/BGRA8). Anything else fails to a flat plate.
bool LoadDisplacementTexture(const char* szPath, STerrainPlateHeightField& out);
}

enum class ETerrainPlateSource
{
	HeightmapFile = 0,
	DisplacementTexture,
};

static void ReflectType(Schematyc::CTypeDesc<ETerrainPlateSource>& desc)
{
	desc.SetGUID("{0E4D6C1B-6A48-4B26-9C7E-5B1D0A2F8E73}"_cry_guid);
	desc.SetLabel("Terrain Plate Source");
	desc.SetDescription("Where the plate takes its relief from");
	desc.SetDefaultValue(ETerrainPlateSource::HeightmapFile);
	desc.AddConstant(ETerrainPlateSource::HeightmapFile, "HeightmapFile", "Heightmap File");
	desc.AddConstant(ETerrainPlateSource::DisplacementTexture, "DisplacementTexture", "Displacement Texture");
}

enum class ETerrainPlateGridMode
{
	Fixed = 0,
	Spacing,
};

static void ReflectType(Schematyc::CTypeDesc<ETerrainPlateGridMode>& desc)
{
	desc.SetGUID("{9A62C0D5-4E17-4B83-8F06-2C5D71B4EA98}"_cry_guid);
	desc.SetLabel("Terrain Plate Grid Mode");
	desc.SetDescription("How the tessellation of the plate is decided");
	desc.SetDefaultValue(ETerrainPlateGridMode::Fixed);
	desc.AddConstant(ETerrainPlateGridMode::Fixed, "Fixed", "Fixed");
	desc.AddConstant(ETerrainPlateGridMode::Spacing, "Spacing", "Spacing");
}

enum class ETerrainPlateBakeMode
{
	Raise = 0,
	RaiseAndLower,
};

static void ReflectType(Schematyc::CTypeDesc<ETerrainPlateBakeMode>& desc)
{
	desc.SetGUID("{4D8B1E60-3C79-45A2-9E31-0B6F2A84D5C7}"_cry_guid);
	desc.SetLabel("Terrain Plate Bake Mode");
	desc.SetDescription("How the baked terrain meets the original ground");
	desc.SetDefaultValue(ETerrainPlateBakeMode::Raise);
	desc.AddConstant(ETerrainPlateBakeMode::Raise, "Raise", "Raise");
	desc.AddConstant(ETerrainPlateBakeMode::RaiseAndLower, "RaiseAndLower", "Raise And Lower");
}

//! Terrain metrics of the current level, in terrain units. Invalid when there is no terrain.
struct SBakeTerrainMetrics
{
	float unitSize = 1.f;    //!< metres per terrain unit
	int   hmapSize = 0;      //!< terrain size in units
	int   sectorUnits = 1;   //!< units along one sector edge

	bool IsValid() const { return hmapSize > 0 && sectorUnits > 0 && unitSize > 0.f; }
};

//! One of the cells the bake polling watches, with the height the bake left there.
struct SBakeProbe
{
	int   ux = 0;
	int   uy = 0;
	float expectedZ = 0.f;
};

//! Console variables owned by the component; a cvar must never outlive the module that owns it.
void RegisterTerrainPlateCVars();
void UnregisterTerrainPlateCVars();

class CTerrainPlateComponent;

//! Panel grouping helper: the plate's rows are collected into four collapsible groups (Shape / Bake /
//! Weld / Editor). The panel walks the AddMember list directly - a Serialize() on the component itself
//! is never called - so a group cannot be wrapped around existing members. Instead every data member
//! stays exactly where it is (same AddMember, id, szName and order, so no archive key changes) and only
//! its LABEL is emptied, which hides the row while the archive keeps reading and writing it. The groups
//! own no data: each holds a back pointer to the component and re-emits those members with their real
//! labels in an editor archive. The two master tick boxes (Bake Into Terrain, Edge Weld) keep their
//! label and stay top level rows, each right in front of the drawer it opens; a drawer that must be
//! ABSENT rather than empty is suppressed instead by the free Serialize overload below returning false.
//! Two rules keep this safe: the four group members are AddMember'd AFTER every member they show,
//! because the input pass walks in AddMember order and the group row - the one the user edited - must
//! write last; and a COPY of a group never carries the owner, so copies in Schematyc scratchpads and
//! undo buffers emit nothing (assignment keeps the DESTINATION's owner, so Revert to Defaults is safe).
struct STerrainPlateUIGroup
{
	STerrainPlateUIGroup() = default;
	STerrainPlateUIGroup(const STerrainPlateUIGroup&) {}
	STerrainPlateUIGroup& operator=(const STerrainPlateUIGroup&) { return *this; }
	//! The groups hold no data, so two are always equal; the panel's change test sees only real members.
	bool operator==(const STerrainPlateUIGroup&) const { return true; }

	CTerrainPlateComponent* m_pOwner = nullptr;
};

//! Shape: where the relief comes from and how it is tessellated.
struct STerrainPlateShapeGroup : public STerrainPlateUIGroup
{
	void Serialize(Serialization::IArchive& archive);
};

//! Bake tuning rows; the drawer exists only while Bake Into Terrain is ticked (that tick box is top level).
struct STerrainPlateBakeGroup : public STerrainPlateUIGroup
{
	void Serialize(Serialization::IArchive& archive);
};

//! Weld tuning rows; the drawer exists only while Edge Weld is on (that tick box is top level).
struct STerrainPlateWeldGroup : public STerrainPlateUIGroup
{
	void Serialize(Serialization::IArchive& archive);
};

//! Editor: how the plate behaves while it is being edited. No effect at game time.
struct STerrainPlateEditorGroup : public STerrainPlateUIGroup
{
	void Serialize(Serialization::IArchive& archive);
};

static void ReflectType(Schematyc::CTypeDesc<STerrainPlateShapeGroup>& desc)
{
	desc.SetGUID("{1B4E7A32-9D05-4C6E-8F17-2A3C5D68B910}"_cry_guid);
	desc.SetLabel("Terrain Plate Shape Group");
	desc.SetDescription("Panel grouping only, holds no data");
}

static void ReflectType(Schematyc::CTypeDesc<STerrainPlateBakeGroup>& desc)
{
	desc.SetGUID("{2C5F8B43-AE16-4D7F-9028-3B4D6E79CA21}"_cry_guid);
	desc.SetLabel("Terrain Plate Bake Group");
	desc.SetDescription("Panel grouping only, holds no data");
}

static void ReflectType(Schematyc::CTypeDesc<STerrainPlateWeldGroup>& desc)
{
	desc.SetGUID("{3D609C54-BF27-4E80-A139-4C5E7F8ADB32}"_cry_guid);
	desc.SetLabel("Terrain Plate Weld Group");
	desc.SetDescription("Panel grouping only, holds no data");
}

static void ReflectType(Schematyc::CTypeDesc<STerrainPlateEditorGroup>& desc)
{
	desc.SetGUID("{4E71AD65-C038-4F91-B24A-5D6F809BEC43}"_cry_guid);
	desc.SetLabel("Terrain Plate Editor Group");
	desc.SetDescription("Panel grouping only, holds no data");
}

//! Free serialization overloads for the two conditional drawers: yasli finds these by ADL and prefers
//! them over the member Serialize(), and they run before the member's row is created, so returning
//! false emits NOTHING - the drawer is absent rather than an empty collapsible line.
bool Serialize(Serialization::IArchive& archive, STerrainPlateBakeGroup& value, const char* szName, const char* szLabel);
bool Serialize(Serialization::IArchive& archive, STerrainPlateWeldGroup& value, const char* szName, const char* szLabel);

//! A procedural grid mesh built from a heightmap, hosted in an entity slot so that it reaches the
//! terrain integration path exactly like an ordinary mesh component does.
class CTerrainPlateComponent
	: public CBaseMeshComponent
	, public ITerrainPlateCommit
{
public:
	//! e_TerrainPlateCullBuried changed: the classification lives in the index buffer, so every plate has
	//! to rebuild its mesh. Public because ICVar::AddOnChange takes a plain function.
	static void OnCullBuriedCVarChanged(ICVar* pCVar);

	//! e_TerrainPlatePhysSurfaceFromTerrain changed. The per face ids live inside the physics trimesh, so
	//! - like the buried cull above - the cvar can only take effect by rebuilding every plate.
	static void OnPhysSurfaceCVarChanged(ICVar* pCVar);

protected:
	friend CPlugin_CryDefaultEntities;
	// The panel groups draw this component's own members. @see STerrainPlateUIGroup.
	friend struct STerrainPlateShapeGroup;
	friend struct STerrainPlateBakeGroup;
	friend struct STerrainPlateWeldGroup;
	friend struct STerrainPlateEditorGroup;
	// The two conditional drawers read the tick box they belong to before deciding to emit a row at all.
	friend bool Serialize(Serialization::IArchive& archive, STerrainPlateBakeGroup& value, const char* szName, const char* szLabel);
	friend bool Serialize(Serialization::IArchive& archive, STerrainPlateWeldGroup& value, const char* szName, const char* szLabel);
	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	// IEntityComponent
	virtual void                    Initialize() final;
	//! Called before the slot is freed and the component released. @see RestoreBakeOnTeardown.
	virtual void                    OnShutDown() final;
	virtual void                    ProcessEvent(const SEntityEvent& event) final;
	virtual Cry::Entity::EventFlags GetEventMask() const final;
	// ~IEntityComponent

	// IEditorEntityComponent
	virtual bool SetMaterial(int slotId, const char* szMaterial) override;
	// ~IEditorEntityComponent

public:
	CTerrainPlateComponent()
	{
			// A plate exists to be merged into the terrain, so it defaults to GI mode Integrate Into Terrain.
		m_renderParameters.m_giMode = EMeshGIMode::IntegrateIntoTerrain;
			// A plate is ground: it collides out of the box, with no physics component. @see UpdateNativePhysics.
		m_type = EMeshType::RenderAndCollider;
			// The panel groups draw our members, so they need their owner. Never copied: @see STerrainPlateUIGroup.
		m_uiShape.m_pOwner = this;
		m_uiBake.m_pOwner = this;
		m_uiWeld.m_pOwner = this;
		m_uiEditor.m_pOwner = this;
	}
	virtual ~CTerrainPlateComponent();

	static void ReflectType(Schematyc::CTypeDesc<CTerrainPlateComponent>& desc)
	{
		desc.SetGUID("{3F1D8B4A-6C2E-4E9F-9A21-7B5C0D4E8A13}"_cry_guid);
		desc.SetEditorCategory("Geometry");
		desc.SetLabel("Terrain Plate");
		desc.SetDescription("A procedural relief plate built from a heightmap file, meant to be merged into the terrain. "
		                   "The plate is a unit plate: 1 x 1 m in XY and 0..1 m of relief; its size and height in the world come from the "
		                   "entity or component transform scale (X/Y = size, Z = height of the relief).");
		desc.SetIcon("icons:ObjectTypes/object.ico");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Transform, IEntityComponent::EFlags::Socket, IEntityComponent::EFlags::Attach });

		desc.AddBase<IEditorEntityComponent>();

		SRenderParameters plateRenderDefaults;
		plateRenderDefaults.m_giMode = EMeshGIMode::IntegrateIntoTerrain;

		desc.AddMember(&CTerrainPlateComponent::m_type, 'type', "Type", "Type",
		               "Determines the behavior of the generated mesh. Collider gives the plate its own static physics, so it can be "
		               "walked on and collided with without any physics component on the entity.", EMeshType::RenderAndCollider);

		// Parked: with no AddMember, Source Mode and Displacement Texture lose row and persistence alike.
		//desc.AddMember(&CTerrainPlateComponent::m_sourceMode, 'srcm', "SourceMode", "Source Mode", "Whether the relief comes from a heightmap file or from a displacement texture", ETerrainPlateSource::HeightmapFile);
		desc.AddMember(&CTerrainPlateComponent::m_heightmapPath, 'hmap', "HeightmapFile", /*"Heightmap File"*/ "", "16 bit raw/r16 (headerless, square), binary PGM (P5) or uncompressed BMP.", Schematyc::TerrainHeightmapFileName());
		//desc.AddMember(&CTerrainPlateComponent::m_displacementPath, 'dtex', "DisplacementTexture", "Displacement Texture", "Uncompressed DDS (R8, R16, R16F, R32F, RGBA8/BGRA8). Compressed formats cannot be read on the CPU and give a flat plate. Used when Source Mode is Displacement Texture.", Schematyc::TextureFileName());
		desc.AddMember(&CTerrainPlateComponent::m_materialPath, 'mat', "Material", "Material", "Material of the plate. Empty uses the engine default material; the plate needs a material to be picked up by terrain integration.", "");

		desc.AddMember(&CTerrainPlateComponent::m_gridMode, 'gmod', "GridMode", /*"Grid Mode"*/ "",
		               "Fixed: Grid is the number of vertices per side. Spacing: the grid is derived from Vertex Spacing and the "
		               "size of the plate in the world, so the density stays constant when the plate is resized or scaled.",
		               ETerrainPlateGridMode::Fixed);
		desc.AddMember(&CTerrainPlateComponent::m_grid, 'grid', "Grid", /*"Grid"*/ "", "Vertices per side of the plate. Only used when Grid Mode is Fixed.", 65);
		desc.AddMember(&CTerrainPlateComponent::m_vertexSpacing, 'vspc', "VertexSpacing", /*"Vertex Spacing"*/ "",
		               "Distance in metres between two vertices of the plate, measured in world space (the entity scale is taken into account). "
		               "Only used when Grid Mode is Spacing. The result is clamped to 2..513 vertices per axis, to the resolution of the source, "
		               "and to half of e_TerrainIntegrateObjectsMaxVertices.",
		               0.1f);
		desc.AddMember(&CTerrainPlateComponent::m_effectiveGridInfo, 'einf', "EffectiveGrid", /*"Effective Grid"*/ "",
		               "Read only: the grid that was actually generated (vertices along X x vertices along Y) and the resulting spacing in metres. "
		               "Anything typed here is overwritten by the next rebuild.", Schematyc::CSharedString());
		desc.AddMember(&CTerrainPlateComponent::m_bNormalize, 'norm', "Normalize", /*"Normalize"*/ "",
		               "On: the darkest sample becomes 0 and the brightest becomes 1. Off: the raw 0..1 value of the source is used. "
		               "Either way the relief spans 0..1 m before the entity scale; scale Z to set the height, and X/Y to set the size. "
		               "Ground Level slides that band down, so part of it can end up below the plate's own ground.", true);
		desc.AddMember(&CTerrainPlateComponent::m_bMirrorX, 'mirx', "MirrorX", /*"Mirror X"*/ "", "Mirrors the relief and its UVs along X (the editor cannot apply a negative scale)", false);
		desc.AddMember(&CTerrainPlateComponent::m_bMirrorY, 'miry', "MirrorY", /*"Mirror Y"*/ "", "Mirrors the relief and its UVs along Y (the editor cannot apply a negative scale)", false);
		desc.AddMember(&CTerrainPlateComponent::m_bMirrorZ, 'mirz', "MirrorZ", /*"Mirror Z"*/ "",
		               "Reads the relief upside down, so the darkest sample becomes the highest point and a hill map becomes a pit map. "
		               "The UVs are not touched. On its own the plate still only rises out of the ground - use Ground Level to sink it.",
		               false);
		desc.AddMember(&CTerrainPlateComponent::m_groundLevel, 'glvl', "GroundLevel", /*"Ground Level"*/ "",
		               "Which height of the relief sits on the ground the plate stands on, measured on the 0..1 relief and so "
		               "independent of the Z scale. 0 (default) puts the bottom of the relief on the ground, so the whole plate rises "
		               "out of it - the behaviour before this option existed. Raising it sinks everything BELOW that level under the "
		               "ground, so a pit is dug instead of a hill added; 1 puts the highest point of the relief on the ground and "
		               "everything else below it. The border is still welded to the surrounding ground. Anything below the ground "
		               "needs Bake Mode = Raise And Lower for the terrain to follow the plate down.",
		               0.f);

		// Edge Weld and Bake Into Terrain sit further down, right in front of the drawer they open.
		desc.AddMember(&CTerrainPlateComponent::m_weldInnerFalloff, 'wfal', "WeldInnerFalloff", /*"Weld Inner Falloff"*/ "", "Fraction of the half size over which the welded border blends back into the plate's own relief. 0 welds only the outermost ring and can leave a step.", 0.25f);
		desc.AddMember(&CTerrainPlateComponent::m_weldSink, 'wsnk', "WeldSink", /*"Weld Sink"*/ "",
		               "Metres by which the welded border is pushed below the terrain surface, so that the border always ends inside the terrain "
		               "instead of on it. Sampling the terrain height can never reproduce the rendered terrain mesh exactly (flipped quad diagonals, "
		               "coarser meshes at LOD >= 1, and the straight plate edge between two ring vertices), and any residual mismatch on an exact weld "
		               "shows up as a seam. The sink is applied over the outer quarter of the Weld Inner Falloff band, so the plate is still above the "
		               "terrain over most of the band and only dives under right at the ring. 0 restores the exact surface weld.",
		               0.1f);
		desc.AddMember(&CTerrainPlateComponent::m_skirt, 'skrt', "Skirt", /*"Skirt"*/ "", "Metres of extra geometry extruded straight down from the border, so the plate still seals where the terrain does not match (steep slopes, distant sectors rendered at a coarser LOD). 0 disables the skirt.", 0.25f);

		desc.AddMember(&CTerrainPlateComponent::m_bUpdateDuringDrag, 'udrg', "UpdateDuringDrag", /*"Update During Drag"*/ "",
		               "Off (default): while the plate is moved, rotated or scaled in the editor the mesh is left exactly as it was and simply "
		               "follows the entity, so the weld against the terrain is temporarily wrong; the weld, the physics mesh and the shadow cache "
		               "are all recomputed once, when the mouse button is released. On: the plate is regenerated while dragging (at most once per "
		               "frame), which is the old behaviour and is heavy on a dense plate. Only affects editor transforms; game time and TrackView "
		               "movement always update immediately.",
		               false);

		desc.AddMember(&CTerrainPlateComponent::m_bakeMode, 'bmod', "BakeMode", /*"Bake Mode"*/ "",
		               "Raise: the terrain only ever goes up to meet the plate, so a plate that dips below the original ground leaves "
		               "that ground alone (and the old ground then pokes through the plate mesh). Raise And Lower: the terrain follows "
		               "the plate down as well, so a valley modelled into the plate is cut into the terrain. Either way the rim of the "
		               "footprint blends back to the original ground, and a cell the plate only partly covers is never lowered.",
		               ETerrainPlateBakeMode::Raise);
		desc.AddMember(&CTerrainPlateComponent::m_bakePriority, 'bpri', "BakePriority", /*"Bake Priority"*/ "",
		               "Order in which overlapping baking plates are written into the terrain: the higher priority plate bakes last "
		               "and therefore wins where two footprints meet, with the lower one showing through as the ground it sits on. "
		               "Plates with the same priority are ordered by their entity GUID, so the result is the same on every load. "
		               "Only matters when two baking plates overlap.",
		               0);
		desc.AddMember(&CTerrainPlateComponent::m_bakeOffset, 'boff', "BakeOffset", /*"Bake Offset"*/ "",
		               "Extra height in metres added to the baked terrain over this plate, on top of the global "
		               "e_TerrainPlateBakeEpsilon. It is 0 at the welded edge and reaches the full value at the highest "
		               "point of the relief, so the seam with the surrounding ground never moves: the baked shape is "
		               "pulled up or pushed down proportionally to how high it stands. Positive raises the terrain "
		               "towards the plate and closes a visible gap under the plate; negative lowers it, so terrain that "
		               "pokes up through the plate mesh drops back below it. 0 is the plain bake. A few centimetres is "
		               "usually enough.",
		               0.f);

		// Live terrain colour overlay. Empty label, drawn by the Bake drawer. @see STerrainPlateUIGroup.
		desc.AddMember(&CTerrainPlateComponent::m_bLiveTerrainColour, 'ltcl', "LiveTerrainColour", /*"Terrain Colour From Plate"*/ "",
		               "On: the terrain drawn over this plate takes the plate's colour as its own terrain (macro) colour, fading "
		               "into the surrounding painted colour over the same rim band the bake uses. It is a LIVE, non destructive "
		               "EDITOR PREVIEW: the level's painted colour is never touched, and unticking this or deleting the plate "
		               "brings the original ground colour straight back. Press Stamp Colour to make exactly this picture "
		               "permanent.", false);

		// Hidden row: no label, so the panel never shows it, while AddMember keeps it persisted under its
		// szName. Internal state, not a knob, and it turns no feature off: the ground itself carries this
		// plate's colour now, so the preview must not blend the same colour over it a SECOND time.
		desc.AddMember(&CTerrainPlateComponent::m_bColourStamped, 'cstm', "ColourStamped", "",
		               "Read only, set by Stamp Colour and cleared by any change to the plate (move, scale, material, shape): the "
		               "plate's colour is part of the level's terrain colour, so the live preview and the export leave it alone. "
		               "The picture is the same either way - what is in the ground IS what the preview drew.",
		               false);

		// The visible panel, in order: the rows follow the AddMember order, so this block IS the layout.
		// Everything above is either hidden (empty label) or drawn by one of the groups below, which hold no
		// data. @see STerrainPlateUIGroup for why they must come last. A leading "+" / "-" in a label is a
		// yasli control code: "+" opens the group by default, "-" leaves it closed.
		desc.AddMember(&CTerrainPlateComponent::m_uiShape, 'sgrp', "ShapeGroup", "+Shape", "Where the relief comes from and how finely it is tessellated", STerrainPlateShapeGroup());

		desc.AddMember(&CTerrainPlateComponent::m_bBakeIntoTerrain, 'bake', "BakeIntoTerrain", "Bake Into Terrain",
		               "Writes the low frequency shape of the plate into the live engine terrain, so that everything which reads the "
		               "terrain height follows the plate instead of the original ground: roads, terrain decals, the terrain silhouette "
		               "in cached shadows and HeightMap AO, SVOGI, the physics heightfield and the AI navmesh. The written terrain is "
		               "kept 2 cm below the plate mesh and the border of the footprint blends back to the original ground, so the "
		               "terrain outside the plate and the painted surface layers everywhere are left untouched. Editor and Profile "
		               "only - the game export serialises the live terrain, so the shipped level carries the bake with no release "
		               "code at all. Turning it off (or moving, rescaling or deleting the plate) puts the original ground back first. "
		               "Plates that share ground are always restored and re-baked together as one set, so moving or deleting more "
		               "than 16 baking plates in one operation is not recommended (cost grows with overlap; prefer smaller selections).",
		               false);
		desc.AddMember(&CTerrainPlateComponent::m_uiBake, 'bgrp', "BakeGroup", "+Bake", "How the shape of the plate is written into the live engine terrain", STerrainPlateBakeGroup());

		desc.AddMember(&CTerrainPlateComponent::m_bEdgeWeld, 'weld', "EdgeWeld", "Edge Weld",
		               "Snaps the outermost ring of vertices onto the terrain surface under it, so the border of the plate seals against the terrain", true);
		desc.AddMember(&CTerrainPlateComponent::m_uiWeld, 'wgrp', "WeldGroup", "-Weld", "How the border of the plate seals against the surrounding terrain", STerrainPlateWeldGroup());

		desc.AddMember(&CTerrainPlateComponent::m_uiEditor, 'egrp', "EditorGroup", "-Editor", "How the plate behaves while it is being edited. No effect at game time.", STerrainPlateEditorGroup());

		// A physics row, so it sits in front of Physics Settings rather than in one of the four drawers. Stays
		// visible and inert with Type = Render: a row that disappears is harder to find than an inert one.
		desc.AddMember(&CTerrainPlateComponent::m_bPhysSurfaceFromTerrain, 'psft', "PhysSurfaceFromTerrain", "Physics Surface From Terrain",
		               "On (default): what a contact with the plate reports as its surface type - the footstep sound, the bullet "
		               "impact particle and decal, the vehicle dust, the friction - is taken from the TERRAIN LAYER painted under "
		               "the plate, instead of from the plate's single material. The layers are sampled when the plate is rebuilt "
		               "and again a moment after a layer under it is repainted. Where the terrain has no answer (a hole, off the "
		               "map, or a part of the plate the terrain does not reach) the plate's own material is used. "
		               "Off: the whole plate reports its own material's surface type, which is the behaviour before this option "
		               "existed. Needs Type = Collider to mean anything. The console kill switch is "
		               "e_TerrainPlatePhysSurfaceFromTerrain 0.",
		               true);

		desc.AddMember(&CTerrainPlateComponent::m_renderParameters, 'rend', "Render", "Rendering Settings", "Settings for the rendered representation of the component", plateRenderDefaults);
		desc.AddMember(&CTerrainPlateComponent::m_physics, 'phys', "Physics", "Physics Settings",
		               "Physical properties for the object. With Type = Render and Collider the plate physicalizes itself as a static "
		               "body, and the weight only matters once a physics component (rigid body, character controller) takes the entity over.",
		               SPhysicsParameters());
	}

	//! Rebuilds the height field from the current source and regenerates the mesh.
	virtual void Rebuild();

	//! Regenerates the mesh from the cached height field (no file access).
	virtual void RegenerateMesh();

	//! The entity writes the raw component transform into the slot local TM when that transform is edited,
	//! which would put the scale back onto the render node; regenerating restores the invariant.
	virtual void OnTransformChanged() override;

	// ITerrainPlateCommit
	virtual bool  IsBakeIntoTerrainEnabled() const final { return m_bBakeIntoTerrain; }
	virtual bool  IsLiveColourEnabled() const final;
	virtual bool  GetCommitFootprint(int& outX1, int& outY1, int& outSize) const final;
	virtual bool  GetCommitAppearanceFootprint(int& outX1, int& outY1, int& outSize) const final;
	virtual float GetCommitWeightAtWorld(float worldX, float worldY, float sampleSizeMeters) const final;
	virtual bool  GetCommitAlbedoAtWorld(float worldX, float worldY, float texelSizeMeters,
	                                     ColorF& outLinearColor) const final;
	virtual bool  GetLiveColourState(int& outX1, int& outY1, int& outSize, ColorB& outTint,
	                                 bool& outSettling) const final;
	void          ClearColourStamp() const;
	//! @see ITerrainPlateCommit::GetCommitOrderPriority. The same value BakesBefore orders by.
	virtual int   GetCommitOrderPriority() const final { return (int)m_bakePriority; }
	//! @see ITerrainPlateCommit::GetCommitMaterial. Inline, because GetPlateMaterial is protected and the
	//! editor can only reach the plate through the interface.
	virtual const IMaterial* GetCommitMaterial() const final { return GetPlateMaterial(); }
	virtual void  OnHeightStamped() final;
	virtual void  OnColourStamped() final;
	virtual bool  IsColourStamped() const final;
	virtual void  SetColourStampRecord(bool bStamped) final;
	virtual void  NudgeBakeAfterTerrainWrite(int rectX1, int rectY1, int rectSize) final;
	// ~ITerrainPlateCommit

protected:
	//! Loads the source into m_heightField if the source changed since the last load.
	void        LoadSourceIfNeeded();
	//! Regenerates now, or once in the next frame if this frame already did.
	void        RequestRegenerate();
	//! Runs the rebuild that was held back for the duration of an editor transform gesture.
	void        FlushDeferredTransform();
	//! Frames without an ENTITY_EVENT_XFORM after which a held back rebuild is run anyway, from
	//! e_TerrainPlateSettleFrames. 0 disables the fallback (only the mouse up event flushes).
	static int  GetSettleFrames();
	//! Settle window this plate actually uses. A BAKING plate always waits at least one frame, whatever the
	//! cvar or Update During Drag say: its restore has to be ordered against the rest of the move batch.
	int         GetEffectiveSettleFrames() const;
	//! True when this plate holds a live or prospective footprint, so a transform must go via the batch.
	bool        WantsBakeMoveBatch() const;
	//! True when the mesh depends on the entity transform (weld, skirt, or Spacing grid mode).
	bool        NeedsRegenerateOnTransform() const;
	//! The plate's DESIGN frame: maps the NORMALISED unit plate (-0.5 .. +0.5 in XY, 0 .. 1 in Z) onto the
	//! world, so it carries the plate's size in its scale. Entity world TM times component TM - NOT the
	//! slot world matrix, which carries no scale (@see GetPlateNodeTM). Valid before the slot exists.
	//! Its column lengths are re-imposed from GetWorldScale, so the frame, the node, the slot compensation
	//! and the grid can never disagree about how big the plate is.
	Matrix34    GetPlateFrameTM() const;
	//! Inverse of a plate frame, refused when the frame is singular. The single guard against a zero scale
	//! turning every vertex it touches into NaN; Matrix34::Invert has none of its own.
	static bool InvertPlateFrameTM(const Matrix34& frameTM, Matrix34& outInvTM);
	//! The RENDER NODE's world matrix: the design frame with the size the MESH WAS BUILT AT divided out of
	//! each column. The mesh is generated in world metres, so the slot cancels the scale (@see
	//! ApplySlotScaleCompensation); the columns are unit length except during a deferred drag.
	Matrix34    GetPlateNodeTM() const;
	//! THE authoritative size of the plate in metres: x/y edge lengths, z relief height. Read ONLY from the
	//! entity world transform and the component transform - never from the slot, which carries the plate's
	//! own scale compensation, and never from the mesh, which was built from this. Components below 0.0001
	//! read as 1; nothing else is imposed, so the mesh always matches the entity scale the gizmo shows.
	Vec3        GetWorldScale() const;
	//! Size the mesh currently in the statobj was generated at (m_lastWorldScale, guarded against zero).
	Vec3        GetMeshBuildScale() const;
	//! Writes the slot local transform that cancels the size already baked into the mesh, so that
	//! IEntity::GetSlotWorldTM(slot) == GetPlateNodeTM() - unit scale whenever the mesh is up to date.
	void        ApplySlotScaleCompensation();
	void        ApplyMaterial();
	//! Half of e_TerrainIntegrateObjectsMaxVertices, or 0 when the cvar is unavailable.
	int         GetIntegrationVertexBudget() const;
	//! Resolves Grid Mode into the vertex counts along X and Y, clamping and warning as needed.
	void        ComputeEffectiveGrid(int& vertsX, int& vertsY);
	//! Drops the trimesh of the previous generation, on the statobj and on the physical entity.
	void        ReleasePhysicsGeometry();
	//! Static physical entity of our own when the type asks for a collider and no physics component owns the entity.
	void        UpdateNativePhysics();
	//! Destroys the physical entity, but only the one this component created.
	void        ReleaseNativePhysics();

	// physics surface types
	// The plate carries ONE material, so every contact would resolve to that material's surface type. A
	// contact reports the MAPPED id: GetMatId runs the raw per face id through the part's pMatMapping, and
	// terrain and objects share one global id space, so a terrain layer's id resolves exactly as terrain does.

	//! True when the plate takes its physics surface types from the terrain (property, cvar, collider, terrain).
	bool        IsPhysSurfaceFromTerrainEnabled() const;
	//! The material the plate renders and physicalises with (slot override, then the statobj's), or null.
	IMaterial*  GetPlateMaterial() const;
	//! Surface type id of the plate's own material, i.e. entry 0 of the mapping. Both the fallback where
	//! the terrain has no answer and what the part degrades to if the mapping is ever lost.
	int         GetPlateMaterialSurfaceTypeId() const;
	//! Global surface type id of the terrain under (worldX, worldY), or -1 when it has no answer. A downward
	//! ray limited to ent_terrain: its ray_hit::surface_idx IS what a footstep there would report.
	int         SampleTerrainSurfaceTypeId(float worldX, float worldY) const;
	//! Samples the kPhysSurfaceProbeGrid x kPhysSurfaceProbeGrid grid over the plate's world footprint and
	//! returns the dominant id, or -1. FNV-1a hashes the grid into outHash: no engine notification exists
	//! for a layer repaint, so that hash is the whole detection mechanism.
	int         SampleFootprintSurfaceTypes(uint32& outHash) const;
	//! Snapshots the streams the physics trimesh is built from, in the 16 bit index form CreateMesh needs.
	//! False past the 16 bit index limit. Editor only: a layer repaint rebuilds the trimesh alone.
	bool        CachePhysicsMesh(const Vec3* pPositions, int vertexCount, const vtx_idx* pIndices, int indexCount);
	//! Drops the cache above.
	void        ReleasePhysicsMeshCache();
	//! Recomputes m_physMatMapping, and m_physFaceMats on the per face path, from the terrain. Casts the rays.
	void        BuildPhysicsSurfaceMapping();
	//! Builds the collision trimesh by hand, so each triangle carries the id of the terrain under its own
	//! centroid. Returns a registered phys_geometry for IStatObj::SetPhysGeom, or null to fall back to the
	//! stock path. Every flag and BV heuristic is copied verbatim from CStatObj::PhysicalizeGeomType.
	phys_geometry* CreatePhysicsGeometryFromCache();
	//! Repaint / clobber repair: rebuilds the trimesh from the cache, touching no mesh, weld, shadow or bake.
	void        RephysicalizePlateSurface();
	//! Pushes m_physMatMapping onto the entity part with pe_params_part (the cheap half - no rays). Must run
	//! AFTER CEntityPhysics::UpdateParamsFromRenderMaterial, which would otherwise overwrite it.
	void        ApplyPhysicsSurfaceMapping();
	//! Editor only, every e_TerrainPlatePhysSurfaceProbeFrames frames: re-samples the footprint grid,
	//! re-physicalises after a repaint and heals a clobbered mapping. Independent of PollBake.
	void        PollPhysicsSurface();
	//! Tells the engine the geometry changed, so the static shadow cache and HeightMap AO are rebuilt.
	void        InvalidateShadowCache();

	// virtual bake
	//! Sector aligned square footprint of the plate, in terrain units. False when the plate cannot be
	//! baked (no terrain, degenerate transform, or a footprint too large to build a block for).
	bool  ComputeBakeRect(const SBakeTerrainMetrics& metrics, const Matrix34& worldTM, int& outX1, int& outY1, int& outSize) const;
	//! Brings m_bakeBaseline onto the current rect WITHOUT re-reading ground this plate wrote itself. A
	//! height never survives a write/read round trip through the 12 bit sector encoding, so re-capturing
	//! on every rebuild ratchets the stored ground downwards; cells whose terrain still matches the stored
	//! float keep that float, only genuinely changed and brand new cells are read from the terrain.
	void  RefreshBakeBaseline(const SBakeTerrainMetrics& metrics);
	//! e_TerrainPlateDebug >= 2 only: max drift of the baseline against its very first capture. Zero drift
	//! with no adopted cells is the invariant RefreshBakeBaseline exists to hold.
	void  LogBakeBaselineDrift(int adoptedCells, int keptCells);
	//! Builds m_bakeResult from m_bakeBaseline and the plate surface. Reports what it changed.
	void  ComputeBakeBlock(const SBakeTerrainMetrics& metrics, const Matrix34& worldTM, int& outCellsWritten, float& outMinDelta, float& outMaxDelta);
	//! Second pass of ComputeBakeBlock: the per plate Bake Offset, scaled by each cell's relief over the
	//! crest, so the welded rim does not move. Called with the crest ComputeBakeBlock measured.
	void  ApplyBakeOffset(float maxRelief);
	//! Third pass: floors m_bakeResult at 0. Terrain heights below 0 are not representable - they wrap into
	//! the 12 bit height field as huge positive spikes - so the request is cut off before it is written and
	//! before the probes are built from it, and the plate warns once. @see ClampBakeResultToTerrainRange.
	void  ClampBakeResultToTerrainRange();
	//! One heights-only SetTerrainElevation over the current rect.
	bool  WriteTerrainBlock(const SBakeTerrainMetrics& metrics, const std::vector<float>& heights);
	//! Restores the original ground over the footprint of the live bake, if there is one. Must run
	//! before anything reads the terrain again (the edge weld does) and before the plate moves.
	void  RestoreBakeBaseline();
	//! The single teardown funnel: out of the registry, original ground back, every further bake refused.
	//! Idempotent; reached from ENTITY_EVENT_DONE, OnShutDown and the destructor alike.
	void  RestoreBakeOnTeardown();
	//! Teardown half of the ordered unwind: with live plates stacked ON TOP of the footprint this plate
	//! hands back, a plain restore would write their relief into the ground as terrain nobody owns. Runs
	//! RunBakeCascade with this plate as the unwind-only member; false when a plain restore is correct.
	bool  RestoreBakeOnTeardownCascaded();
	//! Captures the baseline, computes the block and writes it. Does nothing until the level is loaded.
	void  ApplyBake();
	//! Decals, shadows and navmesh over a footprint that just changed.
	void  AfterBakeInvalidation(const AABB& box);
	//! World box of the current bake rect, from the captured baseline and the written result.
	AABB  GetBakeWorldBox(const SBakeTerrainMetrics& metrics) const;
	//! A grid of watched cells over the footprint: at least one probe per touched sector and one every
	//! e_TerrainPlateBakeProbeSpacing units, each snapped to the most changed cell around it.
	void  BuildBakeProbes(const SBakeTerrainMetrics& metrics);
	//! Compares the probes with the terrain every e_TerrainPlateBakeProbeFrames frames.
	void  PollBake();
	//! Rebuilds the baseline from what the editor left behind and re-applies the plate on top of it.
	void  ReapplyBakeAfterExternalEdit(const SBakeTerrainMetrics& metrics);
	//! After a cascade the terrain over this rect holds the COMPOSITE of the whole overlapping set, so the
	//! probe expectations and m_bakeResult are re-read here, or a lower plate reads an external edit.
	void  RefreshBakeCompositeAfterCascade(const SBakeTerrainMetrics& metrics);
	//! Warns once when the bake cost a touched sector more than half of its height precision.
	void  WarnOnBakeQuantisation(const SBakeTerrainMetrics& metrics);

	//! File static registry of terrain plates. Called from Initialize and the destructor, so no hook is needed.
	void  RegisterPlate();
	void  UnregisterPlate();
	//! Undo of RestoreBakeOnTeardown: clears the torn-down flag AND puts the plate back in the registry.
	//! Both, always - a re-armed plate outside the registry bakes where no cascade can see it.
	void  ReArmAfterTeardown();
	//! Every rect this plate occupies for the cascade, as (x1, y1, size) triples: the LIVE one and, after a
	//! move, the PROSPECTIVE one. A move must unwind both or it misses what it leaves behind or lands on.
	void  GetBakeCascadeRects(std::vector<int>& outRects) const;
	//! Every baking plate transitively overlapping this one, itself included, sorted by priority then GUID.
	//! COMPLETE, always: e_TerrainPlateBakeMaxBatch only decides when the cost of the set is reported.
	void  CollectBakeCascadeSet(std::vector<CTerrainPlateComponent*>& out) const;
	//! The closure itself, seeded with rects rather than a plate, so a plate already out of the registry (a
	//! teardown) can still ask who stands on the footprint it gives back. Appends to out and sorts it.
	static void CollectBakeCascadeSetFromRects(std::vector<int>& rects, std::vector<CTerrainPlateComponent*>& out);
	//! Cascade order: lower Bake Priority bakes first; ties by entity GUID, which is stable across loads.
	bool  BakesBefore(const CTerrainPlateComponent& other) const;
	//! ApplyBake when the plate owns its footprint alone, the whole cascade when it shares it.
	void  ApplyBakeCascaded();
	//! The cascade driver, shared by ApplyBakeCascaded and the move batch: restore every member in
	//! DESCENDING priority (the baselines nest), regenerate and re-bake in ASCENDING priority, then refresh
	//! the composite expectations. pRemoved unwinds with the set but is not re-baked.
	static void RunBakeCascade(std::vector<CTerrainPlateComponent*>& affected, CTerrainPlateComponent* pSeed, const char* szReason,
	                           CTerrainPlateComponent* pRemoved = nullptr);

	// move batch: several plates at once
	//! Puts this plate on the list of plates whose move still owes a restore / re-bake, and pulls the
	//! settle of every other deferring baking plate forward so that one gesture produces ONE batch.
	void        JoinBakeMoveBatch();
	//! Takes this plate off that list. Called from the teardown, before the pointer can go stale.
	void        LeaveBakeMoveBatch();
	//! Restores the OLD footprints of every plate that moved in the same gesture, then regenerates and
	//! re-bakes them through RunBakeCascade. Once per batch, so restores and bakes cannot interleave.
	static void FlushBakeMoveBatch();
	//! Second line of defence against a missed transform: a held rect that no longer touches the plate's
	//! current footprint is an orphan nobody will restore. Puts it back and warns - this is a bug report.
	bool  HealOrphanedBakeRect(const SBakeTerrainMetrics& metrics);
	//! Logs once that this plate shares its footprint with another one, now that the cascade handles it.
	void  LogBakeOverlapOnce(const CTerrainPlateComponent& other);
	//! Relief at a local XY, in the unit plate's 0..1 local Z. Reproduces the surface RegenerateMesh
	//! generated: sampled on the MESH GRID and interpolated between those vertices, Normalize and Mirror
	//! included. Sampling the source directly returns detail the mesh never had and sinks the bake.
	float GetPlateReliefAt(float lx, float ly) const;
	//! Projects a world XY straight down onto the plate surface. False when it misses the plate.
	bool  SamplePlateSurface(const Matrix34& worldTM, float worldX, float worldY, float& outWorldZ, float& outLocalX, float& outLocalY) const;
	//! Frames between two probe passes, from e_TerrainPlateBakeProbeFrames. 0 disables the polling.
	static int GetBakeProbeFrames();
	//! Entity name for the log lines, never null even before the component has an entity.
	const char* GetPlateName() const;
	//! Names why the bake chain did nothing, under e_TerrainPlateDebug 1. Every early out goes through here.
	void  LogBakeSkip(const char* szReason) const;
	//! Reads the most changed cells back out of the terrain and compares. SetTerrainElevation reports
	//! nothing back, so this is the only place a failed write is noticed. Warns once, false if nothing moved.
	bool  VerifyBakeWrite(const SBakeTerrainMetrics& metrics);
	//! e_TerrainPlateDebug 2: the sector aligned bake rect as a wire box over the baked terrain.
	void  DrawBakeDebug() const;

	ETerrainPlateSource                          m_sourceMode = ETerrainPlateSource::HeightmapFile;
	Schematyc::TerrainHeightmapFileName          m_heightmapPath;
	Schematyc::TextureFileName                   m_displacementPath;
	Schematyc::MaterialFileName                  m_materialPath;

	ETerrainPlateGridMode                        m_gridMode = ETerrainPlateGridMode::Fixed;
	Schematyc::Range<2, 513, 2, 513, int>        m_grid = 65;
	Schematyc::Range<0, 1000, 0, 10, float>      m_vertexSpacing = 0.1f;
	Schematyc::CSharedString                     m_effectiveGridInfo;
	bool                                         m_bNormalize = true;
	bool                                         m_bMirrorX = false;
	bool                                         m_bMirrorY = false;
	//! Reads the relief upside down (h -> 1 - h, after Normalize): a hill map becomes a pit map.
	bool                                         m_bMirrorZ = false;
	//! Which height of the normalised relief sits on the plate's ground. 0 = the old behaviour, everything rises.
	Schematyc::Range<0, 1, 0, 1, float>          m_groundLevel = 0.f;

	bool                                         m_bEdgeWeld = true;
	Schematyc::Range<0, 1, 0, 1, float>          m_weldInnerFalloff = 0.25f;
	Schematyc::Range<0, 100, 0, 2, float>        m_weldSink = 0.1f;
	Schematyc::Range<0, 1000, 0, 20, float>      m_skirt = 0.25f;
	bool                                         m_bUpdateDuringDrag = false;
	//! Contacts report the terrain layer's surface type, not the plate material's. Off = pre-feature behaviour.
	bool                                         m_bPhysSurfaceFromTerrain = true;
	bool                                         m_bBakeIntoTerrain = false;
	ETerrainPlateBakeMode                        m_bakeMode = ETerrainPlateBakeMode::Raise;
	Schematyc::Range<-128, 127, -128, 127, int>  m_bakePriority = 0;
	//! Per plate bake height refinement in metres, signed. 0 at the welded rim, full value at the crest.
	Schematyc::Range<-100, 100, -1, 1, float>    m_bakeOffset = 0.f;
	//! Stamp Colour: this plate's colour is in the level's own CRGBLayer now, so the live preview and the
	//! export must not blend it over itself. Serialised, because the ground keeps the colour across a reload
	//! while the plate is unchanged. Mutable: IsColourStamped is the query that finds the record stale (the
	//! plate moved, was rescaled or took a new material) and drops it there and then.
	mutable bool                                 m_bColourStamped = false;

	//! What the plate looked like when its colour was stamped, taken lazily on the first query after the
	//! stamp or after a level load. Only compared, never dereferenced. @see IsColourStamped.
	mutable bool                                 m_bColourStampRefValid = false;
	mutable Matrix34                             m_colourStampFrameTM = Matrix34(IDENTITY);
	mutable const IMaterial*                     m_pColourStampMaterial = nullptr;
	//! When the stamp happened, so the property change the button itself provokes cannot clear it.
	uint32                                       m_colourStampFrameId = 0;

	//! The two Bake drawer buttons. No data, so no AddMember: the drawer archives them directly, and that
	//! archive is edit only. @see STerrainPlateBakeGroup::Serialize.
	Serialization::FunctorActionButton<std::function<void()>> m_stampHeightButton;
	Serialization::FunctorActionButton<std::function<void()>> m_stampColourButton;

	//! The live, editor only terrain colour overlay. Default OFF - it repaints ground the user did not paint.
	bool                                         m_bLiveTerrainColour = false;

	//! Everything GetCommitAlbedoAtWorld would otherwise look up per texel. Resolved at most once per frame,
	//! never serialised and never valid across a frame - which keeps the borrowed texel pointer safe.
	struct SColourSource
	{
		//! The renderer's low resolution system copy. Owned by the renderer, borrowed for one frame.
		const ColorB* pTexels = nullptr;
		int           width = 0;
		int           height = 0;
		//! The copy of an sRGB texture is stored LINEAR; of anything else, raw.
		bool          bTexelsLinear = false;
		//! The material's own diffuse colour, multiplied over the texture the way the shader does it.
		ColorF        diffuse = ColorF(1.f, 1.f, 1.f, 1.f);
		//! The material's texture modifier, so a tiled material tiles on the terrain too.
		float         tilingU = 1.f;
		float         tilingV = 1.f;
		float         offsetU = 0.f;
		float         offsetV = 0.f;

		//! Identity of what was resolved, so a material or texture swap re-resolves. Opaque: no ITexture here.
		const void*   pTextureKey = nullptr;
		int           maxTexSize = 0;
		int           frameId = -1;
		bool          bValid = false;
		//! One warning per plate per texture, not one per texel.
		bool          bWarned = false;
	};
	mutable SColourSource                        m_colourSource;

	//! Fills m_colourSource for this frame. False when there is nothing to sample.
	bool ResolveColourSource(float texelSizeMeters) const;

	//! Panel grouping only - no data of their own, never persisted values. @see STerrainPlateUIGroup.
	STerrainPlateShapeGroup                      m_uiShape;
	STerrainPlateBakeGroup                       m_uiBake;
	STerrainPlateWeldGroup                       m_uiWeld;
	STerrainPlateEditorGroup                     m_uiEditor;

	// Cached state, never serialised: the mesh is always rebuilt on load.
	STerrainPlateHeightField m_heightField;
	string                   m_loadedSource;
	ETerrainPlateSource      m_loadedSourceMode = ETerrainPlateSource::HeightmapFile;
	bool                     m_bSourceLoaded = false;
	bool                     m_bRegenPending = false;
	//! True while a rebuild is held back until the end of an editor transform gesture.
	bool                     m_bDeferredXformPending = false;
	//! True while this plate waits in the move batch, i.e. the terrain still holds its PRE-move bake.
	bool                     m_bBakeMoveBatchPending = false;
	//! Frame of the last ENTITY_EVENT_XFORM that was held back, for the settle fallback.
	uint32                   m_lastXformFrameId = 0;
	bool                     m_bBudgetWarningIssued = false;
	bool                     m_bBudgetClampWarningIssued = false;
	//! True while the entity's physical entity is the static one this component created.
	bool                     m_bOwnsPhysicalEntity = false;
	bool                     m_bPhysMeshWarningIssued = false;
	uint32                   m_lastRegenFrameId = 0;
	//! World scale the current mesh was generated for. The mesh itself is scale independent in XY, but
	//! the normals, the weld, the skirt and the Spacing grid are not, so a scale change regenerates.
	Vec3                     m_lastWorldScale = Vec3(1.f, 1.f, 1.f);
	//! Fires once if a rebuild ever changes the plate's OWN size - the signature of a feedback loop, where
	//! the size is read back out of something the rebuild wrote. @see RegenerateMesh.
	bool                     m_bSizeFeedbackWarned = false;
	//! Vertex counts the last RegenerateMesh actually produced; the bake evaluates the same piecewise
	//! surface. 0 until the first generation, when GetPlateReliefAt falls back to the source resolution.
	int                      m_lastGridX = 0;
	int                      m_lastGridY = 0;
	//! FNV-1a of the position stream of the last generated mesh; 0 until the first generation.
	uint32                   m_geometryHash = 0;
	_smart_ptr<IStatObj>     m_pStatObj;

	// virtual bake, runtime only. Nothing here is reflected or serialised: the level file keeps the
	// pristine CHeightmap, so the baseline is re-captured from scratch each session.
	//! Offset / scale that turn a raw source sample into the unit plate's local Z, as the last Normalize
	//! pass solved them, so the baked relief and the rendered mesh are the same surface.
	float                    m_reliefOffset = 0.f;
	float                    m_reliefScale = 1.f;
	//! Mirror Z and Ground Level, folded into one affine step applied AFTER the Normalize rescale:
	//! h = h * m_reliefSign + m_reliefShift. Defaults are the identity, i.e. the pre-feature relief.
	float                    m_reliefSign = 1.f;
	float                    m_reliefShift = 0.f;
	//! Ground Level as the last property change saw it, so the Bake Mode guard fires on the 0 -> > 0 edit
	//! only. Seeded in Initialize, so loading a plate that already has one is not an edit. @see ProcessEvent.
	float                    m_lastGroundLevel = 0.f;
	//! True while the terrain holds this plate's bake over m_bakeRect*.
	bool                     m_bBakeApplied = false;
	//! Global order in which the live bakes landed. The cascade restores in DESCENDING order of THIS, not
	//! of Bake Priority: the baselines in the ground are a stack pushed in the old order. 0 = never baked.
	uint64                   m_bakeApplySeq = 0;
	//! True until the level is loaded: never bake from Initialize().
	bool                     m_bInitialBakePending = true;
	//! True once the plate has been torn down; it must never bake again, or a teardown running before the
	//! last update tick would put the relief straight back. Cleared again by a property change.
	bool                     m_bBakeTornDown = false;
	//! Sector aligned square footprint of the live bake, in terrain units.
	int                      m_bakeRectX1 = 0;
	int                      m_bakeRectY1 = 0;
	int                      m_bakeRectSize = 0;
	//! (m_bakeRectSize + 1)^2 heights, row major, index (ux - X1) * dim + (uy - Y1); ux is along
	//! world X and uy along world Y, the order SetTerrainElevation itself uses.
	std::vector<float>       m_bakeBaseline;   //!< the original ground, restored on any change
	std::vector<float>       m_bakeResult;     //!< what the last bake actually wrote
	//! The rect m_bakeBaseline was captured over. Usually the live rect, but it is kept separately so a
	//! move can carry the overlapping cells' float values over instead of re-reading them. Size 0 = none.
	int                      m_bakeBaselineX1 = 0;
	int                      m_bakeBaselineY1 = 0;
	int                      m_bakeBaselineSize = 0;
	//! e_TerrainPlateDebug >= 2 only: the first baseline this plate ever captured, and its rect.
	std::vector<float>       m_bakeBaselineFirst;
	int                      m_bakeBaselineFirstX1 = 0;
	int                      m_bakeBaselineFirstY1 = 0;
	int                      m_bakeBaselineFirstSize = 0;
	std::vector<SBakeProbe>  m_bakeProbes;
	//! e_TerrainPlateBakeEpsilon / e_TerrainPlateBakeSlopeCorrect as the live bake used them. A cvar
	//! change does not move the terrain, so the probes cannot see it; PollBake compares these instead.
	float                    m_bakeEpsilonUsed = 0.f;
	int                      m_bakeSlopeCorrectUsed = 0;
	//! Same, for the two knobs that shape the rim: e_TerrainPlateBakeEdgeAA and e_TerrainPlateBakeWeldCurve.
	//! Without this the probes would repair the terrain back to the old rim shape for ever.
	int                      m_bakeEdgeAAUsed = 0;
	int                      m_bakeWeldCurveUsed = 0;
	//! Same, for the per plate Bake Offset. The belt to Rebuild's braces: if the value ever reaches the
	//! component without a re-bake, PollBake repairs instead of reading an external edit for ever.
	float                    m_bakeOffsetUsed = 0.f;
	//! A few quantisation steps of the coarsest touched sector: a height never round trips exactly.
	float                    m_bakeProbeTolerance = 0.01f;
	uint32                   m_lastBakeProbeFrameId = 0;
	//! Runaway guard: probe repairs run since m_bakeReapplyWindowStart, and the extra frames waited between
	//! two polls because of them. Reset as soon as one poll finds the plate at rest.
	uint32                   m_bakeReapplyWindowStart = 0;
	int                      m_bakeReapplyCount = 0;
	int                      m_bakeBackoffFrames = 0;
	bool                     m_bBakeRunawayWarned = false;
	bool                     m_bBakeOverlapWarningIssued = false;
	bool                     m_bBakeQuantWarningIssued = false;
	bool                     m_bBakeSizeWarningIssued = false;
	//! One warning per plate for a bake whose write never landed, cleared by the next one that does.
	bool                     m_bBakeWriteWarningIssued = false;
	//! Did the last write actually reach the terrain? False makes RefreshBakeBaseline keep the stored
	//! baseline instead of adopting whatever the refused write left in the ground. @see VerifyBakeWrite.
	bool                     m_bBakeWriteLanded = true;
	//! One warning per plate for a bake asking the terrain to go below 0 m, cleared when it stops.
	bool                     m_bBakeFloorWarningIssued = false;
	//! e_TerrainPlateDebug 1: says once, per plate, that the component really is being ticked. If this
	//! line never appears the plate never received ENTITY_EVENT_UPDATE and no bake trigger can fire.
	bool                     m_bUpdateTickLogged = false;

	// physics surface types, runtime only
	//! Plate local index -> global ISurfaceTypeManager id, pushed onto the entity part with pe_params_part.
	//! Entry 0 is always the plate's own material surface type, so an out of range face id clamps to today's
	//! behaviour. Empty = inactive. At most kPhysSurfaceMaxMappingEntries: nMats is an unsigned:7.
	std::vector<int>         m_physMatMapping;
	//! One plate-local index per TRIANGLE of the collision mesh, i.e. mesh_data::pMats. Empty means the
	//! single dominant id path (per face refused: a multi sub material, too many layers, or a mesh over the
	//! 16 bit index limit). char, not int: CreateMesh takes char* and the part's nMats is an unsigned:7.
	std::vector<char>        m_physFaceMats;
	//! The mesh the hand-built trimesh is made of, in the 16 bit index form CreateMesh needs. Released
	//! again outside the editor as soon as the trimesh exists; only a layer repaint needs to keep it.
	std::vector<Vec3>        m_physVertices;
	std::vector<uint16>      m_physIndices;
	//! FNV-1a of the footprint probe grid's surface ids, as the live mapping was built from them. No engine
	//! notification exists for a layer repaint, so this hash is the whole detection mechanism.
	uint32                   m_physSurfaceHash = 0;
	//! Mapping entries the part was last given, so the poll can notice a clobber without re-sampling.
	int                      m_physMatsApplied = 0;
	uint32                   m_lastPhysSurfaceProbeFrameId = 0;
	//! One line per plate, per reason (multi sub material refusal, sampling/mapping fallback). Debug only.
	bool                     m_bPhysSurfaceMultiSubWarned = false;
	bool                     m_bPhysSurfaceFallbackWarned = false;
};
}
}
