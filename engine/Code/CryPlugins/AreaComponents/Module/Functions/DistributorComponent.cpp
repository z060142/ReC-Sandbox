// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "DistributorComponent.h"

#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/IRenderNode.h>
#include <Cry3DEngine/IStatObj.h>
#include <CryPhysics/IPhysics.h>
#include <CryPhysics/physinterface.h>

#include <algorithm>

namespace Cry
{
namespace AreaComponents
{

namespace
{

//! A tiny deterministic stream. cry_random() is seeded globally and would give a different layout
//! on every run, which is exactly what a stored Seed is there to prevent.
//! xorshift32 - three shifts, no state beyond one word, and good enough for placement jitter.
class CInstanceRandom
{
public:
	explicit CInstanceRandom(uint32 seed)
		: m_state(seed != 0 ? seed : 0x9E3779B9u)
	{
		// Two warm-up rounds: a low seed like 1 would otherwise start with a very small first value.
		Next();
		Next();
	}

	uint32 Next()
	{
		m_state ^= m_state << 13;
		m_state ^= m_state >> 17;
		m_state ^= m_state << 5;
		return m_state;
	}

	//! Uniform in [0,1).
	float Unit()
	{
		return static_cast<float>(Next() & 0xFFFFFFu) / static_cast<float>(0x1000000);
	}

