// Copyright 2014-2021 Crytek GmbH / Crytek Group. All rights reserved.

/*=============================================================================

   Revision history:
* Created by Vladimir Kajalin

   =============================================================================*/

#include "StdAfx.h"

#if defined(FEATURE_SVO_GI)

	#include <Cry3DEngine/I3DEngine.h>
	#include "D3DPostProcess.h"
	#include "D3D_SVO.h"
	#include "Common/RenderView.h"
	#include "GraphicsPipeline/TiledLightVolumes.h"
	#include "GraphicsPipeline/ShadowMap.h"
	// rt stage 2 (decision 06 section 6.3): the hit-shading pass fills a forward-shaped
	// CBPerPassForward itself instead of borrowing CSceneForwardStage's, because the order of
	// the two stages inside a frame is not guaranteed (research/06 section 9.9).
	#include "GraphicsPipeline/ClipVolumes.h"
	#include "GraphicsPipeline/Fog.h"
	#include "GraphicsPipeline/VolumetricFog.h"
	#include "GraphicsPipeline/SceneForward.h"
	// rt stage 4A (decision 09 section 9.1): a ray that misses is shaded with the engine's own
	// sky, which means binding the sky stage's two Nishita dome textures.
	#include "GraphicsPipeline/Sky.h"
	// rt stage 5F (decision 09 section 9.2): the secondary cloud march needs the clouds stage's
	// noise volumes, its Mie phase LUT and this frame's cloud shadow volume.
	#include "GraphicsPipeline/VolumetricClouds.h"
	#include "Common/ShadowUtils.h"

// Must match cbuffer CBPerPassForward (b5) in Engine/Shaders/HWScripts/CryFX/ForwardShading.cfi,
// field for field, and it is the same layout SceneForward.cpp:17 SPerPassConstantBuffer uses.
struct SSvoShadeForwardConstantBuffer
{
	CFogStage::SForwardParams                 cbFog;
	CVolumetricFogStage::SForwardParams       cbVoxelFog;
	CShadowUtils::SShadowCascadesSamplingInfo cbShadowSampling;
	CSceneForwardStage::SCloudShadingParams   cbClouds;
	CSvoRenderer::SForwardParams              cbSVOGI;
};

_smart_ptr<CTexture> CSvoRenderer::s_pRsmColorMap;
_smart_ptr<CTexture> CSvoRenderer::s_pRsmNormlMap;
_smart_ptr<CTexture> CSvoRenderer::s_pRsmPoolCol;
_smart_ptr<CTexture> CSvoRenderer::s_pRsmPoolNor;

CSvoRenderer* CSvoRenderer::s_pInstance = 0;

SSvoPrimitivePasses::SSvoPrimitivePasses(CGraphicsPipeline* pGraphicsPipeline)
	: m_tsDiff(pGraphicsPipeline)
	, m_tsSpec(pGraphicsPipeline)
	, m_passClearBricks(pGraphicsPipeline)
	, m_passInjectDynamicLights(pGraphicsPipeline)
	, m_passInjectStaticLights(pGraphicsPipeline)
	, m_passInjectAirOpacity(pGraphicsPipeline)
	, m_passPropagateLighting_1to2(pGraphicsPipeline)
	, m_passPropagateLighting_2to3(pGraphicsPipeline)
	, m_passTroposphere(pGraphicsPipeline)
	, m_passBuildRTLightList(pGraphicsPipeline)
	, currentKey(pGraphicsPipeline->GetKey())
{
}

CSvoRenderer::CSvoRenderer()
{
	//	InitCVarValues();

	ZeroStruct(m_texInfo.arrAnalyticalOccluders);

	m_pNoiseTex = CTexture::ForNamePtr("%ENGINE%/EngineAssets/Textures/noise.dds", FT_DONT_STREAM, eTF_Unknown);

	m_pShader = CShaderMan::s_ShaderSVOGI;

	ZeroStruct(m_mGpuVoxViewProj);
	ZeroStruct(m_wsOffset);
	ZeroStruct(m_tcOffset);
	ZeroStruct(m_arrNodesForUpdate);
	ZeroStruct(m_nCurPropagationPassID);
}

CSvoRenderer* CSvoRenderer::GetInstance(bool bCheckAlloce)
{
	if (!s_pInstance && bCheckAlloce)
		s_pInstance = new CSvoRenderer();

	return s_pInstance;
}

void CSvoRenderer::Release()
{
	SAFE_DELETE(s_pInstance);

	s_pRsmColorMap.reset();
	s_pRsmNormlMap.reset();
	s_pRsmPoolCol.reset();
	s_pRsmPoolNor.reset();
}

void CSvoRenderer::SetEditingHelper(const Sphere& sp)
{
	m_texInfo.helperInfo = sp;
}

void CSvoRenderer::UpdateCompute(CRenderView* pRenderView)
{
	FUNCTION_PROFILER_RENDERER();

	InitCVarValues();

	if (!e_svoEnabled)
		return;

	if (!e_svoRender)
		return;

	static int nTI_Compute_FrameId = -1;
	if (nTI_Compute_FrameId == gRenDev->GetRenderFrameID())
		return;
	nTI_Compute_FrameId = gRenDev->GetRenderFrameID();

	if (!gEnv->p3DEngine->GetSvoStaticTextures(m_texInfo, &m_arrLightsStatic, &m_arrLightsDynamic))
		return;

	m_pRenderView = pRenderView;

	m_arrNodesForUpdateIncr.Clear();
	m_arrNodesForUpdateNear.Clear();

	#ifdef FEATURE_SVO_GI_ALLOW_HQ

	if (GetIntegratioMode())
	{
		gEnv->p3DEngine->GetSvoBricksForUpdate(m_arrNodeInfo, (float)0, &m_arrVerts);

		for (int n = 0; n < m_arrNodeInfo.Count(); n++)
		{
			m_arrNodesForUpdateIncr.Add(m_arrNodeInfo[n]);
		}

		m_arrNodeInfo.Clear();

		if (e_svoTI_DynLights)
		{
			gEnv->p3DEngine->GetSvoBricksForUpdate(m_arrNodeInfo, e_svoMinNodeSize, 0);

			for (int n = 0; n < m_arrNodeInfo.Count(); n++)
			{
				m_arrNodesForUpdateNear.Add(m_arrNodeInfo[n]);
			}
		}
	}

	{
		// get UAV access
		vp_RGB0.Init(m_texInfo.pTexRgb0);
		vp_RGB1.Init(m_texInfo.pTexRgb1);
		vp_DYNL.Init(m_texInfo.pTexDynl);
		vp_RGB2.Init(m_texInfo.pTexRgb2);
		vp_RGB3.Init(m_texInfo.pTexRgb3);
		vp_RGB4.Init(m_texInfo.pTexRgb4);
		vp_NORM.Init(m_texInfo.pTexNorm);
		vp_ALDI.Init(m_texInfo.pTexAldi);
		vp_OPAC.Init(m_texInfo.pTexOpac);
	}

	if ((!(e_svoTI_Active && e_svoTI_Apply) && !e_svoDVR) || !m_texInfo.bSvoReady || !GetIntegratioMode())
		return;

	// force sync shaders compiling
	int nPrevAsync = CRenderer::CV_r_shadersasynccompiling;
	CRenderer::CV_r_shadersasynccompiling = 0;

	// clear pass
	{
		PROFILE_LABEL_SCOPE("TI_INJECT_CLEAR");

		for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count();)
			ExecuteComputeShader("ComputeClearBricks", m_pPasses->m_passClearBricks, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
	}

	// voxelize dynamic meshes
	{
		//		PROFILE_LABEL_SCOPE( "TI_VOXELIZE" );

		//gcpRendD3D->SVO_VoxelizeMeshes(0, 0);
	}

	if (e_svoTI_Troposphere_Active && (e_svoTI_Troposphere_CloudGen_Freq || e_svoTI_Troposphere_Layer0_Dens || e_svoTI_Troposphere_Layer1_Dens))
	{
		PROFILE_LABEL_SCOPE("TI_INJECT_AIR");

		for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count();)
			ExecuteComputeShader("ComputeInjectAtmosphere", m_pPasses->m_passInjectAirOpacity, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
	}

	{
		PROFILE_LABEL_SCOPE("TI_INJECT_LIGHT");

		for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count();)
			ExecuteComputeShader("ComputeDirectStaticLighting", m_pPasses->m_passInjectStaticLights, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
	}

	if (e_svoTI_PropagationBooster || e_svoTI_InjectionMultiplier)
	{
		if (e_svoTI_NumberOfBounces > 1)
		{
			PROFILE_LABEL_SCOPE("TI_INJECT_REFL0");

			m_nCurPropagationPassID = 0;
			for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count();)
				ExecuteComputeShader("ComputePropagateLighting", m_pPasses->m_passPropagateLighting_1to2, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
		}

		if (e_svoTI_NumberOfBounces > 2)
		{
			PROFILE_LABEL_SCOPE("TI_INJECT_REFL1");

			m_nCurPropagationPassID++;
			for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count();)
				ExecuteComputeShader("ComputePropagateLighting", m_pPasses->m_passPropagateLighting_2to3, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
		}
	}

	static int nLightsDynamicCountPrevFrame = 0;

	if ((e_svoTI_DynLights && (m_arrLightsDynamic.Count() || nLightsDynamicCountPrevFrame)) || e_svoTI_SunRSMInject)
	{
		PROFILE_LABEL_SCOPE("TI_INJECT_DYNL");

		// TODO: cull not affected nodes

		for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateNear.Count();)
			ExecuteComputeShader("ComputeDirectDynamicLighting", m_pPasses->m_passInjectDynamicLights, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateNear);
	}

	nLightsDynamicCountPrevFrame = m_arrLightsDynamic.Count();

	CRenderer::CV_r_shadersasynccompiling = nPrevAsync;

	#endif

	m_pRenderView = nullptr;
}

bool CSvoRenderer::VoxelizeMeshes(CShader* ef, SShaderPass* sfm)
{
	return true;
}

void CSvoRenderer::VoxelizeRE()
{

}

void CSvoRenderer::UpdateGpuVoxParams(I3DEngine::SSvoNodeInfo& nodeInfo)
{
	float fBoxSize = nodeInfo.wsBox.GetSize().x;
	Vec3 vOrigin = Vec3(0, 0, 0);
	m_wsOffset = Vec4(nodeInfo.wsBox.GetCenter(), nodeInfo.wsBox.GetSize().x);
	m_tcOffset = Vec4(nodeInfo.tcBox.min, (float)gRenDev->GetMainFrameID());

	Matrix44A m_mOrthoProjection;
	mathMatrixOrtho(&m_mOrthoProjection, fBoxSize, fBoxSize, 0.0f, fBoxSize);

	// Direction Vectors moved to shader constants
	// as we always use global X,-Y,Z vectors

	// Matrices
	Matrix44A VoxelizationView[3];
	Vec3 EdgeCenter = vOrigin - (Vec3(1.0f, 0.0f, 0.0f) * (fBoxSize / 2));
	mathMatrixLookAt(&(VoxelizationView[0]), EdgeCenter, vOrigin, Vec3(0.0f, 0.0f, 1.0f));

	EdgeCenter = vOrigin - (Vec3(0.0f, -1.0f, 0.0f) * (fBoxSize / 2));
	mathMatrixLookAt(&(VoxelizationView[1]), EdgeCenter, vOrigin, Vec3(0.0f, 0.0f, 1.0f));

	EdgeCenter = vOrigin - (Vec3(0.0f, 0.0f, 1.0f) * (fBoxSize / 2));
	mathMatrixLookAt(&(VoxelizationView[2]), EdgeCenter, vOrigin, Vec3(0.0f, -1.0f, 0.0f));

	m_mGpuVoxViewProj[0] = VoxelizationView[0] * m_mOrthoProjection;
	m_mGpuVoxViewProj[1] = VoxelizationView[1] * m_mOrthoProjection;
	m_mGpuVoxViewProj[2] = VoxelizationView[2] * m_mOrthoProjection;
}

void CSvoRenderer::ExecuteComputeShader(const char* szTechFinalName, CSvoComputePass& rp, int* pnNodesForUpdateStartIndex, int nObjPassId, PodArray<I3DEngine::SSvoNodeInfo>& arrNodesForUpdate)
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ

	FUNCTION_PROFILER_RENDERER();

	rp.SetTechnique(m_pShader, szTechFinalName, GetRunTimeFlags(true, false));

	// setup in/out textures

	if (&rp == &m_pPasses->m_passInjectAirOpacity)
	{
		// update OPAC
		rp.SetOutputUAV(2, vp_RGB0.pTex);
		if (vp_OPAC.pUAV)
			rp.SetOutputUAV(7, vp_OPAC.pTex);

		rp.SetTexture(15, m_pNoiseTex);
		SetupSvoTexturesForRead(m_texInfo, rp, 0, 1);
		SetupCommonSamplers(rp);

		rp.BeginConstantUpdate();

		SetupCommonConstants(NULL, rp, NULL);
		SetupLightSources(m_arrLightsStatic, rp);
		SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate, rp);
	}
	else if (&rp == &m_pPasses->m_passInjectStaticLights)
	{
		// update RGB1
		rp.SetOutputUAV(2, vp_RGB0.pTex);
		rp.SetOutputUAV(7, vp_RGB1.pTex);

		if (vp_DYNL.pUAV)
			rp.SetOutputUAV(6, vp_DYNL.pTex);

		SetupSvoTexturesForRead(m_texInfo, rp, 0);
		SetupRsmSunTextures(rp);
		SetupCommonSamplers(rp);

		rp.BeginConstantUpdate();

		SetupCommonConstants(NULL, rp, NULL);
		SetupRsmSunConstants(rp);
		SetupLightSources(m_arrLightsStatic, rp);
		SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate, rp);
	}
	else if (&rp == &m_pPasses->m_passInjectDynamicLights)
	{
		BindTiledLights(m_arrLightsDynamic, (CComputeRenderPass&)rp);

		// update RGB
		rp.SetOutputUAV(2, vp_RGB0.pTex);

		if (e_svoTI_NumberOfBounces == 1)
			rp.SetOutputUAV(7, vp_RGB1.pTex);
		if (e_svoTI_NumberOfBounces == 2)
			rp.SetOutputUAV(7, vp_RGB2.pTex);
		if (e_svoTI_NumberOfBounces == 3)
			rp.SetOutputUAV(7, vp_RGB3.pTex);

		rp.SetOutputUAV(5, vp_DYNL.pTex);
		rp.SetTexture(15, m_pNoiseTex);

		SetupSvoTexturesForRead(m_texInfo, rp, 0);
		SetupRsmSunTextures(rp);
		SetupCommonSamplers(rp);

		rp.BeginConstantUpdate();

		SetupRsmSunConstants(rp);
		SetupCommonConstants(NULL, rp, NULL);
		SetupLightSources(m_arrLightsDynamic, rp);
		SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate, rp);
	}
	else if (&rp == &m_pPasses->m_passPropagateLighting_1to2)
	{
		// update RGB2
		if (vp_RGB0.pUAV)
			rp.SetOutputUAV(0, vp_RGB0.pTex);
		SetupSvoTexturesForRead(m_texInfo, rp, 1); // input
		if (vp_RGB2.pUAV)
			rp.SetOutputUAV(5, vp_RGB2.pTex);
		if (vp_ALDI.pUAV)
			rp.SetOutputUAV(6, vp_ALDI.pTex);
		if (vp_DYNL.pUAV)
			rp.SetOutputUAV(7, vp_DYNL.pTex);

		SetupCommonSamplers(rp);

		rp.BeginConstantUpdate();

		SetupCommonConstants(NULL, rp, NULL);
		SetupLightSources(m_arrLightsStatic, rp);
		SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate, rp);
	}
	else if (&rp == &m_pPasses->m_passPropagateLighting_2to3)
	{
		// update RGB3
		if (vp_RGB0.pUAV)
			rp.SetOutputUAV(0, vp_RGB0.pTex);
		if (vp_RGB1.pUAV)
			rp.SetOutputUAV(1, vp_RGB1.pTex);
		SetupSvoTexturesForRead(m_texInfo, rp, 2); // input
		if (vp_RGB3.pUAV)
			rp.SetOutputUAV(5, vp_RGB3.pTex);
		if (vp_DYNL.pUAV)
			rp.SetOutputUAV(7, vp_DYNL.pTex);

		SetupCommonSamplers(rp);

		rp.BeginConstantUpdate();

		SetupCommonConstants(NULL, rp, NULL);
		SetupLightSources(m_arrLightsStatic, rp);
		SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate, rp);
	}
	else if (&rp == &m_pPasses->m_passClearBricks)
	{
		if (vp_RGB4.pUAV)
			rp.SetOutputUAV(3, vp_RGB4.pTex);
		if (vp_OPAC.pUAV)
			rp.SetOutputUAV(4, vp_OPAC.pTex);
		if (vp_RGB3.pUAV)
			rp.SetOutputUAV(6, vp_RGB3.pTex);
		if (vp_RGB1.pUAV)
			rp.SetOutputUAV(7, vp_RGB1.pTex);
		if (vp_RGB2.pUAV)
			rp.SetOutputUAV(5, vp_RGB2.pTex);
		if (vp_ALDI.pUAV)
			rp.SetOutputUAV(0, vp_ALDI.pTex);

		SetupCommonSamplers(rp);

		rp.BeginConstantUpdate();

		SetupCommonConstants(NULL, rp, NULL);
		SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate, rp);
	}

	{
		rp.SetDispatchSize(e_svoDispatchX, e_svoDispatchY, 1);

		rp.PrepareResourcesForUse(GetDeviceObjectFactory().GetCoreCommandList());

		SScopedComputeCommandList computeCommandList(e_svoTI_AsyncCompute != 0);
		rp.Execute(computeCommandList, EShaderStage_All);
	}

	#endif
}

CTexture* CSvoRenderer::GetGBuffer(const CGraphicsPipelineResources& pipelineResources, int nId) // simplify branch compatibility
{
	CTexture* pRes;

	if (nId == 0)
		pRes = pipelineResources.m_pTexSceneNormalsMap;
	else if (nId == 1)
		pRes = pipelineResources.m_pTexSceneDiffuse;
	else if (nId == 2)
		pRes = pipelineResources.m_pTexSceneSpecular;
	else
		pRes = 0;

	return pRes;
}

CTexture* CSvoRenderer::GetZBuffer(const CGraphicsPipelineResources& pipelineResources, bool bLinear)
{
	return pipelineResources.m_pTexLinearDepth;
}

void CSvoRenderer::TropospherePass()
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ

	CSvoFullscreenPass& rp = m_pPasses->m_passTroposphere;

	if (m_texInfo.bSvoFreeze || !m_texInfo.pTexTree)
		return;

	const char* szTechFinalName = "RenderAtmosphere";

	rp.SetTechnique(m_pShader, szTechFinalName, GetRunTimeFlags(0));
	rp.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
	rp.SetState(GS_NODEPTHTEST);

	rp.SetRenderTarget(0, m_pRT_AIR_MIN);
	rp.SetRenderTarget(1, m_pRT_SHAD_MIN_MAX);
	rp.SetRenderTarget(2, m_pRT_AIR_MAX);
	rp.SetRequireWorldPos(true);
	rp.SetRequirePerViewConstantBuffer(true);

	SetupCommonSamplers(rp);
	SetupSvoTexturesForRead(m_texInfo, rp, e_svoTI_NumberOfBounces, 0, 0);
	SetupGBufferTextures(rp);

	rp.BeginConstantUpdate();

	SetupCommonConstants(NULL, rp, rp.GetRenderTarget(0));
	SetupLightSources(m_arrLightsStatic, rp);

	rp.Execute();

	#endif
}

