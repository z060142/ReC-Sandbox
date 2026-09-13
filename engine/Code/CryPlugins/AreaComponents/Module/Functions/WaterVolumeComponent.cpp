// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "WaterVolumeComponent.h"
#include "SplineSectors.h"

#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/IRenderNode.h>
#include <Cry3DEngine/IMaterial.h>
#include <CryRenderer/IShader.h>
#include <CryPhysics/IPhysics.h>
#include <CryPhysics/physinterface.h>

namespace Cry
{
namespace AreaComponents
{

namespace
{

//! FNV-1a, the digest the distributor already uses for the same job (report 05 section 5d).
struct SWaterSignature
{
	uint64 hash = 0xCBF29CE484222325ull;

	void Mix(uint64 value)
	{
		hash ^= value;
		hash *= 0x100000001B3ull;
	}

	void MixFloat(float value)
	{
		uint32 bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		Mix(bits);
	}

	void MixVec(const Vec3& value)
	{
		MixFloat(value.x);
		MixFloat(value.y);
		MixFloat(value.z);
	}

	void MixString(const string& value)
	{
		for (const char c : value)
			Mix(static_cast<uint64>(static_cast<unsigned char>(c)));
		Mix(0x5Aull);
	}
};

} // namespace

void CWaterVolumeComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions: everything a script could want to change is a reflected member, and
	// everything it could want to ask is a physics query on the area.
}

// ---------------------------------------------------------------------------
// Binding by rule
// ---------------------------------------------------------------------------

IShapeComponent* CWaterVolumeComponent::EnsureBound()
{
	IShapeComponent* pShape = (m_pEntity != nullptr) ? m_pEntity->GetComponent<IShapeComponent>() : nullptr;

	if (pShape == nullptr)
	{
		Unbind();

		if (!m_bWarnedNoShape)
		{
			m_bWarnedNoShape = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Water Volume on entity \"%s\": no shape on this entity - add \"Shape: Polygon\" for a water volume or \"Shape: Spline\" for a river.",
			           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>");
		}

		return nullptr;
	}

	m_bWarnedNoShape = false;

	if (m_pBoundShape != pShape)
	{
		Unbind();
		m_pBoundShape = pShape;
		pShape->AddListener(this);
	}

	return pShape;
}

void CWaterVolumeComponent::Unbind()
{
	if (m_pBoundShape != nullptr)
	{
		m_pBoundShape->RemoveListener(this);
		m_pBoundShape = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Material
// ---------------------------------------------------------------------------

IMaterial* CWaterVolumeComponent::ResolveMaterial() const
{
	if (!m_material.value.empty() && gEnv->p3DEngine != nullptr)
	{
		if (IMaterialManager* pManager = gEnv->p3DEngine->GetMaterialManager())
			return pManager->LoadMaterial(m_material.value.c_str(), false);
	}

	return (m_pEntity != nullptr) ? m_pEntity->GetMaterial() : nullptr;
}

void CWaterVolumeComponent::ValidateMaterial(IMaterial* pMaterial)
{
	if (pMaterial == nullptr)
	{
		m_bWarnedMaterial = false;
		return;
	}

	// The same check both legacy objects make (WaterShapeObject.cpp:255-258,
	// RiverObject.cpp:184-189): a water volume drawn with anything but a Water shader renders as a
	// flat sheet of whatever that shader does, which is the single most common "my water is
	// invisible" report.
	const SShaderItem& shaderItem = pMaterial->GetShaderItem();
	const bool         bWrongShader = shaderItem.m_pShader != nullptr && shaderItem.m_pShader->GetShaderType() != eST_Water;

	if (!bWrongShader)
	{
		m_bWarnedMaterial = false;
		return;
	}

	if (!m_bWarnedMaterial)
	{
		m_bWarnedMaterial = true;
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_ERROR,
		           "Water Volume on entity \"%s\": material \"%s\" does not use a Water shader. Pick a material whose shader is \"Water\".",
		           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>", pMaterial->GetName());
	}
}

// ---------------------------------------------------------------------------
// Properties onto a node
// ---------------------------------------------------------------------------

void CWaterVolumeComponent::PushNodeProperties(IWaterVolumeRenderNode* pNode) const
{
	if (pNode == nullptr)
		return;

	pNode->SetFogDensity(m_fogDensity);
	// The multiplier is folded in here, exactly as CWaterShapeObject::GetPremulFogColor does
	// (WaterShapeObject.cpp:184-187) - the render node has no multiplier of its own.
	const float  multiplier = max(m_fogColorMultiplier, 0.0f);
	pNode->SetFogColor(Vec3(m_fogColor.r, m_fogColor.g, m_fogColor.b) * multiplier);
	pNode->SetFogColorAffectedBySun(m_bFogColorAffectedBySun);
	pNode->SetFogShadowing(m_fogShadowing);
	pNode->SetCapFogAtVolumeDepth(m_bCapFogAtVolumeDepth);

	pNode->SetCaustics(m_bCaustics);
	pNode->SetCausticIntensity(m_causticIntensity);
	pNode->SetCausticTiling(m_causticTiling);
	pNode->SetCausticHeight(m_causticHeight);

	// Before the geometry: SetVolumeDepth re-runs UpdateBoundingBox (.cpp:146-151) and the depth is
	// also what the physics area is extruded down by (CreatePhysicsAreaFromSettings, .cpp:1051).
	pNode->SetVolumeDepth(m_volumeDepth);
	pNode->SetStreamSpeed(m_streamSpeed);

	pNode->SetPhysParams(m_waterDensity, m_waterResistance);

	pe_params_area auxParams;
	FillAuxPhysParams(auxParams);
	pNode->SetAuxPhysParams(&auxParams);
}

void CWaterVolumeComponent::FillAuxPhysParams(pe_params_area& out) const
{
	// CWaterShapeObject::UpdateGameArea, WaterShapeObject.cpp:308-323, field for field. Everything
	// it leaves alone stays UNUSED, which is how physics knows not to touch it.
	out.volume = m_fixedVolume;
	out.volumeAccuracy = m_volumeAccuracy;
	out.borderPad = m_borderPad;
	out.bConvexBorder = m_bConvexBorder ? 1 : 0;
	out.objectVolumeThreshold = m_objVolThreshold;
	out.cellSize = m_waveSimCell;
	out.waveSim.waveSpeed = m_waveSpeed;
	out.waveSim.dampingCenter = m_waveDamping;
	out.waveSim.timeStep = m_waveTimestep;
	out.waveSim.heightLimit = m_heightLimit;
	out.waveSim.simDepth = m_simDepth;
	out.waveSim.minVel = m_minWaveVel;
	out.waveSim.resistance = m_waveResistance;
	out.growthReserve = m_simAreaGrowth;
}

bool CWaterVolumeComponent::GetContourOriginInEntitySpace(IShapeComponent& shape, Vec3& out) const
{
	if (m_pEntity == nullptr)
		return false;

	Vec3      firstPoint(ZERO);
	const int totalPoints = shape.GetContour(&firstPoint, 1, true);
	if (totalPoints < 1)
		return false;

	out = m_pEntity->GetWorldTM().GetInverted().TransformPoint(firstPoint);
	return true;
}

Matrix34 CWaterVolumeComponent::GetNodeWorldTM() const
{
	if (m_pEntity == nullptr)
		return Matrix34(IDENTITY);

	return m_pEntity->GetWorldTM() * Matrix34::CreateTranslationMat(m_localOrigin);
}

// ---------------------------------------------------------------------------
// Area water - the slot node
// ---------------------------------------------------------------------------

bool CWaterVolumeComponent::BuildArea(IShapeComponent& shape)
{
	const int totalPoints = shape.GetContour(nullptr, 0, true);
	if (totalPoints < 4)
	{
		// CreateArea wants 3, SetAreaPhysicsArea wants more than 3 (.cpp:440), and the legacy object
		// only builds above 3 (WaterShapeObject.cpp:288) - so 4 is the real minimum for water that
		// both renders and floats things. Said out loud, because "nothing appeared and nothing was
		// logged" is the failure mode that costs an afternoon.
		if (!m_bWarnedTooFewPoints)
		{
			m_bWarnedTooFewPoints = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Water Volume on entity \"%s\": the polygon has %d points, and water needs at least 4 - add points with Edit Shape.",
			           m_pEntity->GetName(), totalPoints);
		}

		return false;
	}

	m_bWarnedTooFewPoints = false;

	const int pointCount = min(totalPoints, kMaxStackPoints);

	Vec3 worldPoints[kMaxStackPoints];
	shape.GetContour(worldPoints, pointCount, true);

	const Matrix34 entityWorldTM = m_pEntity->GetWorldTM();
	const Matrix34 entityInverse = entityWorldTM.GetInverted();

	Vec3 localPoints[kMaxStackPoints];
	for (int i = 0; i < pointCount; ++i)
		localPoints[i] = entityInverse.TransformPoint(worldPoints[i]);

	// Everything is expressed relative to contour point 0, and the slot carries that point. The
	// node's own matrix is then entityTM * T(point0), so CWaterVolumeRenderNode::SetMatrix builds
	// the fog plane as "the matrix' Z column through the matrix' translation" (.cpp:667-676) -
	// which is exactly the plane the legacy object builds by hand from its world Z column through
	// world point 0 (WaterShapeObject.cpp:296). Without the offset the fog plane would sit at the
	// entity's own height and the above/below-water test would be wrong by the contour's height.
	m_localOrigin = localPoints[0];
	for (int i = 0; i < pointCount; ++i)
		localPoints[i] -= m_localOrigin;

	IWaterVolumeRenderNode* pNode = m_nodes.empty() ? nullptr : m_nodes[0];
	const bool              bIsNewNode = pNode == nullptr;

	if (bIsNewNode)
	{
		pNode = static_cast<IWaterVolumeRenderNode*>(gEnv->p3DEngine->CreateRenderNode(eERType_WaterVolume));
		if (pNode == nullptr)
			return false;

		// "Must be called right after construction" (IRenderNode.h:1043). It is what makes SetMatrix
		// do anything at all (.cpp:669) and it is one-way: Dephysicalize() clears it as a side
		// effect (.cpp:871-879), which is why this component never dephysicalizes an attached node.
		pNode->SetAreaAttachedToEntity();
		m_nodes.push_back(pNode);
	}

	// The matrix first, so that CreateArea's own RegisterEntity (.cpp:340) indexes the node by a
	// bounding box built from the right transform (UpdateBoundingBox, .cpp:931-946).
	const Matrix34 nodeWorldTM = GetNodeWorldTM();
	pNode->SetMatrix(nodeWorldTM);

	PushNodeProperties(pNode);

	Plane fogPlane;
	fogPlane.SetPlane(Vec3(0.0f, 0.0f, 1.0f), Vec3(ZERO));

	// keepSerializationParams = FALSE. With true the node would hand the octree exporter a full set
	// of serialization params and the level would ship a second, baked copy of this volume
	// (ObjectsTree_Serialize.cpp:411-419). The owner filter added in Cry3DEngine covers the same
	// ground from the other side; both are deliberate.
	pNode->CreateArea(m_pEntity->GetGuid().hipart, localPoints, static_cast<unsigned int>(pointCount),
	                  Vec2(m_uScale, m_vScale), fogPlane, false);

	DestroyAreaPhysics();
	CreateAreaPhysics(pNode, localPoints, pointCount);

	IMaterial* pMaterial = ResolveMaterial();
	ValidateMaterial(pMaterial);

	if (bIsNewNode)
	{
		const int slotId = GetOrMakeEntitySlotId();
		m_pEntity->SetSlotLocalTM(slotId, Matrix34::CreateTranslationMat(m_localOrigin));
		// From here the slot owns the node: hide, layer hiding, selection highlight, the entity
		// material and every transform update come from it (EntitySlot.cpp:164-231, :271-278), and
		// it deletes the node when the slot goes (EntitySlot.cpp:49-86).
		m_pEntity->SetSlotRenderNode(slotId, pNode);
		m_bNodeInSlot = true;
	}
	else
	{
		m_pEntity->SetSlotLocalTM(GetEntitySlotId(), Matrix34::CreateTranslationMat(m_localOrigin));
	}

	m_pEntity->SetSlotMaterial(GetEntitySlotId(), pMaterial);

	// After the slot, which pushes the entity's own view distance ratio over ours
	// (EntitySlot.cpp, the ENTITY_SLOT_CUSTOM_VIEWDIST_RATIO branch).
	pNode->SetViewDistRatio(m_viewDistRatio);

	return true;
}

void CWaterVolumeComponent::CreateAreaPhysics(IWaterVolumeRenderNode* pNode, const Vec3* pLocalVertices, int vertexCount)
{
	if (pNode == nullptr || gEnv->pPhysicalWorld == nullptr)
		return;

	// An attached node cannot physicalize itself - CWaterVolumeRenderNode::Physicalize() returns
	// immediately when IsAttachedToEntity() (.cpp:840). SetAndCreatePhysicsArea is the one entry
	// point that builds the area without that guard (.cpp:542-547), and the caller then has to place
	// it and give it a buoyancy plane by hand. This is CGameVolume_Water::CreatePhysicsArea
	// (GameVolume_Water.cpp:418-470) plus the two things that game object never did: the density and
	// resistance of SetPhysParams, and the pe_params_area wave-sim block.
	m_pPhysArea = pNode->SetAndCreatePhysicsArea(pLocalVertices, static_cast<unsigned int>(vertexCount));
	if (m_pPhysArea == nullptr)
		return;

	pe_status_pos posStatus;
	m_pPhysArea->GetStatus(&posStatus);
	m_physLocalCentre = posStatus.pos;

	pe_params_foreign_data foreignData;
	foreignData.pForeignData = pNode;
	foreignData.iForeignData = PHYS_FOREIGN_ID_WATERVOLUME;
	foreignData.iForeignFlags = 0;
	m_pPhysArea->SetParams(&foreignData);

	pe_params_area auxParams;
	FillAuxPhysParams(auxParams);
	m_pPhysArea->SetParams(&auxParams);

	UpdateAreaPhysicsPlacement();
}

void CWaterVolumeComponent::UpdateAreaPhysicsPlacement()
{
	if (m_pPhysArea == nullptr || m_pEntity == nullptr)
		return;

	const Matrix34 nodeWorldTM = GetNodeWorldTM();

	Matrix33 rotation(nodeWorldTM);
	rotation.OrthonormalizeFast();
	const Quat worldRotation(rotation);

	// GameVolume_Water.cpp:440-446. The area was built around a centre physics chose for itself, so
	// rotating it about the entity origin means compensating for where that centre moves to.
	const Vec3 areaPosition = nodeWorldTM.GetTranslation() + ((worldRotation * m_physLocalCentre) - m_physLocalCentre);

	pe_params_pos position;
	position.pos = areaPosition;
	position.q = worldRotation;
	m_pPhysArea->SetParams(&position);

	pe_params_buoyancy buoyancy;
	buoyancy.waterPlane.n = worldRotation * Vec3(0.0f, 0.0f, 1.0f);
	buoyancy.waterPlane.origin = areaPosition;
	buoyancy.waterDensity = m_waterDensity;
	buoyancy.waterResistance = m_waterResistance;
	m_pPhysArea->SetParams(&buoyancy);
}

void CWaterVolumeComponent::DestroyAreaPhysics()
{
	if (m_pPhysArea != nullptr)
	{
		if (gEnv->pPhysicalWorld != nullptr)
			gEnv->pPhysicalWorld->DestroyPhysicalEntity(m_pPhysArea);

		m_pPhysArea = nullptr;
		m_physLocalCentre = ZERO;
	}
}

// ---------------------------------------------------------------------------
// River - N free nodes, physicalized by the engine
// ---------------------------------------------------------------------------

bool CWaterVolumeComponent::BuildRiver(IShapeComponent& shape, ISplineShape& spline)
{
	// The same walk the road uses, which is CRoadObject::SetRoadSectors' - and that is exactly
	// right, because CRiverObject IS a CRoadObject: it inherits the sector generation verbatim and
	// replaces only what each sector is turned into (RiverObject.cpp:195-230). Per Bezier segment,
	// sector count from that segment's own length, position and frame at one and the same
	// parameter - never a position by arc length paired with a frame by parameter, which samples
	// two different points on the curve and shears the ribbon.
	std::vector<SSplineStation> stations;
	const int stationCount = SplineSectors::BuildStations(spline, m_riverStep, m_riverWidth,
	                                                      kMaxRiverSectors + 1, stations);
	if (stationCount < 2)
		return false;

	const int   sectorCount = stationCount - 1;
	const float tileLength = max(0.001f, m_riverTileLength);

	std::vector<Vec3>  left(static_cast<size_t>(stationCount));
	std::vector<Vec3>  right(static_cast<size_t>(stationCount));
	std::vector<float> texCoord(static_cast<size_t>(stationCount));

	for (int k = 0; k < stationCount; ++k)
	{
		const SSplineStation& station = stations[static_cast<size_t>(k)];
		const Vec3            halfWidth = 0.5f * station.width * station.worldNormal;

		// CRoadObject's own ordering: left is +normal, right is -normal (RoadObject.cpp:180-181).
		left[static_cast<size_t>(k)] = station.worldPos + halfWidth;
		right[static_cast<size_t>(k)] = station.worldPos - halfWidth;
		texCoord[static_cast<size_t>(k)] = station.distance / tileLength;
	}

	// One fog plane for the whole river, as CRiverObject builds it: the world Z column through the
	// first sector's first point (RiverObject.cpp:199-201).
	Plane fogPlane;
	fogPlane.SetPlane(m_pEntity->GetWorldTM().GetColumn2().GetNormalized(), left[0]);
	if (fogPlane.n.Dot(Vec3(0.0f, 0.0f, 1.0f)) <= 1e-4f)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
		           "Water Volume on entity \"%s\": the entity is tipped past horizontal, so the river has no valid fog plane. Level the entity.",
		           m_pEntity->GetName());
		return false;
	}

