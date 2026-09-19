// Copyright 2017-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IShader.h>

#include <CryPhysics/physinterface.h>

#include <Cry3DEngine/IStatObj.h>

#include <CrySchematyc/ResourceTypes.h>
#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>

#include "DefaultComponents/ComponentHelpers/PhysicsParameters.h"

namespace Cry
{
namespace DefaultComponents
{
enum class EMeshType : uint32
{
	None              = 0,
	Render            = 1 << 0,
	Collider          = 1 << 1,
	RenderAndCollider = Render | Collider,
};

static void ReflectType(Schematyc::CTypeDesc<EMeshType>& desc)
{
	desc.SetGUID("{A786ABAF-3B6F-4EB4-A073-502A87EC5F64}"_cry_guid);
	desc.SetLabel("Mesh Type");
	desc.SetDescription("Determines what to use a mesh for");
	desc.SetDefaultValue(EMeshType::RenderAndCollider);
	desc.AddConstant(EMeshType::None, "None", "Disabled");
	desc.AddConstant(EMeshType::Render, "Render", "Render");
	desc.AddConstant(EMeshType::Collider, "Collider", "Collider");
	desc.AddConstant(EMeshType::RenderAndCollider, "RenderAndCollider", "Render and Collider");
}

// Used to indicate the minimum graphical setting for an effect
enum class EMiniumSystemSpec
{
	Disabled = 0,
	Always,
	Medium,
	High,
	VeryHigh
};

static void ReflectType(Schematyc::CTypeDesc<EMiniumSystemSpec>& desc)
{
	desc.SetGUID("{9DDF8F33-CB8C-4BEE-A539-01BC8DAFED2E}"_cry_guid);
	desc.SetLabel("Minimum Graphical Setting");
	desc.SetDescription("Minimum graphical setting to enable an effect");
	desc.SetDefaultValue(EMiniumSystemSpec::Disabled);
	desc.AddConstant(EMiniumSystemSpec::Disabled, "None", "None");
	desc.AddConstant(EMiniumSystemSpec::Always, "Low", "Low");
	desc.AddConstant(EMiniumSystemSpec::Medium, "Medium", "Medium");
	desc.AddConstant(EMiniumSystemSpec::High, "High", "High");
	desc.AddConstant(EMiniumSystemSpec::VeryHigh, "VeryHigh", "Very High");
}

// Used to indicate the minimum graphical setting for an effect
enum class EMeshGIMode
{
	Disabled             = IRenderNode::EGIMode::eGM_None,
	StaticVoxelization   = IRenderNode::EGIMode::eGM_StaticVoxelization,
	AnalyticalProxySoft  = IRenderNode::EGIMode::eGM_AnalyticalProxy_Soft,
	AnalyticalProxyHard  = IRenderNode::EGIMode::eGM_AnalyticalProxy_Hard,
	AnalyticalOccluder   = IRenderNode::EGIMode::eGM_AnalytPostOccluder,
	IntegrateIntoTerrain = IRenderNode::EGIMode::eGM_IntegrateIntoTerrain
};

static void ReflectType(Schematyc::CTypeDesc<EMeshGIMode>& desc)
{
	desc.SetGUID("{9C646BE3-861D-44E7-88EA-BD2252557D7C}"_cry_guid);
	desc.SetLabel("Global Illumination Mode");
	desc.SetDefaultValue(EMeshGIMode::Disabled);
	desc.AddConstant(EMeshGIMode::Disabled, "None", "Disabled");
	desc.AddConstant(EMeshGIMode::StaticVoxelization, "StaticVoxelization", "Static Voxelization");
	desc.AddConstant(EMeshGIMode::AnalyticalProxySoft, "AnalyticalProxySoft", "Analytical Proxy Soft");
	desc.AddConstant(EMeshGIMode::AnalyticalProxyHard, "AnalyticalProxyHard", "Analytical Proxy Hard");
	desc.AddConstant(EMeshGIMode::AnalyticalOccluder, "AnalyticalOccluder", "Analytical Occluder");
	desc.AddConstant(EMeshGIMode::IntegrateIntoTerrain, "IntegrateIntoTerrain", "Integrate Into Terrain");
}

static void UpdateGIModeEntitySlotFlags(uint8 giModeFlags, uint32& slotFlags)
{
	if (((uint8)giModeFlags & 1) != 0)
	{
		slotFlags |= ENTITY_SLOT_GI_MODE_BIT0;
	}
	else
	{
		slotFlags &= ~ENTITY_SLOT_GI_MODE_BIT0;
	}

	if (((uint8)giModeFlags & 2) != 0)
	{
		slotFlags |= ENTITY_SLOT_GI_MODE_BIT1;
	}
	else
	{
		slotFlags &= ~ENTITY_SLOT_GI_MODE_BIT1;
	}

	if (((uint8)giModeFlags & 4) != 0)
	{
		slotFlags |= ENTITY_SLOT_GI_MODE_BIT2;
	}
	else
	{
		slotFlags &= ~ENTITY_SLOT_GI_MODE_BIT2;
	}
}

struct SRenderParameters
{
	inline bool operator==(const SRenderParameters& rhs) const { return 0 == memcmp(this, &rhs, sizeof(rhs)); }