void CSvoRenderer::TraceSunShadowsPass()
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ

	CSvoFullscreenPass& rp = m_pPasses->m_passTroposphere;

	if (m_texInfo.bSvoFreeze || !m_texInfo.pTexTree)
		return;

	const char* szTechFinalName = "TraceSunShadows";

	rp.SetTechnique(m_pShader, szTechFinalName, GetRunTimeFlags(0));
	rp.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
	rp.SetState(GS_NODEPTHTEST);

	rp.SetRenderTarget(0, m_pRT_SHAD_MIN_MAX);

	rp.SetRequireWorldPos(true);
	rp.SetRequirePerViewConstantBuffer(true);

	SetupRsmSunTextures(rp);
	SetupCommonSamplers(rp);
	SetupSvoTexturesForRead(m_texInfo, rp, e_svoTI_NumberOfBounces, 0, 0);
	SetupGBufferTextures(rp);

	int nTex0, nTex1, nTex2;
	ITerrain* pTerrain = gEnv->p3DEngine->GetITerrain();
	if (pTerrain)
		pTerrain->GetAtlasTexId(nTex0, nTex1, nTex2);
	CTexture* pHM = CTexture::GetByID(nTex2);
	rp.SetTexture(8, pHM);

	rp.BeginConstantUpdate();

	SetupCommonConstants(NULL, rp, rp.GetRenderTarget(0));
	SetupLightSources(m_arrLightsStatic, rp);

	rp.Execute();

	#endif
}

void CSvoRenderer::SetupGBufferTextures(CSvoFullscreenPass& rp)
{
	const CGraphicsPipelineResources& pipelineResources = RenderView()->GetGraphicsPipeline()->GetPipelineResources();

	rp.SetTexture(4, GetZBuffer(pipelineResources, true));
	rp.SetTexture(14, GetGBuffer(pipelineResources, 0));
	rp.SetTexture(5, GetGBuffer(pipelineResources, 1));
	rp.SetTexture(7, GetGBuffer(pipelineResources, 2));
}

void CSvoRenderer::ConeTracePass(SSvoTargetsSet* pTS)
{
	CSvoFullscreenPass& rp = pTS->passConeTrace;

	// rt stage 2: this frame's g-data is not shaded yet; DemosaicPass reads the tracing targets
	// unless ShadePass says otherwise.
	pTS->bShaded = false;

	CheckAllocateRT(pTS == &m_pPasses->m_tsSpec);

	if (!e_svoTI_Active || !e_svoTI_Apply || !e_svoRender || !m_pShader || m_texInfo.bSvoFreeze || !m_texInfo.pTexTree)
		return;

	const char* szTechFinalName = "ConeTracePass";
	const bool bBindDynamicLights = !GetIntegratioMode() && e_svoTI_InjectionMultiplier && m_arrLightsDynamic.Count();
	const CGraphicsPipelineResources& pipelineResources = RenderView()->GetGraphicsPipeline()->GetPipelineResources();

	rp.SetTechnique(m_pShader, szTechFinalName, GetRunTimeFlags(pTS == &m_pPasses->m_tsDiff));
	rp.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
	rp.SetState(GS_NODEPTHTEST);

	rp.SetRenderTarget(0, pTS->pRT_ALD_0);
	rp.SetRenderTarget(1, pTS->pRT_RGB_0);

	// rt stage 2 (decision 06 section 6.6): two more MRTs carry the hit position + smoothness
	// and the ray direction + distance. Only on the specular set, only with RT on - and the
	// targets are explicitly unbound again otherwise, because the pass object is a member and
	// would keep last frame's bindings while the shader has only two outputs.
	if (e_svoTI_RT_Active && pTS->pRT_HITPOS_0 && pTS->pRT_RAYDIR_0 && pTS->pRT_HITGI_0 && pTS->pRT_HITID_0)
	{
		rp.SetRenderTarget(2, pTS->pRT_HITPOS_0);
		rp.SetRenderTarget(3, pTS->pRT_RAYDIR_0);
		rp.SetRenderTarget(4, pTS->pRT_HITGI_0);
		// decision 11: the hit identity - which triangle, which material record, where on it.
		rp.SetRenderTarget(5, pTS->pRT_HITID_0);
	}
	else
	{
		rp.SetRenderTarget(2, nullptr);
		rp.SetRenderTarget(3, nullptr);
		rp.SetRenderTarget(4, nullptr);
		rp.SetRenderTarget(5, nullptr);
	}

	rp.SetRequireWorldPos(true);
	rp.SetRequirePerViewConstantBuffer(true);

	SetupRsmSunTextures(rp);

	SetupSvoTexturesForRead(m_texInfo, rp, (e_svoTI_Active ? e_svoTI_NumberOfBounces : 0), 0, 0);

	rp.SetTexture(10, pTS->pRT_ALD_1);
	rp.SetTexture(11, pTS->pRT_RGB_1);

	SetupGBufferTextures(rp);

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	// Mesh ray tracing pools (rt decision 02 section 2.1): BVH node/triangle/material records at
	// t9, material texture atlas at t55. The atlas moved from t18 to t28 in stage 2 (t18 became
	// Fwd_TiledLightsShadeInfo) and from t28 to t55 in decision 11, because ShadePass now reads
	// the same two pools and t28 is ForwardShading.cfi's Fwd_ShadowMap2 there - see
	// CommonSVO.cfi. Both slots must stay in step with the HLSL registers.
	// The old per-voxel index pool (t13) and per-voxel triangle list (t17) are retired together
	// with RayTraceMesh, so nothing binds them any more.
	if (e_svoTI_RT_Active)
	{
		if (m_texInfo.pTexTriA)
			rp.SetTexture(9, (CTexture*)m_texInfo.pTexTriA.get());

		if (m_texInfo.pTexTexA)
			rp.SetTexture(55, (CTexture*)m_texInfo.pTexTexA.get());

		// rt stage 4B (decision 08 item 1): the blue noise mask the GGX-VNDF sampler draws its
		// two uniform pairs from. Loaded the same way PostAA loads AreaTex.dds - a loose .dds
		// under %ENGINE%/EngineAssets, FT_DONT_STREAM | FT_NOMIPS, read with Load() so no
		// sampler is involved. Missing file => IsLoaded() is false, SVO_RTParams2.w stays 0 and
		// the shader uses its integer hash instead.
		if (!m_bTriedLoadRTBlueNoise)
		{
			m_bTriedLoadRTBlueNoise = true;
			m_pTexRTBlueNoise.Assign_NoAddRef(CTexture::ForName(
			  "%ENGINE%/EngineAssets/Textures/rt_bluenoise_64.dds", FT_DONT_STREAM | FT_NOMIPS, eTF_Unknown));
		}

		rp.SetTexture(32, IsRtBlueNoiseReady() ? m_pTexRTBlueNoise.get() : CRendererResources::s_ptexBlack);
	}
	#endif

	if (bBindDynamicLights)
	{
		BindTiledLights(m_arrLightsDynamic, (CFullscreenPass&)rp);
	}

	if (GetIntegratioMode() && e_svoTI_SSDepthTrace)
	{
		auto pTexHDRTargetPrev = pipelineResources.m_pTexHDRTargetPrev[RenderView()->GetCurrentEye()];
		if (pTexHDRTargetPrev->GetUpdateFrameID() > 1)
			rp.SetTexture(12, pTexHDRTargetPrev);
		else
			rp.SetTexture(12, CRendererResources::s_ptexBlack);
	}

	{
		const bool setupCloudShadows = gcpRendD3D->m_bShadowsEnabled && gcpRendD3D->m_bCloudShadowsEnabled;
		if (setupCloudShadows)
		{
			// cloud shadow map
			m_pCloudShadowTex = gcpRendD3D->GetCloudShadowTextureId() > 0 ? CTexture::GetByID(gcpRendD3D->GetCloudShadowTextureId()) : CRendererResources::s_ptexWhite;
			assert(m_pCloudShadowTex);

			rp.SetTexture(15, m_pCloudShadowTex);
		}
		else
		{
			rp.SetTexture(15, CRendererResources::s_ptexWhite);
		}
	}

	rp.SetTexture(8, GetUtils().GetVelocityObjectRT(RenderView()));

	SetupCommonSamplers(rp);

	rp.BeginConstantUpdate();
	SetupCommonConstants(pTS, rp, pTS->pRT_ALD_0);
	SetupRsmSunConstants(rp);

	if (bBindDynamicLights)
	{
		SetupLightSources(m_arrLightsDynamic, rp);
	}

	rp.Execute();
}

///////////////////////////////////////////////////////////////////////////////////
// rt stage 2 - hit shading (decision 06)
///////////////////////////////////////////////////////////////////////////////////

bool CSvoRenderer::IsRtHitShadingActive() const
{
	// Every condition the specular ConeTracePass itself needs, plus the RT master switch.
	// With e_svoTI_RT_Active == 0 (the default, VF_EXPERIMENTAL) nothing below ever runs, no
	// target is allocated, no constant is set, and DemosaicPass reads the stock pair.
	return e_svoTI_RT_Active
	       && e_svoTI_Active && e_svoTI_Apply && e_svoRender
	       && GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier
	       && m_pShader && !m_texInfo.bSvoFreeze && m_texInfo.pTexTree;
}

template<class T>
void CSvoRenderer::SetupRTLightGridConstants(T& rp)
{
	static CCryNameR paramNameMin("SVO_RTLightGridMin");
	static CCryNameR paramNameMax("SVO_RTLightGridMax");
	static CCryNameR paramNameDims("SVO_RTLightGridDims");

	rp.SetConstantArray(paramNameMin, (Vec4*)&m_rtLightGridMin, 1);
	rp.SetConstantArray(paramNameMax, (Vec4*)&m_rtLightGridMax, 1);
	rp.SetConstantArray(paramNameDims, (Vec4*)&m_rtLightGridDims, 1);
}

// Neo's BuildRayTracingLightListCS. One thread per grid cell, 8 x 8 x 8 groups, testing every
// valid entry of the EXISTING tiled light list (Fwd_TiledLightsShadeInfo) against the cell's
// AABB. No new CPU-side light collection at all: the point of the design is that the shade-time
// index structure stays byte-identical to the screen tile mask and only the address function
// changes (research/06 section 3.4).
void CSvoRenderer::BuildRTLightGridPass()
{
	std::shared_ptr<CGraphicsPipeline> pActivePipeline = RenderView()->GetGraphicsPipeline();
	auto* pTiledLights = pActivePipeline->GetStage<CTiledLightVolumesStage>();

	if (!pTiledLights)
		return;

	// the compute shader is [numthreads(8,8,8)] with one thread per cell
	const int nDim = clamp_tpl(((e_svoTI_RT_LightGridDim + 7) / 8) * 8, 8, 128);

	if (m_rtLightGridDim != nDim)
	{
		m_rtLightGridBuf.Create(nDim * nDim * nDim * 8, sizeof(uint32), DXGI_FORMAT_R32_UINT,
		                        CDeviceObjectFactory::BIND_SHADER_RESOURCE | CDeviceObjectFactory::BIND_UNORDERED_ACCESS, NULL);
		m_rtLightGridDim = nDim;
	}

	// The box follows the camera and reaches as far as a triangle ray can: a hit is at most
	// e_svoTI_RT_MaxDistCam (primary surface) + e_svoTI_RT_MaxDistRay (ray) away. Hits outside
	// it are clamped to the nearest cell by the shader, not read out of bounds as in Neo.
	const Vec3 vCamPos = gEnv->pSystem->GetViewCamera().GetPosition();
	const float fReach = max(1.f, e_svoTI_RT_MaxDistCam + e_svoTI_RT_MaxDistRay);

	m_rtLightGridMin = Vec4(vCamPos - Vec3(fReach, fReach, fReach), 0);
	m_rtLightGridMax = Vec4(vCamPos + Vec3(fReach, fReach, fReach), 0);
	m_rtLightGridDims = Vec4((float)nDim, (float)nDim, (float)nDim, (float)pTiledLights->GetValidLightCount());

	CSvoComputePass& rp = m_pPasses->m_passBuildRTLightList;

	rp.SetTechnique(m_pShader, "BuildRayTracingLightList", GetRunTimeFlags(false, true));

	// u0 is written, never OR-ed: one thread owns one cell, so the store is also the clear.
	// Neo accumulates with InterlockedOr and has no visible clear, so stale masks survive.
	rp.SetOutputUAV(0, &m_rtLightGridBuf);
	rp.SetBuffer(18, pTiledLights->GetLightShadeInfoBuffer());

	rp.SetDispatchSize(nDim / 8, nDim / 8, nDim / 8);

	rp.BeginConstantUpdate();

	SetupRTLightGridConstants(rp);

	rp.PrepareResourcesForUse(GetDeviceObjectFactory().GetCoreCommandList());

	{
		SScopedComputeCommandList computeCommandList(false);
		rp.Execute(computeCommandList, EShaderStage_All);
	}
}

// The forward per-pass resource set, filled from CSvoRenderer rather than borrowed from
// CSceneForwardStage (decision 06 section 6.3). Only the slots ShadePS actually reaches are
// bound; everything else the forward includes declare is unreferenced in this technique and is
// stripped by the compiler.
void CSvoRenderer::SetupShadeForwardResources(CSvoFullscreenPass& rp)
{
	std::shared_ptr<CGraphicsPipeline> pActivePipeline = RenderView()->GetGraphicsPipeline();

	auto* pTiledLights = pActivePipeline->GetStage<CTiledLightVolumesStage>();
	auto* pShadowMapStage = pActivePipeline->GetStage<CShadowMapStage>();

	// samplers declared by ForwardShading.cfi / TiledShading.cfi
	rp.SetSampler(10, EDefaultSamplerStates::BilinearWrap);    // ssFwdBilinearWrap
	rp.SetSampler(11, EDefaultSamplerStates::LinearCompare);   // ssFwdComparison
	rp.SetSampler(15, EDefaultSamplerStates::TrilinearClamp);  // SampStateTrilinearClamp

	// the light list itself, and the atlases the probe / projector / shadow terms sample
	rp.SetBuffer(18, pTiledLights->GetLightShadeInfoBuffer());          // Fwd_TiledLightsShadeInfo
	rp.SetTexture(20, pTiledLights->GetSpecularProbeAtlas());           // Fwd_SpecCubeArray
	rp.SetTexture(21, pTiledLights->GetDiffuseProbeAtlas());            // Fwd_DiffuseCubeArray
	rp.SetTexture(22, pTiledLights->GetProjectedLightAtlas());          // Fwd_SpotTexArray
	rp.SetTexture(23, pShadowMapStage->m_pTexRT_ShadowPool);            // Fwd_ShadowPool
	rp.SetTexture(24, CRendererResources::s_ptexShadowJitterMap);       // Fwd_RandomRotations
	rp.SetTexture(40, CRendererResources::s_ptexEnvironmentBRDF);       // Fwd_EnvironmentBRDF

	// the world-space light mask grid built one pass earlier
	rp.SetBuffer(33, &m_rtLightGridBuf);                                // Fwd_TileLightMaskRayTracing

	// Sun cascades at a world position. This is the volumetric-fog precedent (VolumetricFog.cpp
	// :932, :986) - three calls, no forward stage involved - and it is the only option that gives
	// the hit the same cascade walk, the same bias and the same cloud shadows as the primary
	// surface. rsmSunShadowMap (one cascade, unfiltered, injection resolution) is not a substitute.
	CShadowUtils::SShadowCascades cascades;
	CShadowUtils::SetupShadowsForFog(cascades, RenderView());
	// the two helpers are explicitly instantiated for CFullscreenPass and CComputeRenderPass
	// only (ShadowUtils.cpp:584-598), so pass the base reference as BindTiledLights does
	CShadowUtils::SetShadowCascadesToRenderPass((CFullscreenPass&)rp, 26, 30, cascades);          // t26..t29 + cloud t30
	CShadowUtils::SetShadowSamplingContextToRenderPass((CFullscreenPass&)rp, 11, 8, 9, 10, 31);   // s11/s8/s9/s10 + noise t31

	// CBPerPassForward (b5)
	if (!m_pShadeForwardCB)
		m_pShadeForwardCB = gcpRendD3D->m_DevBufMan.CreateConstantBuffer(sizeof(SSvoShadeForwardConstantBuffer));

	{
		PREFAST_SUPPRESS_WARNING(6263)
		CryStackAllocWithSizeCleared(SSvoShadeForwardConstantBuffer, cb, CDeviceBufferManager::AlignBufferSizeForStreaming);

		CShadowUtils::GetShadowCascadesSamplingInfo(cb->cbShadowSampling, RenderView());

		// rt stage 4A, energy audit item 9 / E9. The [U] the audit could not settle by reading.
		// Fwd_SampleSunShadowMaps walks the cascades only for the bits set in the mask that rides
		// in kernelRadius.z (ShadowCommon.cfi:369 GetForwardShadowsCascadeMask =
		// asuint(fKernelRadius.z)); with a zero mask ShadowDepthTest returns 1, shadowMask comes
		// back 0 and EVERY hit is fully sunlit - an unbounded first-order brightness error that
		// looks exactly like "the reflection is too bright" and nothing else. The mask also
		// carries the cloud-shadow bit, so the cascade half is the low MaxCascadesNum bits.
		// Logged once per session, only when the sun really does have cascades this frame.
		{
			const uint32 nMask = alias_cast<uint32>(cb->cbShadowSampling.kernelRadius.z);
			const uint32 nCascadeBits = nMask & ((1u << CShadowUtils::MaxCascadesNum) - 1u);
			const size_t nFrustums = RenderView()->GetShadowFrustumsByType(CRenderView::eShadowFrustumRenderType_SunDynamic).size();

			static bool bWarned = false;
			if (!bWarned && nFrustums > 0 && nCascadeBits == 0)
			{
				bWarned = true;
				CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
				           "SVOGI RT hit shading: the sun has %d cascade(s) this frame but the ShadePass cascade mask is 0 "
				           "(kernelRadius.z). Every ray traced hit will be shaded fully sunlit. e_svoTI_RT_Debug 7 will be all white.",
				           (int)nFrustums);
			}
		}

		if (auto* pForwardStage = pActivePipeline->GetStage<CSceneForwardStage>())
			pForwardStage->FillCloudShadingParams(cb->cbClouds, true);

		if (auto* pFogStage = pActivePipeline->GetStage<CFogStage>())
			pFogStage->FillForwardParams(cb->cbFog, true);

		if (auto* pVolFogStage = pActivePipeline->GetStage<CVolumetricFogStage>())
			pVolFogStage->FillForwardParams(cb->cbVoxelFog, true);

		FillForwardParams(cb->cbSVOGI, true);

		m_pShadeForwardCB->UpdateBuffer(cb, cbSize);
	}

	rp.SetInlineConstantBuffer(eConstantBufferShaderSlot_PerPass, m_pShadeForwardCB, EShaderStage_Pixel);
}

