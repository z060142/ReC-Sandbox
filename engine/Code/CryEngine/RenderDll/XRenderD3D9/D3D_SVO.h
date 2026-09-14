// Copyright 2014-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#if defined(FEATURE_SVO_GI)

	#include "GraphicsPipeline/Common/ComputeRenderPass.h"
	#include "GraphicsPipeline/Common/FullscreenPass.h"

class CSvoComputePass : public CComputeRenderPass
{
public:

	CSvoComputePass(CGraphicsPipeline* pGraphicsPipeline)
		: CComputeRenderPass::CComputeRenderPass(pGraphicsPipeline, CSvoComputePass::eFlags_ReflectConstantBuffersFromShader)
	{
	}

	int nPrevTargetSize = 0;
};

class CSvoFullscreenPass : public CFullscreenPass
{
public:
	CSvoFullscreenPass(CGraphicsPipeline* pGraphicsPipeline) : CFullscreenPass(pGraphicsPipeline) {}
	int nPrevTargetSize = 0;
};

struct SSvoTargetsSet
{
	SSvoTargetsSet(CGraphicsPipeline* pGraphicsPipeline)
		: passConeTrace(pGraphicsPipeline)
		, passShade(pGraphicsPipeline)
		, passDemosaic(pGraphicsPipeline)
		, passUpscale(pGraphicsPipeline) {}

	// tracing targets
	_smart_ptr<CTexture> pRT_RGB_0, pRT_ALD_0, pRT_RGB_1, pRT_ALD_1;

	// rt stage 2 (decision 06): the two extra g-data targets ConeTracePass writes and the pair
	// ShadePass writes. Allocated only for the specular set and only while e_svoTI_RT_Active;
	// null otherwise, which is what keeps the stock path untouched.
	// ShadePass cannot write into pRT_ALD_0 / pRT_RGB_0 because it reads them as g-data, so it
	// gets its own pair and DemosaicPass is pointed at that pair for the frame (bShaded).
	_smart_ptr<CTexture> pRT_HITPOS_0, pRT_RAYDIR_0, pRT_ALD_SHD, pRT_RGB_SHD;
	// rt stage 4A: the fifth g-data target, SVO diffuse irradiance + voxel AO at the hit, so the
	// reflected surface gets the same indirect model ApplyGI mode 2 gives the primary one.
	_smart_ptr<CTexture> pRT_HITGI_0;
	// decision 11: the sixth g-data target, the hit's IDENTITY - triangle record, material
	// record, barycentrics and flags (RT_PackHitId). Slot 5 of the ConeTracePass MRT set, read
	// by ShadePS at t39, which turns it back into a material with RT_ReconstructHit. It
	// replaces stage 5B's HITMAT, and it is RGBA32F rather than fp16 because the two record
	// indices are exact integers the shader indexes a pool with.
	_smart_ptr<CTexture> pRT_HITID_0;
	bool                 bShaded = false;

	// de-mosaic targets
	_smart_ptr<CTexture> pRT_RGB_DEM_MIN_0, pRT_ALD_DEM_MIN_0, pRT_RGB_DEM_MAX_0, pRT_ALD_DEM_MAX_0;
	_smart_ptr<CTexture> pRT_RGB_DEM_MIN_1, pRT_ALD_DEM_MIN_1, pRT_RGB_DEM_MAX_1, pRT_ALD_DEM_MAX_1;

	// output
	_smart_ptr<CTexture> pRT_FIN_OUT_0, pRT_FIN_OUT_1;

	CSvoFullscreenPass   passConeTrace;
	CSvoFullscreenPass   passShade;
	CSvoFullscreenPass   passDemosaic;
	CSvoFullscreenPass   passUpscale;
};

struct SSvoPrimitivePasses
{
	SSvoTargetsSet m_tsDiff, m_tsSpec;

	// passes
	CSvoComputePass      m_passClearBricks;
	CSvoComputePass      m_passInjectDynamicLights;
	CSvoComputePass      m_passInjectStaticLights;
	CSvoComputePass      m_passInjectAirOpacity;
	CSvoComputePass      m_passPropagateLighting_1to2;
	CSvoComputePass      m_passPropagateLighting_2to3;
	CSvoFullscreenPass   m_passTroposphere;
	CSvoComputePass      m_passBuildRTLightList;   // rt stage 2 (decision 06 section 6.2)

