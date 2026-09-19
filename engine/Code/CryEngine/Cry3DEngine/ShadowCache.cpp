// Copyright 2015-2021 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "ShadowCache.h"
#include "LightEntity.h"
#include "VisAreas.h"

const float ShadowCacheGenerator::AO_FRUSTUM_SLOPE_BIAS = 0.5f;
int ShadowCacheGenerator::m_cacheGenerationId = 0;

uint8 ShadowCacheGenerator::GetNextGenerationID() const
{
	// increase generation ID. Make sure we never return a value that
	// wraps around to 0 as this is used for invalidating render nodes
	int nextID = m_cacheGenerationId++;
	if (uint8(nextID) == 0)
		nextID = m_cacheGenerationId++;

	return uint8(nextID);
}

void ShadowCacheGenerator::InitShadowFrustum(ShadowMapFrustumPtr& pFr, int nLod, int nFirstStaticLod, float fDistFromViewDynamicLod, float fRadiusDynamicLod, const SRenderingPassInfo& passInfo)
{
	FUNCTION_PROFILER_3DENGINE;
	assert(nLod >= 0);

	if (!pFr)
		pFr = new ShadowMapFrustum;

	if (!pFr->pShadowCacheData)
		pFr->pShadowCacheData = std::make_shared<ShadowMapFrustum::ShadowCacheData>();

	const int shadowCacheLod = nLod - nFirstStaticLod;
	CRY_ASSERT(shadowCacheLod >= 0 && shadowCacheLod < MAX_GSM_CACHED_LODS_NUM);

	// check if we have come too close to the border of the map
	ShadowMapFrustum::ShadowCacheData::eUpdateStrategy nUpdateStrategy = m_nUpdateStrategy;
	if (nUpdateStrategy == ShadowMapFrustum::ShadowCacheData::eIncrementalUpdate && Get3DEngine()->m_CachedShadowsBounds.IsReset())
	{
		const float fDistFromCenter = (passInfo.GetCamera().GetPosition() - pFr->aabbCasters.GetCenter()).GetLength() + fDistFromViewDynamicLod + fRadiusDynamicLod;
		if (fDistFromCenter > pFr->aabbCasters.GetSize().x / 2.0f)
		{
			nUpdateStrategy = ShadowMapFrustum::ShadowCacheData::eFullUpdate;

			if (!gEnv->IsEditing())
			{
				CryLog("Update required for cached shadow map %d.", shadowCacheLod);
				CryLog("\tConsider increasing shadow cache resolution (r_ShadowsCacheResolutions) " \
				  "or setting up manual bounds for cached shadows via flow graph if this happens too often");
			}
		}
	}

	AABB projectionBoundsLS(AABB::RESET);
	const int nTexRes = GetRenderer()->GetCachedShadowsResolution()[shadowCacheLod];

	// non incremental update: set new bounding box and estimate near/far planes
	if (nUpdateStrategy != ShadowMapFrustum::ShadowCacheData::eIncrementalUpdate)
	{
		Matrix34 matView = Matrix34(GetViewMatrix(passInfo).GetTransposed());

		if (!Get3DEngine()->m_CachedShadowsBounds.IsReset())
		{
			float fBoxScale = powf(Get3DEngine()->m_fCachedShadowsCascadeScale, float(shadowCacheLod));
			Vec3 fBoxScaleXY(max(1.f, fBoxScale));
			fBoxScaleXY.z = 1.f;

			Vec3 vExt = Get3DEngine()->m_CachedShadowsBounds.GetSize().CompMul(fBoxScaleXY * 0.5f);
			Vec3 vCenter = Get3DEngine()->m_CachedShadowsBounds.GetCenter();

			pFr->aabbCasters = AABB(vCenter - vExt, vCenter + vExt);
			projectionBoundsLS = AABB::CreateTransformedAABB(matView, pFr->aabbCasters);
		}
		else
		{
			const float fDesiredPixelDensity = fRadiusDynamicLod / GetCVars()->e_ShadowsMaxTexRes;
			GetCasterBox(pFr->aabbCasters, projectionBoundsLS, fDesiredPixelDensity * nTexRes, matView, passInfo);
		}
	}

	// finally init frustum
	pFr->m_eFrustumType = ShadowMapFrustum::e_GsmCached;
	pFr->bBlendFrustum = GetCVars()->e_ShadowsBlendCascades > 0;
	pFr->fBlendVal = pFr->bBlendFrustum ? GetCVars()->e_ShadowsBlendCascadesVal : 1.0f;
	InitCachedFrustum(pFr, nUpdateStrategy, nLod, shadowCacheLod, nTexRes, m_pLightEntity->GetLightProperties().m_Origin, projectionBoundsLS, passInfo);

	// frustum debug
	if (GetCVars()->e_ShadowsCacheUpdate > 2 || GetCVars()->e_ShadowsFrustums > 0)
	{
		if (IRenderAuxGeom* pAux = GetRenderer()->GetIRenderAuxGeom())
		{
			SAuxGeomRenderFlags prevAuxFlags = pAux->GetRenderFlags();
			pAux->SetRenderFlags(e_Mode3D | e_AlphaNone | e_DepthTestOn);

			const ColorF cascadeColors[] = { Col_Red, Col_Green, Col_Blue, Col_Yellow, Col_Magenta, Col_Cyan };
			const uint colorCount = CRY_ARRAY_COUNT(cascadeColors);

			if (GetCVars()->e_ShadowsCacheUpdate > 2)
				pAux->DrawAABB(pFr->aabbCasters, false, cascadeColors[pFr->nShadowMapLod % colorCount], eBBD_Faceted);

			if (GetCVars()->e_ShadowsFrustums > 0)
				pFr->DrawFrustum(GetRenderer(), std::numeric_limits<int>::max());

			pAux->SetRenderFlags(prevAuxFlags);
		}
	}
}