	IMaterial* pMaterial = ResolveMaterial();
	ValidateMaterial(pMaterial);

	const uint64 volumeID = m_pEntity->GetGuid().hipart;
	const bool   bHidden = m_pEntity->IsHidden();

	m_nodes.reserve(static_cast<size_t>(sectorCount));

	for (int i = 0; i < sectorCount; ++i)
	{
		Vec3 sectorPoints[4];
		sectorPoints[0] = left[i];
		sectorPoints[1] = right[i];
		sectorPoints[2] = left[i + 1];
		sectorPoints[3] = right[i + 1];

		// CRiverObject skips degenerate sectors rather than handing CreateRiver a quad it cannot
		// triangulate (RiverObject.cpp:219-224).
		if (sectorPoints[0].IsEquivalent(sectorPoints[1]) || sectorPoints[0].IsEquivalent(sectorPoints[2]) ||
		    sectorPoints[1].IsEquivalent(sectorPoints[2]) || sectorPoints[2].IsEquivalent(sectorPoints[3]) ||
		    sectorPoints[1].IsEquivalent(sectorPoints[3]))
		{
			continue;
		}

		IWaterVolumeRenderNode* pNode = static_cast<IWaterVolumeRenderNode*>(gEnv->p3DEngine->CreateRenderNode(eERType_WaterVolume));
		if (pNode == nullptr)
			continue;

		// Owned by the entity, so COctreeNode::SaveObjects skips it (ObjectsTree_Serialize.cpp:295)
		// - the engine change this stage added. NOT attached: an attached node cannot be
		// physicalized, and a river's physics is the engine's own Physicalize() (see the header).
		pNode->SetOwnerEntity(m_pEntity);
		pNode->SetEditorObjectId(m_pEntity->GetEditorObjectID());
		pNode->SetRndFlags(bHidden ? ERF_HIDDEN : 0);

		PushNodeProperties(pNode);

		pNode->CreateRiver(volumeID, sectorPoints, 4, texCoord[i], texCoord[i + 1],
		                   Vec2(m_uScale, m_vScale), fogPlane, false);

		pNode->SetMaterial(pMaterial);
		pNode->SetViewDistRatio(m_viewDistRatio);

		m_nodes.push_back(pNode);
	}