	// Declaring Serialize() makes Schematyc prefer it over the reflected member walk, which is how the
	// terrain parameters below get hidden unless the GI mode asks for them (same trick as
	// SPhysicsParameters hiding Mass behind Weight Type). The AddMember reflection is still needed for
	// graph member access, flowgraph and Track View, and the names here must match it.
	// Only the editor archive branches - every other archive runs with isEdit() == false and always
	// reads and writes all members, so a hidden value survives a mode switch, a save and a reload.
	void Serialize(Serialization::IArchive& archive)
	{
		archive(m_castShadowSpec, "ShadowSpec", "Minimum Shadow Graphics");
		archive(m_bIgnoreVisAreas, "IgnoreVisArea", "Ignore Visareas");
		archive(m_bIgnoreTerrainLayerBlend, "IgnoreTerrainLayerBlend", "Ignore Terrain Layer Blending");
		archive(m_bIgnoreDecalBlend, "IgnoreDecalBlend", "Ignore Decal Blending");
		archive(m_bStaticGeometry, "StaticGeometry", "Static Geometry");
		archive(m_viewDistanceRatio, "ViewDistRatio", "View Distance");
		archive(m_lodDistance, "LODDistance", "LOD Distance");
		archive(m_giMode, "GIMode", "GI and Usage Mode");

		if (!archive.isEdit() || m_giMode == EMeshGIMode::IntegrateIntoTerrain)
		{
			archive(m_terrainSlopeLimit, "TerrainSlopeLimit", "Terrain Slope Limit");
			archive(m_terrainSlopeBlend, "TerrainSlopeBlend", "Terrain Slope Blend");
			archive(m_terrainHeightBand, "TerrainHeightBand", "Terrain Height Band");
		}
	}

	EMiniumSystemSpec                     m_castShadowSpec = EMiniumSystemSpec::Always;
	bool                                  m_bIgnoreVisAreas = false;
	bool                                  m_bIgnoreTerrainLayerBlend = false;
	bool                                  m_bIgnoreDecalBlend = false;
	bool                                  m_bStaticGeometry = false;
	Schematyc::Range<0, 100, 0, 100, int> m_viewDistanceRatio = 100;
	Schematyc::Range<0, 100, 0, 100, int> m_lodDistance = 100;
	EMeshGIMode                           m_giMode = EMeshGIMode::Disabled;