void ShadowCacheGenerator::InitCachedFrustum(ShadowMapFrustumPtr& pFr, ShadowMapFrustum::ShadowCacheData::eUpdateStrategy nUpdateStrategy, int nLod, int cacheLod, int nTexSize, const Vec3& vLightPos, const AABB& projectionBoundsLS, const SRenderingPassInfo& passInfo)
{
	const auto frameID = passInfo.GetFrameID();

	pFr->RequestUpdates(1);
	pFr->nTexSize = nTexSize;

	if (nUpdateStrategy != ShadowMapFrustum::ShadowCacheData::eIncrementalUpdate)
	{
		CRY_ASSERT(cacheLod >= 0 && cacheLod < MAX_GSM_CACHED_LODS_NUM);

		pFr->bIncrementalUpdate = false;
		pFr->pShadowCacheData->Reset(GetNextGenerationID());
		pFr->RequestSamples(1);

		assert(m_pLightEntity->GetLightProperties().m_pOwner);
		pFr->pLightOwner = m_pLightEntity->GetLightProperties().m_pOwner;
		pFr->m_Flags = m_pLightEntity->GetLightProperties().m_Flags;
		pFr->nUpdateFrameId = frameID;
		pFr->nShadowMapLod = nLod;
		pFr->nShadowCacheLod = cacheLod;
		pFr->vProjTranslation = pFr->aabbCasters.GetCenter();
		pFr->vLightSrcRelPos = vLightPos - pFr->aabbCasters.GetCenter();
		pFr->fNearDist = -projectionBoundsLS.max.z;
		pFr->fFarDist = -projectionBoundsLS.min.z;
		pFr->fRendNear = pFr->fNearDist;
		pFr->fFOV = (float)RAD2DEG(atan_tpl(0.5 * projectionBoundsLS.GetSize().y / pFr->fNearDist)) * 2.f;
		pFr->fProjRatio = projectionBoundsLS.GetSize().x / projectionBoundsLS.GetSize().y;
		pFr->fRadius = m_pLightEntity->GetLightProperties().m_fRadius;
		pFr->fRendNear = pFr->fNearDist;
		pFr->fFrustrumSize = 1.0f / (Get3DEngine()->m_fGsmRange * pFr->aabbCasters.GetRadius() * 2.0f);
		pFr->bUseShadowsPool = false;

		const float arrWidthS[] = { 1.94f, 1.0f, 0.8f, 0.5f, 0.3f, 0.3f, 0.3f, 0.3f };
		pFr->fWidthS = pFr->fWidthT = arrWidthS[nLod];
		pFr->fBlurS = pFr->fBlurT = 0.0f;
	}
	else
	{
		pFr->bIncrementalUpdate = true;
	}

	// set up frustum planes for culling
	const ShadowMapInfo* pShadowMapInfo = m_pLightEntity->GetShadowMapInfo();
	const bool isExtendedFrustum = GetCVars()->e_ShadowsCacheExtendLastCascade && nLod == pShadowMapInfo->GetLodCount() - 1 && pFr->m_eFrustumType == ShadowMapFrustum::e_GsmCached;

	if (isExtendedFrustum)
	{
		CCamera frustumCam;
		Vec3 vLightDir = -pFr->vLightSrcRelPos.normalized();

		Matrix34A mat = Matrix33::CreateRotationVDir(vLightDir);
		mat.SetTranslation(pFr->vLightSrcRelPos + pFr->vProjTranslation);

		frustumCam.SetMatrixNoUpdate(mat);
		frustumCam.SetFrustum(256, 256, pFr->fFOV * (gf_PI / 180.0f), pFr->fNearDist, pFr->fFarDist);

		pFr->FrustumPlanes[0] = pFr->FrustumPlanes[1] = frustumCam;
	}
	else
	{
		ShadowMapFrustum* pDynamicFrustum = m_pLightEntity->GetShadowFrustum(nLod);
		CRY_ASSERT(pDynamicFrustum);

		pFr->FrustumPlanes[0] = pDynamicFrustum->FrustumPlanes[0];
		pFr->FrustumPlanes[1] = pDynamicFrustum->FrustumPlanes[1];
	}

	const bool bUseCastersHull = (nUpdateStrategy == ShadowMapFrustum::ShadowCacheData::eFullUpdateTimesliced);
	const int maxNodesPerFrame = (nUpdateStrategy == ShadowMapFrustum::ShadowCacheData::eIncrementalUpdate)
	                             ? GetCVars()->e_ShadowsCacheMaxNodesPerFrame * GetRenderer()->GetActiveGPUCount()
	                             : std::numeric_limits<int>::max();

	CollectCastersForCachedFrustum(pFr, bUseCastersHull ? &m_pLightEntity->GetCastersHull() : nullptr, maxNodesPerFrame, passInfo);
}