	if (m_nodes.empty())
		return false;

	// One physics area for the whole river, on sector 0's node, from the stitched outline:
	// right edge forward, the far end, then the left edge backwards - CRiverObject::Physicalize,
	// RiverObject.cpp:152-163. The count is 2 * (sectors + 1), which satisfies
	// SetRiverPhysicsArea's "even and more than 3" (WaterVolumeRenderNode.cpp:477).
	std::vector<Vec3> outline;
	outline.reserve(static_cast<size_t>(2 * stationCount));

	for (int k = 0; k < stationCount; ++k)
		outline.push_back(right[k]);
	for (int k = stationCount - 1; k >= 0; --k)
		outline.push_back(left[k]);

	IWaterVolumeRenderNode* pFirstNode = m_nodes[0];
	pFirstNode->SetRiverPhysicsArea(&outline[0], static_cast<unsigned int>(outline.size()), false);
	// The engine's own Physicalize() builds the area, sets buoyancy from SetPhysParams' density and
	// resistance and applies the stored pe_params_area (.cpp:838-869). That is the whole reason a
	// river node stays unattached.
	pFirstNode->Physicalize();

	return true;
}

// ---------------------------------------------------------------------------
// Rebuild
// ---------------------------------------------------------------------------

uint64 CWaterVolumeComponent::ComputeSignature(IShapeComponent& shape, EMode mode) const
{
	SWaterSignature signature;

	signature.Mix(static_cast<uint64>(mode));
	signature.Mix(m_bEnabled ? 1 : 0);
	signature.MixString(m_material.value);

	signature.MixFloat(m_volumeDepth);
	signature.MixFloat(m_streamSpeed);
	signature.MixFloat(m_fogDensity);
	signature.MixFloat(m_fogColor.r);
	signature.MixFloat(m_fogColor.g);
	signature.MixFloat(m_fogColor.b);
	signature.MixFloat(m_fogColorMultiplier);
	signature.Mix(m_bFogColorAffectedBySun ? 1 : 0);
	signature.MixFloat(m_fogShadowing);
	signature.Mix(m_bCapFogAtVolumeDepth ? 1 : 0);
	signature.MixFloat(m_uScale);
	signature.MixFloat(m_vScale);
	signature.Mix(static_cast<uint64>(m_viewDistRatio));
	signature.Mix(m_bCaustics ? 1 : 0);
	signature.MixFloat(m_causticIntensity);
	signature.MixFloat(m_causticTiling);
	signature.MixFloat(m_causticHeight);
	signature.MixFloat(m_waterDensity);
	signature.MixFloat(m_waterResistance);

	signature.MixFloat(m_fixedVolume);
	signature.MixFloat(m_volumeAccuracy);
	signature.MixFloat(m_borderPad);
	signature.Mix(m_bConvexBorder ? 1 : 0);
	signature.MixFloat(m_objVolThreshold);
	signature.MixFloat(m_waveSimCell);
	signature.MixFloat(m_waveSpeed);
	signature.MixFloat(m_waveDamping);
	signature.MixFloat(m_waveTimestep);
	signature.MixFloat(m_minWaveVel);
	signature.MixFloat(m_simDepth);
	signature.MixFloat(m_heightLimit);
	signature.MixFloat(m_waveResistance);
	signature.MixFloat(m_simAreaGrowth);

	signature.MixFloat(m_riverWidth);
	signature.MixFloat(m_riverStep);
	signature.MixFloat(m_riverTileLength);

	const int totalPoints = shape.GetContour(nullptr, 0, true);
	signature.Mix(static_cast<uint64>(totalPoints));

	const int pointCount = min(totalPoints, kMaxStackPoints);
	if (pointCount > 0 && m_pEntity != nullptr)
	{
		Vec3 points[kMaxStackPoints];
		shape.GetContour(points, pointCount, true);

		// Area water is transform-independent by construction - the node is in a slot and a move is a
		// matrix update - so its digest is taken in ENTITY space, and moving the entity does not make
		// it look like a rebuild is due. A river's nodes hold world vertices, so its digest is taken
		// in world space and a move genuinely is a rebuild.
		if (mode == EMode::Area)
		{
			const Matrix34 entityInverse = m_pEntity->GetWorldTM().GetInverted();
			for (int i = 0; i < pointCount; ++i)
				signature.MixVec(entityInverse.TransformPoint(points[i]));
		}
		else
		{
			for (int i = 0; i < pointCount; ++i)
				signature.MixVec(points[i]);
		}
	}

	// A river reads the curve itself, not just the contour the shape reports.
	if (mode == EMode::River)
	{
		if (ISplineShape* pSpline = shape.GetSpline())
		{
			signature.MixFloat(pSpline->TotalLength());
			signature.Mix(pSpline->IsClosed() ? 1 : 0);
		}
	}

	// 0 means "never built", so never hand it back as a real answer.
	return signature.hash != 0 ? signature.hash : 1;
}

