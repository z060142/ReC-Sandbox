// Copyright 2015-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include <CryCore/Platform/platform.h>
#include "../RenderDll/Common/Shadow_Renderer.h"

class ShadowCacheGenerator : public Cry3DEngineBase
{
public:
	ShadowCacheGenerator(CLightEntity* pLightEntity, ShadowMapFrustum::ShadowCacheData::eUpdateStrategy nUpdateStrategy)
		: m_pLightEntity(pLightEntity)
		, m_nUpdateStrategy(nUpdateStrategy)
	{}

	static void  ResetGenerationID() { m_cacheGenerationId = 0; }

	void InitShadowFrustum(ShadowMapFrustumPtr& pFr, int nLod, int nFirstStaticLod, float fDistFromViewDynamicLod, float fRadiusDynamicLod, const SRenderingPassInfo& passInfo);
	void InitHeightMapAOFrustum(ShadowMapFrustumPtr& pFr, int nLod, int nFirstStaticLod, const SRenderingPassInfo& passInfo);
	void InitLPVRsmFrustum(ShadowMapFrustumPtr& pFr, int nLod, int nCacheLod, int nCascade, const SRenderingPassInfo& passInfo);

	// World space box the given light propagation volume cascade covers this frame. Has to stay in
	// sync with CLPVStage::UpdateCascadeParameters(), which derives the grid origin the very same way.
	static AABB GetLPVBox(const SRenderingPassInfo& passInfo, int nCascade);

	// True for render node types whose shadow submission is deterministic with respect to the main
	// camera. See COctreeNode::RenderObjectIntoShadowViews for why the others are excluded.
	static bool IsStableRsmCaster(EERType type);

	// Fixed traversal-marking bit of the LPV RSM in IRenderNode::m_onePassTraversalShadowCascades.
	// The frustum's nShadowMapLod is a slot index that moves around when the dynamic cascade line-up
	// changes from frame to frame, so marking with BIT(nShadowMapLod) both drifts and collides with
	// the bits of the real cascades. The marking and the cull mask check use this constant instead.
	static const int kLPVRsmTraversalLod = 30;

private:
	static const int    MAX_RENDERNODES_PER_FRAME = 50;
	static const float  AO_FRUSTUM_SLOPE_BIAS;

	static int   m_cacheGenerationId;

	void         InitCachedFrustum(ShadowMapFrustumPtr& pFr, ShadowMapFrustum::ShadowCacheData::eUpdateStrategy nUpdateStrategy, int nLod, int cacheLod, int nTexSize, const Vec3& vLightPos, const AABB& projectionBoundsLS, const SRenderingPassInfo& passInfo);
	void         CollectCastersForCachedFrustum(ShadowMapFrustumPtr& pFr, const PodArray<struct SPlaneObject>* pCastersHull, int maxNodesPerFrame, const SRenderingPassInfo& passInfo);
	void         AddTerrainCastersToFrustum(ShadowMapFrustum* pFr, const SRenderingPassInfo& passInfo);

	void         GetCasterBox(AABB& BBoxWS, AABB& BBoxLS, float fRadius, const Matrix34& matView, const SRenderingPassInfo& passInfo);
	Matrix44     GetViewMatrix(const SRenderingPassInfo& passInfo);

	uint8        GetNextGenerationID() const;

	CLightEntity* m_pLightEntity;
	ShadowMapFrustum::ShadowCacheData::eUpdateStrategy m_nUpdateStrategy;
};