// rt stage 4A (decision 09 section 9.1, energy audit item 7): the sky a MISSED ray sees.
//
// There is no "sky render target" to sample: CSkyStage::Execute runs SkyPassPS straight into the
// HDR colour target from the sky-light manager's two lat-long Nishita textures (and, on a skybox
// level, from the level's skybox texture). So the ray tracer binds the same three textures and
// SVO_RT_SampleSky evaluates the same closed form.
//
// Both halves of the exposure question were checked against the pass itself rather than assumed:
// CSkyStage::SetSkyParameters (Sky.cpp:158) and SetHDRSkyParameters (Sky.cpp:211) multiply every
// RADIANCE constant by GetSceneReferredExposure() on the CPU and leave the textures unexposed, so
// the constants below are uploaded the same way and SVO_RT_SampleSky returns EXPOSED radiance -
// the same units ShadePS's `acc` is in, which its single divide then hands to ApplyGI's multiply
// for a net x1. GetSceneReferredExposure() is numerically PS_HDR_RANGE_ADAPT_MAX
// (GraphicsPipeline.cpp:1303 publishes it as CV_SceneExposure.x + 1), and it is exactly 1.0 off
// the scene-referred switch.
//
// m_bSkyBound records whether anything was bound at all; with no sky (an interior level, or the
// sky stage inactive) SVO_SkyParams stays zero, SVO_RT_SampleSky returns false and the shader
// keeps the env-probe fallback - which is the right answer for a ray that left a room rather
// than the world. The SVO's analytic GetSkyColor is NOT used as that fallback: it reads
// SvoParamsSkyColor / globalSpecCM, neither of which ShadePass uploads or may bind (globalSpecCM
// sits in the SVO resource set this technique aliases away).
void CSvoRenderer::SetupShadeSkyTextures(CSvoFullscreenPass& rp)
{
	const int threadID = gRenDev->GetRenderThreadID();
	const N3DEngineCommon::SSkyInfo& skyInfo = gcpRendD3D->m_p3DEngineCommon[threadID].m_SkyInfo;

	auto* pSkyStage = RenderView()->GetGraphicsPipeline()->GetStage<CSkyStage>();

	CTexture* pMie = pSkyStage ? pSkyStage->GetSkyDomeTextureMie() : nullptr;
	CTexture* pRay = pSkyStage ? pSkyStage->GetSkyDomeTextureRayleigh() : nullptr;
	CTexture* pBox = skyInfo.m_pSkyBoxTexture.get();

	m_bSkyDomeBound = skyInfo.m_bIsVisible && skyInfo.m_bApplySkyDome && pMie && pRay && pMie->GetDevTexture() && pRay->GetDevTexture();
	m_bSkyBoxBound = skyInfo.m_bIsVisible && skyInfo.m_bApplySkyBox && pBox && pBox->GetDevTexture();

	rp.SetTexture(34, m_bSkyDomeBound ? pMie : CRendererResources::s_ptexBlack);
	rp.SetTexture(35, m_bSkyDomeBound ? pRay : CRendererResources::s_ptexBlack);
	rp.SetTexture(36, m_bSkyBoxBound ? pBox : CRendererResources::s_ptexBlack);

	// The dome is a lat-long map: azimuth wraps, elevation must not. Same state the sky pass
	// builds for its own dome fetch (Sky.cpp:424).
	static SamplerStateHandle skySampler = EDefaultSamplerStates::Unspecified;
	if (skySampler == EDefaultSamplerStates::Unspecified)
	{
		const SSamplerState desc(FILTER_LINEAR, eSamplerAddressMode_Wrap, eSamplerAddressMode_Clamp, eSamplerAddressMode_Clamp, 0);
		skySampler = GetDeviceObjectFactory().GetOrCreateSamplerStateHandle(desc);
	}
	rp.SetSampler(3, skySampler);
}

void CSvoRenderer::SetupShadeSkyConstants(CSvoFullscreenPass& rp)
{
	const int threadID = gRenDev->GetRenderThreadID();
	const N3DEngineCommon::SSkyInfo& skyInfo = gcpRendD3D->m_p3DEngineCommon[threadID].m_SkyInfo;
	const float sceneExposure = gcpRendD3D->GetSceneReferredExposure();

	Vec4 vParams(m_bSkyDomeBound ? 1.f : 0.f, m_bSkyBoxBound ? 1.f : 0.f,
	             DEG2RAD(skyInfo.m_fSkyBoxAngle), skyInfo.m_fSkyBoxStretching);
	Vec4 vPeakClamp(CRendererResources::GetSceneReferredSkyPeakClamp(sceneExposure), 0.f, 0.f, 0.f);

	Vec4 vMie(0, 0, 0, 0), vRayleigh(0, 0, 0, 0), vSunDir(0, 0, 0, 0), vPhase(0, 0, 0, 0);
	Vec4 vNightBase(0, 0, 0, 0), vNightDelta(0, 0, 0, 0), vNightShift(0, 0, 0, 0);

	if (m_bSkyDomeBound)
	{
		I3DEngine* const p3DEngine = gEnv->p3DEngine;
		const SSkyLightRenderParams* const pRenderParams = p3DEngine->GetSkyLightRenderParams();

		vMie = pRenderParams->m_partialMieInScatteringConst * sceneExposure;
		vRayleigh = pRenderParams->m_partialRayleighInScatteringConst * sceneExposure;
		vSunDir = pRenderParams->m_sunDirection;
		vPhase = pRenderParams->m_phaseFunctionConsts;

		Vec3 nightSkyHorizonCol, nightSkyZenithCol;
		p3DEngine->GetGlobalParameter(E3DPARAM_NIGHSKY_HORIZON_COLOR, nightSkyHorizonCol);
		p3DEngine->GetGlobalParameter(E3DPARAM_NIGHSKY_ZENITH_COLOR, nightSkyZenithCol);
		const float nightSkyZenithColShift = p3DEngine->GetGlobalParameter(E3DPARAM_NIGHSKY_ZENITH_SHIFT);
		const float minNightSkyZenithGradient = -0.1f;

		vNightBase = Vec4(nightSkyHorizonCol * sceneExposure, 0);
		vNightDelta = Vec4((nightSkyZenithCol - nightSkyHorizonCol) * sceneExposure, 0);
		vNightShift = Vec4(1.0f / (nightSkyZenithColShift - minNightSkyZenithGradient),
		                   -minNightSkyZenithGradient / (nightSkyZenithColShift - minNightSkyZenithGradient), 0, 0);
	}

	Vec4 vBoxExposure(skyInfo.m_vSkyBoxEmittance * sceneExposure, 1.0f);
	Vec4 vBoxOpacity(skyInfo.m_vSkyBoxFilter, 1.0f);

	static CCryNameR nameParams("SVO_SkyParams");
	static CCryNameR namePeak("SVO_SkyPeakClamp");
	static CCryNameR nameMie("SVO_SkyMieConst");
	static CCryNameR nameRayleigh("SVO_SkyRayleighConst");
	static CCryNameR nameSunDir("SVO_SkySunDirection");
	static CCryNameR namePhase("SVO_SkyPhaseConst");
	static CCryNameR nameNightBase("SVO_SkyNightColBase");
	static CCryNameR nameNightDelta("SVO_SkyNightColDelta");
	static CCryNameR nameNightShift("SVO_SkyNightZenithColShift");
	static CCryNameR nameBoxExposure("SVO_SkyBoxExposure");
	static CCryNameR nameBoxOpacity("SVO_SkyBoxOpacity");

	rp.SetConstantArray(nameParams, &vParams, 1);
	rp.SetConstantArray(namePeak, &vPeakClamp, 1);
	rp.SetConstantArray(nameMie, &vMie, 1);
	rp.SetConstantArray(nameRayleigh, &vRayleigh, 1);
	rp.SetConstantArray(nameSunDir, &vSunDir, 1);
	rp.SetConstantArray(namePhase, &vPhase, 1);
	rp.SetConstantArray(nameNightBase, &vNightBase, 1);
	rp.SetConstantArray(nameNightDelta, &vNightDelta, 1);
	rp.SetConstantArray(nameNightShift, &vNightShift, 1);
	rp.SetConstantArray(nameBoxExposure, &vBoxExposure, 1);
	rp.SetConstantArray(nameBoxOpacity, &vBoxOpacity, 1);
}

// rt stage 5E (decision 09 section 9.3): VOLUMETRIC FOG ON THE REFLECTED SEGMENT.
//
// The frame-order question the decision note left open, answered by reading
// StandardGraphicsPipeline.cpp:
//
//   :460  SVOGI                              <- we are here
//   :523  CSceneForwardStage::ExecuteOpaque  <- samples the froxel volume (Fwd_volFogTex)
//   :536  CVolumetricFogStage::Execute       <- BUILDS the froxel volume
//
// The forward opaque pass already reads a froxel volume that was written one frame earlier. So
// the previous frame's volume is not a compromise we are introducing, it is the data CE's own
// forward shading has always used, and reading it from SVOGI needs NO pipeline reordering at
// all - the stock frame order is byte for byte unchanged, which is the whole reason this option
// was preferred over moving CVolumetricFogStage::Execute() ahead of the SVO block.
//
// WHICH volume: NOT GetVolumetricFogTex() (s_ptexVolumetricFog). That one is the raymarch
// OUTPUT and holds in-scatter accumulated FROM THE CAMERA, which can only answer "how much fog
// is between the camera and this point" - a question about the camera ray, not about a reflected
// segment. We bind the raymarch INPUT instead (GetLocalInscatterVolume, plus the separate
// density volume when the in-scatter format has no alpha), whose two fields are the LOCAL
// per-unit-length in-scattered radiance and extinction of each froxel and are therefore
// integrable along any direction. The shader runs CE's own integrator over them
// (VolumeLighting.cfi RaymarchVolumetricFogCS) along the reflected segment.
//
// Both volumes are plain _smart_ptr<CTexture> members of the stage, allocated in
// ResizeResource and released only on a resolution change, so they are alive at :460; the pair
// is double buffered by m_tick, and m_tick is only advanced inside Execute(), so
// GetLocalInscatterVolume() at :460 is exactly the texture last frame's temporal reprojection
// wrote. Nothing here writes to them.
//
// t41 and t50 are free on the ShadePass technique: the highest register ForwardShading.cfi /
// TiledShading.cfi / ShadeLib.cfi reach is t49 (LTCTex_2), t41 is the one gap in that range,
// and MAX_TMU is 64 off Orbis.
void CSvoRenderer::SetupShadeFogTextures(CSvoFullscreenPass& rp)
{
	m_bVolFogBound = false;
	m_bVolFogSeparateDensity = false;

	if (!e_svoTI_RT_Active)
		return;

	// the fallback path, deliberately: with r_VolumetricFog 0 the stage is inactive and
	// ShadePS keeps the analytic global fog it has always applied to the segment.
	if (!gRenDev->m_bVolumetricFogEnabled || !RenderView()->IsGlobalFogEnabled() || !CVolumetricFogStage::IsEnabledInFrame())
		return;

	auto* pVolFog = RenderView()->GetGraphicsPipeline()->GetStage<CVolumetricFogStage>();
	if (!pVolFog || !pVolFog->AreLocalVolumesValid())
		return;

	CTexture* pInscatter = pVolFog->GetLocalInscatterVolume();
	if (!CTexture::IsTextureExist(pInscatter))
		return;

	const bool bSeparate = pVolFog->HasSeparateDensityVolume();
	CTexture* pDensity = pVolFog->GetLocalDensityVolume();
	if (bSeparate && !CTexture::IsTextureExist(pDensity))
		return;

	rp.SetTexture(41, pInscatter);                              // SVO_VolFogInscatter
	rp.SetTexture(50, bSeparate ? pDensity : pInscatter);       // SVO_VolFogDensity

	m_bVolFogBound = true;
	m_bVolFogSeparateDensity = bSeparate;
}

void CSvoRenderer::SetupShadeFogConstants(CSvoFullscreenPass& rp)
{
	// Eight steps. The segment is at most e_svoTI_RT_MaxDistRay (48 m) long and the froxel grid
	// is far coarser than that, so more steps buy resolution the source does not have; fewer
	// start to miss a thin fog volume the segment crosses.
	const float fSteps = 8.f;

	static CCryNameR paramName("SVO_VolFogParams");
	Vec4 vData(m_bVolFogBound ? 1.f : 0.f, m_bVolFogSeparateDensity ? 1.f : 0.f, fSteps, 0.f);
	rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
}

// rt stage 5F (decision 09 section 9.2): VOLUMETRIC CLOUDS ON A RAY THAT MISSES ABOVE THE HORIZON.
//
// THE FRAME ORDER, read in StandardGraphicsPipeline.cpp:
//
//   :457  CVolumetricCloudsStage::ExecuteShadowGen()   <- fills m_pTexVolCloudShadow
//   :460  SVOGI                                        <- we are here
//   :544  CVolumetricCloudsStage::Execute()            <- renders the cloud IMAGE
//
// The cloud shadow volume is therefore THIS frame's, three lines fresh, which is what makes
// decision 09 section 9.2's "sun transmittance approximated by the cloud shadow map" the cheap
// and correct choice rather than a stale one. The cloud image is not available and is not what a
// reflection needs anyway: it is the sky the CAMERA saw in that pixel, not the sky along the
// reflected ray. So the shader marches the density field again (CloudsCommon.cfi, shared with
// Clouds.cfx) and this function gives it the field's parameters.
//
// WHY THE CONSTANTS ARE REBUILT HERE RATHER THAN BORROWED. The clouds stage keeps them in its
// own inline constant buffer at b3, filled inside Execute() at :544 - after us, and at a slot
// ShadePass already uses. Every value below is read from the SAME source the stage reads
// (the E3DPARAM_VOLCLOUD_* time-of-day globals and this render view's shader constants), in the
// same frame, so the two agree by construction rather than by copy.
//
// EXPOSURE, verified against CVolumetricCloudsStage::GenerateCloudShaderParam rather than
// assumed. That function multiplies shadeColorFromSun (VolumetricClouds.cpp:1486) and
// skylightRayleighInScatter (:1518) by GetSceneReferredExposure() and deliberately leaves the
// scattering / extinction COEFFICIENTS alone, because a coefficient that lives inside exp() must
// never take an exposure. The two multiplies are repeated below and nothing else is scaled, so
// the shader's cloud in-scatter comes out in EXPOSED radiance - the same units
// SVO_RT_SampleSky's result and the rest of ShadePS's `acc` are in, and it is composited before
// the single divide. Off the scene-referred switch the factor is 1.0 and nothing moves.
//
// t51..t54 and s4/s5 are free on the ShadePass technique: the highest register the forward stack
// reaches is t49 (LTCTex_2), stage 5E took t41 and t50, and the samplers in use are s0 (point
// clamp), s3 (sky), s10/s11/s14/s15 (forward).
void CSvoRenderer::SetupShadeCloudTextures(CSvoFullscreenPass& rp)
{
	m_bCloudsBound = false;

	if (!e_svoTI_RT_Active || !e_svoTI_RT_Clouds)
		return;

	// The same two gates CVolumetricCloudsStage itself uses: e_Clouds / r_VolumetricClouds for
	// "this level draws volumetric clouds at all", and m_bVolumetricCloudsEnabled for "the stage
	// runs this frame" (which is literally what its IsStageActive returns).
	if (!CVolumetricCloudsStage::IsRenderable() || !gcpRendD3D->m_bVolumetricCloudsEnabled)
		return;

	auto* pClouds = RenderView()->GetGraphicsPipeline()->GetStage<CVolumetricCloudsStage>();
	if (!pClouds)
		return;

	CTexture* pShadow = pClouds->GetVolCloudShadowTex();
	CTexture* pMie = pClouds->GetCloudMiePhaseTex();
	if (!CTexture::IsTextureExist(pShadow) || !CTexture::IsTextureExist(pMie))
		return;

	// Resolve the two noise volumes exactly as ExecuteVolumetricCloudShadowGen does, so the march
	// samples the same noise the shadow volume we are about to read was generated from. The
	// stage's own cached pointers are last frame's and are null on the first frame; the level's
	// texture ids plus the engine default are not.
	SVolumetricCloudTexInfo texInfo;
	gcpRendD3D->GetVolumetricCloudTextureInfo(texInfo);

	CTexture* pNoise = pClouds->GetDefaultNoiseTex();
	if (texInfo.cloudNoiseTexId > 0)
	{
		if (CTexture* pTex = CTexture::GetByID(texInfo.cloudNoiseTexId))
			pNoise = pTex;
	}

	CTexture* pEdgeNoise = pClouds->GetDefaultNoiseTex();
	if (texInfo.edgeNoiseTexId > 0)
	{
		if (CTexture* pTex = CTexture::GetByID(texInfo.edgeNoiseTexId))
			pEdgeNoise = pTex;
	}

	if (!CTexture::IsTextureExist(pNoise) || !CTexture::IsTextureExist(pEdgeNoise))
		return;

	rp.SetTexture(51, pNoise);        // SVO_CloudNoiseTex
	rp.SetTexture(52, pEdgeNoise);    // SVO_CloudEdgeNoiseTex
	rp.SetTexture(53, pShadow);       // SVO_CloudShadowTex
	rp.SetTexture(54, pMie);          // SVO_CloudMiePhaseTex

	// The two states Clouds.cfx uses for exactly these two fetches: TrilinearWrap for the noise
	// volumes (the density field tiles), and the border state for the cloud shadow volume, whose
	// tiling region ends in "no shadow" rather than in a repeat.
	rp.SetSampler(4, EDefaultSamplerStates::TrilinearWrap);
	rp.SetSampler(5, EDefaultSamplerStates::TrilinearBorder_Black);

	m_bCloudsBound = true;
}