void ShadowCacheGenerator::CollectCastersForCachedFrustum(ShadowMapFrustumPtr& pFr, const PodArray<SPlaneObject>* pCastersHull, int maxNodesPerFrame, const SRenderingPassInfo& passInfo)
{
	const bool bExcludeDynamicDistanceShadows = GetCVars()->e_DynamicDistanceShadows != 0;

	IRenderNode* pCastingException = static_cast<CLightEntity*>(m_pLightEntity->GetLightProperties().m_pOwner)->GetCastingException();

	auto jobLambda = [=]()
	{
		m_pObjManager->MakeStaticShadowCastersList(pCastingException, pFr, pCastersHull,
			bExcludeDynamicDistanceShadows ? ERF_DYNAMIC_DISTANCESHADOWS : 0, maxNodesPerFrame, passInfo);

		AddTerrainCastersToFrustum(pFr, passInfo);
	};

	if (GetCVars()->e_ShadowsCacheJobs)
	{
		DECLARE_LAMBDA_JOB("job:shadows:MakeStaticShadowCastersList", TMakeStaticShadowCastersListJob);
		TMakeStaticShadowCastersListJob(jobLambda).Run(JobManager::eRegularPriority, &pFr->pShadowCacheData->mTraverseOctreeJobState);
	}
	else
	{
		jobLambda();
	}
}