	float Range(float low, float high) { return low + (high - low) * Unit(); }

private:
	uint32 m_state;
};

//! The per-instance seed: the component seed and the index mixed so that neighbouring instances do
//! not get neighbouring streams (which xorshift32 would otherwise correlate visibly).
uint32 MakeInstanceSeed(uint32 seed, int index)
{
	uint32 hash = seed * 747796405u + static_cast<uint32>(index) * 2891336453u + 2891336453u;
	hash ^= hash >> 15;
	hash *= 2246822519u;
	hash ^= hash >> 13;
	return hash;
}

} // namespace

void CDistributorComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet: everything the distributor does is driven by its own properties.
}

// ---------------------------------------------------------------------------
// Binding by rule
// ---------------------------------------------------------------------------

ISplineShape* CDistributorComponent::EnsureBound()
{
	IShapeComponent* pShape = (m_pEntity != nullptr) ? m_pEntity->GetComponent<IShapeComponent>() : nullptr;
	ISplineShape*    pSpline = (pShape != nullptr) ? pShape->GetSpline() : nullptr;

	if (pSpline == nullptr)
	{
		Unbind();

		if (!m_bWarnedNoSpline)
		{
			m_bWarnedNoSpline = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Distributor on entity \"%s\": no spline shape on this entity - add \"Shape: Spline\" to give it a curve.",
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

	return pSpline;
}

void CDistributorComponent::Unbind()
{
	if (m_pBoundShape != nullptr)
	{
		m_pBoundShape->RemoveListener(this);
		m_pBoundShape = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

int CDistributorComponent::GetEndCapMeshIndex() const
{
	return static_cast<int>(m_meshes.meshes.size());
}

void CDistributorComponent::ReloadMeshes()
{
	m_statObjs.clear();

	if (gEnv->p3DEngine == nullptr)
		return;

	// One slot per list entry, plus one for the end cap, so that an instance's mesh index is simply
	// an index into this array and the end cap needs no second lookup path.
	m_statObjs.reserve(m_meshes.meshes.size() + 1);

	for (const SDistributorMesh& entry : m_meshes.meshes)
	{
		IStatObj* pStatObj = (!entry.mesh.value.empty()) ? gEnv->p3DEngine->LoadStatObj(entry.mesh.value.c_str()) : nullptr;
		m_statObjs.push_back(pStatObj);
	}

	IStatObj* pEndCap = (!m_endCapMesh.value.empty()) ? gEnv->p3DEngine->LoadStatObj(m_endCapMesh.value.c_str()) : nullptr;
	m_statObjs.push_back(pEndCap);
}

int CDistributorComponent::PickMesh(float random01) const
{
	const int count = static_cast<int>(m_meshes.meshes.size());
	if (count == 0)
		return -1;

	float totalWeight = 0.0f;
	for (int i = 0; i < count; ++i)
	{
		if (i < static_cast<int>(m_statObjs.size()) && m_statObjs[i] != nullptr)
			totalWeight += max(0.0f, m_meshes.meshes[i].weight);
	}

	if (totalWeight <= 0.0f)
		return -1;

	float pick = clamp_tpl(random01, 0.0f, 0.999999f) * totalWeight;
	for (int i = 0; i < count; ++i)
	{
		if (i >= static_cast<int>(m_statObjs.size()) || m_statObjs[i] == nullptr)
			continue;

		const float weight = max(0.0f, m_meshes.meshes[i].weight);
		if (pick < weight)
			return i;

		pick -= weight;
	}

	return -1;
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

Matrix33 CDistributorComponent::GetForwardAxisFix() const
{
	// The frame's column 0 is the direction the instance should face. This rotation brings the
	// MESH's chosen axis onto that column, so a fence whose length runs along local +Y needs no
	// re-export - it needs one dropdown.
	switch (m_forwardAxis)
	{
	case EDistributorForwardAxis::PlusY:
		return Matrix33::CreateRotationZ(DEG2RAD(-90.0f));
	case EDistributorForwardAxis::MinusX:
		return Matrix33::CreateRotationZ(DEG2RAD(180.0f));
	case EDistributorForwardAxis::MinusY:
		return Matrix33::CreateRotationZ(DEG2RAD(90.0f));
	case EDistributorForwardAxis::PlusX:
	default:
		return Matrix33(IDENTITY);
	}
}

bool CDistributorComponent::GetMeshForwardExtent(int meshIndex, float& length, float& start) const
{
	length = 0.0f;
	start = 0.0f;

	if (meshIndex < 0 || meshIndex >= static_cast<int>(m_statObjs.size()) || m_statObjs[meshIndex] == nullptr)
		return false;

	const AABB box = m_statObjs[meshIndex]->GetAABB();
	if (box.IsReset())
		return false;

	switch (m_forwardAxis)
	{
	case EDistributorForwardAxis::PlusX:  length = box.max.x - box.min.x; start = box.min.x; break;
	case EDistributorForwardAxis::MinusX: length = box.max.x - box.min.x; start = box.max.x; break;
	case EDistributorForwardAxis::PlusY:  length = box.max.y - box.min.y; start = box.min.y; break;
	case EDistributorForwardAxis::MinusY: length = box.max.y - box.min.y; start = box.max.y; break;
	}

	return length > 0.0001f;
}

Matrix34 CDistributorComponent::BuildInstanceTransform(const Matrix33& frame, int meshIndex, float uniformScale,
                                                       float forwardStretch, const Vec3& position, const Vec3& offset) const
{
	// Scale lives in MESH space, because "stretch along the forward axis" means the mesh's own
	// forward axis: a fence panel gets longer, not taller. So the order is
	//     frame (place and aim) * axis fix (bring the mesh's axis onto the frame's) * scale (local).
	Matrix33 localScale(IDENTITY);
	localScale.SetIdentity();
	localScale.m00 = uniformScale;
	localScale.m11 = uniformScale;
	localScale.m22 = uniformScale;

	switch (m_forwardAxis)
	{
	case EDistributorForwardAxis::PlusX:
	case EDistributorForwardAxis::MinusX:
		localScale.m00 = uniformScale * forwardStretch;
		break;
	case EDistributorForwardAxis::PlusY:
	case EDistributorForwardAxis::MinusY:
		localScale.m11 = uniformScale * forwardStretch;
		break;
	}

	const Matrix33 rotation = frame * GetForwardAxisFix() * localScale;

	Matrix34 tm(rotation);
	Vec3     translation = position + frame * offset;

	if (m_bPivotAtStart)
	{
		// Put the START FACE of the mesh's bounding box on the sample point rather than the mesh's
		// own pivot. A section exported around its centre would otherwise begin half a section
		// before the curve does, and every gap in the chain would be half a section wide.
		float meshLength = 0.0f;
		float meshStart = 0.0f;
		if (GetMeshForwardExtent(meshIndex, meshLength, meshStart))
		{
			Vec3 localStart(ZERO);
			switch (m_forwardAxis)
			{
			case EDistributorForwardAxis::PlusX:
			case EDistributorForwardAxis::MinusX:
				localStart.x = meshStart;
				break;
			case EDistributorForwardAxis::PlusY:
			case EDistributorForwardAxis::MinusY:
				localStart.y = meshStart;
				break;
			}

			translation -= rotation * localStart;
		}
	}

	tm.SetTranslation(translation);
	return tm;
}

Matrix33 CDistributorComponent::BuildFrame(const Vec3& position, const Vec3& tangent, const Vec3& normal) const
{
	switch (m_alignMode)
	{
	case EDistributorAlign::Chord:
	case EDistributorAlign::FollowTangent:
		{
			// The frame CSplineDistributor builds (SplineDistributor.cpp:286-293): X along the
			// curve, Y the roll-aware normal, Z their cross product - so the per-point Angle rolls
			// the instance for free, which is what banking a row of fence posts along a road means.
			//
			// Chord alignment arrives here too, with `tangent` already replaced by the direction of
			// the chord to the next instance: same frame, different forward.
			if (tangent.IsZero() || normal.IsZero())
				break;

			const Vec3 x = tangent.GetNormalized();

			// The normal is not generally perpendicular to a chord, so it is re-orthogonalised
			// rather than used raw - otherwise the frame shears and so does the mesh.
			Vec3 y = normal - x * normal.Dot(x);
			if (y.IsZero(0.0001f))
				break;
			y.Normalize();

			const Vec3 z = x.Cross(y);
			if (z.IsZero())
				break;

			Matrix33 frame;
			frame.SetColumn(0, x);
			frame.SetColumn(1, y);
			frame.SetColumn(2, z.GetNormalized());
			return frame;
		}
	case EDistributorAlign::WorldUp:
		{
			// Upright, but turned to face along the curve - the orientation a lamp post wants.
			Vec3 forward(tangent.x, tangent.y, 0.0f);
			if (forward.IsZero())
				break;
			forward.Normalize();

			const Vec3 up(0.0f, 0.0f, 1.0f);

			Matrix33 frame;
			frame.SetColumn(0, forward);
			frame.SetColumn(1, up.Cross(forward));
			frame.SetColumn(2, up);
			return frame;
		}
	case EDistributorAlign::SurfaceNormal:
		{
			Vec3 up(0.0f, 0.0f, 1.0f);

			if (gEnv->pPhysicalWorld != nullptr)
			{
				ray_hit hit;
				ZeroStruct(hit);

				const Vec3 origin = position + Vec3(0.0f, 0.0f, kSurfaceRayLength * 0.5f);
				if (gEnv->pPhysicalWorld->RayWorldIntersection(origin, Vec3(0.0f, 0.0f, -kSurfaceRayLength),
				                                              ent_terrain | ent_static, rwi_stop_at_pierceable, &hit, 1) != 0)
				{
					if (!hit.n.IsZero())
						up = hit.n.GetNormalized();
				}
			}

			Vec3 forward = tangent - up * tangent.Dot(up);
			if (forward.IsZero())
				break;
			forward.Normalize();

			Matrix33 frame;
			frame.SetColumn(0, forward);
			frame.SetColumn(1, up.Cross(forward));
			frame.SetColumn(2, up);
			return frame;
		}
	case EDistributorAlign::Fixed:
	default:
		break;
	}

	return Matrix33(IDENTITY);
}

void CDistributorComponent::BuildInstances(ISplineShape& spline, std::vector<SInstance>& out) const
{
	out.clear();

	m_lastRequestedCount = 0;

	const float totalLength = spline.TotalLength();
	if (totalLength <= 0.0f)
		return;

	const float trimStart = clamp_tpl(m_trimStart, 0.0f, totalLength);
	const float trimEnd = clamp_tpl(m_trimEnd, 0.0f, totalLength - trimStart);
	const float usable = totalLength - trimStart - trimEnd;
	if (usable < 0.0f)
		return;

	// How many, and how far apart, per mode.
	int   instanceCount = 0;
	float spacing = 0.0f;

	switch (m_spacingMode)
	{
	case EDistributorSpacing::FixedStep:
		spacing = max(0.01f, m_step);
		instanceCount = static_cast<int>(usable / spacing) + 1;
		break;
	case EDistributorSpacing::FixedCount:
		instanceCount = max(0, m_count);
		spacing = (instanceCount > 1) ? (usable / static_cast<float>(instanceCount - 1)) : usable;
		break;
	case EDistributorSpacing::Density:
		instanceCount = static_cast<int>(usable * max(0.0f, m_density) + 0.5f);
		spacing = (instanceCount > 0) ? (usable / static_cast<float>(instanceCount)) : usable;
		break;
	case EDistributorSpacing::MeshLength:
		{
			// As many sections as the mesh's OWN length tiles into the curve, rounded, with the
			// residual spread evenly over the spacing. Together with Stretch To Fit that residual
			// disappears into a scale of at most a few per cent; without it, it shows as even gaps.
			// The first loadable mesh sets the length - with a weighted list of mixed lengths there
			// is no single answer, and picking the first is at least predictable.
			float meshLength = 0.0f;
			float meshStart = 0.0f;
			for (int i = 0; i < static_cast<int>(m_meshes.meshes.size()); ++i)
			{
				if (GetMeshForwardExtent(i, meshLength, meshStart))
					break;
			}

			if (meshLength > 0.0001f)
			{
				instanceCount = max(1, static_cast<int>(usable / meshLength + 0.5f));
				spacing = usable / static_cast<float>(instanceCount);
			}
			else
			{
				spacing = max(0.01f, m_step);
				instanceCount = static_cast<int>(usable / spacing) + 1;
			}
		}
		break;
	}

	// A closed curve's last instance would sit on top of the first one; drop it.
	if (spline.IsClosed() && m_spacingMode == EDistributorSpacing::FixedStep && instanceCount > 1 &&
	    fabs_tpl(usable - static_cast<float>(instanceCount - 1) * spacing) < 0.001f)
	{
		--instanceCount;
	}

	instanceCount = min(instanceCount, kMaxInstances);
	m_lastRequestedCount = instanceCount;
	if (instanceCount <= 0)
		return;

	out.reserve(static_cast<size_t>(instanceCount) + 2);

	const float scaleLow = min(m_scaleMin, m_scaleMax);
	const float scaleHigh = max(m_scaleMin, m_scaleMax);

	for (int i = 0; i < instanceCount; ++i)
	{
		CInstanceRandom random(MakeInstanceSeed(m_seed, i));

		// Draw in a fixed order, always, so that turning one feature off does not reshuffle
		// everything after it: skip, mesh, spacing jitter, scale, rotation, offset.
		const float skipDraw = random.Unit();
		const float meshDraw = random.Unit();
		const float spacingDraw = random.Range(-1.0f, 1.0f);
		const float scaleDraw = random.Unit();
		const Vec3  rotationDraw(random.Range(-1.0f, 1.0f), random.Range(-1.0f, 1.0f), random.Range(-1.0f, 1.0f));
		const Vec3  offsetDraw(random.Unit(), random.Unit(), random.Unit());

		if (m_skipProbability > 0.0f && skipDraw < m_skipProbability)
			continue;

		const int meshIndex = PickMesh(meshDraw);
		if (meshIndex < 0)
			continue;

		float distance = trimStart + static_cast<float>(i) * spacing;
		if (m_spacingJitter > 0.0f)
			distance += spacingDraw * m_spacingJitter * spacing * 0.5f;
		distance = clamp_tpl(distance, trimStart, trimStart + usable);

		const float t = (totalLength > 0.0f) ? clamp_tpl(distance / totalLength, 0.0f, 1.0f) : 0.0f;

		Vec3 position = spline.PosByDistance(distance);
		if (m_bSnapToTerrain && gEnv->p3DEngine != nullptr)
			position.z = gEnv->p3DEngine->GetTerrainElevation(position.x, position.y);

		const Vec3 tangent = spline.EvalTangent(t);
		const Vec3 normal = spline.EvalNormal(t);

		// Chord alignment aims the instance at where the NEXT one starts instead of along the
		// tangent at its own sample. A straight mesh is a chord, not a curve: oriented by the
		// tangent, both of its ends sit off the line and the chain opens at every bend. The last
		// instance has no successor, so it aims at the end of the usable curve, and if that is where
		// it already is it keeps the tangent.
		Vec3  forward = tangent;
		float chordLength = 0.0f;

		if (m_alignMode == EDistributorAlign::Chord)
		{
			const float nextDistance = min(distance + spacing, trimStart + usable);
			const Vec3  nextPosition = spline.PosByDistance(nextDistance);

			Vec3 chord = nextPosition - position;
			chordLength = chord.GetLength();

			if (chordLength > 0.0001f)
			{
				forward = chord / chordLength;
			}
		}

		Matrix33 frame = BuildFrame(position, forward, normal);

		// The constant turn first, then the jitter - the same order legacy applies mv_zAngle in
		// (SplineDistributor.cpp:274-279), so an existing setup keeps its look.
		if (m_zAngle != 0.0f)
			frame = frame * Matrix33::CreateRotationZ(DEG2RAD(m_zAngle));

		if (!m_rotationJitter.IsZero())
		{
			const Ang3 jitter(DEG2RAD(rotationDraw.x * m_rotationJitter.x),
			                  DEG2RAD(rotationDraw.y * m_rotationJitter.y),
			                  DEG2RAD(rotationDraw.z * m_rotationJitter.z));
			frame = frame * Matrix33::CreateRotationXYZ(jitter);
		}

		// Scale: the jitter range, a linear ramp along the curve, and optionally the spline's own
		// per-point width - which is what makes a row of rocks taper with the path that carries it.
		float scale = scaleLow + (scaleHigh - scaleLow) * scaleDraw;
		scale *= (1.0f - t) * m_scaleRampStart + t * m_scaleRampEnd;

		if (m_bWidthScales)
			scale *= spline.GetLocalWidth(t, m_defaultWidth);

		if (scale <= 0.0f)
			continue;

		const Vec3 offset(m_offsetMin.x + (m_offsetMax.x - m_offsetMin.x) * offsetDraw.x,
		                  m_offsetMin.y + (m_offsetMax.y - m_offsetMin.y) * offsetDraw.y,
		                  m_offsetMin.z + (m_offsetMax.z - m_offsetMin.z) * offsetDraw.z);

		// Stretch To Fit makes the section exactly as long as the gap it has to bridge, which is
		// what closes a fence instead of merely aiming it.
		float forwardStretch = 1.0f;
		if (m_bStretchToFit && m_alignMode == EDistributorAlign::Chord && chordLength > 0.0001f)
		{
			float meshLength = 0.0f;
			float meshStart = 0.0f;
			if (GetMeshForwardExtent(meshIndex, meshLength, meshStart) && scale > 0.0f)
			{
				forwardStretch = chordLength / (meshLength * scale);
			}
		}

		SInstance instance;
		instance.meshIndex = meshIndex;
		instance.tm = BuildInstanceTransform(frame, meshIndex, scale, forwardStretch, position, offset);

		out.push_back(instance);
	}

	// The end caps: one at each end of the usable range, unrolled by anything random so that they
	// land exactly where the curve does. A closed curve has no ends and gets none.
	const int endCapIndex = GetEndCapMeshIndex();
	if (!spline.IsClosed() && endCapIndex < static_cast<int>(m_statObjs.size()) && m_statObjs[endCapIndex] != nullptr)
	{
		const float capDistances[2] = { trimStart, trimStart + usable };

		for (const float distance : capDistances)
		{
			const float t = (totalLength > 0.0f) ? clamp_tpl(distance / totalLength, 0.0f, 1.0f) : 0.0f;

			Vec3 position = spline.PosByDistance(distance);
			if (m_bSnapToTerrain && gEnv->p3DEngine != nullptr)
				position.z = gEnv->p3DEngine->GetTerrainElevation(position.x, position.y);

			Matrix33 frame = BuildFrame(position, spline.EvalTangent(t), spline.EvalNormal(t));
			if (m_zAngle != 0.0f)
				frame = frame * Matrix33::CreateRotationZ(DEG2RAD(m_zAngle));

			SInstance instance;
			instance.meshIndex = endCapIndex;
			instance.tm = BuildInstanceTransform(frame, endCapIndex, 1.0f, 1.0f, position, Vec3(ZERO));

			out.push_back(instance);
		}
	}
}

// ---------------------------------------------------------------------------
// Render nodes
// ---------------------------------------------------------------------------

uint64 CDistributorComponent::BuildRenderFlags() const
{
	uint64 flags = 0;

	if (m_bOutdoorOnly)
		flags |= ERF_OUTDOORONLY;
	if (m_bCastShadows)
		flags |= ERF_CASTSHADOWMAPS | ERF_HAS_CASTSHADOWMAPS;
	if (m_bRainOccluder)
		flags |= ERF_RAIN_OCCLUDER;
	if (m_bRegisterByBBox)
		flags |= ERF_REGISTER_BY_BBOX;
	if (m_hideable == EDistributorHideable::Hidable)
		flags |= ERF_HIDABLE;
	if (m_hideable == EDistributorHideable::HidableSecondary)
		flags |= ERF_HIDABLE_SECONDARY;
	if (m_bExcludeFromTriangulation)
		flags |= ERF_EXCLUDE_FROM_TRIANGULATION;
	if (m_bNoDecals)
		flags |= ERF_NO_DECALNODE_DECALS;
	if (m_bRecvWind)
		flags |= ERF_RECVWIND;
	if (m_bGoodOccluder)
		flags |= ERF_GOOD_OCCLUDER;

	// The instances belong to the entity, so they follow the entity's own visibility.
	if (m_pEntity != nullptr && m_pEntity->IsHidden())
		flags |= ERF_HIDDEN;

	return flags;
}

void CDistributorComponent::SetNodeCount(int count)
{
	if (gEnv->p3DEngine == nullptr)
		return;

	const int oldCount = static_cast<int>(m_renderNodes.size());

	if (oldCount < count)
	{
		m_renderNodes.resize(count, nullptr);
		m_nodeRegistered.resize(count, 0);

		for (int i = oldCount; i < count; ++i)
		{
			// eERType_MovableBrush, not eERType_Brush: the movable one is the brush class that
			// implements SetOwnerEntity (Brush.h:142-152), and it is not one of the types the octree
			// exporter writes (ObjectsTree_Serialize.cpp:398 lists eERType_Brush only). Both halves
			// matter - the owner keeps SaveObjects skipping it, the type keeps it out of the export
			// even if an owner is ever lost.
			IBrush* pBrush = static_cast<IBrush*>(gEnv->p3DEngine->CreateRenderNode(eERType_MovableBrush));
			if (pBrush == nullptr)
				continue;

			pBrush->SetOwnerEntity(m_pEntity);
			m_renderNodes[i] = pBrush;
			m_nodeRegistered[i] = 0;
		}
	}
	else if (oldCount > count)
	{
		for (int i = count; i < oldCount; ++i)
		{
			if (m_renderNodes[i] != nullptr)
				gEnv->p3DEngine->DeleteRenderNode(m_renderNodes[i]); // unregisters first
		}
		m_renderNodes.resize(count);
		m_nodeRegistered.resize(count);
	}
}

void CDistributorComponent::DestroyNodes()
{
	if (gEnv->p3DEngine != nullptr)
	{
		for (IRenderNode* pNode : m_renderNodes)
		{
			if (pNode != nullptr)
				gEnv->p3DEngine->DeleteRenderNode(pNode);
		}
	}

	m_renderNodes.clear();
	m_nodeRegistered.clear();
	m_instances.clear();
	m_lastBuildSignature = 0;
}

void CDistributorComponent::ApplyInstance(int index, const SInstance& instance, bool bOnlyTransform)
{
	if (index < 0 || index >= static_cast<int>(m_renderNodes.size()))
		return;

	IRenderNode* pNode = m_renderNodes[index];
	if (pNode == nullptr)
		return;

	IStatObj* pStatObj = (instance.meshIndex >= 0 && instance.meshIndex < static_cast<int>(m_statObjs.size()))
	                     ? m_statObjs[instance.meshIndex].get()
	                     : nullptr;

	// Flags and ratios BEFORE the geometry: SetEntityStatObj ends in CBrush::SetMatrix, which
	// re-registers the node in the octree (Brush.cpp:224-227), and that registration should see the
	// final flags rather than the ones the node was created with.
	if (!bOnlyTransform)
	{
		pNode->SetRndFlags(static_cast<IRenderNode::RenderFlagsType>(BuildRenderFlags()));
		pNode->SetViewDistRatio(m_viewDistRatio);
		pNode->SetLodRatio(m_lodRatio);
		pNode->SetMaterial(m_pEntity != nullptr ? m_pEntity->GetMaterial() : nullptr);

		if (m_pEntity != nullptr)
		{
			// So that clicking an instance in the viewport selects the entity that owns it.
			pNode->SetEditorObjectId(m_pEntity->GetEditorObjectID());
		}
	}

	Matrix34A tm = instance.tm;
	pNode->SetEntityStatObj(pStatObj, &tm);

	// Registered exactly ONCE, the first time this node carries geometry.
	//
	// CBrush::SetMatrix already unregisters and re-registers the node itself whenever the matrix
	// really changes (Brush.cpp:224-227), and that is also the ONLY place CalcBBox() runs - so the
	// world bounding box the octree indexes the node by comes from there. This function used to add
	// its own RegisterEntity() for every instance of every rebuild on top of that, which is at best
	// redundant (AsyncOctreeUpdate unregisters and re-inserts synchronously, 3dEngine.cpp:6580) and
	// at worst re-indexes a node the engine has just indexed correctly. Doing it once, on the first
	// apply, is what CEntitySlot::UpdateRenderNode does (EntitySlot.cpp:213-220) and is enough:
	// every later move re-registers through SetMatrix.
	//
	// Note the matching trap on the other side: SetMatrix EARLY-OUTS when the matrix is unchanged
	// (Brush.cpp:203-204), so a node whose placement happens to equal the matrix it already has
	// gets no CalcBBox() and no registration from that call. The explicit first-apply registration
	// below covers the only case where that can be the node's whole life - a brand new node whose
	// instance matrix is the identity it was constructed with.
	if (index < static_cast<int>(m_nodeRegistered.size()) && m_nodeRegistered[index] == 0)
	{
		gEnv->p3DEngine->RegisterEntity(pNode);
		m_nodeRegistered[index] = 1;
	}
}

// ---------------------------------------------------------------------------
// Rebuild guard and diagnostics
// ---------------------------------------------------------------------------

uint64 CDistributorComponent::ComputeBuildSignature(ISplineShape& spline) const
{
	// A digest, not a hash of record: it only has to change whenever something a rebuild reads
	// changes. Floats go in through their bit pattern so that a small difference is never rounded
	// away, and the curve contributes its length so that a moved spline is noticed here too.
	uint64 hash = 0xCBF29CE484222325ull;

	const auto mix = [&hash](uint64 value)
	{
		hash ^= value;
		hash *= 0x100000001B3ull;
	};

	const auto mixFloat = [&mix](float value)
	{
		uint32 bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		mix(bits);
	};

	const auto mixString = [&mix](const string& value)
	{
		for (const char* p = value.c_str(); *p != 0; ++p)
			mix(static_cast<uint64>(static_cast<unsigned char>(*p)));
		mix(0xFFull);
	};

	mix(m_bEnabled ? 1 : 0);
	mix(m_seed);
	mix(static_cast<uint64>(m_spacingMode));
	mixFloat(m_step);
	mix(static_cast<uint64>(m_count));
	mixFloat(m_density);
	mixFloat(m_spacingJitter);
	mixFloat(m_trimStart);
	mixFloat(m_trimEnd);
	mixFloat(m_skipProbability);
	mix(static_cast<uint64>(m_alignMode));
	mix(static_cast<uint64>(m_forwardAxis));
	mix(m_bStretchToFit ? 1 : 0);
	mix(m_bPivotAtStart ? 1 : 0);
	mixFloat(m_zAngle);
	mixFloat(m_rotationJitter.x);
	mixFloat(m_rotationJitter.y);
	mixFloat(m_rotationJitter.z);
	mixFloat(m_scaleMin);
	mixFloat(m_scaleMax);
	mixFloat(m_scaleRampStart);
	mixFloat(m_scaleRampEnd);
	mix(m_bWidthScales ? 1 : 0);
	mixFloat(m_defaultWidth);
	mixFloat(m_offsetMin.x);
	mixFloat(m_offsetMin.y);
	mixFloat(m_offsetMin.z);
	mixFloat(m_offsetMax.x);
	mixFloat(m_offsetMax.y);
	mixFloat(m_offsetMax.z);
	mix(m_bSnapToTerrain ? 1 : 0);

	mix(m_bOutdoorOnly ? 1 : 0);
	mix(m_bCastShadows ? 1 : 0);
	mix(m_bRainOccluder ? 1 : 0);
	mix(m_bRegisterByBBox ? 1 : 0);
	mix(static_cast<uint64>(m_hideable));
	mix(m_bExcludeFromTriangulation ? 1 : 0);
	mix(m_bNoDecals ? 1 : 0);
	mix(m_bRecvWind ? 1 : 0);
	mix(m_bGoodOccluder ? 1 : 0);
	mix(static_cast<uint64>(m_viewDistRatio));
	mix(static_cast<uint64>(m_lodRatio));

	for (const SDistributorMesh& entry : m_meshes.meshes)
	{
		mixFloat(entry.weight);
		mixString(entry.mesh.value);
	}
	mixString(m_endCapMesh.value);

	mixFloat(spline.TotalLength());
	mix(spline.IsClosed() ? 1 : 0);

	// Never 0: that value means "never built".
	return hash != 0 ? hash : 1;
}

void CDistributorComponent::LogRebuild(float curveLength, int requestedCount, int placedCount) const
{
	static const char* const szSpacingNames[] = { "Fixed Step", "Fixed Count", "Density", "Mesh Length" };
	static const char* const szAlignNames[] = { "Follow Tangent", "World Up", "Surface Normal", "Fixed", "Point To Next" };
	static const char* const szAxisNames[] = { "+X", "+Y", "-X", "-Y" };

	const int spacingIndex = clamp_tpl(static_cast<int>(m_spacingMode), 0, static_cast<int>(CRY_ARRAY_COUNT(szSpacingNames)) - 1);
	const int alignIndex = clamp_tpl(static_cast<int>(m_alignMode), 0, static_cast<int>(CRY_ARRAY_COUNT(szAlignNames)) - 1);

	int loadedMeshes = 0;
	for (size_t i = 0; i < m_meshes.meshes.size() && i < m_statObjs.size(); ++i)
	{
		if (m_statObjs[i] != nullptr)
			++loadedMeshes;
	}

	const int  endCapIndex = GetEndCapMeshIndex();
	const bool bEndCapLoaded = endCapIndex < static_cast<int>(m_statObjs.size()) && m_statObjs[endCapIndex] != nullptr;

	CryLog("Distributor on entity \"%s\": curve %.2f m, %s (step %.2f, count %d, density %.3f), trim %.2f/%.2f, "
	       "skip %.2f, align %s (forward %s%s%s), meshes %d listed / %d loaded, end cap %s - %d asked for, "
	       "%d placed, %d nodes.",
	       (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>",
	       curveLength, szSpacingNames[spacingIndex], m_step, m_count, m_density,
	       m_trimStart, m_trimEnd, m_skipProbability, szAlignNames[alignIndex],
	       szAxisNames[clamp_tpl(static_cast<int>(m_forwardAxis), 0, static_cast<int>(CRY_ARRAY_COUNT(szAxisNames)) - 1)],
	       m_bStretchToFit ? ", stretched" : "", m_bPivotAtStart ? ", pivot at start" : "",
	       static_cast<int>(m_meshes.meshes.size()), loadedMeshes, bEndCapLoaded ? "loaded" : "none",
	       requestedCount, placedCount, static_cast<int>(m_renderNodes.size()));
}

// ---------------------------------------------------------------------------
// Rebuild
// ---------------------------------------------------------------------------

void CDistributorComponent::Rebuild(bool bForce)
{
	if (!m_bEnabled)
	{
		DestroyNodes();
		return;
	}

	ISplineShape* pSpline = EnsureBound();
	if (pSpline == nullptr)
	{
		DestroyNodes();
		return;
	}

	// ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED arrives on every inspector serialization pass, not
	// only when a value really changed, so an open property panel used to tear every render node
	// down and build it again many times a second. Nothing good comes of that, and one of the bad
	// things is losing a node between the octree's asynchronous unregister and the next register.
	const uint64 signature = ComputeBuildSignature(*pSpline);
	if (!bForce && signature == m_lastBuildSignature && m_renderNodes.size() == m_instances.size())
	{
		return;
	}

	m_lastBuildSignature = signature;

	ReloadMeshes();

	BuildInstances(*pSpline, m_instances);

	SetNodeCount(static_cast<int>(m_instances.size()));

	for (int i = 0; i < static_cast<int>(m_instances.size()); ++i)
		ApplyInstance(i, m_instances[i], false);

	LogRebuild(pSpline->TotalLength(), m_lastRequestedCount, static_cast<int>(m_instances.size()));
	WarnIfNothingToDistribute();
}

void CDistributorComponent::WarnIfNothingToDistribute()
{
	// The Meshes list is what gets distributed ALONG the curve. End Cap Mesh is a different thing:
	// it places exactly one instance at each end and nothing in between, so a distributor whose
	// only mesh is the end cap looks broken - two objects, one at the start, one at the end. That
	// is an easy mistake to make, because the end cap is a single inviting mesh picker while the
	// list needs a row to be added first, and until now it failed silently.
	bool bAnyMeshLoaded = false;
	for (size_t i = 0; i < m_meshes.meshes.size() && i < m_statObjs.size(); ++i)
	{
		if (m_statObjs[i] != nullptr)
		{
			bAnyMeshLoaded = true;
			break;
		}
	}

	if (bAnyMeshLoaded)
	{
		m_bWarnedNoMesh = false;
		return;
	}

	if (m_bWarnedNoMesh)
		return;

	m_bWarnedNoMesh = true;

	const int  endCapIndex = GetEndCapMeshIndex();
	const bool bEndCapLoaded = endCapIndex < static_cast<int>(m_statObjs.size()) && m_statObjs[endCapIndex] != nullptr;

	if (bEndCapLoaded)
	{
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Distributor on entity \"%s\": the Meshes list is empty, so only the End Cap Mesh was placed - one "
		           "instance at each end of the curve and nothing in between. Add the mesh to Meshes to distribute it "
		           "along the curve.",
		           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>");
	}
	else if (!m_meshes.meshes.empty())
	{
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Distributor on entity \"%s\": none of the %d meshes in the list could be loaded - nothing was placed.",
		           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>",
		           static_cast<int>(m_meshes.meshes.size()));
	}
	else
	{
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Distributor on entity \"%s\": the Meshes list is empty - nothing to place along the curve.",
		           (m_pEntity != nullptr) ? m_pEntity->GetName() : "<none>");
	}
}

void CDistributorComponent::RebuildTransformsOnly()
{
	if (!m_bEnabled || m_renderNodes.empty())
		return;

	ISplineShape* pSpline = EnsureBound();
	if (pSpline == nullptr)
		return;

	// The same instance list, re-placed. Nothing random is drawn again, so a move cannot reshuffle
	// which mesh went where - the fast path CSplineDistributor::InvalidateTM takes (:204-212).
	std::vector<SInstance> instances;
	BuildInstances(*pSpline, instances);

	if (instances.size() != m_instances.size())
	{
		// The curve's length changed with the transform (a scaled entity), so the count changed too:
		// this is a full rebuild after all.
		Rebuild();
		return;
	}

	m_instances = instances;
	for (int i = 0; i < static_cast<int>(m_instances.size()); ++i)
		ApplyInstance(i, m_instances[i], true);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void CDistributorComponent::Initialize()
{
	Rebuild();
}

// ---------------------------------------------------------------------------
// Editor actions and baking
// ---------------------------------------------------------------------------

bool CDistributorComponent::GetEditorAction(int index, SEditorActionDesc& out) const
{
	if (index != 0)
		return false;

	// String literals: the property tree keeps the pointers SEditorActionDesc hands it.
	out.szToolClassName = "EditTool.AreaDistributorBake";
	out.szLabel = "Bake To Brushes";
	return true;
}

int CDistributorComponent::GetBakeInstanceCount() const
{
	return static_cast<int>(m_instances.size());
}

bool CDistributorComponent::GetBakeInstance(int index, SBakeInstance& out) const
{
	if (index < 0 || index >= static_cast<int>(m_instances.size()))
		return false;

	const SInstance& instance = m_instances[index];

	out = SBakeInstance();
	out.tm = instance.tm;
	out.renderFlags = BuildRenderFlags();
	out.viewDistRatio = m_viewDistRatio;
	out.lodRatio = m_lodRatio;

	// The path, copied into the caller's fixed buffer - no string crosses the boundary (heap rule,
	// IDistributorBake.h). The end cap lives one past the mesh list, which is how ReloadMeshes
	// lays m_statObjs out.
	const char* szPath = nullptr;
	if (instance.meshIndex >= 0 && instance.meshIndex < static_cast<int>(m_meshes.meshes.size()))
	{
		szPath = m_meshes.meshes[instance.meshIndex].mesh.value.c_str();
	}
	else if (instance.meshIndex == GetEndCapMeshIndex())
	{
		szPath = m_endCapMesh.value.c_str();
	}

	if (szPath != nullptr)
	{
		cry_strcpy(out.meshPath, szPath);
	}

	return true;
}

void CDistributorComponent::SetBakeEnabled(bool bEnabled)
{
	if (m_bEnabled == bEnabled)
		return;

	m_bEnabled = bEnabled;

	// Forced: Enabled is part of the signature, but going through the guard would be one more thing
	// to get wrong on a path whose whole job is to make the live instances disappear.
	Rebuild(true);
}

void CDistributorComponent::OnShapeChanged(IShapeComponent& shape, EShapeChangeReason reason)
{
	if (reason == EShapeChangeReason::Transform)
	{
		RebuildTransformsOnly();
		return;
	}

	// Forced: a point can move without changing the curve's length, and the length is all the
	// signature can see of the shape.
	Rebuild(true);
}

void CDistributorComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		Rebuild();
		break;
	case ENTITY_EVENT_HIDE:
	case ENTITY_EVENT_UNHIDE:
		{
			const IRenderNode::RenderFlagsType flags = static_cast<IRenderNode::RenderFlagsType>(BuildRenderFlags());
			for (IRenderNode* pNode : m_renderNodes)
			{
				if (pNode != nullptr)
					pNode->SetRndFlags(flags);
			}
		}
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CDistributorComponent::GetEventMask() const
{
	// No update event: an idle distributor costs nothing per frame.
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED | ENTITY_EVENT_HIDE | ENTITY_EVENT_UNHIDE;
}

void CDistributorComponent::OnShutDown()
{
	Unbind();
	DestroyNodes();
	m_statObjs.clear();
}

} // namespace AreaComponents
} // namespace Cry