void CSvoRenderer::SetupShadeCloudConstants(CSvoFullscreenPass& rp)
{
	// The clouds stage keeps these four in an anonymous namespace of VolumetricClouds.cpp, which
	// belongs to the paused clouds branch and is not ours to touch. They are physical constants
	// and a texture-space unit, not tunables - if that file ever changes them, this block has to
	// follow, which is why they are named and sourced here rather than folded into a magic number.
	//   VolumetricClouds.cpp:47-53
	const float VCDropletDensity = 1e9f;
	const float VCDropletRadius = 0.000015f;
	const float VCScatterCoefficient = VCDropletDensity * (gf_PI * VCDropletRadius * VCDropletRadius);
	const Vec3  VCBaseNoiseScale(0.00003125f, 0.00003125f, 0.00003125f);
	const float VCEdgeNoiseScale = 19.876521f;
	const float VCMinSphereRadius = 100000.0f;
	const float VCMaxSphereRadius = 10000000.0f;

	Vec4 vParams0(0, 0, 0, 0), vParams1(0, 0, 0, 0), vParams2(0, 0, 0, 0);
	Vec4 vNoiseScale(0, 0, 0, 0), vBaseScale(0, 0, 0, 0), vBaseOffset(0, 0, 0, 0);
	Vec4 vDensity(0, 0, 0, 0), vEdgeNoiseScale(0, 0, 0, 0), vEdgeTurbulence(0, 0, 0, 0);
	Vec4 vInvTiling(0, 0, 0, 0), vShadowOffset(0, 0, 0, 0);
	Vec4 vSunLight(0, 0, 0, 0), vSunDir(0, 0, 0, 0), vSkyLight(0, 0, 0, 0), vGroundLight(0, 0, 0, 0);
	Vec4 vScatter(0, 0, 0, 0), vMultiScatter(0, 0, 0, 0);

	if (m_bCloudsBound)
	{
		I3DEngine* const p3DEngine = gEnv->p3DEngine;
		const SRenderViewShaderConstants& PF = RenderView()->GetShaderConstants();
		const float sceneExposure = gcpRendD3D->GetSceneReferredExposure();

		Vec3 genParams, scatteringLow, scatteringHigh, groundColor, scatteringMulti, turbulence;
		Vec3 envParams, globalNoiseScale, renderParams, turbulenceNoiseScale, turbulenceNoiseParams;
		Vec3 densityParams, miscParams, skylightRayleigh;
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_GEN_PARAMS, genParams);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_SCATTERING_LOW, scatteringLow);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_SCATTERING_HIGH, scatteringHigh);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_GROUND_COLOR, groundColor);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_SCATTERING_MULTI, scatteringMulti);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_TURBULENCE, turbulence);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_ENV_PARAMS, envParams);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_GLOBAL_NOISE_SCALE, globalNoiseScale);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_RENDER_PARAMS, renderParams);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_TURBULENCE_NOISE_SCALE, turbulenceNoiseScale);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_TURBULENCE_NOISE_PARAMS, turbulenceNoiseParams);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_DENSITY_PARAMS, densityParams);
		p3DEngine->GetGlobalParameter(E3DPARAM_VOLCLOUD_MISC_PARAM, miscParams);
		p3DEngine->GetGlobalParameter(E3DPARAM_SKYLIGHT_RAYLEIGH_INSCATTER, skylightRayleigh);

		const float altitude = genParams.y;
		const float thickness = max(1e-3f, genParams.z);
		const float absorptionFactor = turbulence.z;
		const float extinction = VCScatterCoefficient + (VCScatterCoefficient * absorptionFactor);
		const float shadowTilingSize = miscParams.z;
		const float edgeNoiseErode = turbulenceNoiseParams.x;

		vParams0 = Vec4(1.f, altitude, thickness, genParams.x);
		vParams1 = Vec4(VCScatterCoefficient, extinction, scatteringMulti.z,
		                clamp_tpl<float>(envParams.x, VCMinSphereRadius, VCMaxSphereRadius));
		// .w = the clouds-on-a-hit distance, deliberately 0 (off): inside e_svoTI_RT_MaxDistRay
		// no hit is far enough for a cloud deck to be in front of it.
		vParams2 = Vec4((float)clamp_tpl<int>(e_svoTI_RT_CloudSteps, 1, 64),
		                renderParams.x, renderParams.y, 0.f);

		vNoiseScale = Vec4(VCBaseNoiseScale.CompMul(globalNoiseScale), 0.f);

		const Vec3& baseTexTiling = PF.pVolCloudTilingSize;
		vBaseScale = Vec4(1.0f / max(baseTexTiling.x, 1e-6f), 1.0f / max(baseTexTiling.y, 1e-6f),
		                  1.0f / max(baseTexTiling.z, 1e-6f), 0.f);

		// THE ANIMATION. pVolCloudTilingOffset is the wind-driven offset the time of day system
		// advances every frame, and the -altitude on z is how Clouds.cfx folds the layer's base
		// height into the same vector (VolumetricClouds.cpp:1587). Using the same lane is what
		// makes a cloud drift identically in the reflection and in the direct view.
		vBaseOffset = Vec4(PF.pVolCloudTilingOffset.x, PF.pVolCloudTilingOffset.y,
		                   PF.pVolCloudTilingOffset.z - altitude, 0.f);

		vDensity = Vec4(densityParams.x, densityParams.y, densityParams.z, miscParams.x);
		vEdgeNoiseScale = Vec4(VCBaseNoiseScale.CompMul(turbulenceNoiseScale * VCEdgeNoiseScale), 0.f);
		vEdgeTurbulence = Vec4((edgeNoiseErode > 0.0f) ? 2.0f * turbulence.x : 8.0f * turbulence.x,
		                       turbulence.y + 1e-4f, edgeNoiseErode, turbulenceNoiseParams.y);

		vInvTiling = Vec4(1.0f / max(shadowTilingSize, 1e-6f), 1.0f / max(shadowTilingSize, 1e-6f),
		                  1.0f / thickness, 0.f);
		vShadowOffset = Vec4(PF.pCloudShadowAnimParams.x, PF.pCloudShadowAnimParams.y, 0.f, 0.f);

		vSunLight = Vec4(PF.pCloudShadingColorSun * sceneExposure, 0.f);
		vSunDir = Vec4(PF.pSunDirection, 0.f);
		vSkyLight = Vec4(skylightRayleigh * sceneExposure, scatteringHigh.y);
		vGroundLight = Vec4(groundColor, scatteringHigh.z);
		vScatter = Vec4(scatteringLow.x, scatteringLow.y, scatteringLow.z, scatteringHigh.x);
		vMultiScatter = Vec4(scatteringMulti.x, scatteringMulti.y, 0.f, 0.f);
	}

	struct SNamedVec4 { const char* szName; const Vec4* pValue; };
	const SNamedVec4 arrConsts[] =
	{
		{ "SVO_CloudParams0",        &vParams0        },
		{ "SVO_CloudParams1",        &vParams1        },
		{ "SVO_CloudParams2",        &vParams2        },
		{ "SVO_CloudNoiseScale",     &vNoiseScale     },
		{ "SVO_CloudBaseScale",      &vBaseScale      },
		{ "SVO_CloudBaseOffset",     &vBaseOffset     },
		{ "SVO_CloudDensityParams",  &vDensity        },
		{ "SVO_CloudEdgeNoiseScale", &vEdgeNoiseScale },
		{ "SVO_CloudEdgeTurbulence", &vEdgeTurbulence },
		{ "SVO_CloudInvTiling",      &vInvTiling      },
		{ "SVO_CloudShadowOffset",   &vShadowOffset   },
		{ "SVO_CloudSunLight",       &vSunLight       },
		{ "SVO_CloudSunDir",         &vSunDir         },
		{ "SVO_CloudSkyLight",       &vSkyLight       },
		{ "SVO_CloudGroundLight",    &vGroundLight    },
		{ "SVO_CloudScatterParams",  &vScatter        },
		{ "SVO_CloudMultiScatter",   &vMultiScatter   },
	};

	for (const SNamedVec4& c : arrConsts)
	{
		const CCryNameR name(c.szName);
		rp.SetConstantArray(name, (Vec4*)c.pValue, 1);
	}
}