void ShadowCacheGenerator::InitHeightMapAOFrustum(ShadowMapFrustumPtr& pFr, int nLod, int nFirstStaticLod, const SRenderingPassInfo& passInfo)
{
	FUNCTION_PROFILER_3DENGINE;
	assert(nLod >= 0);

	if (!pFr)
		pFr = new ShadowMapFrustum;

	if (!pFr->pShadowCacheData)
		pFr->pShadowCacheData = std::make_shared<ShadowMapFrustum::ShadowCacheData>();

	static ICVar* pHeightMapAORes = gEnv->pConsole->GetCVar("r_HeightMapAOResolution");
	static ICVar* pHeightMapAORange = gEnv->pConsole->GetCVar("r_HeightMapAORange");

	ShadowMapFrustum::ShadowCacheData::eUpdateStrategy nUpdateStrategy = m_nUpdateStrategy;

	// check if we have come too close to the border of the map
	const float fDistFromCenter = (passInfo.GetCamera().GetPosition() - pFr->aabbCasters.GetCenter()).GetLength() + pHeightMapAORange->GetFVal() * 0.25f;
	if (fDistFromCenter > pFr->aabbCasters.GetSize().x / 2.0f)
	{
		nUpdateStrategy = ShadowMapFrustum::ShadowCacheData::eFullUpdate;

		if (!gEnv->IsEditing())
		{
			CryLog("Update required for height map AO.");
			CryLog("\tConsider increasing height map AO range (r_HeightMapAORange) if this happens too often");
		}
	}

	AABB projectionBoundsLS(AABB::RESET);

	// non incremental update: set new bounding box and estimate near/far planes
	if (nUpdateStrategy != ShadowMapFrustum::ShadowCacheData::eIncrementalUpdate)
	{
		// Top down view
		Matrix34 topDownView(IDENTITY);
		topDownView.m03 = -passInfo.GetCamera().GetPosition().x;
		topDownView.m13 = -passInfo.GetCamera().GetPosition().y;
		topDownView.m23 = -passInfo.GetCamera().GetPosition().z - m_pLightEntity->GetLightProperties().m_Origin.GetLength();

		GetCasterBox(pFr->aabbCasters, projectionBoundsLS, pHeightMapAORange->GetFVal() / 2.0f, topDownView, passInfo);

		// snap to texels
		const float fSnap = pHeightMapAORange->GetFVal() / pHeightMapAORes->GetFVal();
		pFr->aabbCasters.min.x = fSnap * int(pFr->aabbCasters.min.x / fSnap);
		pFr->aabbCasters.min.y = fSnap * int(pFr->aabbCasters.min.y / fSnap);
		pFr->aabbCasters.min.z = fSnap * int(pFr->aabbCasters.min.z / fSnap);

		pFr->aabbCasters.max.x = pFr->aabbCasters.min.x + pHeightMapAORange->GetFVal();
		pFr->aabbCasters.max.y = pFr->aabbCasters.min.y + pHeightMapAORange->GetFVal();
		pFr->aabbCasters.max.z = fSnap * int(pFr->aabbCasters.max.z / fSnap);

		pFr->fDepthSlopeBias = AO_FRUSTUM_SLOPE_BIAS;
		pFr->fDepthConstBias = 0;

		pFr->mLightViewMatrix.SetIdentity();
		pFr->mLightViewMatrix.m30 = -pFr->aabbCasters.GetCenter().x;
		pFr->mLightViewMatrix.m31 = -pFr->aabbCasters.GetCenter().y;
		pFr->mLightViewMatrix.m32 = -pFr->aabbCasters.GetCenter().z - m_pLightEntity->GetLightProperties().m_Origin.GetLength();

		mathMatrixOrtho(&pFr->mLightProjMatrix, projectionBoundsLS.GetSize().x, projectionBoundsLS.GetSize().y, -projectionBoundsLS.max.z, -projectionBoundsLS.min.z);
	}

	const Vec3 lightPos = pFr->aabbCasters.GetCenter() + Vec3(0, 0, 1) * m_pLightEntity->GetLightProperties().m_Origin.GetLength();

	// finally init frustum
	const int nTexRes = (int)clamp_tpl(pHeightMapAORes->GetFVal(), 0.f, 16384.f);
	pFr->m_eFrustumType = ShadowMapFrustum::e_HeightMapAO;
	InitCachedFrustum(pFr, nUpdateStrategy, nLod, nLod - nFirstStaticLod, nTexRes, lightPos, projectionBoundsLS, passInfo);
}