	SGraphicsPipelineKey currentKey;

	SSvoPrimitivePasses(CGraphicsPipeline* pGraphicsPipeline);
};

class CSvoRenderer : public ISvoRenderer
{
public:
	struct SForwardParams
	{
		Vec4 IntegrationMode;
	};

	// ISvoRenderer
	void                 Release() final;
	void                 InitCVarValues() final;

	static CSvoRenderer* GetInstance(bool bCheckAlloce = false);
	static bool          IsActive();
	void                 UpdateCompute(CRenderView* pRenderView);
	void                 UpdateRender(CRenderView* pRenderView);
	int                  GetIntegratioMode() const;
	int                  GetIntegratioMode(bool& bSpecTracingInUse) const;
	bool                 GetUseLightProbes() const { return e_svoTI_SkyColorMultiplier >= 0; }
	void                 DebugDrawStats(const RPProfilerStats* pBasicStats, float& ypos, const float ystep, float xposms);
	size_t               GetAllocatedMemory();

	static bool          SetShaderParameters(float*& pSrc, uint32 paramType, UFloat4* sData);
	static CTexture*     GetRsmColorMap(CGraphicsPipeline& graphicsPipeline, const ShadowMapFrustum& rFr, bool bCheckUpdate = false);
	static CTexture*     GetRsmNormlMap(CGraphicsPipeline& graphicsPipeline, const ShadowMapFrustum& rFr, bool bCheckUpdate = false);
	ShadowMapFrustum*    GetRsmSunFrustum(const CRenderView* pRenderView) const;
	CTexture*            GetTroposphereMinRT();
	CTexture*            GetTroposphereMaxRT();
	CTexture*            GetTroposphereShadRT();
	CTexture*            GetTracedSunShadowsRT();
	CTexture*            GetDiffuseFinRT();
	CTexture*            GetSpecularFinRT();
	float                GetSsaoAmount()           { return IsActive() ? e_svoTI_SSAOAmount : 1.f; }
	float                GetVegetationMaxOpacity() { return e_svoTI_VegetationMaxOpacity; }
	CTexture*            GetRsmPoolCol()           { return IsActive() ? s_pRsmPoolCol.get() : NULL; }
	CTexture*            GetRsmPoolNor()           { return IsActive() ? s_pRsmPoolNor.get() : NULL; }
	static void          GetRsmTextures(_smart_ptr<CTexture>& pRsmColorMap, _smart_ptr<CTexture>& pRsmNormlMap, _smart_ptr<CTexture>& pRsmPoolCol, _smart_ptr<CTexture>& pRsmPoolNor);
	ColorF               GetSkyColor()             { return m_texInfo.vSkyColorTop; }

	void                 FillForwardParams(SForwardParams& svogiParams, bool enable = true) const;

	void                 BeginFrame(CGraphicsPipeline* pGraphicsPipeline)
	{
		if (!m_pPasses || m_pPasses->currentKey != pGraphicsPipeline->GetKey())
		{
			m_pPasses = stl::make_unique<SSvoPrimitivePasses>(pGraphicsPipeline);
		}
	}

protected:

	CSvoRenderer();
	virtual ~CSvoRenderer() {}

	// ISvoRenderer
	void                   SetEditingHelper(const Sphere& sp) final;
	bool                   IsShaderItemUsedForVoxelization(SShaderItem& rShaderItem, IRenderNode* pRN) final;

	static CTexture*       GetGBuffer(const CGraphicsPipelineResources& pipelineResources, int nId);
	static CTexture*       GetZBuffer(const CGraphicsPipelineResources& pipelineResources, bool bLinear = true);
	void                   UpscalePass(SSvoTargetsSet* pTS);
	void                   DemosaicPass(SSvoTargetsSet* pTS);
	void                   ConeTracePass(SSvoTargetsSet* pTS);