// Hit shading. Reads the four g-data targets, runs CE's own forward lighting at the hit world
// position, writes the ALD + RGB pair DemosaicPass consumes.
void CSvoRenderer::ShadePass(SSvoTargetsSet* pTS)
{
	CSvoFullscreenPass& rp = pTS->passShade;

	if (!pTS->pRT_ALD_SHD || !pTS->pRT_RGB_SHD || !pTS->pRT_HITPOS_0 || !pTS->pRT_RAYDIR_0 || !pTS->pRT_HITGI_0 || !pTS->pRT_HITID_0)
		return;

	rp.SetTechnique(m_pShader, "ShadePass", GetRunTimeFlags(false, true));
	rp.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
	rp.SetState(GS_NODEPTHTEST);

	rp.SetRenderTarget(0, pTS->pRT_ALD_SHD);
	rp.SetRenderTarget(1, pTS->pRT_RGB_SHD);
	rp.SetRequireWorldPos(true);
	rp.SetRequirePerViewConstantBuffer(true);

	// The six g-data targets. t0..t3 alias the BRICK pools declared in CommonSVO.cfi; see the
	// warning at the top of Total_Illumination_Shading.cfi. NOTE the two calls that are NOT here:
	// SetupSvoTexturesForRead and SetupRsmSunTextures would fight the forward set for t17..t20
	// and t26..t31, and ShadePS must not reach a BRICK pool. The two RAY TRACING pools it now
	// does reach are bound by hand below, at slots chosen to be free on this technique.
	rp.SetTexture(0, pTS->pRT_ALD_0);
	rp.SetTexture(1, pTS->pRT_RGB_0);
	rp.SetTexture(2, pTS->pRT_HITPOS_0);
	rp.SetTexture(3, pTS->pRT_RAYDIR_0);
	rp.SetTexture(37, pTS->pRT_HITGI_0);                   // refl_GGI, rt stage 4A
	rp.SetTexture(39, pTS->pRT_HITID_0);                   // refl_GID, decision 11
	rp.SetSampler(0, EDefaultSamplerStates::PointClamp);   // ssSvoPointClamp

	// decision 11: THE RAY TRACING POOLS ON THE SHADE PASS. RT_ReconstructHit reads the BVH /
	// triangle / material records out of geomPool_Tris (t9) and every atlas layer out of
	// geomPool_TTex (t55), because the hit material is rebuilt here now and not in the tracer.
	// Neither slot collides with the forward set (t17-t24, t25-t32, t33, t34-t36, t37, t38,
	// t39, t40, t41, t42-t49, t50-t54); t55 exists for exactly this reason, see CommonSVO.cfi.
	// Still NOT bound and still forbidden here: the brick pools, which alias t0-t3 with the
	// g-data targets above.
	if (m_texInfo.pTexTriA)
		rp.SetTexture(9, (CTexture*)m_texInfo.pTexTriA.get());

	if (m_texInfo.pTexTexA)
		rp.SetTexture(55, (CTexture*)m_texInfo.pTexTexA.get());

	// The atlas layers are sampled with ssSvoLinearClamp (tex3DlodLC), which the trace pass gets
	// from SetupSvoTexturesForRead. This pass does not call it, so s1 is bound here.
	rp.SetSampler(1, EDefaultSamplerStates::LinearClamp);  // ssSvoLinearClamp

	SetupShadeForwardResources(rp);
	SetupShadeSkyTextures(rp);
	SetupShadeFogTextures(rp);
	SetupShadeCloudTextures(rp);

	// rt stage 4A, debug view 13 = validator C (energy audit section 5). The only two resources
	// the pass needs beyond hit shading, and they are bound ONLY for that view: the depth buffer
	// to prove the camera really sees the hit, and the PREVIOUS frame's HDR target to read the
	// direct shading of it. $HDRTargetPrev is pre-exposed, and feeding a pre-exposed buffer back
	// into SVO maths is exactly what e_svoTI_SSDepthTrace is forced to 0 for on the scene-referred
	// path - so it stays inside the debug branch, one frame stale, and never reaches a pixel that
	// is drawn for real. t4 and t12 are free on this technique; TiledShading.cfi's CausticsRT at
	// t12 lives inside TILED_DEFERRED_SHADING_TECHNIQUE, which this shader never compiles.
	const bool bValidatorC = (e_svoTI_RT_Debug == 13);
	if (bValidatorC)
	{
		const CGraphicsPipelineResources& pipelineResources = RenderView()->GetGraphicsPipeline()->GetPipelineResources();
		rp.SetTexture(4, GetZBuffer(pipelineResources, true));

		auto pTexHDRTargetPrev = pipelineResources.m_pTexHDRTargetPrev[RenderView()->GetCurrentEye()];
		rp.SetTexture(12, (pTexHDRTargetPrev && pTexHDRTargetPrev->GetUpdateFrameID() > 1) ? pTexHDRTargetPrev : CRendererResources::s_ptexBlack);
	}

	rp.BeginConstantUpdate();

	SetupRTLightGridConstants(rp);
	SetupShadeSkyConstants(rp);
	SetupShadeFogConstants(rp);
	SetupShadeCloudConstants(rp);

	{
		static CCryNameR paramName("SVO_ShadeParams0");
		// .x flat albedo add (off), .y debug albedo (off), .z AO range - deliberately 0, which
		// makes Neo's distance-AO multiply a no-op: it darkens by hit distance and double-counts
		// against ApplyGI mode 0, which is also AO (R05, research/06 section 1.5 note 4).
		// .w = 1 for the specular set.
		Vec4 vData(0.f, 0.f, 0.f, 1.f);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		// decision 11: RT_ReconstructHit addresses the record pools with these dimensions, so
		// the lane that was tracer-only until now has to be on this pass as well. Same values,
		// same order, same fallbacks as SetupCommonConstants - if the two ever disagree, the
		// shade pass decodes a different record from the one the tracer hit.
		static CCryNameR paramName("SVO_RTPoolInfo");
		Vec4 vData(
		  (float)(m_texInfo.rtPoolXY   ? m_texInfo.rtPoolXY   : e_svoTI_RT_TriPoolXY),
		  (float)(m_texInfo.rtPoolZ    ? m_texInfo.rtPoolZ    : e_svoTI_RT_TriPoolZ),
		  (float)(m_texInfo.rtTexRes   ? m_texInfo.rtTexRes   : e_svoTI_RT_MaxTexRes),
		  (float)(m_texInfo.rtTexPoolZ ? m_texInfo.rtTexPoolZ : e_svoTI_RT_TexPoolZ));
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		// decision 11: the hit's normal-map distance fade moved here with the reconstruction.
		// (normalsFading, reserved x3), as in SetupCommonConstants.
		static CCryNameR paramName("SVO_RTParams1");
		Vec4 vData(e_svoTI_RT_NormalsFading, 0.f, 0.f, 0.f);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		// rt stage 2C: ShadePass does not call SetupCommonConstants (it needs none of it), so
		// the debug lane has to be uploaded here or views 7/8/9 read an undefined register.
		static CCryNameR paramName("SVO_RTParams0");
		Vec4 vData((float)e_svoTI_RT_MaxBounces, 1.f, 0.f, (float)e_svoTI_RT_Debug);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		// rt stage 4B: ShadePS reads .x to decide whether RayDir.xyz carries the VNDF estimator
		// weight in its length. Same lane, same order as SetupCommonConstants.
		static CCryNameR paramName("SVO_RTParams2");
		Vec4 vData((float)e_svoTI_RT_GlossyMode, e_svoTI_RT_GlossScale,
		           (float)max(e_svoTI_RT_TemporalFrames, 1), IsRtBlueNoiseReady() ? 1.f : 0.f);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	// rt stage 5E: the froxel march projects each sample point with this matrix, so it is no
	// longer debug-only. Uploaded when either consumer needs it, never otherwise.
	if (bValidatorC || m_bVolFogBound)
	{
		// SVO_ViewProj lives in SetupCommonConstants, which ShadePass deliberately does not call
		// (it needs none of the rest of it). Same matrix, same transpose, D3D_SVO.cpp:1039.
		static CCryNameR paramName("SVO_ViewProj");
		Matrix44A mViewProj = RenderView()->GetViewInfo(CCamera::eEye_Left).cameraProjMatrix;
		mViewProj.Transpose();
		rp.SetConstantArray(paramName, alias_cast<Vec4*>(&mViewProj), 4);
	}

	rp.Execute();

	pTS->bShaded = true;
}

template<class T>
void CSvoRenderer::SetupCommonConstants(SSvoTargetsSet* pTS, T& rp, CTexture* pRT)
{
	CD3D9Renderer* const __restrict rd = gcpRendD3D;

	CRenderView* pRenderView = RenderView();

	const int32 renderWidth = pRenderView->GetRenderResolution()[0];
	const int32 renderHeight = pRenderView->GetRenderResolution()[1];

	const SRenderViewInfo& viewInfo = pRenderView->GetViewInfo(CCamera::eEye_Left);

	{
		static CCryNameR paramName("SVO_ReprojectionMatrix");

		static int nReprojFrameId = -1;
		if ((pTS == &m_pPasses->m_tsDiff) && nReprojFrameId != pRenderView->GetFrameId())
		{
			nReprojFrameId = pRenderView->GetFrameId();

			const CCamera& cam = pRenderView->GetCamera(CCamera::eEye_Left);

			Matrix44A matView = cam.GetViewMatrix();

			Vec3 zAxis = matView.GetRow(1);
			matView.SetRow(1, -matView.GetRow(2));
			matView.SetRow(2, zAxis);
			float z = matView.m13;
			matView.m13 = -matView.m23;
			matView.m23 = z;

			Matrix44A matProj;
			mathMatrixPerspectiveFov(&matProj, cam.GetFov(), cam.GetProjRatio(), cam.GetNearPlane(), cam.GetFarPlane());
			static Matrix44A matPrevView = matView;
			static Matrix44A matPrevProj = matProj;
			rd->GetReprojectionMatrix(m_matReproj, matView, matProj, matPrevView, matPrevProj, cam.GetFarPlane());
			matPrevView = matView;
			matPrevProj = matProj;
		}
		rp.SetConstantArray(paramName, (Vec4*)m_matReproj.GetData(), 3);
	}

	{
		static CCryNameR paramName("SVO_FrameIdByte");
		Vec4 vData((float)(pRenderView->GetFrameId() & 255), (float)pRenderView->GetFrameId(), 3, 4);
		if (rd->GetActiveGPUCount() > 1)
			vData.x = 0;
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		static CCryNameR paramName("SVO_CamPos");
		Vec4 vData(gEnv->pSystem->GetViewCamera().GetPosition(), 0);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		static CCryNameR paramName("SVO_ViewProj");
		static CCryNameR paramNamePrev("SVO_ViewProjPrev");

		Matrix44A mViewProj;
		mViewProj = viewInfo.cameraProjMatrix;
		mViewProj.Transpose();

		rp.SetConstantArray(paramName, alias_cast<Vec4*>(&mViewProj), 4);
		rp.SetConstantArray(paramNamePrev, alias_cast<Vec4*>(&m_matViewProjPrev), 4);
	}

	if (pRT)
	{
		std::shared_ptr<CGraphicsPipeline> pActivePipeline = pRenderView->GetGraphicsPipeline();

		int nTargetSize = pRT->GetWidth() + pRT->GetHeight() + int(e_svoTI_SkyColorMultiplier > 0) + e_svoTI_Diffuse_Cache + e_svoTI_ShadowsFromSun;
		bool bNoReprojection = (rp.nPrevTargetSize != nTargetSize) || (pActivePipeline->GetRenderFlags() & SHDF_CUBEMAPGEN) || (rd->GetActiveGPUCount() > 1);
		rp.nPrevTargetSize = nTargetSize;

		static CCryNameR paramName("SVO_TargetResScale");
		float fSizeRatioW = float(renderWidth) / pRT->GetWidth();
		float fSizeRatioH = float(renderHeight) / pRT->GetHeight();
		Vec4 vData(fSizeRatioW, fSizeRatioH, e_svoTI_TemporalFilteringBase, (float)bNoReprojection);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		static CCryNameR paramName("SVO_helperInfo");
		rp.SetConstantArray(paramName, (Vec4*)&m_texInfo.helperInfo, 1);
	}

	{
		static CCryNameR paramName0("SVO_FrustumVerticesCam0");
		static CCryNameR paramName1("SVO_FrustumVerticesCam1");
		static CCryNameR paramName2("SVO_FrustumVerticesCam2");
		static CCryNameR paramName3("SVO_FrustumVerticesCam3");
		Vec3 pvViewFrust[8];
		pRenderView->GetCamera(CCamera::eEye_Left).CalcAsymmetricFrustumVertices(pvViewFrust);
		Vec3 vOrigin = pRenderView->GetCamera(CCamera::eEye_Left).GetPosition();
		Vec4 vData0(pvViewFrust[4] - vOrigin, 0);
		Vec4 vData1(pvViewFrust[5] - vOrigin, 0);
		Vec4 vData2(pvViewFrust[6] - vOrigin, 0);
		Vec4 vData3(pvViewFrust[7] - vOrigin, 0);
		rp.SetConstantArray(paramName0, (Vec4*)&vData0, 1);
		rp.SetConstantArray(paramName1, (Vec4*)&vData1, 1);
		rp.SetConstantArray(paramName2, (Vec4*)&vData2, 1);
		rp.SetConstantArray(paramName3, (Vec4*)&vData3, 1);
	}

	if ((e_svoTI_AnalyticalGI || e_svoTI_AnalyticalOccluders) && m_texInfo.arrAnalyticalOccluders[0][0].radius)
	{
		static CCryNameR paramName("SVO_AnalyticalOccluders");
		rp.SetConstantArray(paramName, (Vec4*)&m_texInfo.arrAnalyticalOccluders[0][0], sizeof(m_texInfo.arrAnalyticalOccluders[0]) / sizeof(Vec4));
	}

	if (e_svoTI_AnalyticalOccluders && m_texInfo.arrAnalyticalOccluders[1][0].radius)
	{
		static CCryNameR paramName("SVO_PostOccluders");
		rp.SetConstantArray(paramName, (Vec4*)&m_texInfo.arrAnalyticalOccluders[1][0], sizeof(m_texInfo.arrAnalyticalOccluders[1]) / sizeof(Vec4));
	}

	if (m_texInfo.arrPortalsPos[0].z)
	{
		static CCryNameR paramName_PortalsPos("SVO_PortalsPos");
		rp.SetConstantArray(paramName_PortalsPos, (Vec4*)&m_texInfo.arrPortalsPos[0], SVO_MAX_PORTALS);
		static CCryNameR paramName_PortalsDir("SVO_PortalsDir");
		rp.SetConstantArray(paramName_PortalsDir, (Vec4*)&m_texInfo.arrPortalsDir[0], SVO_MAX_PORTALS);
	}

	{
		static CCryNameR paramName("SVO_CloudShadowAnimParams");
		SRenderViewShaderConstants& PF = pRenderView->GetShaderConstants();
		Vec4 vData;
		vData[0] = PF.pCloudShadowAnimParams.x;
		vData[1] = PF.pCloudShadowAnimParams.y;
		vData[2] = PF.pCloudShadowAnimParams.z;
		vData[3] = PF.pCloudShadowAnimParams.w;
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		static CCryNameR paramName("SVO_CloudShadowParams");
		SRenderViewShaderConstants& PF = pRenderView->GetShaderConstants();
		Vec4 vData;
		vData[0] = PF.pCloudShadowParams.x;
		vData[1] = PF.pCloudShadowParams.y;
		vData[2] = PF.pCloudShadowParams.z;
		vData[3] = PF.pCloudShadowParams.w;
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	if (pTS)
	{
		static CCryNameR paramName("SVO_SrcPixSize");
		Vec4 vData(0, 0, 0, 0);
		vData.x = 1.f / float(pTS->pRT_ALD_DEM_MIN_0->GetWidth());
		vData.y = 1.f / float(pTS->pRT_ALD_DEM_MIN_0->GetHeight());
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	{
		auto screenResolution = Vec2i(CRendererResources::s_renderWidth, CRendererResources::s_renderHeight);
		static CCryNameR paramName("SVO_DepthTargetRes");
		Vec4 vData((float)screenResolution.x, (float)screenResolution.y, 0, 0);
		rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
	}

	if (e_svoTI_RT_Active)
	{
		// Mesh ray tracing pool dimensions, the single source of truth for every address the
		// shader computes (rt decision 02 section 2.1). The 3DEngine reports what it actually
		// allocated; the cvars are only the fallback before the first allocation.
		{
			static CCryNameR paramName("SVO_RTPoolInfo");
			Vec4 vData(
			  (float)(m_texInfo.rtPoolXY   ? m_texInfo.rtPoolXY   : e_svoTI_RT_TriPoolXY),
			  (float)(m_texInfo.rtPoolZ    ? m_texInfo.rtPoolZ    : e_svoTI_RT_TriPoolZ),
			  (float)(m_texInfo.rtTexRes   ? m_texInfo.rtTexRes   : e_svoTI_RT_MaxTexRes),
			  (float)(m_texInfo.rtTexPoolZ ? m_texInfo.rtTexPoolZ : e_svoTI_RT_TexPoolZ));
			rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
		}

		{
			// (maxBounces, glossScale, mixWithProbes, debugMode). glossScale and mixWithProbes
			// get their own cvars in stage 4; .w carries e_svoTI_RT_Debug (PLAN stage 1A item 3).
			static CCryNameR paramName("SVO_RTParams0");
			Vec4 vData((float)e_svoTI_RT_MaxBounces, 1.f, 0.f, (float)e_svoTI_RT_Debug);
			rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
		}

		{
			// rt stage 2C: (normalsFading, reserved x3). Neo drove the same quantity off
			// CV_TerrainInfo.y, which in CE 5.7.1 is a hard zero (GraphicsPipeline.cpp:1280
			// fills CV_TerrainInfo as Vec4(GetTerrainTextureMultiplier(), 0, 0, 0)), so the
			// transplanted line silently flattened every ray traced normal map.
			static CCryNameR paramName("SVO_RTParams1");
			Vec4 vData(e_svoTI_RT_NormalsFading, 0.f, 0.f, 0.f);
			rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
		}

		{
			// rt stage 4B (decision 08): (glossyMode, glossScale, temporalFrames, noiseBound).
			// Read by ConeTracePS (the sampler), DemosaicPS (the history length) and UpScalePS
			// (the filter radius and the cross-fade band), so it has to go on the common path.
			static CCryNameR paramName("SVO_RTParams2");
			Vec4 vData((float)e_svoTI_RT_GlossyMode, e_svoTI_RT_GlossScale,
			           (float)max(e_svoTI_RT_TemporalFrames, 1), IsRtBlueNoiseReady() ? 1.f : 0.f);
			rp.SetConstantArray(paramName, (Vec4*)&vData, 1);
		}
	}
}

// rt stage 4B: true only when the loose blue noise .dds really came in. The shader's fallback
// hash is a correct sampler, just a noisier one, so a missing asset degrades instead of failing.
bool CSvoRenderer::IsRtBlueNoiseReady() const
{
	return m_pTexRTBlueNoise && m_pTexRTBlueNoise->IsLoaded() && m_pTexRTBlueNoise->GetDevTexture();
}

template<class T>
void CSvoRenderer::SetupCommonSamplers(T& rp)
{
	rp.SetSampler(0, EDefaultSamplerStates::PointClamp);
	rp.SetSampler(1, EDefaultSamplerStates::LinearClamp);
	rp.SetSampler(2, EDefaultSamplerStates::LinearWrap);
}

void CSvoRenderer::DrawPonts(PodArray<SVF_P3F_C4B_T2F>& arrVerts)
{
	std::shared_ptr<CGraphicsPipeline> pActivePipeline = m_pRenderView->GetGraphicsPipeline();
	SPostEffectsUtils::UpdateFrustumCorners(*pActivePipeline);

	CVertexBuffer strip(arrVerts.GetElements(), EDefaultInputLayouts::P3F_C4B_T2F);

	// OLD PIPELINE
	ASSERT_LEGACY_PIPELINE
	//gRenDev->DrawPrimitivesInternal(&strip, arrVerts.Count() / max(1, e_svoRender), eptPointList);
}

void CSvoRenderer::UpdateRender(CRenderView* pRenderView)
{
	FUNCTION_PROFILER_RENDERER();

	m_pRenderView = pRenderView;
	int nPrevAsync = CRenderer::CV_r_shadersasynccompiling;
	if (gEnv->IsEditor())
		CRenderer::CV_r_shadersasynccompiling = 0;

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (e_svoEnabled && e_svoTI_Active && e_svoTI_Apply && m_texInfo.bSvoReady && e_svoTI_Troposphere_Active && m_pRT_AIR_MIN)
	{
		PROFILE_LABEL_SCOPE("TI_GEN_AIR");

		if (e_svoTI_DiffuseAmplifier || e_svoTI_SpecularAmplifier || !e_svoTI_DiffuseBias)
			TropospherePass();
	}

	if (e_svoEnabled && e_svoTI_Active && e_svoTI_Apply && m_texInfo.bSvoReady && e_svoTI_ShadowsFromSun && m_pRT_SHAD_MIN_MAX)
	{
		PROFILE_LABEL_SCOPE("TI_GEN_SHAD");

		TraceSunShadowsPass();
	}
	#endif

	{
		PROFILE_LABEL_SCOPE("TI_GEN_DIFF");

		ConeTracePass(&m_pPasses->m_tsDiff);
	}
	if (GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier)
	{
		PROFILE_LABEL_SCOPE("TI_GEN_SPEC");

		ConeTracePass(&m_pPasses->m_tsSpec);
	}

	// rt stage 2 (decision 06): light grid, then hit shading, between the specular cone trace
	// and its de-mosaic. Nothing downstream of ShadePass changes.
	if (IsRtHitShadingActive())
	{
		{
			PROFILE_LABEL_SCOPE("TI_LIGHTGRID");

			BuildRTLightGridPass();
		}

		{
			PROFILE_LABEL_SCOPE("TI_SHADE_SPEC");

			ShadePass(&m_pPasses->m_tsSpec);
		}
	}

	{
		PROFILE_LABEL_SCOPE("TI_DEMOSAIC_DIFF");

		DemosaicPass(&m_pPasses->m_tsDiff);
	}
	if (GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier)
	{
		PROFILE_LABEL_SCOPE("TI_DEMOSAIC_SPEC");

		DemosaicPass(&m_pPasses->m_tsSpec);
	}

	{
		PROFILE_LABEL_SCOPE("TI_UPSCALE_DIFF");

		UpscalePass(&m_pPasses->m_tsDiff);
	}
	if (GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier && !e_svoTI_SpecularFromDiffuse)
	{
		PROFILE_LABEL_SCOPE("TI_UPSCALE_SPEC");

		UpscalePass(&m_pPasses->m_tsSpec);
	}

	const SRenderViewInfo& viewInfo = pRenderView->GetViewInfo(CCamera::eEye_Left);

	{
		m_matViewProjPrev = viewInfo.cameraProjMatrix;
		m_matViewProjPrev.Transpose();
	}

	if (gEnv->IsEditor())
		CRenderer::CV_r_shadersasynccompiling = nPrevAsync;

	m_pRenderView = nullptr;
}

void CSvoRenderer::DemosaicPass(SSvoTargetsSet* pTS)
{
	CSvoFullscreenPass& rp = pTS->passDemosaic;

	const char* szTechFinalName = "DemosaicPass";

	if (!e_svoTI_Active || !e_svoTI_Apply || !e_svoRender || !m_pShader || !pTS->pRT_ALD_0 || m_texInfo.bSvoFreeze || !m_texInfo.pTexTree)
		return;

	rp.SetTechnique(m_pShader, szTechFinalName, GetRunTimeFlags(pTS == &m_pPasses->m_tsDiff));
	rp.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
	rp.SetState(GS_NODEPTHTEST);

	rp.SetRenderTarget(0, pTS->pRT_RGB_DEM_MIN_0);
	rp.SetRenderTarget(1, pTS->pRT_ALD_DEM_MIN_0);
	rp.SetRenderTarget(2, pTS->pRT_RGB_DEM_MAX_0);
	rp.SetRenderTarget(3, pTS->pRT_ALD_DEM_MAX_0);
	rp.SetRequireWorldPos(true);
	rp.SetRequirePerViewConstantBuffer(true);

	SetupCommonSamplers(rp);

	SetupSvoTexturesForRead(m_texInfo, rp, (e_svoTI_Active ? e_svoTI_NumberOfBounces : 0), 0, 0);

	SetupGBufferTextures(rp);

	// rt stage 2: when the hit-shading pass ran, the shaded pair is the ALD + RGB contract this
	// pass consumes; the tracing pair still holds raw g-data for the RT pixels.
	rp.SetTexture(10, pTS->bShaded ? pTS->pRT_ALD_SHD.get() : pTS->pRT_ALD_0.get());
	rp.SetTexture(11, pTS->bShaded ? pTS->pRT_RGB_SHD.get() : pTS->pRT_RGB_0.get());

	rp.SetTexture(6, pTS->pRT_RGB_DEM_MIN_1);
	rp.SetTexture(9, pTS->pRT_ALD_DEM_MIN_1);
	rp.SetTexture(12, pTS->pRT_RGB_DEM_MAX_1);
	rp.SetTexture(13, pTS->pRT_ALD_DEM_MAX_1);

	// rt stage 4B (decision 08 item 2): the hit distance, for the virtual-image reprojection.
	// It is the one g-data lane the shaded ALD + RGB pair does not carry.
	if (e_svoTI_RT_Active && pTS->pRT_RAYDIR_0)
		rp.SetTexture(38, pTS->pRT_RAYDIR_0);
	else
		rp.SetTexture(38, CRendererResources::s_ptexBlack);

	rp.SetTexture(8, GetUtils().GetVelocityObjectRT(RenderView()));

	rp.BeginConstantUpdate();

	SetupCommonConstants(pTS, rp, pTS->pRT_ALD_0);

	rp.Execute();
}

template<class T>
void CSvoRenderer::SetupLightSources(PodArray<I3DEngine::SLightTI>& lightsTI, T& rp)
{
	const int nLightGroupsNum = 2;

	static CCryNameR paramNamesLightPos[nLightGroupsNum] =
	{
		CCryNameR("SVO_LightPos0"),
		CCryNameR("SVO_LightPos1"),
	};
	static CCryNameR paramNamesLightDir[nLightGroupsNum] =
	{
		CCryNameR("SVO_LightDir0"),
		CCryNameR("SVO_LightDir1"),
	};
	static CCryNameR paramNamesLightCol[nLightGroupsNum] =
	{
		CCryNameR("SVO_LightCol0"),
		CCryNameR("SVO_LightCol1"),
	};

	for (int g = 0; g < nLightGroupsNum; g++)
	{
		Vec4 LightPos[4];
		Vec4 LightDir[4];
		Vec4 LightCol[4];

		for (int x = 0; x < 4; x++)
		{
			int nId = g * 4 + x;

			LightPos[x] = (nId < lightsTI.Count()) ? lightsTI[nId].vPosR : Vec4(0, 0, 0, 0);
			LightDir[x] = (nId < lightsTI.Count()) ? lightsTI[nId].vDirF : Vec4(0, 0, 0, 0);
			LightCol[x] = (nId < lightsTI.Count()) ? lightsTI[nId].vCol : Vec4(0, 0, 0, 0);
		}

		rp.SetConstantArray(paramNamesLightPos[g], alias_cast<Vec4*>(&LightPos[0][0]), 4);
		rp.SetConstantArray(paramNamesLightDir[g], alias_cast<Vec4*>(&LightDir[0][0]), 4);
		rp.SetConstantArray(paramNamesLightCol[g], alias_cast<Vec4*>(&LightCol[0][0]), 4);
	}
}

void CSvoRenderer::SetupNodesForUpdate(int& nNodesForUpdateStartIndex, PodArray<I3DEngine::SSvoNodeInfo>& arrNodesForUpdate, CSvoComputePass& rp)
{
	static CCryNameR paramNames[SVO_MAX_NODE_GROUPS] =
	{
		CCryNameR("SVO_NodesForUpdate0"),
		CCryNameR("SVO_NodesForUpdate1"),
		CCryNameR("SVO_NodesForUpdate2"),
		CCryNameR("SVO_NodesForUpdate3"),
	};

	for (int g = 0; g < SVO_MAX_NODE_GROUPS; g++)
	{
		Vec4 matVal[4];

		for (int x = 0; x < 4; x++)
		{
			for (int y = 0; y < 4; y++)
			{
				int nId = nNodesForUpdateStartIndex + g * 16 + x * 4 + y;
				matVal[x][y] = 0.1f + ((nId < arrNodesForUpdate.Count()) ? arrNodesForUpdate[nId].nAtlasOffset : -2);

				if (nId < arrNodesForUpdate.Count())
				{
					float fNodeSize = arrNodesForUpdate[nId].wsBox.GetSize().x;

					if (e_svoDebug == 8)
						gRenDev->GetIRenderAuxGeom()->DrawAABB(arrNodesForUpdate[nId].wsBox, 0, ColorF(fNodeSize == 4.f, fNodeSize == 8.f, fNodeSize == 16.f), eBBD_Faceted);
				}
			}
		}

		rp.SetConstantArray(paramNames[g], &matVal[0], 4);
	}

	nNodesForUpdateStartIndex += 4 * 4 * SVO_MAX_NODE_GROUPS;
}

template<class T>
void CSvoRenderer::SetupSvoTexturesForRead(I3DEngine::SSvoStaticTexInfo& texInfo, T& rp, int nStage, int nStageOpa, int nStageNorm)
{
	rp.SetTexture(0, (static_cast<CTexture*>(texInfo.pTexTree.get())));

	rp.SetTexture(1, CRendererResources::s_ptexWhite3D);

	#ifdef FEATURE_SVO_GI_ALLOW_HQ

	if (nStage == 0)
		rp.SetTexture(1, vp_RGB0.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_RGB0.nTexId));
	else if (nStage == 1)
		rp.SetTexture(1, vp_RGB1.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_RGB1.nTexId));
	else if (nStage == 2)
		rp.SetTexture(1, vp_RGB2.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_RGB2.nTexId));
	else if (nStage == 3)
		rp.SetTexture(1, vp_RGB3.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_RGB3.nTexId));
	else if (nStage == 4)
		rp.SetTexture(1, vp_RGB4.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_RGB4.nTexId));

	if (nStageNorm == 0)
		rp.SetTexture(2, vp_NORM.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_NORM.nTexId));

	#endif


	if (nStageOpa == 0)
	{
		CTexture* pTexOpac = static_cast<CTexture*>(texInfo.pTexOpac.get());
		rp.SetTexture(3, !pTexOpac ? CRendererResources::s_ptexWhite3D : pTexOpac);
	}
	#ifdef FEATURE_SVO_GI_ALLOW_HQ

	else if (nStageOpa == 1)
		rp.SetTexture(3, vp_RGB4.nTexId == 0 ? CRendererResources::s_ptexWhite3D : CTexture::GetByID(vp_RGB4.nTexId));

	// The per-voxel "first triangle id + tris count" pool (brickPool_RTri, t17) died with
	// RayTraceMesh; nothing in the shader reads t17 any more.

	if (texInfo.pGlobalSpecCM)
		rp.SetTexture(6, static_cast<CTexture*>(texInfo.pGlobalSpecCM.get()));

	CTexture* pTexRgb0 = static_cast<CTexture*>(texInfo.pTexRgb0.get());
	if (CTexture::IsTextureExist(pTexRgb0))
		rp.SetTexture(25, pTexRgb0);

	#endif
}