AABB ShadowCacheGenerator::GetLPVBox(const SRenderingPassInfo& passInfo, int nCascade)
{
	static ICVar* pLPVSize = gEnv->pConsole->GetCVar("r_LPVSize");
	static ICVar* pLPVGridSize = gEnv->pConsole->GetCVar("r_LPVGridSize");
	static ICVar* pLPVCascadeScale = gEnv->pConsole->GetCVar("r_LPVCascadeScale");

	// Every cascade owns its own RSM fitted to its own volume, like the cascaded shadow maps: the
	// texel-to-cell ratio and the caster sweep stay scale-invariant per cascade.
	// r_LPVCascadeScale is the PER STEP ratio between adjacent cascades - has to stay in sync
	// with CLPVStage::UpdateCascadeParameters().
	float extent = max(1.0f, pLPVSize ? pLPVSize->GetFVal() : 96.0f);
	const float scaleStep = max(1.0f, pLPVCascadeScale ? pLPVCascadeScale->GetFVal() : 3.0f);
	for (int i = 0; i < nCascade; ++i)
		extent *= scaleStep;

	// Same clamping as CLPVStage::GetValidGridSize().
	int32 gridSize = pLPVGridSize ? pLPVGridSize->GetIVal() : 48;
	gridSize = clamp_tpl(gridSize, 8, 64);
	gridSize = gridSize - (gridSize % 4);

	const float cellSize = extent / (float)gridSize;
	const float halfExtent = extent * 0.5f;

	// CLPVStage snaps the grid origin to whole cells, so the box centre is the snapped camera position.
	const Vec3 camPos = passInfo.GetCamera().GetPosition();
	const Vec3 centre(floorf(camPos.x / cellSize) * cellSize,
	                  floorf(camPos.y / cellSize) * cellSize,
	                  floorf(camPos.z / cellSize) * cellSize);

	return AABB(centre - Vec3(halfExtent), centre + Vec3(halfExtent));
}

bool ShadowCacheGenerator::IsStableRsmCaster(EERType type)
{
	switch (type)
	{
	// Merged meshes drop their dynamic render mesh on every frame they were not drawn in the general
	// pass (CMergedMeshesManager::Update) and choose static/dynamic render mode plus lod from the main
	// camera in CMergedMeshRenderNode::Render, so their shadow output follows the main view.
	case eERType_MergedMesh:
	case eERType_MergedMeshInstance:
	// Characters (CGA/CHR) and geom caches are skinned and streamed on main view visibility.
	case eERType_Character:
	case eERType_GeomCache:
	// Not meaningful bounce sources, and inherently unstable frame to frame.
	case eERType_ParticleEmitter:
	case eERType_WaterVolume:
	case eERType_WaterWave:
	// Coplanar overlays on top of terrain/brush geometry. In the RSM they z-fight with the surface
	// underneath and the draw order decides the albedo/depth winner per texel - the order is not
	// deterministic across RSM renders, so the injected flux of whole patches (e.g. pavement slabs)
	// jumps between two states from one relight to the next. Excluding them makes the underlying
	// geometry win deterministically at the cost of losing the overlay tint in the bounce.
	case eERType_Road:
	case eERType_Decal:
		return false;

	default:
		return true;
	}
}

