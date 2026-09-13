// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "RoadComponent.h"
#include "SplineSectors.h"

#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/IRenderNode.h>
#include <Cry3DEngine/IMaterial.h>

namespace Cry
{
namespace AreaComponents
{

namespace
{

//! The tool class name the inspector button starts, and its label. String literals with static
//! storage, which is what SEditorActionDesc requires - the property tree keeps the pointers.
const char* const szRoadAlignToolClassName = "EditTool.AreaRoadAlignTerrain";
const char* const szRoadAlignToolLabel = "Align Terrain To Road";

//! FNV-1a, the digest the distributor and the water component already use for the same job.
struct SRoadSignature
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

void CRoadComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions: a road is authored, not scripted.
}

bool CRoadComponent::GetEditorAction(int index, SEditorActionDesc& out) const
{
	if (index != 0)
		return false;

	out.szToolClassName = szRoadAlignToolClassName;
	out.szLabel = szRoadAlignToolLabel;
	return true;
}

// ---------------------------------------------------------------------------
// Binding by rule
// ---------------------------------------------------------------------------

IShapeComponent* CRoadComponent::EnsureBound()
{
	IShapeComponent* pShape = (m_pEntity != nullptr) ? m_pEntity->GetComponent<IShapeComponent>() : nullptr;

	if (pShape == nullptr || pShape->GetSpline() == nullptr)
	{
		Unbind();

		if (!m_bWarnedNoSpline)
		{
			m_bWarnedNoSpline = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Road on entity \"%s\": no spline shape on this entity - add \"Shape: Spline\" to give it a curve to follow.",
			           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>");
		}

		return nullptr;
	}

	m_bWarnedNoSpline = false;

	if (m_pBoundShape != pShape)
	{
		Unbind();
		m_pBoundShape = pShape;
		pShape->AddListener(this);
	}

	return pShape;
}

void CRoadComponent::Unbind()
{
	if (m_pBoundShape != nullptr)
	{
		m_pBoundShape->RemoveListener(this);
		m_pBoundShape = nullptr;
	}
}

// ---------------------------------------------------------------------------
// The road itself
// ---------------------------------------------------------------------------

IMaterial* CRoadComponent::ResolveMaterial() const
{
	if (!m_material.value.empty() && gEnv->p3DEngine != nullptr)
	{
		if (IMaterialManager* pManager = gEnv->p3DEngine->GetMaterialManager())
			return pManager->LoadMaterial(m_material.value.c_str(), false);
	}

	return (m_pEntity != nullptr) ? m_pEntity->GetMaterial() : nullptr;
}

bool CRoadComponent::BuildSamples(ISplineShape& spline)
{
	m_samples.clear();
	m_sampleTex.clear();

	// CRoadObject::SetRoadSectors' own walk, in SplineSectors.h - per Bezier segment, sector count
	// from that segment's length, position and frame taken at one and the same parameter.
	std::vector<SSplineStation> stations;
	const int stationCount = SplineSectors::BuildStations(spline, m_step, m_width, kMaxStations, stations);
	if (stationCount < 2)
		return false;

	const float tileLength = max(0.001f, m_tileLength);

	m_samples.resize(static_cast<size_t>(stationCount));
	m_sampleTex.resize(static_cast<size_t>(stationCount));

	for (int i = 0; i < stationCount; ++i)
	{
		m_samples[static_cast<size_t>(i)].worldPos = stations[static_cast<size_t>(i)].worldPos;
		m_samples[static_cast<size_t>(i)].worldNormal = stations[static_cast<size_t>(i)].worldNormal;
		m_samples[static_cast<size_t>(i)].width = stations[static_cast<size_t>(i)].width;
		m_sampleTex[static_cast<size_t>(i)] = stations[static_cast<size_t>(i)].distance / tileLength;
	}

	return true;
}

void CRoadComponent::BuildNodes()
{
	DestroyNodes();

	const int stationCount = static_cast<int>(m_samples.size());
	if (stationCount < 2 || gEnv->p3DEngine == nullptr || m_pEntity == nullptr)
		return;

	const int sectorCount = stationCount - 1;
	const int chunkCount = (sectorCount + kSectorsPerChunk - 1) / kSectorsPerChunk;

	IMaterial* pMaterial = ResolveMaterial();
	const bool bHidden = m_pEntity->IsHidden();

	// The whole road's texture range, which every chunk is told about so that the shader can fade
	// the two ends: CRoadObject passes fabs(sectorFirstGlobal.t0) and fabs(sectorLastGlobal.t1)
	// (RoadObject.cpp:317-318). Ours are already positive - the legacy object negates the final t1
	// as an end-of-road marker and then takes its absolute value again at every use, so the sign
	// carries no information here.
	const float texGlobalBegin = m_sampleTex[0];
	const float texGlobalEnd = m_sampleTex[static_cast<size_t>(stationCount - 1)];

	m_nodes.reserve(static_cast<size_t>(chunkCount));

	for (int chunk = 0; chunk < chunkCount; ++chunk)
	{
		const int firstSector = chunk * kSectorsPerChunk;
		const int sectorsInChunk = min(kSectorsPerChunk, sectorCount - firstSector);
		if (sectorsInChunk <= 0)
			break;

		IRoadRenderNode* pNode = static_cast<IRoadRenderNode*>(gEnv->p3DEngine->CreateRenderNode(eERType_Road));
		if (pNode == nullptr)
			continue;

		// Owned by the entity, so COctreeNode::SaveObjects skips it (ObjectsTree_Serialize.cpp:295)
		// - the engine change this stage added.
		pNode->SetOwnerEntity(m_pEntity);

		// CRoadObject::UpdateSectors' order, RoadObject.cpp:283-287: flags, view distance, min spec,
		// material layers, editor id - all before the vertices, because SetVertices schedules the
		// rebuild that turns them into a mesh.
		pNode->SetRndFlags(bHidden ? ERF_HIDDEN : 0);
		pNode->SetViewDistRatio(m_viewDistRatio);
		pNode->SetMinSpec(0);
		pNode->SetMaterialLayers(0);
		pNode->SetEditorObjectId(m_pEntity->GetEditorObjectID());

		// The quad strip: one left/right pair per station of the chunk. The render node knows
		// nothing about curves (research/08 section 2.1).
		std::vector<Vec3> vertices;
		vertices.reserve(static_cast<size_t>(2 * (sectorsInChunk + 1)));

		for (int i = 0; i < sectorsInChunk; ++i)
		{
			const SRoadSample& sample = m_samples[static_cast<size_t>(firstSector + i)];
			const Vec3         halfWidth = 0.5f * sample.width * sample.worldNormal;
			vertices.push_back(sample.worldPos + halfWidth);
			vertices.push_back(sample.worldPos - halfWidth);
		}

		// The last boundary is pushed 7.5 cm further along the road, which is how CRoadObject hides
		// the seam two f16 meshes leave between chunks (RoadObject.cpp:299-312).
		{
			const SRoadSample& lastSample = m_samples[static_cast<size_t>(firstSector + sectorsInChunk)];
			const SRoadSample& prevSample = m_samples[static_cast<size_t>(firstSector + sectorsInChunk - 1)];

			const Vec3 lastHalfWidth = 0.5f * lastSample.width * lastSample.worldNormal;
			const Vec3 prevHalfWidth = 0.5f * prevSample.width * prevSample.worldNormal;

			const Vec3 leftEnd = lastSample.worldPos + lastHalfWidth;
			const Vec3 rightEnd = lastSample.worldPos - lastHalfWidth;

			Vec3 leftOffset = leftEnd - (prevSample.worldPos + prevHalfWidth);
			Vec3 rightOffset = rightEnd - (prevSample.worldPos - prevHalfWidth);
			leftOffset = leftOffset.IsZero() ? Vec3(ZERO) : leftOffset.GetNormalized() * kChunkOverlap;
			rightOffset = rightOffset.IsZero() ? Vec3(ZERO) : rightOffset.GetNormalized() * kChunkOverlap;

			vertices.push_back(leftEnd + leftOffset);
			vertices.push_back(rightEnd + rightOffset);
		}

		const float texBegin = m_sampleTex[static_cast<size_t>(firstSector)];
		const float texEnd = m_sampleTex[static_cast<size_t>(firstSector + sectorsInChunk)];

		pNode->SetVertices(&vertices[0], static_cast<int>(vertices.size()), texBegin, texEnd, texGlobalBegin, texGlobalEnd);
		pNode->SetSortPriority(static_cast<uint8>(clamp_tpl(m_sortPriority, 0, 255)));
		pNode->SetIgnoreTerrainHoles(m_bIgnoreTerrainHoles);
		pNode->SetPhysicalize(m_bPhysicalize);
		pNode->SetMaterial(pMaterial);

		m_nodes.push_back(pNode);
	}
}

void CRoadComponent::DestroyNodes()
{
	if (gEnv->p3DEngine != nullptr)
	{
		for (IRoadRenderNode* pNode : m_nodes)
		{
			if (pNode != nullptr)
				gEnv->p3DEngine->DeleteRenderNode(pNode); // unregisters first
		}
	}

	m_nodes.clear();
}

// ---------------------------------------------------------------------------
// IRoadAlignSource
// ---------------------------------------------------------------------------

int CRoadComponent::GetRoadSampleCount() const
{
	return static_cast<int>(m_samples.size());
}

bool CRoadComponent::GetRoadSample(int index, SRoadSample& out) const
{
	if (index < 0 || index >= static_cast<int>(m_samples.size()))
		return false;

	out = m_samples[static_cast<size_t>(index)];
	return true;
}

// ---------------------------------------------------------------------------
// Rebuild
// ---------------------------------------------------------------------------

uint64 CRoadComponent::ComputeSignature(IShapeComponent& shape) const
{
	SRoadSignature signature;

	signature.Mix(m_bEnabled ? 1 : 0);
	signature.MixString(m_material.value);
	signature.MixFloat(m_width);
	signature.MixFloat(m_borderWidth);
	signature.MixFloat(m_step);
	signature.MixFloat(m_tileLength);
	signature.Mix(static_cast<uint64>(m_sortPriority));
	signature.Mix(static_cast<uint64>(m_viewDistRatio));
	signature.Mix(m_bIgnoreTerrainHoles ? 1 : 0);
	signature.Mix(m_bPhysicalize ? 1 : 0);

	// World space on purpose: a road's vertices are world vertices, so moving the entity really is
	// a rebuild and the digest has to notice it.
	const int totalPoints = shape.GetContour(nullptr, 0, true);
	signature.Mix(static_cast<uint64>(totalPoints));

	if (totalPoints > 0)
	{
		std::vector<Vec3> points;
		GetShapeContour(shape, points, true);
		for (const Vec3& point : points)
			signature.MixVec(point);
	}

	if (ISplineShape* pSpline = shape.GetSpline())
	{
		signature.MixFloat(pSpline->TotalLength());
		signature.Mix(pSpline->IsClosed() ? 1 : 0);
	}

	return signature.hash != 0 ? signature.hash : 1;
}

void CRoadComponent::RebuildIfNeeded(bool bForce)
{
	if (m_pEntity == nullptr || gEnv->p3DEngine == nullptr)
		return;

	IShapeComponent* pShape = EnsureBound();
	if (pShape == nullptr)
	{
		DestroyNodes();
		m_samples.clear();
		m_sampleTex.clear();
		m_lastBuildSignature = 0;
		return;
	}

	const uint64 signature = ComputeSignature(*pShape);
	if (!bForce && signature == m_lastBuildSignature)
		return;

	m_lastBuildSignature = signature;

	if (!m_bEnabled)
	{
		DestroyNodes();
		m_samples.clear();
		m_sampleTex.clear();
		return;
	}

	ISplineShape* pSpline = pShape->GetSpline();
	if (pSpline == nullptr || !BuildSamples(*pSpline))
	{
		DestroyNodes();
		m_samples.clear();
		m_sampleTex.clear();
		return;
	}

	BuildNodes();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void CRoadComponent::Initialize()
{
	RebuildIfNeeded(true);
}

void CRoadComponent::OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason)
{
	// Even a transform is a rebuild: the render node holds world vertices. Notifications arrive once
	// per edit gesture, because the shape coalesces everything between BeginEdit and EndEdit.
	RebuildIfNeeded(reason != EShapeChangeReason::Transform);
}

void CRoadComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		RebuildIfNeeded(false);
		break;
	case ENTITY_EVENT_HIDE:
	case ENTITY_EVENT_UNHIDE:
		{
			if (m_pEntity != nullptr)
			{
				const bool bHidden = m_pEntity->IsHidden();
				for (IRoadRenderNode* pNode : m_nodes)
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

Cry::Entity::EventFlags CRoadComponent::GetEventMask() const
{
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED | ENTITY_EVENT_HIDE | ENTITY_EVENT_UNHIDE;
}

void CRoadComponent::OnShutDown()
{
	Unbind();
	DestroyNodes();
	m_samples.clear();
	m_sampleTex.clear();
}

} // namespace AreaComponents
} // namespace Cry