	// Per instance parameters of GI Mode = Integrate Into Terrain, @see STerrainIntegrationParams.
	Schematyc::Range<0, 90, 0, 90, float>       m_terrainSlopeLimit = 90.f;
	Schematyc::Range<0, 90, 0, 90, float>       m_terrainSlopeBlend = 0.f;
	Schematyc::Range<-1, 1000, -1, 1000, float> m_terrainHeightBand = -1.f;
};

static void ReflectType(Schematyc::CTypeDesc<SRenderParameters>& desc)
{
	desc.SetGUID("{291C60F6-A493-41B3-AC63-B9E0187DC074}"_cry_guid);
	desc.AddMember(&SRenderParameters::m_castShadowSpec, 'shad', "ShadowSpec", "Minimum Shadow Graphics", "Minimum graphical setting to cast shadows from this light", EMiniumSystemSpec::Always);
	desc.AddMember(&SRenderParameters::m_bIgnoreVisAreas, 'visa', "IgnoreVisArea", "Ignore Visareas", "Whether this component will ignore vis areas", false);
	desc.AddMember(&SRenderParameters::m_bIgnoreTerrainLayerBlend, 'itlb', "IgnoreTerrainLayerBlend", "Ignore Terrain Layer Blending", "Whether this component will ignore terrain layer blending", false);
	desc.AddMember(&SRenderParameters::m_bIgnoreDecalBlend, 'idb', "IgnoreDecalBlend", "Ignore Decal Blending", "Whether this component will ignore decal blending", false);
	desc.AddMember(&SRenderParameters::m_bStaticGeometry, 'stgm', "StaticGeometry", "Static Geometry",
	               "Enables deferred decals and static object treatment for a mesh that never moves and has no physics. "
	               "Has no effect when the entity already has a static physical entity.", false);
	desc.AddMember(&SRenderParameters::m_viewDistanceRatio, 'view', "ViewDistRatio", "View Distance", "View distance from 0 to 100, 100 being always visible", 100);
	desc.AddMember(&SRenderParameters::m_lodDistance, 'lodd', "LODDistance", "LOD Distance", "Level of Detail distance from 0 to 100, 100 being always best LOD", 100);
	desc.AddMember(&SRenderParameters::m_giMode, 'gimo', "GIMode", "GI and Usage Mode", "The way object is used by GI and by some other systems", EMeshGIMode::Disabled);
	desc.AddMember(&SRenderParameters::m_terrainSlopeLimit, 'tsll', "TerrainSlopeLimit", "Terrain Slope Limit",
	               "Only applies with GI and Usage Mode = Integrate Into Terrain, and only when the level flag Terrain/IntegrateObjects is on. "
	               "Maximum angle in degrees between a face of this mesh and the horizontal plane for that face to receive the terrain layers. "
	               "0 keeps only horizontal faces, 90 (the default) accepts every face and is the stock behaviour.", 90.f);
	desc.AddMember(&SRenderParameters::m_terrainSlopeBlend, 'tslb', "TerrainSlopeBlend", "Terrain Slope Blend",
	               "Only applies with GI and Usage Mode = Integrate Into Terrain, and only when the level flag Terrain/IntegrateObjects is on. "
	               "Width in degrees of the fade out band directly below Terrain Slope Limit: inside it the terrain layers fade out instead of "
	               "stopping at a hard edge. 0 (the default) is a hard edge.", 0.f);
	desc.AddMember(&SRenderParameters::m_terrainHeightBand, 'tshb', "TerrainHeightBand", "Terrain Height Band",
	               "Only applies with GI and Usage Mode = Integrate Into Terrain, and only when the level flag Terrain/IntegrateObjects is on. "
	               "Per object override in metres of e_TerrainIntegrateObjectsMaxHeight. It does NOT limit how high the relief may rise: it is "
	               "the maximum gap between this mesh and the terrain under it before the mesh counts as lifted clear of the ground and keeps "
	               "its own material instead of the terrain layers. -1 (the default) uses the global cvar value.", -1.f);
}

// Base implementation for our physics mesh components
class CBaseMeshComponent
	: public IEditorEntityComponent
{
protected:
	//! Terrain integration reads LOD0 triangles on the CPU when a sector rebuilds, so an integrating mesh
	//! must not be streamable. Mirrors CEntityRender::LoadGeometry (RenderProxy.cpp), which passes
	//! bUseStreaming = false to LoadStatObj for slots whose GI mode is Integrate Into Terrain; reloading an
	//! already loaded path with that flag makes CObjManager call CStatObj::DisableStreaming on it, which is
	//! also how a GI mode switched to Integrate after the load gets its mesh back in memory.
	static void MakeStatObjTerrainResident(IStatObj* pStatObj)
	{
		if (pStatObj == nullptr || gEnv == nullptr || gEnv->p3DEngine == nullptr)
			return;

		// Cloned and procedurally built objects are CPU side and already resident (CStatObj starts with
		// m_bCanUnload false), and they have no file to reload.
		if ((pStatObj->GetFlags() & (STATIC_OBJECT_CLONE | STATIC_OBJECT_GENERATED)) != 0)
			return;

		const char* szFilePath = pStatObj->GetFilePath();
		if (szFilePath == nullptr || szFilePath[0] == 0)
			return;

		gEnv->p3DEngine->LoadStatObj(szFilePath, nullptr, nullptr, false);
	}

	// IEntityComponent
	virtual void ProcessEvent(const SEntityEvent& event) override
	{
		switch (event.event)
		{
		case ENTITY_EVENT_SLOT_CHANGED:
		{
			if (event.nParam[0] /* Updated Slot Id */ == GetEntitySlotId())
			{

		case ENTITY_EVENT_PHYSICAL_TYPE_CHANGED:
				ApplyBaseMeshProperties();
			}
		}
		break;

		case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		{
			switch (m_physics.m_weightType)
			{
			case SPhysicsParameters::EWeightType::Mass:
			{
				m_physics.m_mass = max((float)m_physics.m_mass, SPhysicsParameters::s_weightTypeEpsilon);
			}
			break;
			case SPhysicsParameters::EWeightType::Density:
			{
				m_physics.m_density = max((float)m_physics.m_density, SPhysicsParameters::s_weightTypeEpsilon);
			}
			break;
			}

			m_pEntity->UpdateComponentEventMask(this);

			ApplyBaseMeshProperties();
		}
		break;
		}
	}

	virtual Cry::Entity::EventFlags GetEventMask() const override
	{
		Cry::Entity::EventFlags bitFlags = ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED | ENTITY_EVENT_SLOT_CHANGED;

		if (((uint32)m_type & (uint32)EMeshType::Collider) != 0)
		{
			bitFlags |= ENTITY_EVENT_PHYSICAL_TYPE_CHANGED;
		}

		return bitFlags;
	}
	// ~IEntityComponent

	void ApplyBaseMeshProperties()
	{
		if (((uint32)m_type & (uint32)EMeshType::Render) != 0)
		{
			m_pEntity->SetSlotFlags(GetEntitySlotId(), ENTITY_SLOT_RENDER);
		}
		else
		{
			m_pEntity->SetSlotFlags(GetEntitySlotId(), 0);
		}

		if (IRenderNode* pRenderNode = m_pEntity->GetSlotRenderNode(GetEntitySlotId()))
		{
			uint32 slotFlags = m_pEntity->GetSlotFlags(GetEntitySlotId());
			if (m_renderParameters.m_castShadowSpec != EMiniumSystemSpec::Disabled && (int)gEnv->pSystem->GetConfigSpec() >= (int)m_renderParameters.m_castShadowSpec)
			{
				slotFlags |= ENTITY_SLOT_CAST_SHADOW;
			}
			if (m_renderParameters.m_bIgnoreVisAreas)
			{
				slotFlags |= ENTITY_SLOT_IGNORE_VISAREAS;
			}
			if (!m_renderParameters.m_bIgnoreTerrainLayerBlend)
			{
				slotFlags |= ENTITY_SLOT_ALLOW_TERRAIN_LAYER_BLEND;
			}
			if (!m_renderParameters.m_bIgnoreDecalBlend)
			{
				slotFlags |= ENTITY_SLOT_ALLOW_DECAL_BLEND;
			}
			// Without this a slot whose entity has no physical entity is marked ERF_MOVES_EVERY_FRAME and
			// receives no deferred decals. m_bForceStaticGeometry is for meshes that cannot move by
			// construction; the property is the opt in for ordinary meshes.
			if (m_renderParameters.m_bStaticGeometry || m_bForceStaticGeometry)
			{
				slotFlags |= ENTITY_SLOT_STATIC_GEOMETRY;
			}
				
			UpdateGIModeEntitySlotFlags((uint8)m_renderParameters.m_giMode, slotFlags);

			const uint32 integrateIntoTerrainBits =
				(((uint8)EMeshGIMode::IntegrateIntoTerrain & 1) ? ENTITY_SLOT_GI_MODE_BIT0 : 0) |
				(((uint8)EMeshGIMode::IntegrateIntoTerrain & 2) ? ENTITY_SLOT_GI_MODE_BIT1 : 0) |
				(((uint8)EMeshGIMode::IntegrateIntoTerrain & 4) ? ENTITY_SLOT_GI_MODE_BIT2 : 0);

			// Request update of terrain mesh if IntegrateIntoTerrain state was modified
			if ((slotFlags & (ENTITY_SLOT_GI_MODE_BIT0 | ENTITY_SLOT_GI_MODE_BIT1 | ENTITY_SLOT_GI_MODE_BIT2)) == integrateIntoTerrainBits)
			{
				AABB nodeBox = pRenderNode->GetBBox();
				gEnv->p3DEngine->GetITerrain()->ResetTerrainVertBuffers(&nodeBox);
			}

			m_pEntity->SetSlotFlags(GetEntitySlotId(), slotFlags);

			// Covers a GI mode switched to Integrate after the mesh was already loaded streamed.
			if (m_renderParameters.m_giMode == EMeshGIMode::IntegrateIntoTerrain)
			{
				MakeStatObjTerrainResident(m_pEntity->GetStatObj(GetEntitySlotId()));
			}

			// The slot stores these and rebuilds the covered terrain sectors when they change.
			STerrainIntegrationParams terrainIntegrationParams;
			terrainIntegrationParams.slopeLimitDeg = m_renderParameters.m_terrainSlopeLimit;
			terrainIntegrationParams.slopeBlendDeg = m_renderParameters.m_terrainSlopeBlend;
			terrainIntegrationParams.heightBand = m_renderParameters.m_terrainHeightBand;
			m_pEntity->SetSlotTerrainIntegrationParams(GetEntitySlotId(), terrainIntegrationParams);

			// We want to manage our own view distance ratio
			m_pEntity->AddFlags(ENTITY_FLAG_CUSTOM_VIEWDIST_RATIO);
			pRenderNode->SetViewDistRatio((int)floor(((float)m_renderParameters.m_viewDistanceRatio / 100.f) * 255));
			pRenderNode->SetLodRatio((int)floor(((float)m_renderParameters.m_lodDistance / 100.f) * 255));
		}

		m_pEntity->UnphysicalizeSlot(GetEntitySlotId());

		if (((uint32)m_type & (uint32)EMeshType::Collider) != 0)
		{
			if (IPhysicalEntity* pPhysicalEntity = m_pEntity->GetPhysicalEntity())
			{
				SEntityPhysicalizeParams physParams;
				physParams.nSlot = GetEntitySlotId();
				physParams.type = pPhysicalEntity->GetType();
				physParams.nLod = m_ragdollLod;
				physParams.fStiffnessScale = m_ragdollStiffness;

				switch (m_physics.m_weightType)
				{
				case SPhysicsParameters::EWeightType::Mass:
					{
						physParams.mass = m_physics.m_mass;
						physParams.density = -1.0f;
					}
					break;
				case SPhysicsParameters::EWeightType::Density:
					{
						physParams.density = m_physics.m_density;
						physParams.mass = -1.0f;
					}
					break;
				case SPhysicsParameters::EWeightType::Immovable:
					{
						physParams.density = -1.0f;
						physParams.mass = 0.0f;
					}
					break;
				}

				m_pEntity->PhysicalizeSlot(GetEntitySlotId(), physParams);
			}
		}
	}

public:
	virtual void SetType(EMeshType type)
	{
		m_type = type;

		ApplyBaseMeshProperties();
	}
	EMeshType                   GetType() const              { return m_type; }

	virtual SPhysicsParameters& GetPhysicsParameters()       { return m_physics; }
	const SPhysicsParameters&   GetPhysicsParameters() const { return m_physics; }

	//! Set by derived components whose geometry never moves; ORs ENTITY_SLOT_STATIC_GEOMETRY into the
	//! slot flags regardless of SRenderParameters::m_bStaticGeometry.
	bool  m_bForceStaticGeometry = false;

	int   m_ragdollLod       = 1;
	float m_ragdollStiffness = 0;

protected:
	SPhysicsParameters m_physics;
	EMeshType          m_type = EMeshType::RenderAndCollider;
	SRenderParameters  m_renderParameters;
};
}
}