void ShadowCacheGenerator::InitLPVRsmFrustum(ShadowMapFrustumPtr& pFr, int nLod, int nCacheLod, int nCascade, const SRenderingPassInfo& passInfo)
{
	FUNCTION_PROFILER_3DENGINE;

	if (!pFr)
		pFr = new ShadowMapFrustum;

	if (!pFr->pShadowCacheData)
		pFr->pShadowCacheData = std::make_shared<ShadowMapFrustum::ShadowCacheData>();

	static ICVar* pLPVRsmSamples = gEnv->pConsole->GetCVar("r_LPVRSMSamples");

	const SRenderLight& light = m_pLightEntity->GetLightProperties();
	const AABB lpvBox = GetLPVBox(passInfo, nCascade);
	const Vec3 boxCentre = lpvBox.GetCenter();
	const float halfExtent = lpvBox.GetSize().x * 0.5f;

	// Resolution LOD: every further cascade halves the RSM resolution (1024 / 512 / 256 with the
	// default) - the volume grows by r_LPVCascadeScale per step, the cells grow with it, so a
	// lower texel density still leaves several surfels per cell face while the injection dispatch
	// and the caster render shrink quadratically.
	int nTexRes = pLPVRsmSamples ? clamp_tpl(pLPVRsmSamples->GetIVal(), 64, 1024) : 1024;
	nTexRes = max(256, nTexRes >> nCascade);

	// Light space basis, has to match mathMatrixLookAt() so the snapping below happens in the very
	// same space the projection is built in later on (CShadowUtils::GetShadowMatrixOrtho).
	Vec3 vLightSrcRelPos = light.m_Origin - passInfo.GetCamera().GetPosition();
	const Vec3 zAxis = vLightSrcRelPos.GetNormalized();
	const Vec3 up = (fabsf(zAxis.Dot(Vec3(0.0f, 0.0f, 1.0f))) > 0.9995f) ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
	const Vec3 xAxis = up.Cross(zAxis).GetNormalized();
	const Vec3 yAxis = zAxis.Cross(xAxis);

	// Footprint of the volume as seen from the sun: project the 8 corners onto the light space axes.
	float halfWidth = 0.0f;
	float halfDepth = 0.0f;
	for (int i = 0; i < 8; ++i)
	{
		const Vec3 corner(((i & 1) ? lpvBox.max.x : lpvBox.min.x) - boxCentre.x,
		                  ((i & 2) ? lpvBox.max.y : lpvBox.min.y) - boxCentre.y,
		                  ((i & 4) ? lpvBox.max.z : lpvBox.min.z) - boxCentre.z);

		halfWidth = max(halfWidth, max(fabsf(corner.Dot(xAxis)), fabsf(corner.Dot(yAxis))));
		halfDepth = max(halfDepth, fabsf(corner.Dot(zAxis)));
	}

	// Quantize the footprint so that the world size of an RSM texel only changes in coarse steps.
	// Without this the texel grid the projection is snapped to would drift with the sun direction.
	const float quantum = max(0.01f, halfExtent * 0.125f);
	halfWidth = ceilf(halfWidth / quantum) * quantum;

	const float texelSize = (2.0f * halfWidth) / (float)nTexRes;

	// Snap the projection centre to whole texels in light space, plus one texel of slack so the volume
	// stays fully covered after the snap.
	Vec3 vProjTranslation = boxCentre;
	vProjTranslation -= xAxis * (boxCentre.Dot(xAxis) - floorf(boxCentre.Dot(xAxis) / texelSize) * texelSize);
	vProjTranslation -= yAxis * (boxCentre.Dot(yAxis) - floorf(boxCentre.Dot(yAxis) / texelSize) * texelSize);
	halfWidth += texelSize;

	// Everything between the sun and the volume can throw light into it, so extend the near plane.
	const float casterRange = max(Get3DEngine()->m_fSunClipPlaneRange, 2.0f * halfExtent);

	// Sun-ward occlusion fidelity trade-off (r_LPVRsmClipRange):
	// - 0 (default): the near plane sits at the collection sweep distance. Geometry overhanging the
	//   sweep towards the sun is CLIPPED AWAY, so distant tree lines / hills do not occlude the
	//   volume in the RSM. At grazing sun elevations this is what keeps the volume lit at all (a
	//   20m tree line shadows ~150m of ground at 7.6 degrees), at the cost of some wrongly sunlit
	//   injection in areas that are genuinely shadowed by external geometry - and of erasure blobs
	//   when huge member nodes (hills, merged tree patches) get their overhang pancaked.
	// - > 0: the clip range extends to this many meters, members render fully and occlude honestly.
	static ICVar* pClipRange = gEnv->pConsole->GetCVar("r_LPVRsmClipRange");
	const float clipRange = max(casterRange, pClipRange ? pClipRange->GetFVal() : 0.0f);

	// Virtual light distance instead of the astronomic distance to the real sun light origin: with
	// fDist ~1e6 the resulting projection (near ~= far ~= 1e6, FOV ~0.01 deg) is so ill conditioned
	// that the fp32 inversion in CLPVStage::UpdateFrameParameters degenerates and the VPLs land at
	// garbage positions. 10km keeps the direction parallax below ~0.5 degrees over the volume while
	// the matrix stays well conditioned. Only the direction of vLightSrcRelPos is kept.
	const float fDist = max(10000.0f, 40.0f * halfWidth);
	vLightSrcRelPos = zAxis * fDist;

	pFr->m_eFrustumType = ShadowMapFrustum::e_LPVRsm;
	pFr->RequestUpdates(1);
	pFr->RequestSamples(1);
	pFr->bIncrementalUpdate = false;
	pFr->bIsMGPUCopy = false;
	pFr->bUseShadowsPool = false;
	pFr->bOmniDirectionalShadow = false;
	pFr->bBlendFrustum = false;
	pFr->fBlendVal = 1.0f;
	pFr->fShadowFadingDist = 0.0f;
	pFr->nShadowPoolUpdateRate = 0;
	pFr->pShadowCacheData->Reset(GetNextGenerationID());

	CRY_ASSERT(light.m_pOwner);
	pFr->pLightOwner = light.m_pOwner;
	pFr->m_Flags = light.m_Flags;
	pFr->nUpdateFrameId = passInfo.GetFrameID();
	pFr->nShadowMapLod = nLod;
	pFr->nShadowCacheLod = nCacheLod;
	pFr->nLpvCascadeIndex = (uint8)nCascade;
	pFr->nTexSize = nTexRes;
	pFr->fRadius = light.m_fRadius;

	pFr->vProjTranslation = vProjTranslation;
	pFr->vLightSrcRelPos = vLightSrcRelPos;

	// The renderer builds the (very narrow) perspective projection from these, exactly like for a sun
	// cascade, so the RSM depth encoding stays identical to the cascade based path.
	pFr->fFOV = (float)RAD2DEG(atan_tpl(halfWidth / max(fDist, 1.0f))) * 2.0f;
	pFr->fProjRatio = 1.0f;
	pFr->fNearDist = fDist - halfDepth - clipRange;
	pFr->fFarDist = fDist + halfDepth;
	pFr->fRendNear = pFr->fNearDist;

	if (pFr->fFarDist > light.m_fRadius)
		pFr->fFarDist = light.m_fRadius;
	if (pFr->fNearDist < pFr->fFarDist * 0.005f)
		pFr->fNearDist = pFr->fFarDist * 0.005f;

	pFr->fFrustrumSize = 1.0f / max(0.001f, 2.0f * halfWidth);
	pFr->fWidthS = pFr->fWidthT = 1.0f;
	pFr->fBlurS = pFr->fBlurT = 0.0f;

	// No depth bias at all: the RSM is never used for a shadow comparison, its depth is inverted back
	// into world space to place the virtual point lights, and any bias would displace them.
	pFr->fDepthConstBias = 0.0f;
	pFr->fDepthSlopeBias = 0.0f;
	pFr->fDepthTestBias = 0.0f;
	pFr->fDepthBiasClamp = 0.0f;

	// Caster box: the volume swept towards the sun, so occluders above it are collected as well.
	// The sweep has to reach as far as the clip range renders: with a clip range beyond the caster
	// sweep, occluders between the two distances would be inside the rendered depth range but never
	// collected as members. The small near cascade then misses the distant hills / tree lines that
	// the big far cascade naturally contains, the near volume over-injects in genuinely shadowed
	// terrain and the cascade boundary shows up as a hard brightness step.
	pFr->aabbCasters = lpvBox;
	pFr->aabbCasters.Add(AABB(lpvBox.min + zAxis * clipRange, lpvBox.max + zAxis * clipRange));

	// Culling camera, only used by the generic frustum code paths (the caster collection below is what
	// actually feeds this frustum).
	CCamera& frustumCam = pFr->FrustumPlanes[0] = CCamera();
	Matrix34A mat = Matrix33::CreateRotationVDir(-zAxis);
	mat.SetTranslation(pFr->vLightSrcRelPos + pFr->vProjTranslation);
	frustumCam.SetMatrixNoUpdate(mat);
	frustumCam.SetFrustum(256, 256, max(0.0001f, pFr->fFOV * (gf_PI / 180.0f)), pFr->fNearDist, pFr->fFarDist);
	pFr->FrustumPlanes[1] = frustumCam;

	// Independent octree query over aabbCasters. This is the whole point of the dedicated view: the
	// caster set does not depend on the main camera orientation at all.
	CollectCastersForCachedFrustum(pFr, nullptr, std::numeric_limits<int>::max(), passInfo);
}