	// rt stage 2 (decision 06): hit shading
	bool                   IsRtHitShadingActive() const;
	bool                   IsRtBlueNoiseReady() const;
	void                   BuildRTLightGridPass();
	void                   ShadePass(SSvoTargetsSet* pTS);
	void                   SetupShadeForwardResources(CSvoFullscreenPass& rp);
	// rt stage 4A (decision 09 section 9.1): the engine's own sky for a ray that misses.
	void                   SetupShadeSkyTextures(CSvoFullscreenPass& rp);
	void                   SetupShadeSkyConstants(CSvoFullscreenPass& rp);
	// rt stage 5E (decision 09 section 9.3): volumetric fog on the reflected segment.
	void                   SetupShadeFogTextures(CSvoFullscreenPass& rp);
	void                   SetupShadeFogConstants(CSvoFullscreenPass& rp);

	void                   SetupShadeCloudTextures(CSvoFullscreenPass& rp);
	void                   SetupShadeCloudConstants(CSvoFullscreenPass& rp);
	template<class T> void SetupRTLightGridConstants(T& rp);

	template<class T> void SetupCommonSamplers(T& rp);
	template<class T> void SetupCommonConstants(SSvoTargetsSet* pTS, T& rp, CTexture* pRT);
	template<class T> void SetupRsmSunTextures(T& rp);
	template<class T> void SetupRsmSunConstants(T& rp);

	void                   TropospherePass();
	void                   TraceSunShadowsPass();

	void                   SetupGBufferTextures(CSvoFullscreenPass& rp);

	void                   CheckCreateUpdateRT(_smart_ptr<CTexture>& pTex, int nWidth, int nHeight, ETEX_Format eTF, ETEX_Type eTT, int nTexFlags, const char* szName);

	void                   UpdateGpuVoxParams(I3DEngine::SSvoNodeInfo& nodeInfo);
	void                   ExecuteComputeShader(const char* szTechFinalName, CSvoComputePass& rp, int* nNodesForUpdateStartIndex, int nObjPassId, PodArray<I3DEngine::SSvoNodeInfo>& arrNodesForUpdate);
	uint64                 GetRunTimeFlags(bool bDiffuseMode = true, bool bPixelShader = true);
	template<class T> void SetupSvoTexturesForRead(I3DEngine::SSvoStaticTexInfo& texInfo, T& rp, int nStage, int nStageOpa = 0, int nStageNorm = 0);
	void                   SetupNodesForUpdate(int& nNodesForUpdateStartIndex, PodArray<I3DEngine::SSvoNodeInfo>& arrNodesForUpdate, CSvoComputePass& rp);
	void                   CheckAllocateRT(bool bSpecPass);
	template<class T> void SetupLightSources(PodArray<I3DEngine::SLightTI>& lightsTI, T& rp);
	template<class T> void BindTiledLights(PodArray<I3DEngine::SLightTI>& lightsTI, T& rp);
	void                   DrawPonts(PodArray<SVF_P3F_C4B_T2F>& arrVerts);
	void                   VoxelizeRE();
	bool                   VoxelizeMeshes(CShader* ef, SShaderPass* sfm);

	CRenderView*           RenderView() const { return m_pRenderView; }

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	_smart_ptr<CTexture> m_pRT_NID_0;
	_smart_ptr<CTexture> m_pRT_AIR_MIN;
	_smart_ptr<CTexture> m_pRT_AIR_MAX;
	_smart_ptr<CTexture> m_pRT_SHAD_MIN_MAX;
	_smart_ptr<CTexture> m_pRT_SHAD_FIN_0, m_pRT_SHAD_FIN_1;
	#endif

	Matrix44A                   m_matReproj;
	Matrix44A                   m_matViewProjPrev;
	CShader*                    m_pShader;
	_smart_ptr<CTexture>        m_pNoiseTex;
	_smart_ptr<CTexture>        m_pCloudShadowTex;
	// rt stage 4B (decision 08 item 1): 64 x 64 RGBA8 void-and-cluster blue noise, the random
	// numbers of the GGX-VNDF sampler. Loaded lazily the first time e_svoTI_RT_Active is on and
	// never released; if the file is missing the shader falls back to a hash.
	_smart_ptr<CTexture>        m_pTexRTBlueNoise;
	bool                        m_bTriedLoadRTBlueNoise = false;
	static _smart_ptr<CTexture> s_pRsmColorMap;
	static _smart_ptr<CTexture> s_pRsmNormlMap;
	static _smart_ptr<CTexture> s_pRsmPoolCol;
	static _smart_ptr<CTexture> s_pRsmPoolNor;