void CSvoRenderer::CheckAllocateRT(bool bSpecPass)
{
	auto screenResolution = Vec2i(CRendererResources::s_renderWidth, CRendererResources::s_renderHeight);
	int nWidth = screenResolution.x;
	int nHeight = screenResolution.y;

	int nResScaleBase = max(e_svoTI_ResScaleBase + e_svoTI_LowSpecMode, 1);
	int resScaleSpec = max(e_svoTI_ResScaleSpecular + e_svoTI_LowSpecMode, 1);

	// rt stage 2C, finding (b): a triangle ray carries a final, fully shaded, per-pixel colour,
	// and every reconstruction filter downstream of it is now a single tap. Run that set below
	// the screen resolution and the reflection is a magnified point sample - visibly blockier
	// than the cone traced one it replaces, because the cone trace at least had a real spatial
	// filter to hide it. e_svoTI_ResScaleSpecular already defaults to 1; this pins it there so
	// e_svoTI_LowSpecMode (which ADDS to every scale) cannot quietly halve it underneath the RT
	// path. Cost at 1080p: the specular set is 10 fp16x4 targets, so 1 -> 2 is about 24 MB of
	// target memory and 4x the trace, shade and demosaic pixels.
	if (e_svoTI_RT_Active)
		resScaleSpec = 1;

	int nInW = (nWidth / nResScaleBase);
	int nInH = (nHeight / nResScaleBase);
#ifdef FEATURE_SVO_GI_ALLOW_HQ
	int nDownscaleAir = max(e_svoTI_ResScaleAir + e_svoTI_LowSpecMode, 1);
	int nAirW = (nWidth / nDownscaleAir);
	int nAirH = (nHeight / nDownscaleAir);
#endif

	int specW = (nWidth / resScaleSpec);
	int specH = (nHeight / resScaleSpec);

	SSvoTargetsSet& tsDiff = m_pPasses->m_tsDiff;
	SSvoTargetsSet& tsSpec = m_pPasses->m_tsSpec;

	if (!bSpecPass)
	{
		CheckCreateUpdateRT(tsDiff.pRT_ALD_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_ALD");
		CheckCreateUpdateRT(tsDiff.pRT_ALD_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_ALD");
		CheckCreateUpdateRT(tsDiff.pRT_RGB_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_RGB");
		CheckCreateUpdateRT(tsDiff.pRT_RGB_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_RGB");
	}
	else
	{
		CheckCreateUpdateRT(tsSpec.pRT_ALD_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_ALD");
		CheckCreateUpdateRT(tsSpec.pRT_ALD_1, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_ALD");
		CheckCreateUpdateRT(tsSpec.pRT_RGB_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RGB");
		CheckCreateUpdateRT(tsSpec.pRT_RGB_1, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_RGB");

		// rt stage 2 (decision 06 section 6.6). HITPOS is RGBA32F on purpose: fp16 quantises to
		// 1.5 cm at 16-32 m, which is shadow-acne territory once the position is fed to the
		// cascade walk (R01). RAYDIR keeps fp16 as Neo. The SHD pair exists because ShadePass
		// reads ALD_0 / RGB_0 as g-data and cannot write them at the same time.
		if (e_svoTI_RT_Active)
		{
			CheckCreateUpdateRT(tsSpec.pRT_HITPOS_0, specW, specH, eTF_R32G32B32A32F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RT_HITPOS");
			CheckCreateUpdateRT(tsSpec.pRT_RAYDIR_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RT_RAYDIR");
			// rt stage 4A: SVO diffuse irradiance (unexposed, TexGiDiffuse's units) + voxel AO.
			CheckCreateUpdateRT(tsSpec.pRT_HITGI_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RT_HITGI");
			// decision 11: the hit identity - (triangle record + 1, material record + 1,
			// u * 65535, v * 65535 * 16 + flags), four exact float integers. RGBA32F, not the
			// fp16 the HITMAT lane it replaces used: a record index is a pool address of up to
			// a few million and fp16 is exact only to 2048. ETEX_Format has no integer render
			// target (ITexture.h), so the channels hold VALUES rather than asuint bit patterns -
			// asfloat(5) is a denormal and the output merger may flush it to zero. 16 bytes per
			// specular pixel, and only while e_svoTI_RT_Active.
			CheckCreateUpdateRT(tsSpec.pRT_HITID_0, specW, specH, eTF_R32G32B32A32F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RT_HITID");
			CheckCreateUpdateRT(tsSpec.pRT_ALD_SHD, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RT_SHD_ALD");
			CheckCreateUpdateRT(tsSpec.pRT_RGB_SHD, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_RT_SHD_RGB");
		}
		else if (tsSpec.pRT_HITPOS_0)
		{
			tsSpec.pRT_HITPOS_0 = nullptr;
			tsSpec.pRT_RAYDIR_0 = nullptr;
			tsSpec.pRT_HITGI_0 = nullptr;
			tsSpec.pRT_HITID_0 = nullptr;
			tsSpec.pRT_ALD_SHD = nullptr;
			tsSpec.pRT_RGB_SHD = nullptr;
		}
	}

	if (!bSpecPass)
	{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
		if (e_svoTI_Troposphere_Active)
		{
			CheckCreateUpdateRT(m_pRT_NID_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_NID");
			CheckCreateUpdateRT(m_pRT_AIR_MIN, nAirW, nAirH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_AIR_MIN");
			CheckCreateUpdateRT(m_pRT_AIR_MAX, nAirW, nAirH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_AIR_MAX");
		}
		if (e_svoTI_ShadowsFromSun || e_svoTI_SpecularFromDiffuse)
		{
			CheckCreateUpdateRT(m_pRT_SHAD_MIN_MAX, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SHAD_MIN_MAX");
			CheckCreateUpdateRT(m_pRT_SHAD_FIN_0, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SHAD_FIN");
			CheckCreateUpdateRT(m_pRT_SHAD_FIN_1, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SHAD_FIN");
		}
	#endif

		CheckCreateUpdateRT(tsDiff.pRT_RGB_DEM_MIN_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_RGB_MIN");
		CheckCreateUpdateRT(tsDiff.pRT_ALD_DEM_MIN_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_ALD_MIN");
		CheckCreateUpdateRT(tsDiff.pRT_RGB_DEM_MAX_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_RGB_MAX");
		CheckCreateUpdateRT(tsDiff.pRT_ALD_DEM_MAX_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_ALD_MAX");
		CheckCreateUpdateRT(tsDiff.pRT_RGB_DEM_MIN_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_RGB_MIN");
		CheckCreateUpdateRT(tsDiff.pRT_ALD_DEM_MIN_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_ALD_MIN");
		CheckCreateUpdateRT(tsDiff.pRT_RGB_DEM_MAX_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_RGB_MAX");
		CheckCreateUpdateRT(tsDiff.pRT_ALD_DEM_MAX_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_ALD_MAX");

		CheckCreateUpdateRT(tsDiff.pRT_FIN_OUT_0, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_FIN_DIFF_OUT");
		CheckCreateUpdateRT(tsDiff.pRT_FIN_OUT_1, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_FIN_DIFF_OUT");
	}
	else
	{
		CheckCreateUpdateRT(tsSpec.pRT_RGB_DEM_MIN_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_RGB_MIN");
		CheckCreateUpdateRT(tsSpec.pRT_ALD_DEM_MIN_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_ALD_MIN");
		CheckCreateUpdateRT(tsSpec.pRT_RGB_DEM_MAX_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_RGB_MAX");
		CheckCreateUpdateRT(tsSpec.pRT_ALD_DEM_MAX_0, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_ALD_MAX");
		CheckCreateUpdateRT(tsSpec.pRT_RGB_DEM_MIN_1, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_RGB_MIN");
		CheckCreateUpdateRT(tsSpec.pRT_ALD_DEM_MIN_1, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_ALD_MIN");
		CheckCreateUpdateRT(tsSpec.pRT_RGB_DEM_MAX_1, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_RGB_MAX");
		CheckCreateUpdateRT(tsSpec.pRT_ALD_DEM_MAX_1, specW, specH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_ALD_MAX");

		CheckCreateUpdateRT(tsSpec.pRT_FIN_OUT_0, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_FIN_SPEC_OUT");
		CheckCreateUpdateRT(tsSpec.pRT_FIN_OUT_1, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_FIN_SPEC_OUT");
	}

	if (!bSpecPass)
	{
		// swap ping-pong RT
		std::swap(tsDiff.pRT_ALD_0, tsDiff.pRT_ALD_1);
		std::swap(tsDiff.pRT_RGB_0, tsDiff.pRT_RGB_1);
		std::swap(tsDiff.pRT_RGB_DEM_MIN_0, tsDiff.pRT_RGB_DEM_MIN_1);
		std::swap(tsDiff.pRT_ALD_DEM_MIN_0, tsDiff.pRT_ALD_DEM_MIN_1);
		std::swap(tsDiff.pRT_RGB_DEM_MAX_0, tsDiff.pRT_RGB_DEM_MAX_1);
		std::swap(tsDiff.pRT_ALD_DEM_MAX_0, tsDiff.pRT_ALD_DEM_MAX_1);
		std::swap(tsDiff.pRT_FIN_OUT_0, tsDiff.pRT_FIN_OUT_1);
	}
	else
	{
		std::swap(tsSpec.pRT_ALD_0, tsSpec.pRT_ALD_1);
		std::swap(tsSpec.pRT_RGB_0, tsSpec.pRT_RGB_1);
		std::swap(tsSpec.pRT_RGB_DEM_MIN_0, tsSpec.pRT_RGB_DEM_MIN_1);
		std::swap(tsSpec.pRT_ALD_DEM_MIN_0, tsSpec.pRT_ALD_DEM_MIN_1);
		std::swap(tsSpec.pRT_RGB_DEM_MAX_0, tsSpec.pRT_RGB_DEM_MAX_1);
		std::swap(tsSpec.pRT_ALD_DEM_MAX_0, tsSpec.pRT_ALD_DEM_MAX_1);
		std::swap(tsSpec.pRT_FIN_OUT_0, tsSpec.pRT_FIN_OUT_1);
	}

	if (!bSpecPass)
	{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
		std::swap(m_pRT_SHAD_FIN_0, m_pRT_SHAD_FIN_1);
	#endif
	}
}

size_t CSvoRenderer::GetAllocatedMemory()
{
	size_t sizeSum = 0;

	SSvoTargetsSet& tsDiff = m_pPasses->m_tsDiff;
	SSvoTargetsSet& tsSpec = m_pPasses->m_tsSpec;

	if (tsDiff.pRT_ALD_0.get()) sizeSum += tsDiff.pRT_ALD_0->GetActualSize();
	if (tsDiff.pRT_ALD_1.get()) sizeSum += tsDiff.pRT_ALD_1->GetActualSize();
	if (tsDiff.pRT_RGB_0.get()) sizeSum += tsDiff.pRT_RGB_0->GetActualSize();
	if (tsDiff.pRT_RGB_1.get()) sizeSum += tsDiff.pRT_RGB_1->GetActualSize();

	if (tsSpec.pRT_ALD_0.get()) sizeSum += tsSpec.pRT_ALD_0->GetActualSize();
	if (tsSpec.pRT_ALD_1.get()) sizeSum += tsSpec.pRT_ALD_1->GetActualSize();
	if (tsSpec.pRT_RGB_0.get()) sizeSum += tsSpec.pRT_RGB_0->GetActualSize();
	if (tsSpec.pRT_RGB_1.get()) sizeSum += tsSpec.pRT_RGB_1->GetActualSize();

#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (m_pRT_NID_0  .get()) sizeSum += m_pRT_NID_0  ->GetActualSize();
	if (m_pRT_AIR_MIN.get()) sizeSum += m_pRT_AIR_MIN->GetActualSize();
	if (m_pRT_AIR_MAX.get()) sizeSum += m_pRT_AIR_MAX->GetActualSize();

	if (m_pRT_SHAD_MIN_MAX.get()) sizeSum += m_pRT_SHAD_MIN_MAX->GetActualSize();
	if (m_pRT_SHAD_FIN_0  .get()) sizeSum += m_pRT_SHAD_FIN_0  ->GetActualSize();
	if (m_pRT_SHAD_FIN_1  .get()) sizeSum += m_pRT_SHAD_FIN_1  ->GetActualSize();
#endif

	if (tsDiff.pRT_RGB_DEM_MIN_0.get()) sizeSum += tsDiff.pRT_RGB_DEM_MIN_0->GetActualSize();
	if (tsDiff.pRT_ALD_DEM_MIN_0.get()) sizeSum += tsDiff.pRT_ALD_DEM_MIN_0->GetActualSize();
	if (tsDiff.pRT_RGB_DEM_MAX_0.get()) sizeSum += tsDiff.pRT_RGB_DEM_MAX_0->GetActualSize();
	if (tsDiff.pRT_ALD_DEM_MAX_0.get()) sizeSum += tsDiff.pRT_ALD_DEM_MAX_0->GetActualSize();
	if (tsDiff.pRT_RGB_DEM_MIN_1.get()) sizeSum += tsDiff.pRT_RGB_DEM_MIN_1->GetActualSize();
	if (tsDiff.pRT_ALD_DEM_MIN_1.get()) sizeSum += tsDiff.pRT_ALD_DEM_MIN_1->GetActualSize();
	if (tsDiff.pRT_RGB_DEM_MAX_1.get()) sizeSum += tsDiff.pRT_RGB_DEM_MAX_1->GetActualSize();
	if (tsDiff.pRT_ALD_DEM_MAX_1.get()) sizeSum += tsDiff.pRT_ALD_DEM_MAX_1->GetActualSize();

	if (tsDiff.pRT_FIN_OUT_0.get()) sizeSum += tsDiff.pRT_FIN_OUT_0->GetActualSize();
	if (tsDiff.pRT_FIN_OUT_1.get()) sizeSum += tsDiff.pRT_FIN_OUT_1->GetActualSize();

	if (tsSpec.pRT_RGB_DEM_MIN_0.get()) sizeSum += tsSpec.pRT_RGB_DEM_MIN_0->GetActualSize();
	if (tsSpec.pRT_ALD_DEM_MIN_0.get()) sizeSum += tsSpec.pRT_ALD_DEM_MIN_0->GetActualSize();
	if (tsSpec.pRT_RGB_DEM_MAX_0.get()) sizeSum += tsSpec.pRT_RGB_DEM_MAX_0->GetActualSize();
	if (tsSpec.pRT_ALD_DEM_MAX_0.get()) sizeSum += tsSpec.pRT_ALD_DEM_MAX_0->GetActualSize();
	if (tsSpec.pRT_RGB_DEM_MIN_1.get()) sizeSum += tsSpec.pRT_RGB_DEM_MIN_1->GetActualSize();
	if (tsSpec.pRT_ALD_DEM_MIN_1.get()) sizeSum += tsSpec.pRT_ALD_DEM_MIN_1->GetActualSize();
	if (tsSpec.pRT_RGB_DEM_MAX_1.get()) sizeSum += tsSpec.pRT_RGB_DEM_MAX_1->GetActualSize();
	if (tsSpec.pRT_ALD_DEM_MAX_1.get()) sizeSum += tsSpec.pRT_ALD_DEM_MAX_1->GetActualSize();

	if (tsSpec.pRT_FIN_OUT_0.get()) sizeSum += tsSpec.pRT_FIN_OUT_0->GetActualSize();
	if (tsSpec.pRT_FIN_OUT_1.get()) sizeSum += tsSpec.pRT_FIN_OUT_1->GetActualSize();

	return sizeSum;
}

bool CSvoRenderer::IsShaderItemUsedForVoxelization(SShaderItem& rShaderItem, IRenderNode* pRN)
{
	CShader* pS = (CShader*)rShaderItem.m_pShader;

	// skip some objects marked by level designer
	//	if(pRN && pRN->IsRenderNode() && pRN->GetIntegrationType())
	//	return false;

	// skip transparent meshes except decals
	//	CShaderResources* pR = (CShaderResources*)rShaderItem.m_pShaderResources;
	//	if((pR->Opacity() != 1.f) && !(pS->GetFlags()&EF_DECAL))
	//	return false;

	// skip windows
	if (pS && pS->GetShaderType() == eST_Glass)
		return false;

	// skip water
	if (pS && pS->GetShaderType() == eST_Water)
		return false;

	// skip outdoor vegetations
	//	(fog not working) if(pS->GetShaderType() == eST_Vegetation && pRN->IsRenderNode() && !pRN->GetEntityVisArea())
	//	return false;

	return true;
}

inline Vec3 SVO_StringToVector(const char* str)
{
	Vec3 vTemp(0, 0, 0);
	float x, y, z;
	if (sscanf(str, "%f,%f,%f", &x, &y, &z) == 3)
	{
		vTemp(x, y, z);
	}
	else
	{
		vTemp(0, 0, 0);
	}
	return vTemp;
}

void CSvoRenderer::FillForwardParams(SForwardParams& svogiParams, bool enable) const
{
	if (enable)
	{
		svogiParams.IntegrationMode.x = 1.f - e_svoTI_HighGlossOcclusion;

		float fModeFin = 0;
		int nModeGI = GetIntegratioMode();

		if (nModeGI == 0 && GetUseLightProbes())
		{
			// AO modulates diffuse and specular
			fModeFin = 0;
		}
		else if (nModeGI <= 1)
		{
			// GI replaces diffuse and modulates specular
			fModeFin = 1.f;
		}
		else if (nModeGI == 2)
		{
			// GI replaces diffuse and specular
			fModeFin = 2.f;
		}

		svogiParams.IntegrationMode.y = fModeFin;
		svogiParams.IntegrationMode.z = e_svoDVR ? (float)e_svoDVR : ((m_texInfo.bSvoReady && e_svoTI_NumberOfBounces) ? e_svoTI_SpecularAmplifier : 0);
		svogiParams.IntegrationMode.w = e_svoTI_SkyColorMultiplier;
	}
	else
	{
		// turning off by parameters.
		svogiParams.IntegrationMode = Vec4(0.0f, -1.0f, 0.0f, 0.0f);
	}
}

bool CSvoRenderer::SetShaderParameters(float*& pSrc, uint32 paramType, UFloat4* sData)
{
	bool bRes = true;

	CSvoRenderer* pSR = CSvoRenderer::GetInstance();

	if (!pSR)
	{
		if (paramType == ECGP_PB_SvoParams4)
		{
			// SvoParams4 is used by forward tiled shaders even if GI is disabled, it contains info about GI mode and also is GI active or not
			sData[0].f[0] = 0;
			sData[0].f[1] = -1.f;
			sData[0].f[2] = 0;
			sData[0].f[3] = 0;
			return true;
		}

		return false;
	}

	switch (paramType)
	{
	case ECGP_PB_SvoViewProj0:
	case ECGP_PB_SvoViewProj1:
	case ECGP_PB_SvoViewProj2:
		{
			pSrc = pSR->m_mGpuVoxViewProj[((paramType) - ECGP_PB_SvoViewProj0)].GetData();
			break;
		}

	case ECGP_PB_SvoNodesForUpdate0:
	case ECGP_PB_SvoNodesForUpdate1:
	case ECGP_PB_SvoNodesForUpdate2:
	case ECGP_PB_SvoNodesForUpdate3:
	case ECGP_PB_SvoNodesForUpdate4:
	case ECGP_PB_SvoNodesForUpdate5:
	case ECGP_PB_SvoNodesForUpdate6:
	case ECGP_PB_SvoNodesForUpdate7:
		{
			pSrc = &(pSR->m_arrNodesForUpdate[((paramType) - ECGP_PB_SvoNodesForUpdate0)][0][0]);
			break;
		}

	case ECGP_PB_SvoNodeBoxWS:
		{
			sData[0].f[0] = pSR->m_wsOffset.x;
			sData[0].f[1] = pSR->m_wsOffset.y;
			sData[0].f[2] = pSR->m_wsOffset.z;
			sData[0].f[3] = pSR->m_wsOffset.w;
			break;
		}

	case ECGP_PB_SvoNodeBoxTS:
		{
			sData[0].f[0] = pSR->m_tcOffset.x;
			sData[0].f[1] = pSR->m_tcOffset.y;
			sData[0].f[2] = pSR->m_tcOffset.z;
			sData[0].f[3] = pSR->m_tcOffset.w;
			break;
		}

	case ECGP_PB_SvoTreeSettings0:
		{
			sData[0].f[0] = (float)gEnv->p3DEngine->GetTerrainSize();
			sData[0].f[1] = pSR->e_svoMinNodeSize;
			sData[0].f[2] = (float)pSR->m_texInfo.nBrickSize;
			sData[0].f[3] = (float)pSR->e_svoVoxelPoolResolution;
			break;
		}

	case ECGP_PB_SvoTreeSettings1:
		{
			sData[0].f[0] = pSR->e_svoMaxNodeSize;
			sData[0].f[1] = 0;
			sData[0].f[2] = pSR->e_svoTI_Troposphere_CloudGen_FreqStep;
			sData[0].f[3] = pSR->e_svoTI_Troposphere_Density;
			break;
		}

	case ECGP_PB_SvoTreeSettings2:
		{
			sData[0].f[0] = pSR->e_svoTI_Troposphere_Ground_Height;
			sData[0].f[1] = pSR->e_svoTI_Troposphere_Layer0_Height;
			sData[0].f[2] = pSR->e_svoTI_Troposphere_Layer1_Height;
			sData[0].f[3] = pSR->e_svoTI_Troposphere_CloudGen_Height;
			break;
		}

	case ECGP_PB_SvoTreeSettings3:
		{
			sData[0].f[0] = pSR->e_svoTI_Troposphere_CloudGen_Freq;
			sData[0].f[1] = pSR->e_svoTI_Troposphere_CloudGen_Scale;
			sData[0].f[2] = pSR->e_svoTI_Troposphere_CloudGenTurb_Freq;
			sData[0].f[3] = pSR->e_svoTI_Troposphere_CloudGenTurb_Scale;
			break;
		}

	case ECGP_PB_SvoTreeSettings4:
		{
			sData[0].f[0] = pSR->e_svoTI_Troposphere_Layer0_Rand;
			sData[0].f[1] = pSR->e_svoTI_Troposphere_Layer1_Rand;
			sData[0].f[2] = pSR->e_svoTI_Troposphere_Layer0_Dens;
			sData[0].f[3] = pSR->e_svoTI_Troposphere_Layer1_Dens;
			break;
		}

	case ECGP_PB_SvoTreeSettings5:
		{
			sData[0].f[0] = pSR->e_svoTI_PortalsInject;
			sData[0].f[1] = pSR->e_svoTI_PortalsDeform;
			sData[0].f[2] = pSR->m_texInfo.vSkyColorTop.x;
			sData[0].f[3] = pSR->m_texInfo.vSkyColorTop.y;
			break;
		}

	case ECGP_PB_SvoParams0:
		{
			sData[0].f[0] = 1.f / max(pSR->e_svoTI_ShadowsSoftness, 0.01f);
			sData[0].f[1] = pSR->e_svoTI_Diffuse_Spr;
			sData[0].f[2] = pSR->e_svoTI_DiffuseBias;

			float fDepth = 0;
			float nWS = pSR->m_texInfo.vSvoOriginAndSize.w;
			while (nWS > pSR->e_svoMinNodeSize)
			{
				nWS /= 2;
				fDepth++;
			}
			sData[0].f[3] = 0.1f + fDepth;

			break;
		}

	case ECGP_PB_SvoParams1:
		{
			// diffuse
			sData[0].f[0] = 1.f / (pSR->e_svoTI_DiffuseConeWidth + 0.00001f);
			sData[0].f[1] = pSR->e_svoTI_ConeMaxLength;
			sData[0].f[2] = 1.f / max(pSR->e_svoTI_SSDepthTrace, 0.01f);
			sData[0].f[3] = (pSR->m_texInfo.bSvoReady && pSR->e_svoTI_NumberOfBounces) ? pSR->e_svoTI_DiffuseAmplifier : 0;
			break;
		}

	case ECGP_PB_SvoParams2:
		{
			// inject
			sData[0].f[0] = pSR->e_svoTI_InjectionMultiplier;
			sData[0].f[1] = 1.f / max(pSR->e_svoTI_PropagationBooster, 0.001f);
			sData[0].f[2] = pSR->e_svoTI_Saturation;
			sData[0].f[3] = pSR->e_svoTI_TranslucentBrightness;
			break;
		}

	case ECGP_PB_SvoParams3:
		{
			// specular params
			sData[0].f[0] = pSR->e_svoTI_Specular_Sev;
			sData[0].f[1] = pSR->m_texInfo.vSkyColorTop.z;
			sData[0].f[2] = pSR->e_svoTI_Troposphere_Brightness * pSR->e_svoTI_Troposphere_Active;
			sData[0].f[3] = (pSR->m_texInfo.bSvoReady && pSR->e_svoTI_NumberOfBounces) ? pSR->e_svoTI_SpecularAmplifier : 0;
			break;
		}

	case ECGP_PB_SvoParams4:
		{
			// LOD & sky color
			sData[0].f[0] = 1.f - pSR->e_svoTI_HighGlossOcclusion;

			float fModeFin = 0;
			int nModeGI = CSvoRenderer::GetInstance()->GetIntegratioMode();

			if (nModeGI == 0 && CSvoRenderer::GetInstance()->GetUseLightProbes())
			{
				// AO modulates diffuse and specular
				fModeFin = 0;
			}
			else if (nModeGI <= 1)
			{
				// GI replaces diffuse and modulates specular
				fModeFin = 1.f;
			}
			else if (nModeGI == 2)
			{
				// GI replaces diffuse and specular
				fModeFin = 2.f;
			}

			sData[0].f[1] = pSR->IsActive() ? fModeFin : -1.f;
			sData[0].f[2] = pSR->e_svoDVR ? (float)pSR->e_svoDVR : ((pSR->m_texInfo.bSvoReady && pSR->e_svoTI_NumberOfBounces) ? pSR->e_svoTI_SpecularAmplifier : 0);
			sData[0].f[3] = pSR->e_svoTI_SkyColorMultiplier;
			break;
		}

	case ECGP_PB_SvoParams5:
		{
			// sky color
			sData[0].f[0] = pSR->e_svoTI_EmissiveMultiplier;
			sData[0].f[1] = (float)pSR->m_nCurPropagationPassID;
			sData[0].f[2] = pSR->e_svoTI_NumberOfBounces - 1.f;
			sData[0].f[3] = pSR->m_texInfo.fGlobalSpecCM_Mult;
			break;
		}

	case ECGP_PB_SvoParams6:
		{
			sData[0].f[0] = pSR->e_svoTI_PointLightsMultiplier;
			sData[0].f[1] = pSR->m_texInfo.vSvoOriginAndSize.x;
			sData[0].f[2] = pSR->e_svoTI_MinReflectance;
			sData[0].f[3] = pSR->m_texInfo.vSvoOriginAndSize.y;
			break;
		}

	case ECGP_PB_SvoParams7:
		{
			sData[0].f[0] = pSR->e_svoTI_AnalyticalOccludersRange;
			sData[0].f[1] = pSR->e_svoTI_AnalyticalOccludersSoftness;
			sData[0].f[2] = pSR->m_texInfo.vSvoOriginAndSize.z;
			sData[0].f[3] = pSR->m_texInfo.vSvoOriginAndSize.w;
			break;
		}

	case ECGP_PB_SvoParams8:
		{
			sData[0].f[0] = pSR->e_svoTI_VoxelOpacityMultiplier;
			sData[0].f[1] = pSR->e_svoTI_SkyLightBottomMultiplier;
			sData[0].f[2] = pSR->e_svoTI_PointLightsBias;
			sData[0].f[3] = 0;
			break;
		}

	case ECGP_PB_SvoParams9:
		{
			sData[0].f[0] = pSR->e_svoTI_RT_MaxDistRay;
			sData[0].f[1] = pSR->e_svoTI_RT_MaxDistCam;
			sData[0].f[2] = pSR->e_svoTI_RT_MinGloss;
			sData[0].f[3] = pSR->e_svoTI_RT_MinRefl;
			break;
		}

	default:
		bRes = false;
	}

	return bRes;
}

void CSvoRenderer::DebugDrawStats(const RPProfilerStats* pBasicStats, float& ypos, const float ystep, float xposms)
{
	ColorF color = Col_Yellow;
	const EDrawTextFlags txtFlags = (EDrawTextFlags)(eDrawText_2D | eDrawText_800x600 | eDrawText_FixedSize | eDrawText_Monospace);

	#define SVO_Draw2dLabel(labelName)                                                                                                             \
	  IRenderAuxText::Draw2dLabel(60, ypos += ystep, 2, &color.r, false, (const char*)(((const char*)( # labelName)) + 10));                       \
	  if (pBasicStats[labelName].gpuTimeMax > 0.01)                                                                                                \
	    IRenderAuxText::Draw2dLabelEx(xposms, ypos, 2, color, txtFlags, "%5.2f Aver=%5.2f Max=%5.2f",                                              \
	                                  pBasicStats[labelName].gpuTime, pBasicStats[labelName].gpuTimeSmoothed, pBasicStats[labelName].gpuTimeMax);  \
	  else                                                                                                                                         \
	    IRenderAuxText::Draw2dLabelEx(xposms, ypos, 2, color, txtFlags, "%5.2f", pBasicStats[labelName].gpuTime);                                  \

	SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_CLEAR);
	SVO_Draw2dLabel(eRPPSTATS_TI_VOXELIZE);
	SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_AIR);
	SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_LIGHT);
	SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_REFL0);
	SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_REFL1);
	SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_DYNL);
	SVO_Draw2dLabel(eRPPSTATS_TI_NID_DIFF);
	SVO_Draw2dLabel(eRPPSTATS_TI_GEN_DIFF);
	SVO_Draw2dLabel(eRPPSTATS_TI_GEN_SPEC);
	SVO_Draw2dLabel(eRPPSTATS_TI_GEN_AIR);
	SVO_Draw2dLabel(eRPPSTATS_TI_DEMOSAIC_DIFF);
	SVO_Draw2dLabel(eRPPSTATS_TI_DEMOSAIC_SPEC);
	SVO_Draw2dLabel(eRPPSTATS_TI_UPSCALE_DIFF);
	SVO_Draw2dLabel(eRPPSTATS_TI_UPSCALE_SPEC);
}

uint64 CSvoRenderer::GetRunTimeFlags(bool bDiffuseMode, bool bPixelShader)
{
	uint64 rtFlags = 0;

	if (e_svoTI_LowSpecMode > 0) // simplify shaders
		rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE0];

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (m_texInfo.pGlobalSpecCM && GetIntegratioMode()) // use global env CM
		rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE1];
	#endif

	if (e_svoTI_Troposphere_Active) // compute air lighting as well
		rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE2];

	if (e_svoTI_Diffuse_Cache) // use pre-baked lighting
		rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE3];

	if (bDiffuseMode) // diffuse or specular rendering
		rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE4];

	if (GetIntegratioMode()) // ignore colors and normals for AO only mode
		rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE5];

	if ((bPixelShader && e_svoTI_HalfresKernelPrimary) || (!bPixelShader && e_svoTI_HalfresKernelSecondary)) // smaller kernel - less de-mosaic work and faster compute update
		rtFlags |= g_HWSR_MaskBit[HWSR_HW_PCF_COMPARE];

	if (bPixelShader && !GetIntegratioMode() && e_svoTI_InjectionMultiplier) // read sun light and shadow map during final cone tracing
		rtFlags |= g_HWSR_MaskBit[HWSR_LIGHT_TEX_PROJ];

	if (bPixelShader && !GetIntegratioMode() && e_svoTI_InjectionMultiplier && m_arrLightsDynamic.Count()) // use point lights and shadow maps during final cone tracing
		rtFlags |= g_HWSR_MaskBit[HWSR_POINT_LIGHT];

	if (bPixelShader && e_svoTI_SSDepthTrace) // SS depth trace
		rtFlags |= g_HWSR_MaskBit[HWSR_BLEND_WITH_TERRAIN_COLOR];

	if (bPixelShader && e_svoTI_DualTracing && (gRenDev->GetActiveGPUCount() >= e_svoTI_DualTracing))
		rtFlags |= g_HWSR_MaskBit[HWSR_MOTION_BLUR];

	if (e_svoTI_AnalyticalGI)
		rtFlags |= g_HWSR_MaskBit[HWSR_LIGHTVOLUME1];

	if (e_svoTI_TraceVoxels)
		rtFlags |= g_HWSR_MaskBit[HWSR_TILED_SHADING];

	if (m_texInfo.arrPortalsPos[0].z)
		rtFlags |= g_HWSR_MaskBit[HWSR_LIGHTVOLUME0];

	if (bPixelShader && (e_svoTI_AnalyticalGI || e_svoTI_AnalyticalOccluders) && m_texInfo.arrAnalyticalOccluders[0][0].radius)
		rtFlags |= g_HWSR_MaskBit[HWSR_ENVIRONMENT_CUBEMAP];

	if (bPixelShader && e_svoTI_AnalyticalOccluders && m_texInfo.arrAnalyticalOccluders[1][0].radius)
		rtFlags |= g_HWSR_MaskBit[HWSR_SPRITE];

	if (!bPixelShader && e_svoTI_SunRSMInject)
		rtFlags |= g_HWSR_MaskBit[HWSR_AMBIENT_OCCLUSION];

	#if !CRY_PLATFORM_CONSOLE
	if (bPixelShader && e_svoTI_RsmUseColors > 0)
		rtFlags |= g_HWSR_MaskBit[HWSR_VOLUMETRIC_FOG];
	#endif

	if (e_svoTI_ShadowsFromSun && bDiffuseMode && bPixelShader)
		rtFlags |= g_HWSR_MaskBit[HWSR_REVERSE_DEPTH];

	if (e_svoTI_SpecularFromDiffuse && bDiffuseMode && bPixelShader)
		rtFlags |= g_HWSR_MaskBit[HWSR_PROJECTION_MULTI_RES];

	if (e_svoTI_ShadowsFromHeightmap && bPixelShader)
		rtFlags |= g_HWSR_MaskBit[HWSR_QUALITY1];

	if (e_svoTI_TranslucentBrightness && bPixelShader)
		rtFlags |= g_HWSR_MaskBit[HWSR_NO_TESSELLATION];

	// Software triangle ray tracing (rt decision 05 section 5.1). Every bit below is gated on
	// e_svoTI_RT_Active, so with the feature off GetRunTimeFlags returns exactly what it
	// returned before and no stock permutation changes.
	if (e_svoTI_RT_Active && bPixelShader && !bDiffuseMode)
	{
		rtFlags |= g_HWSR_MaskBit[HWSR_QUALITY];       // mesh ray tracing

		if (e_svoTI_RT_StaticBVH)
			rtFlags |= g_HWSR_MaskBit[HWSR_SAMPLE6];     // per-cell static BVH

		if (e_svoTI_RT_Debug)
			rtFlags |= g_HWSR_MaskBit[HWSR_PARTICLE_SHADOW]; // RT debug views
	}

	return rtFlags;
}

int CSvoRenderer::GetIntegratioMode() const
{
	return e_svoTI_IntegrationMode;
}

int CSvoRenderer::GetIntegratioMode(bool& bSpecTracingInUse) const
{
	bSpecTracingInUse = (e_svoTI_IntegrationMode == 2) || (e_svoTI_SpecularFromDiffuse != 0);
	return e_svoTI_IntegrationMode;
}

bool CSvoRenderer::IsActive()
{
	return CSvoRenderer::s_pInstance && CSvoRenderer::s_pInstance->e_svoTI_Apply;
}

void CSvoRenderer::InitCVarValues()
{
	#define INIT_SVO_CVAR(_type, _var)                              \
	  if (strstr( # _type, "float"))                                \
	    _var = (_type)gEnv->pConsole->GetCVar( # _var)->GetFVal();  \
	  else                                                          \
	    _var = (_type)gEnv->pConsole->GetCVar( # _var)->GetIVal();  \

	INIT_ALL_SVO_CVARS;
	#undef INIT_SVO_CVAR

	// rt stage 4B (decision 08): the three glossy-reflection knobs, resolved by hand so a
	// Cry3DEngine.dll that does not carry their registrations yet keeps the header's defaults
	// instead of dereferencing a null ICVar. See the comment on the members in D3D_SVO.h.
	if (ICVar* pCVarGlossyMode = gEnv->pConsole->GetCVar("e_svoTI_RT_GlossyMode"))
		e_svoTI_RT_GlossyMode = pCVarGlossyMode->GetIVal();
	if (ICVar* pCVarGlossScale = gEnv->pConsole->GetCVar("e_svoTI_RT_GlossScale"))
		e_svoTI_RT_GlossScale = pCVarGlossScale->GetFVal();
	if (ICVar* pCVarTemporalFrames = gEnv->pConsole->GetCVar("e_svoTI_RT_TemporalFrames"))
		e_svoTI_RT_TemporalFrames = pCVarTemporalFrames->GetIVal();

	// rt stage 5F (decision 09 section 9.2), same defensive resolution.
	if (ICVar* pCVarClouds = gEnv->pConsole->GetCVar("e_svoTI_RT_Clouds"))
		e_svoTI_RT_Clouds = pCVarClouds->GetIVal();
	if (ICVar* pCVarCloudSteps = gEnv->pConsole->GetCVar("e_svoTI_RT_CloudSteps"))
		e_svoTI_RT_CloudSteps = pCVarCloudSteps->GetIVal();

	// S8, decisions/s8-svogi-fog-clouds.md item 1 A4. The screen-space depth trace is the ONE
	// place SVOGI reads an already pre-exposed buffer ($HDRTargetPrev, bound at slot 12) back
	// into voxel-space maths that is deliberately kept in unexposed stock units. Mixing the two
	// would put the exposure into the trace twice and nowhere consistently. The cvar's default
	// is already 0 and it is VF_EXPERIMENTAL; on the scene-referred path it is held there.
	if (CRendererResources::IsSceneReferredLightUnits())
		e_svoTI_SSDepthTrace = 0.0f;
}

CTexture* CSvoRenderer::GetTroposphereMinRT()
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (IsActive() && e_svoTI_Troposphere_Active && m_pRT_AIR_MIN)
		return m_pRT_AIR_MIN;
	#endif
	return NULL;
}

CTexture* CSvoRenderer::GetTroposphereMaxRT()
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (IsActive() && e_svoTI_Troposphere_Active && m_pRT_AIR_MAX)
		return m_pRT_AIR_MAX;
	#endif
	return NULL;
}

CTexture* CSvoRenderer::GetTroposphereShadRT()
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (IsActive() && e_svoTI_Troposphere_Active && m_pRT_SHAD_MIN_MAX)
		return m_pRT_SHAD_MIN_MAX;
	#endif
	return NULL;
}

CTexture* CSvoRenderer::GetTracedSunShadowsRT()
{
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (IsActive() && e_svoTI_ShadowsFromSun && m_pRT_SHAD_FIN_0)
		return m_pRT_SHAD_FIN_0;
	#endif
	return NULL;
}

CTexture* CSvoRenderer::GetDiffuseFinRT()
{
	return m_pPasses->m_tsDiff.pRT_FIN_OUT_0;
}

CTexture* CSvoRenderer::GetSpecularFinRT()
{
	return m_pPasses->m_tsSpec.pRT_FIN_OUT_0;
}

void CSvoRenderer::UpscalePass(SSvoTargetsSet* pTS)
{
	CSvoFullscreenPass& rp = pTS->passUpscale;

	const char* szTechFinalName = "UpScalePass";

	if (!e_svoTI_Active || !e_svoTI_Apply || !e_svoRender || !m_pShader)
		return;

	rp.SetTechnique(m_pShader, szTechFinalName, GetRunTimeFlags(pTS == &m_pPasses->m_tsDiff));
	rp.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
	rp.SetState(GS_NODEPTHTEST);

	rp.SetRenderTarget(0, pTS->pRT_FIN_OUT_0);

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (pTS == &m_pPasses->m_tsDiff && m_pRT_SHAD_FIN_0 && (e_svoTI_ShadowsFromSun || e_svoTI_SpecularFromDiffuse))
		rp.SetRenderTarget(1, m_pRT_SHAD_FIN_0);
	#endif

	// compute specular form results of diffuse tracing
	if (pTS == &m_pPasses->m_tsDiff && e_svoTI_SpecularFromDiffuse)
		rp.SetRenderTarget(2, m_pPasses->m_tsSpec.pRT_FIN_OUT_0);

	rp.SetRequireWorldPos(true);
	rp.SetRequirePerViewConstantBuffer(true);

	SetupGBufferTextures(rp);

	rp.SetTexture(10, pTS->pRT_ALD_DEM_MIN_0);
	rp.SetTexture(11, pTS->pRT_RGB_DEM_MIN_0);
	rp.SetTexture(12, pTS->pRT_ALD_DEM_MAX_0);
	rp.SetTexture(13, pTS->pRT_RGB_DEM_MAX_0);

	rp.SetTexture(9, pTS->pRT_FIN_OUT_1);

	// rt stage 4B (decision 08 item 3): the hit distance, the guide signal of the bilateral.
	if (e_svoTI_RT_Active && pTS->pRT_RAYDIR_0)
		rp.SetTexture(38, pTS->pRT_RAYDIR_0);
	else
		rp.SetTexture(38, CRendererResources::s_ptexBlack);

	if (pTS == &m_pPasses->m_tsSpec && m_pPasses->m_tsDiff.pRT_FIN_OUT_0)
	{
		rp.SetTexture(15, m_pPasses->m_tsDiff.pRT_FIN_OUT_0);
	}
	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	else if (pTS == &m_pPasses->m_tsDiff && m_pRT_SHAD_MIN_MAX && e_svoTI_ShadowsFromSun)
	{
		rp.SetTexture(15, m_pRT_SHAD_MIN_MAX);
	}
	#endif
	else
	{
		rp.SetTexture(15, CRendererResources::s_ptexBlack);
	}

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	if (pTS == &m_pPasses->m_tsDiff && m_pRT_SHAD_FIN_1 && e_svoTI_ShadowsFromSun)
		rp.SetTexture(16, m_pRT_SHAD_FIN_1);
	#endif

	rp.SetTexture(8, GetUtils().GetVelocityObjectRT(RenderView()));

	SetupCommonSamplers(rp);

	rp.BeginConstantUpdate();

	SetupCommonConstants(pTS, rp, pTS->pRT_ALD_0);

	rp.Execute();
}

template<class T>
void CSvoRenderer::SetupRsmSunTextures(T& rp)
{
	const int rsmDepthTexSlot = 29;
	const int rsmColorTexSlot = 30;
	const int rsmNormlTexSlot = 31;

	CTexture* pRsmDepthMap = CRendererResources::s_ptexBlack;
	CTexture* pRsmColorMap = CRendererResources::s_ptexBlack;
	CTexture* pRsmNormalMap = CRendererResources::s_ptexBlack;

	if (ShadowMapFrustum* pRsmFrustum = GetRsmSunFrustum(RenderView()))
	{
		assert(!pRsmFrustum->bUseShadowsPool);

		pRsmDepthMap = pRsmFrustum->pDepthTex;

		if (CTexture* pColorMap = GetRsmColorMap(*m_pRenderView->GetGraphicsPipeline().get(), *pRsmFrustum))
			pRsmColorMap = pColorMap;

		if (CTexture* pNormalMap = GetRsmNormlMap(*m_pRenderView->GetGraphicsPipeline().get(), *pRsmFrustum))
			pRsmNormalMap = pNormalMap;
	}

	rp.SetTexture(rsmDepthTexSlot, pRsmDepthMap);
	rp.SetTexture(rsmColorTexSlot, pRsmColorMap);
	rp.SetTexture(rsmNormlTexSlot, pRsmNormalMap);
}

template<class T>
void CSvoRenderer::SetupRsmSunConstants(T& rp)
{
	static CCryNameR lightProjParamName("SVO_RsmSunShadowProj");
	static CCryNameR rsmSunColparamName("SVO_RsmSunCol");
	static CCryNameR rsmSunDirparamName("SVO_RsmSunDir");
	Matrix44A shadowMat;
	shadowMat.SetIdentity();

	ShadowMapFrustum* pRsmFrustum = GetRsmSunFrustum(RenderView());

	const auto& viewInfo = RenderView()->GetViewInfo(CCamera::eEye_Left);

	if (pRsmFrustum && GetRsmColorMap(*m_pRenderView->GetGraphicsPipeline().get(), *pRsmFrustum))
	{
		CShadowUtils::SShadowsSetupInfo shadowsSetup = gcpRendD3D->ConfigShadowTexgen(RenderView(), pRsmFrustum, 0);

		assert(!pRsmFrustum->bUseShadowsPool);

		// set up shadow matrix
		shadowMat = shadowsSetup.ShadowMat;
		const Vec4 vEye(viewInfo.cameraOrigin, 0.f);
		Vec4 vecTranslation(vEye.Dot((Vec4&)shadowMat.m00), vEye.Dot((Vec4&)shadowMat.m10), vEye.Dot((Vec4&)shadowMat.m20), vEye.Dot((Vec4&)shadowMat.m30));
		shadowMat.m03 += vecTranslation.x;
		shadowMat.m13 += vecTranslation.y;
		shadowMat.m23 += vecTranslation.z;
		shadowMat.m33 += vecTranslation.w;
		(Vec4&)shadowMat.m20 *= shadowsSetup.RecpFarDist;
		rp.SetConstantArray(lightProjParamName, alias_cast<Vec4*>(&shadowMat), 4);

		// S8, decisions/s8-svogi-fog-clouds.md item 1 A3: the SVO is kept entirely in STOCK
		// light units, and that includes the sun it injects. On the scene-referred path the sun
		// is radiometric (S2 moved the 1/PI into the BRDF), so GetSunColor() is PI times larger
		// than the number this codec was built for: 11.90 against a hard RGBM ceiling of 4.0
		// (CommonSVO.cfi EncodeHDR), which clips AND hue-shifts every injected texel. Dividing
		// it back restores the 3.788 that CE sized the ceiling against, and it puts the GI
		// output on the same footing as the direct diffuse term, which now carries
		// SCENE_BRDF_NORM_FACTOR. Point lights are NOT touched here - they never had the
		// divide on either path. Off the switch this is the stock expression, untouched.
		Vec3 rsmSunColor = gEnv->p3DEngine->GetSunColor();
		if (CRendererResources::IsSceneReferredLightUnits())
			rsmSunColor /= gf_PI;

		Vec4 vData(rsmSunColor, e_svoTI_InjectionMultiplier);
		rp.SetConstantArray(rsmSunColparamName, (Vec4*)&vData, 1);
		Vec4 vData2(gEnv->p3DEngine->GetSunDirNormalized(), (float)e_svoTI_SunRSMInject);
		rp.SetConstantArray(rsmSunDirparamName, (Vec4*)&vData2, 1);
	}
	else
	{
		Vec4 vData(0, 0, 0, 0);
		rp.SetConstantArray(rsmSunColparamName, (Vec4*)&vData, 1);
		Vec4 vData2(0, 0, 0, 0);
		rp.SetConstantArray(rsmSunDirparamName, (Vec4*)&vData2, 1);
	}
}

ISvoRenderer* CD3D9Renderer::GetISvoRenderer()
{
	return CSvoRenderer::GetInstance(true);
}

template<class T>
void CSvoRenderer::BindTiledLights(PodArray<I3DEngine::SLightTI>& lightsTI, T& rp)
{
	std::shared_ptr<CGraphicsPipeline> pActivePipeline = m_pRenderView->GetGraphicsPipeline();
	auto* tiledLights = pActivePipeline->GetStage<CTiledLightVolumesStage>();
	auto* pShadowMapStage = pActivePipeline->GetStage<CShadowMapStage>();

	rp.SetBuffer(16, tiledLights->GetLightShadeInfoBuffer());
	rp.SetTexture(19, tiledLights->GetProjectedLightAtlas());
	rp.SetTexture(20, pShadowMapStage->m_pTexRT_ShadowPool);

	CTexture* ptexRsmCol = CRendererResources::s_ptexBlack;
	CTexture* ptexRsmNor = CRendererResources::s_ptexBlack;
	if (CSvoRenderer::GetInstance()->IsActive() && CSvoRenderer::GetInstance()->GetSpecularFinRT())
	{
		if (CTexture* pTexPoolCol = CSvoRenderer::GetInstance()->GetRsmPoolCol())
		{
			if (CTexture::IsTextureExist(pTexPoolCol))
				ptexRsmCol = pTexPoolCol;
		}
		if (CTexture* pTexPoolNor = CSvoRenderer::GetInstance()->GetRsmPoolNor())
		{
			if (CTexture::IsTextureExist(pTexPoolNor))
				ptexRsmNor = pTexPoolNor;
		}
	}

	rp.SetTexture(26, ptexRsmCol);
	rp.SetTexture(27, ptexRsmNor);

	CTiledLightVolumesStage::STiledLightShadeInfo* tiledLightShadeInfo = tiledLights->GetTiledLightShadeInfo();

	for (int l = 0; l < lightsTI.Count(); l++)
	{
		I3DEngine::SLightTI& svoLight = lightsTI[l];

		if (!svoLight.vDirF.w)
			continue;

		for (uint32 lightIdx = 0; lightIdx < MaxNumTileLights && tiledLightShadeInfo[lightIdx].lightType != CTiledLightVolumesStage::tlTypeNone; ++lightIdx)
		{
			if ((tiledLightShadeInfo[lightIdx].lightType == CTiledLightVolumesStage::tlTypeRegularProjector) && svoLight.vPosR.IsEquivalent(tiledLightShadeInfo[lightIdx].posRad, .5f))
			{
				if (svoLight.vCol.w > 0)
					svoLight.vCol.w = ((float)lightIdx + 100);
				else
					svoLight.vCol.w = -((float)lightIdx + 100);
			}
		}
	}
}

ShadowMapFrustum* CSvoRenderer::GetRsmSunFrustum(const CRenderView* pRenderView) const
{
	for (const auto& pFrustumToRender : pRenderView->GetShadowFrustumsByType(CRenderView::eShadowFrustumRenderType_SunDynamic))
	{
		if (pFrustumToRender->pFrustum->nShadowMapLod == e_svoTI_GsmCascadeLod)
		{
			return pFrustumToRender->pFrustum;
		}
	}

	return nullptr;
}

CTexture* CSvoRenderer::GetRsmColorMap(CGraphicsPipeline& graphicsPipeline, const ShadowMapFrustum& rFr, bool bCheckUpdate)
{
#if defined(FEATURE_SVO_GI_ALLOW_HQ) || !CRY_PLATFORM_CONSOLE
	auto* pShadowMapStage = graphicsPipeline.GetStage<CShadowMapStage>();
	if (IsActive() && (rFr.nShadowMapLod == CSvoRenderer::GetInstance()->e_svoTI_GsmCascadeLod) && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && CSvoRenderer::GetInstance()->e_svoTI_RsmUseColors >= 0)
	{
		if (bCheckUpdate)
			CSvoRenderer::GetInstance()->CheckCreateUpdateRT(s_pRsmColorMap, rFr.nShadowMapSize, rFr.nShadowMapSize, eTF_R8G8B8A8, eTT_2D, FT_STATE_CLAMP, "SVO_SUN_RSM_COLOR");

		return CTexture::IsTextureExist(s_pRsmColorMap) ? s_pRsmColorMap.get() : nullptr;
	}

	if (IsActive() && rFr.bUseShadowsPool && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && CSvoRenderer::GetInstance()->e_svoTI_RsmUseColors >= 0 && rFr.m_Flags & DLF_USE_FOR_SVOGI)
	{
		if (bCheckUpdate)
			CSvoRenderer::GetInstance()->CheckCreateUpdateRT(s_pRsmPoolCol, pShadowMapStage->m_pTexRT_ShadowPool->GetWidth(), pShadowMapStage->m_pTexRT_ShadowPool->GetHeight(), eTF_R8G8B8A8, eTT_2D, FT_STATE_CLAMP, "SVO_PRJ_RSM_COLOR");

		return CTexture::IsTextureExist(s_pRsmPoolCol) ? s_pRsmPoolCol.get() : nullptr;
	}
#endif
	return NULL;
}

CTexture* CSvoRenderer::GetRsmNormlMap(CGraphicsPipeline& graphicsPipeline, const ShadowMapFrustum& rFr, bool bCheckUpdate)
{
#if defined(FEATURE_SVO_GI_ALLOW_HQ) || !CRY_PLATFORM_CONSOLE
	auto* pShadowMapStage = graphicsPipeline.GetStage<CShadowMapStage>();
	if (IsActive() && (rFr.nShadowMapLod == CSvoRenderer::GetInstance()->e_svoTI_GsmCascadeLod) && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && CSvoRenderer::GetInstance()->e_svoTI_RsmUseColors >= 0)
	{
		if (bCheckUpdate)
			CSvoRenderer::GetInstance()->CheckCreateUpdateRT(s_pRsmNormlMap, rFr.nShadowMapSize, rFr.nShadowMapSize, eTF_R8G8B8A8, eTT_2D, FT_STATE_CLAMP, "SVO_SUN_RSM_NORMAL");

		return CTexture::IsTextureExist(s_pRsmNormlMap) ? s_pRsmNormlMap.get() : nullptr;
	}

	if (IsActive() && rFr.bUseShadowsPool && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && CSvoRenderer::GetInstance()->e_svoTI_RsmUseColors >= 0 && rFr.m_Flags & DLF_USE_FOR_SVOGI)
	{
		if (bCheckUpdate)
			CSvoRenderer::GetInstance()->CheckCreateUpdateRT(s_pRsmPoolNor, pShadowMapStage->m_pTexRT_ShadowPool->GetWidth(), pShadowMapStage->m_pTexRT_ShadowPool->GetHeight(), eTF_R8G8B8A8, eTT_2D, FT_STATE_CLAMP, "SVO_PRJ_RSM_NORMAL");

		return CTexture::IsTextureExist(s_pRsmPoolNor) ? s_pRsmPoolNor.get() : nullptr;
	}
#endif
	return NULL;
}

void CSvoRenderer::GetRsmTextures(_smart_ptr<CTexture>& pRsmColorMap, _smart_ptr<CTexture>& pRsmNormlMap, _smart_ptr<CTexture>& pRsmPoolCol, _smart_ptr<CTexture>& pRsmPoolNor)
{
#if defined(FEATURE_SVO_GI_ALLOW_HQ) || !CRY_PLATFORM_CONSOLE
	bool useRSM = gEnv->IsEditor() || (CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && CSvoRenderer::GetInstance()->e_svoTI_RsmUseColors >= 0);

	if (useRSM)
	{
		if (!s_pRsmColorMap)
			s_pRsmColorMap = CTexture::GetOrCreateTextureObjectPtr("SVO_SUN_RSM_COLOR", 0, 0, 1, eTT_2D, FT_STATE_CLAMP, eTF_R8G8B8A8);
		if (!s_pRsmNormlMap)
			s_pRsmNormlMap = CTexture::GetOrCreateTextureObjectPtr("SVO_SUN_RSM_NORMAL", 0, 0, 1, eTT_2D, FT_STATE_CLAMP, eTF_R8G8B8A8);
		if (!s_pRsmPoolCol)
			s_pRsmPoolCol = CTexture::GetOrCreateTextureObjectPtr("SVO_PRJ_RSM_COLOR", 0, 0, 1, eTT_2D, FT_STATE_CLAMP, eTF_R8G8B8A8);
		if (!s_pRsmPoolNor)
			s_pRsmPoolNor = CTexture::GetOrCreateTextureObjectPtr("SVO_PRJ_RSM_NORMAL", 0, 0, 1, eTT_2D, FT_STATE_CLAMP, eTF_R8G8B8A8);
	}

	pRsmColorMap = s_pRsmColorMap;
	pRsmNormlMap = s_pRsmNormlMap;
	pRsmPoolCol = s_pRsmPoolCol;
	pRsmPoolNor = s_pRsmPoolNor;
#endif
}

void CSvoRenderer::CheckCreateUpdateRT(_smart_ptr<CTexture>& pTex, int nWidth, int nHeight, ETEX_Format eTF, ETEX_Type eTT, int nTexFlags, const char* szName)
{
	if (!CTexture::IsTextureExist(pTex) || pTex->GetWidth() != nWidth || pTex->GetHeight() != nHeight || pTex->GetTextureDstFormat() != eTF)
	{
		const bool bNeedsDecRef = !CTexture::IsTextureExist(pTex); // NOTE: SD3DPostEffectsUtils::GetOrCreateRenderTarget adds ref when !CTexture::IsTextureExist only

		CTexture* pTexRaw = pTex;
		if (SD3DPostEffectsUtils::GetOrCreateRenderTarget(szName, pTexRaw, nWidth, nHeight, Clr_Unknown, 0, false, eTF))
		{
			pTex = pTexRaw;
			pTex->DisableMgpuSync();

			if (bNeedsDecRef)
				pTex->Release();
		}
	}
}

	#ifdef FEATURE_SVO_GI_ALLOW_HQ

void CSvoRenderer::SVoxPool::Init(ITexture* _pTex)
{
	pTex = (CTexture*)_pTex;

	if (CTexture::IsTextureExist(pTex))
	{
		if (CSvoRenderer::s_pInstance && CSvoRenderer::s_pInstance->GetIntegratioMode())
		{
			if (pTex->GetFlags() & FT_USAGE_UAV_RWTEXTURE)
				pUAV = pTex->GetDevTexture()->LookupUAV(EDefaultResourceViews::UnorderedAccess);
			else
				pSRV = pTex->GetDevTexture()->LookupSRV(EDefaultResourceViews::Default);
		}

		nTexId = pTex->GetTextureID();
	}
}

	#endif

#endif