void ShadowCacheGenerator::GetCasterBox(AABB& BBoxWS, AABB& BBoxLS, float fRadius, const Matrix34& matView, const SRenderingPassInfo& passInfo)
{
	AABB projectionBoundsLS;

	BBoxWS = AABB(passInfo.GetCamera().GetPosition(), fRadius);
	BBoxLS = AABB(matView.TransformPoint(passInfo.GetCamera().GetPosition()), fRadius);

	// try to get tighter near/far plane from casters
	AABB casterBoxLS = Get3DEngine()->m_pObjectsTree->GetShadowCastersBox(&BBoxWS, &matView);

	if (CVisAreaManager* pVisAreaManager = GetVisAreaManager())
	{
		for (int i = 0; i < pVisAreaManager->m_lstVisAreas.Count(); ++i)
		{
			if (pVisAreaManager->m_lstVisAreas[i] && pVisAreaManager->m_lstVisAreas[i]->IsObjectsTreeValid())
			{
				casterBoxLS.Add(pVisAreaManager->m_lstVisAreas[i]->GetObjectsTree()->GetShadowCastersBox(&BBoxWS, &matView));
			}
		}

		for (int i = 0; i < pVisAreaManager->m_lstPortals.Count(); ++i)
		{
			if (pVisAreaManager->m_lstPortals[i] && pVisAreaManager->m_lstPortals[i]->IsObjectsTreeValid())
			{
				casterBoxLS.Add(pVisAreaManager->m_lstPortals[i]->GetObjectsTree()->GetShadowCastersBox(&BBoxWS, &matView));
			}
		}
	}

	if (!casterBoxLS.IsReset() && casterBoxLS.GetSize().z < 2 * fRadius)
	{
		float fDepthRange = 2.0f * max(Get3DEngine()->m_fSunClipPlaneRange, casterBoxLS.GetSize().z);
		BBoxLS.max.z = casterBoxLS.max.z + 0.5f; // slight offset here to counter edge case where polygons are projection plane aligned and would come to lie directly on the near plane
		BBoxLS.min.z = casterBoxLS.max.z - fDepthRange;
	}
}