	// Currently used render view
	CRenderView* m_pRenderView;

	#ifdef FEATURE_SVO_GI_ALLOW_HQ
	struct SVoxPool
	{
		SVoxPool() { nTexId = 0; pUAV = 0; pSRV = 0; }
		void Init(ITexture* pTex);
		int                nTexId;
		D3DUAV*            pUAV;
		D3DShaderResource* pSRV;
		CTexture*          pTex;
	};

	SVoxPool             vp_OPAC;
	SVoxPool             vp_RGB0;
	SVoxPool             vp_RGB1;
	SVoxPool             vp_DYNL;
	SVoxPool             vp_RGB2;
	SVoxPool             vp_RGB3;
	SVoxPool             vp_RGB4;
	SVoxPool             vp_NORM;
	SVoxPool             vp_ALDI;
	#endif

	PodArray<I3DEngine::SSvoNodeInfo> m_arrNodesForUpdateIncr;
	PodArray<I3DEngine::SSvoNodeInfo> m_arrNodesForUpdateNear;
	PodArray<I3DEngine::SLightTI>     m_arrLightsStatic;
	PodArray<I3DEngine::SLightTI>     m_arrLightsDynamic;
	PodArray<SVF_P3F_C4B_T2F>         m_arrVerts;
	Matrix44A                         m_mGpuVoxViewProj[3];
	Vec4                              m_wsOffset, m_tcOffset;
	static const int                  SVO_MAX_NODE_GROUPS = 4;
	float                             m_arrNodesForUpdate[SVO_MAX_NODE_GROUPS][4][4];
	int                               m_nCurPropagationPassID;
	I3DEngine::SSvoStaticTexInfo      m_texInfo;
	static CSvoRenderer*              s_pInstance;
	PodArray<I3DEngine::SSvoNodeInfo> m_arrNodeInfo;

	std::unique_ptr<SSvoPrimitivePasses> m_pPasses;

	// rt stage 2 (decision 06 section 6.2): world-space light mask grid, gridDim^3 cells of
	// 8 x uint, byte-compatible with the screen tile mask so the shade-time bit walk is CE's.
	CGpuBuffer         m_rtLightGridBuf;
	int                m_rtLightGridDim = 0;
	Vec4               m_rtLightGridMin = Vec4(ZERO);
	Vec4               m_rtLightGridMax = Vec4(ZERO);
	Vec4               m_rtLightGridDims = Vec4(ZERO);
	CConstantBufferPtr m_pShadeForwardCB;   // CBPerPassForward (b5) for ShadePass

	// rt stage 4A: what SetupShadeSkyTextures actually managed to bind this frame. The shader
	// branches on these through SVO_SkyParams.xy; with neither set a miss keeps the env probe.
	bool               m_bSkyDomeBound = false;
	bool               m_bSkyBoxBound = false;

	// rt stage 5E: whether SetupShadeFogTextures found live froxel volumes this frame, and
	// whether the extinction density lives in its own R16F volume (r_HDRTexFormat 0 makes the
	// in-scatter volume R11G11B10F, which has no alpha to put it in). The shader branches on
	// both through SVO_VolFogParams.xy; with .x = 0 it keeps the analytic global fog.
	bool               m_bVolFogBound = false;
	bool               m_bVolFogSeparateDensity = false;

	// rt stage 5F: whether SetupShadeCloudTextures found a live volumetric cloud stage with a
	// cloud shadow volume this frame. With false the shader's SVO_CloudParams0.x is 0 and the
	// miss path is exactly the plain sky lookup of stage 4A.
	bool               m_bCloudsBound = false;