bool CWaterVolumeComponent::RebuildIfNeeded(bool bForce)
{
	if (m_pEntity == nullptr || gEnv->p3DEngine == nullptr)
		return false;

	IShapeComponent* pShape = EnsureBound();
	if (pShape == nullptr)
	{
		DestroyNodes();
		m_mode = EMode::None;
		m_lastBuildSignature = 0;
		return false;
	}

	EMode mode = EMode::None;
	switch (pShape->GetKind())
	{
	case EShapeKind::Polygon:
		mode = EMode::Area;
		break;
	case EShapeKind::Spline:
		mode = (pShape->GetSpline() != nullptr) ? EMode::River : EMode::None;
		break;
	default:
		break;
	}

	if (mode == EMode::None)
	{
		if (!m_bWarnedWrongKind)
		{
			m_bWarnedWrongKind = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Water Volume on entity \"%s\": a %s shape has no water surface. Use \"Shape: Polygon\" for a water volume or \"Shape: Spline\" for a river.",
			           m_pEntity->GetName(),
			           pShape->GetKind() == EShapeKind::Box ? "box" : "sphere");
		}

		DestroyNodes();
		m_mode = EMode::None;
		m_lastBuildSignature = 0;
		return false;
	}

	m_bWarnedWrongKind = false;

	// The origin guard. Area water's node geometry is LOCAL and its slot transform carries contour
	// point 0 in entity space, so a change to that point has to rebuild even when the digest below
	// would say no. "Recenter Pivot" is exactly that case: it moves the entity and rewrites every
	// local point to compensate, so the world contour never changes - and a cached origin would keep
	// the water translated by the pivot delta for ever. Cheap: one point out of the shape.
	if (!bForce && mode == EMode::Area && mode == m_mode && !m_nodes.empty())
	{
		Vec3 currentOrigin(ZERO);
		if (GetContourOriginInEntitySpace(*pShape, currentOrigin) && !currentOrigin.IsEquivalent(m_localOrigin, 0.0001f))
			bForce = true;
	}

	const uint64 signature = ComputeSignature(*pShape, mode);
	if (!bForce && signature == m_lastBuildSignature && mode == m_mode)
		return false;

	// A mode change throws everything away first: the two modes own their nodes differently (slot
	// versus direct) and nothing survives the switch.
	if (mode != m_mode)
		DestroyNodes();

	m_mode = mode;
	m_lastBuildSignature = signature;

	Rebuild();
	return true;
}