Matrix44 ShadowCacheGenerator::GetViewMatrix(const SRenderingPassInfo& passInfo)
{
	const Vec3 zAxis(0.f, 0.f, 1.f);
	const Vec3 yAxis(0.f, 1.f, 0.f);

	Vec3 At = passInfo.GetCamera().GetPosition();
	Vec3 Eye = m_pLightEntity->GetLightProperties().m_Origin;
	Vec3 Up = fabsf((Eye - At).GetNormalized().Dot(zAxis)) > 0.9995f ? yAxis : zAxis;

	Matrix44 result;
	mathMatrixLookAt(&result, Eye, At, Up);

	return result;
}

void ShadowCacheGenerator::AddTerrainCastersToFrustum(ShadowMapFrustum* pFr, const SRenderingPassInfo& passInfo)
{
	FUNCTION_PROFILER_3DENGINE;

	// The LPV RSM always wants the terrain: it is the main source of indirect bounce outdoors.
	if ((Get3DEngine()->m_bSunShadowsFromTerrain
	     || pFr->m_eFrustumType == ShadowMapFrustum::e_HeightMapAO
	     || pFr->m_eFrustumType == ShadowMapFrustum::e_LPVRsm) && !pFr->bIsMGPUCopy)
	{
		PodArray<CTerrainNode*> lstTerrainNodes;
		GetTerrain()->IntersectWithBox(pFr->aabbCasters, &lstTerrainNodes);

		for (int s = 0; s < lstTerrainNodes.Count(); s++)
		{
			CTerrainNode* pNode = lstTerrainNodes[s];

			const float optimalTerrainSegmentSize = 128.f;
			if (pNode->GetBBox().GetSize().x != optimalTerrainSegmentSize)
				continue;

			if (!pFr->NodeRequiresShadowCacheUpdate(pNode))
				continue;

			// The LPV RSM marks with its fixed bit, see kLPVRsmTraversalLod.
			const int markLod = (pFr->m_eFrustumType == ShadowMapFrustum::e_LPVRsm)
			                    ? kLPVRsmTraversalLod
			                    : pFr->nShadowMapLod;
			pNode->SetTraversalFrameId(passInfo.GetMainFrameID(), markLod);
		}
	}
}