	// rt stage 4B (decision 08). These three are resolved DEFENSIVELY in InitCVarValues instead
	// of through INIT_SVO_CVAR, because their REGISTER_CVAR_AUTO lines live in Cry3DEngine's
	// SceneTreeCVars.inl and that file had another agent's uncommitted work in it when this
	// landed, so it is not committed with this stage (report 04b section 8 lists the three lines
	// to add). INIT_SVO_CVAR dereferences gEnv->pConsole->GetCVar() unchecked, so a
	// Cry3DEngine.dll built without the registrations would crash on the first frame. The values
	// below are the registered defaults, so the feature behaves identically either way; only the
	// console knobs are missing until the registrations land.
	int   e_svoTI_RT_GlossyMode = 1;
	float e_svoTI_RT_GlossScale = 1.f;
	int   e_svoTI_RT_TemporalFrames = 8;

	// rt stage 5F (decision 09 section 9.2), resolved the same defensive way and for the same
	// reason: a Cry3DEngine.dll without the registrations keeps these defaults instead of
	// dereferencing a null ICVar.
	int   e_svoTI_RT_Clouds = 1;
	int   e_svoTI_RT_CloudSteps = 12;

	// cvar values
	#define INIT_ALL_SVO_CVARS                                      \
	  INIT_SVO_CVAR(int, e_svoDVR);                                 \
	  INIT_SVO_CVAR(int, e_svoEnabled);                             \
	  INIT_SVO_CVAR(int, e_svoRender);                              \
	  INIT_SVO_CVAR(int, e_svoTI_ResScaleBase);                     \
	  INIT_SVO_CVAR(int, e_svoTI_ResScaleAir);                      \
	  INIT_SVO_CVAR(int, e_svoTI_ResScaleSpecular);                 \
	  INIT_SVO_CVAR(int, e_svoTI_Active);                           \
	  INIT_SVO_CVAR(int, e_svoTI_IntegrationMode);                  \
	  INIT_SVO_CVAR(float, e_svoTI_InjectionMultiplier);            \
	  INIT_SVO_CVAR(float, e_svoTI_SkyColorMultiplier);             \
	  INIT_SVO_CVAR(float, e_svoTI_DiffuseAmplifier);               \
	  INIT_SVO_CVAR(float, e_svoTI_TranslucentBrightness);          \
	  INIT_SVO_CVAR(int, e_svoTI_NumberOfBounces);                  \
	  INIT_SVO_CVAR(float, e_svoTI_Saturation);                     \
	  INIT_SVO_CVAR(float, e_svoTI_PropagationBooster);             \
	  INIT_SVO_CVAR(float, e_svoTI_DiffuseBias);                    \
	  INIT_SVO_CVAR(float, e_svoTI_DiffuseConeWidth);               \
	  INIT_SVO_CVAR(float, e_svoTI_ConeMaxLength);                  \
	  INIT_SVO_CVAR(float, e_svoTI_SpecularAmplifier);              \
	  INIT_SVO_CVAR(float, e_svoMinNodeSize);                       \
	  INIT_SVO_CVAR(float, e_svoMaxNodeSize);                       \
	  INIT_SVO_CVAR(int, e_svoTI_LowSpecMode);                      \
	  INIT_SVO_CVAR(int, e_svoTI_HalfresKernelPrimary);             \
	  INIT_SVO_CVAR(int, e_svoTI_HalfresKernelSecondary);           \
	  INIT_SVO_CVAR(int, e_svoVoxelPoolResolution);                 \
	  INIT_SVO_CVAR(int, e_svoTI_Apply);                            \
	  INIT_SVO_CVAR(float, e_svoTI_Diffuse_Spr);                    \
	  INIT_SVO_CVAR(int, e_svoTI_Diffuse_Cache);                    \
	  INIT_SVO_CVAR(int, e_svoTI_SpecularFromDiffuse);              \
	  INIT_SVO_CVAR(int, e_svoTI_DynLights);                        \
	  INIT_SVO_CVAR(int, e_svoTI_ShadowsFromSun);                   \
	  INIT_SVO_CVAR(int, e_svoTI_ShadowsFromHeightmap);             \
	  INIT_SVO_CVAR(int, e_svoTI_Troposphere_Active);               \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Brightness);         \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Ground_Height);      \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Layer0_Height);      \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Layer1_Height);      \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Snow_Height);        \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Layer0_Rand);        \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Layer1_Rand);        \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Layer0_Dens);        \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Layer1_Dens);        \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_CloudGen_Height);    \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_CloudGen_Freq);      \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_CloudGen_FreqStep);  \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_CloudGen_Scale);     \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_CloudGenTurb_Freq);  \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_CloudGenTurb_Scale); \
	  INIT_SVO_CVAR(float, e_svoTI_Troposphere_Density);            \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_Active);                      \
	  INIT_SVO_CVAR(float, e_svoTI_RT_MaxDistRay);                  \
	  INIT_SVO_CVAR(float, e_svoTI_RT_MaxDistCam);                  \
	  INIT_SVO_CVAR(float, e_svoTI_RT_MinGloss);                    \
	  INIT_SVO_CVAR(float, e_svoTI_RT_MinRefl);                     \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_StaticBVH);                   \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_Debug);                       \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_TriPoolXY);                   \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_TriPoolZ);                    \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_MaxTexRes);                   \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_TexPoolZ);                    \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_MaxBounces);                  \
	  INIT_SVO_CVAR(int,   e_svoTI_RT_LightGridDim);                \
	  INIT_SVO_CVAR(float, e_svoTI_RT_NormalsFading);               \
	  INIT_SVO_CVAR(float, e_svoTI_ShadowsSoftness);                \
	  INIT_SVO_CVAR(float, e_svoTI_Specular_Sev);                   \
	  INIT_SVO_CVAR(float, e_svoTI_SSAOAmount);                     \
	  INIT_SVO_CVAR(float, e_svoTI_PortalsDeform);                  \
	  INIT_SVO_CVAR(float, e_svoTI_PortalsInject);                  \
	  INIT_SVO_CVAR(int, e_svoTI_SunRSMInject);                     \
	  INIT_SVO_CVAR(int, e_svoDispatchX);                           \
	  INIT_SVO_CVAR(int, e_svoDispatchY);                           \
	  INIT_SVO_CVAR(int, e_svoDebug);                               \
	  INIT_SVO_CVAR(int, e_svoVoxGenRes);                           \
	  INIT_SVO_CVAR(float, e_svoDVR_DistRatio);                     \
	  INIT_SVO_CVAR(int, e_svoTI_GsmCascadeLod);                    \
	  INIT_SVO_CVAR(float, e_svoTI_SSDepthTrace);                   \
	  INIT_SVO_CVAR(float, e_svoTI_EmissiveMultiplier);             \
	  INIT_SVO_CVAR(float, e_svoTI_PointLightsMultiplier);          \
	  INIT_SVO_CVAR(float, e_svoTI_TemporalFilteringBase);          \
	  INIT_SVO_CVAR(float, e_svoTI_HighGlossOcclusion);             \
	  INIT_SVO_CVAR(float, e_svoTI_VegetationMaxOpacity);           \
	  INIT_SVO_CVAR(float, e_svoTI_MinReflectance);                 \
	  INIT_SVO_CVAR(int, e_svoTI_DualTracing);                      \
	  INIT_SVO_CVAR(int, e_svoTI_AnalyticalOccluders);              \
	  INIT_SVO_CVAR(int, e_svoTI_RsmUseColors);                     \
	  INIT_SVO_CVAR(float, e_svoTI_AnalyticalOccludersRange);       \
	  INIT_SVO_CVAR(float, e_svoTI_AnalyticalOccludersSoftness);    \
	  INIT_SVO_CVAR(int, e_svoTI_AnalyticalGI);                     \
	  INIT_SVO_CVAR(int, e_svoTI_TraceVoxels);                      \
	  INIT_SVO_CVAR(int, e_svoTI_AsyncCompute);                     \
	  INIT_SVO_CVAR(float, e_svoTI_SkyLightBottomMultiplier);       \
	  INIT_SVO_CVAR(float, e_svoTI_VoxelOpacityMultiplier);         \
	  INIT_SVO_CVAR(float, e_svoTI_PointLightsBias);                \
	  // INIT_ALL_SVO_CVARS

	#define INIT_SVO_CVAR(_type, _var) _type _var = 0;
	INIT_ALL_SVO_CVARS;
	#undef INIT_SVO_CVAR
};

#endif // FEATURE_SVO_GI