void CWaterVolumeComponent::Rebuild()
{
	if (!m_bEnabled)
	{
		DestroyNodes();
		return;
	}

	IShapeComponent* pShape = m_pBoundShape;
	if (pShape == nullptr)
		return;

	if (m_mode == EMode::Area)
	{
		if (!BuildArea(*pShape))
			DestroyNodes();
	}
	else if (m_mode == EMode::River)
	{
		// A river's nodes hold world vertices, so there is nothing to update in place - the old set
		// goes and a new one is built.
		DestroyNodes();

		if (ISplineShape* pSpline = pShape->GetSpline())
		{
			if (!BuildRiver(*pShape, *pSpline))
				DestroyNodes();
		}
	}
}

void CWaterVolumeComponent::DestroyNodes()
{
	DestroyAreaPhysics();

	if (m_bNodeInSlot)
	{
		// The slot owns the node: freeing the slot calls SetOwnerEntity(nullptr) and
		// DeleteRenderNode for us (EntitySlot.cpp:49-86).
		FreeEntitySlot();
		m_bNodeInSlot = false;
		m_nodes.clear();
	}
	else
	{
		if (gEnv->p3DEngine != nullptr)
		{
			for (IWaterVolumeRenderNode* pNode : m_nodes)
			{
				if (pNode != nullptr)
				{
					// The node's destructor dephysicalizes it, which destroys the river's physics
					// area (WaterVolumeRenderNode.cpp:88).
					gEnv->p3DEngine->DeleteRenderNode(pNode);
				}
			}
		}

		m_nodes.clear();
	}

	m_localOrigin = ZERO;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void CWaterVolumeComponent::Initialize()
{
	RebuildIfNeeded(true);
}

void CWaterVolumeComponent::OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason)
{
	// EVERY reason goes through the digest now, transforms included, and the digest decides.
	//
	// Area water's digest is taken in ENTITY space, so simply dragging the entity leaves it
	// unchanged and the move stays what decision 05 promised: the slot's matrix plus one
	// pe_params_pos on the physics area, no re-tessellation. What the old "Transform means physics
	// only" shortcut could not see is an edit that moves the entity AND rewrites the points to
	// compensate - "Recenter Pivot" - where the world contour is unchanged but the entity-space one
	// is not. That rebuilt nothing and left the water translated by the pivot delta.
	const bool bRebuilt = RebuildIfNeeded(reason != EShapeChangeReason::Transform);

	if (!bRebuilt && reason == EShapeChangeReason::Transform && m_mode == EMode::Area)
	{
		UpdateAreaPhysicsPlacement();
	}
}

void CWaterVolumeComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		// Not forced: this event arrives on every inspector serialization pass, and a forced rebuild
		// here would re-tessellate the volume many times a second while its panel is open.
		RebuildIfNeeded(false);
		break;
	case ENTITY_EVENT_XFORM:
		{
			// Insurance, not the main path: the shape tells its listeners about transforms too. This
			// covers the case where the component was not bound when the entity moved - EnsureBound()
			// runs inside RebuildIfNeeded - and it costs nothing, because the digest decides.
			if (!RebuildIfNeeded(false) && m_mode == EMode::Area)
				UpdateAreaPhysicsPlacement();
		}
		break;
	case ENTITY_EVENT_HIDE:
	case ENTITY_EVENT_UNHIDE:
		{
			// The slot does this for an attached node; a river's own nodes need telling.
			if (!m_bNodeInSlot && m_pEntity != nullptr)
			{
				const bool bHidden = m_pEntity->IsHidden();
				for (IWaterVolumeRenderNode* pNode : m_nodes)
				{
					if (pNode != nullptr)
						pNode->SetRndFlags(ERF_HIDDEN, bHidden);
				}
			}
		}
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CWaterVolumeComponent::GetEventMask() const
{
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED | ENTITY_EVENT_XFORM | ENTITY_EVENT_HIDE | ENTITY_EVENT_UNHIDE;
}

void CWaterVolumeComponent::OnShutDown()
{
	Unbind();
	DestroyNodes();
}

} // namespace AreaComponents
} // namespace Cry
