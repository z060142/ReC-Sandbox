// Copyright 2015-2021 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "PostAA.h"
#include "D3DPostProcess.h"
#include "GraphicsPipeline/LensOptics.h"
#include "GraphicsPipeline/ColorGrading.h"

#include <Common/RenderDisplayContext.h>

struct PostAAConstants
{
	Matrix44 matReprojection;
	Vec4     params;
	Vec4     screenSize;
};

// 4x4 N-Queens pattern
static const Vec2 vNQAA4x[4] =
{
	Vec2(+3.0f / 8.0f, +1.0f / 8.0f),
	Vec2(+1.0f / 8.0f, -3.0f / 8.0f),
	Vec2(-1.0f / 8.0f, +3.0f / 8.0f),
	Vec2(-3.0f / 8.0f, -1.0f / 8.0f)
};

// 5x5 N-Queens pattern
static const Vec2 vNQAA5x[5] =
{
	Vec2(+0.0f / 10.0f, -2.0f / 10.0f),
	Vec2(-2.0f / 10.0f, +4.0f / 10.0f),
	Vec2(+2.0f / 10.0f, +2.0f / 10.0f),
	Vec2(+4.0f / 10.0f, -4.0f / 10.0f),
	Vec2(-4.0f / 10.0f, +0.0f / 10.0f)
};

// 7x7 N-Queens pattern
static const Vec2 vNQAA7x[7] =
{
	Vec2(+4.0f / 14.0f, -6.0f / 14.0f),
	Vec2(-2.0f / 14.0f, +2.0f / 14.0f),
	Vec2(-4.0f / 14.0f, -4.0f / 14.0f),
	Vec2(+6.0f / 14.0f, +4.0f / 14.0f),
	Vec2(+2.0f / 14.0f, -2.0f / 14.0f),
	Vec2(-2.0f / 14.0f, +6.0f / 14.0f),
	Vec2(-6.0f / 14.0f, -0.0f / 14.0f)
};

// 8x8 N-Queens pattern
static const Vec2 vNQAA8x[8] =
{
	Vec2(+7.0f / 16.0f, -7.0f / 16.0f),
	Vec2(+5.0f / 16.0f, +5.0f / 16.0f),
	Vec2(+3.0f / 16.0f, +1.0f / 16.0f),
	Vec2(+1.0f / 16.0f, +7.0f / 16.0f),
	Vec2(-1.0f / 16.0f, -5.0f / 16.0f),
	Vec2(-3.0f / 16.0f, -1.0f / 16.0f),
	Vec2(-5.0f / 16.0f, +3.0f / 16.0f),
	Vec2(-7.0f / 16.0f, -3.0f / 16.0f)
};

static const Vec2 vSSAA2x[2] =
{
	Vec2(-0.25f, +0.25f),
	Vec2(+0.25f, -0.25f)
};

static const Vec2 vSSAA3x[3] =
{
	Vec2(-1.0f / 3.0f, -1.0f / 3.0f),
	Vec2(+1.0f / 3.0f, +0.0f / 3.0f),
	Vec2(+0.0f / 3.0f, +1.0f / 3.0f)
};

static const Vec2 vSSAA4x_regular[4] =
{
	Vec2(-0.25f, -0.25f), Vec2(-0.25f, +0.25f),
	Vec2(+0.25f, -0.25f), Vec2(+0.25f, +0.25f)
};

static const Vec2 vSSAA4x_rotated[4] =
{
	Vec2(-0.125f, -0.375f), Vec2(+0.375f, -0.125f),
	Vec2(-0.375f, +0.125f), Vec2(+0.125f, +0.375f)
};

static const Vec2 vSMAA4x[2] =
{
	Vec2(-0.125f, -0.125f),
	Vec2(+0.125f, +0.125f)
};

static const Vec2 vSSAA8x[8] =
{
	Vec2( 0.0625, -0.1875), Vec2(-0.0625,  0.1875),
	Vec2( 0.3125,  0.0625), Vec2(-0.1875, -0.3125),
	Vec2(-0.3125,  0.3125), Vec2(-0.4375, -0.0625),
	Vec2( 0.1875,  0.4375), Vec2( 0.4375, -0.4375)
};

static const Vec2 vSGSSAA8x8[8] =
{
	Vec2(6.0f / 7.0f, 0.0f / 7.0f) - Vec2(0.5f, 0.5f), Vec2(2.0f / 7.0f, 1.0f / 7.0f) - Vec2(0.5f, 0.5f),
	Vec2(4.0f / 7.0f, 2.0f / 7.0f) - Vec2(0.5f, 0.5f), Vec2(0.0f / 7.0f, 3.0f / 7.0f) - Vec2(0.5f, 0.5f),
	Vec2(7.0f / 7.0f, 4.0f / 7.0f) - Vec2(0.5f, 0.5f), Vec2(3.0f / 7.0f, 5.0f / 7.0f) - Vec2(0.5f, 0.5f),
	Vec2(5.0f / 7.0f, 6.0f / 7.0f) - Vec2(0.5f, 0.5f), Vec2(1.0f / 7.0f, 7.0f / 7.0f) - Vec2(0.5f, 0.5f)
};

void CPostAAStage::CalculateJitterOffsets(int renderWidth, int renderHeight, CRenderView* pRenderView)
{
	pRenderView->m_vProjMatrixSubPixoffset = Vec2(0.0f, 0.0f);

	// TODO: Support temporal AA in the editor
	uint32 aaMode = CRenderer::FX_GetAntialiasingType();

	if (aaMode && gcpRendD3D->IsEditorMode())
		aaMode = 1U << (eAT_SMAA_1X * CRenderer::CV_r_AntialiasingModeEditor);

	if (aaMode & eAT_REQUIRES_SUBPIXELSHIFT_MASK)
	{
		int jitterPattern = CRenderer::CV_r_AntialiasingTAAPattern;
		if (jitterPattern == 1)
		{
			if (aaMode & eAT_SMAA_2TX_MASK)  jitterPattern = 2;
			else if (aaMode & eAT_TSAA_MASK) jitterPattern = 5;
			else                             jitterPattern = 0;
		}

		const int nSampleID = SPostEffectsUtils::m_iFrameCounter;
		Vec2 vCurrSubSample = Vec2(0, 0);
		switch (jitterPattern)
		{
		case -1:
			vCurrSubSample = Vec2(SPostEffectsUtils::srandf(), SPostEffectsUtils::srandf()) * 0.5f;
			break;
		case 2:
			vCurrSubSample = vSSAA2x[nSampleID % 2];
			break;
		case 3:
			vCurrSubSample = vSSAA3x[nSampleID % 3];
			break;
		case 4:
			vCurrSubSample = vSSAA4x_regular[nSampleID % 4];
			break;
		case 5:
			vCurrSubSample = vSSAA4x_rotated[nSampleID % 4];
			break;
		case 6:
			vCurrSubSample = vSSAA8x[nSampleID % 8];
			break;
		case 7:
			vCurrSubSample = vSGSSAA8x8[nSampleID % 8];
			break;
		case 8:
			vCurrSubSample = Vec2(SPostEffectsUtils::HaltonSequence(SPostEffectsUtils::m_iFrameCounter % 8, 2) - 0.5f,
			                      SPostEffectsUtils::HaltonSequence(SPostEffectsUtils::m_iFrameCounter % 8, 3) - 0.5f);
			break;
		case 9:
			vCurrSubSample = Vec2(SPostEffectsUtils::HaltonSequence(SPostEffectsUtils::m_iFrameCounter % 16, 2) - 0.5f,
			                      SPostEffectsUtils::HaltonSequence(SPostEffectsUtils::m_iFrameCounter % 16, 3) - 0.5f);
			break;
		case 10:
			vCurrSubSample = Vec2(SPostEffectsUtils::HaltonSequence(SPostEffectsUtils::m_iFrameCounter % 1024, 2) - 0.5f,
			                      SPostEffectsUtils::HaltonSequence(SPostEffectsUtils::m_iFrameCounter % 1024, 3) - 0.5f);
			break;
		case 11:
			vCurrSubSample = vNQAA4x[nSampleID % 4];
			break;
		case 12:
			vCurrSubSample = vNQAA5x[nSampleID % 5];
			break;
		case 13:
			vCurrSubSample = vNQAA7x[nSampleID % 7];
			break;
		case 14:
			vCurrSubSample = vNQAA8x[nSampleID % 8];
			break;
		}

		const auto& downscaleFactor = gRenDev->GetRenderQuality().downscaleFactor;

		pRenderView->m_vProjMatrixSubPixoffset.x = (vCurrSubSample.x * 2.0f / (float)renderWidth)  / downscaleFactor.x;
		pRenderView->m_vProjMatrixSubPixoffset.y = (vCurrSubSample.y * 2.0f / (float)renderHeight) / downscaleFactor.y;
	}
}

void CPostAAStage::Init()
{
	m_pTexAreaSMAA.Assign_NoAddRef(CTexture::ForName("%ENGINE%/EngineAssets/ScreenSpace/AreaTex.dds", FT_DONT_STREAM, eTF_Unknown));
	m_pTexSearchSMAA.Assign_NoAddRef(CTexture::ForName("%ENGINE%/EngineAssets/ScreenSpace/SearchTex.dds", FT_DONT_STREAM, eTF_Unknown));
	m_lastFrameID = -1;

	m_passTemporalAA.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_VS);
	m_passTemporalAA.AllocateTypedConstantBuffer<PostAAConstants>(eConstantBufferShaderSlot_PerPrimitive, EShaderStage_Pixel);
}

void CPostAAStage::ApplySMAA(CTexture*& pCurrRT, CTexture*& pDestRT)
{
	CTexture* pEdgesRT        = m_graphicsPipelineResources.m_pTexClipVolumes;      // Pick a 2-channel texture
	CTexture* pBlendWeightsRT = m_graphicsPipelineResources.m_pTexHDRTargetMasked;  // Reusing ESRAM resident target (FP16 RT accessed using point filtering which gives full rate on GCN)
	CTexture* pSTexture       = RenderView()->GetDepthTarget();

	if (!pEdgesRT || !pBlendWeightsRT)
		return;

#if DURANGO_USE_ESRAM
	pBlendWeightsRT->AcquireESRAMResidency(CDeviceResource::eResCoherence_Uninitialize);
#endif

	// Prepare stencil prepass
	int stencilRef = -1;
	if (CRenderer::CV_r_AntialiasingModeSCull)
	{
		m_graphicsPipeline.m_nStencilMaskRef += 1;

		if (m_graphicsPipeline.m_nStencilMaskRef > STENC_MAX_REF)
		{
			CClearSurfacePass::Execute(pSTexture, CLEAR_STENCIL, 0.0f, 0);
			m_graphicsPipeline.m_nStencilMaskRef = 1;
		}

		stencilRef = m_graphicsPipeline.m_nStencilMaskRef;
	}

	// Pass 1: Edge Detection
	{
		if (m_passSMAAEdgeDetection.IsDirty(pCurrRT->GetTextureID(), pSTexture->GetTextureID(), CRenderer::CV_r_AntialiasingModeSCull))
		{
			static CCryNameTSCRC techEdgeDetection("LumaEdgeDetectionSMAA");
			m_passSMAAEdgeDetection.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
			m_passSMAAEdgeDetection.SetPrimitiveType(CRenderPrimitive::ePrim_ProceduralTriangle);
			m_passSMAAEdgeDetection.SetTechnique(CShaderMan::s_shPostAA, techEdgeDetection, 0);
			m_passSMAAEdgeDetection.SetTargetClearMask(CPrimitiveRenderPass::eClear_Color0);
			m_passSMAAEdgeDetection.SetRenderTarget(0, pEdgesRT);
			if (CRenderer::CV_r_AntialiasingModeSCull)
				m_passSMAAEdgeDetection.SetDepthTarget(pSTexture);
			m_passSMAAEdgeDetection.SetState(GS_NODEPTHTEST);
			m_passSMAAEdgeDetection.SetRequirePerViewConstantBuffer(true);
			m_passSMAAEdgeDetection.SetTexture(0, pCurrRT, EDefaultResourceViews::Linear);
			m_passSMAAEdgeDetection.SetSampler(0, EDefaultSamplerStates::PointClamp);
		}

		if (CRenderer::CV_r_AntialiasingModeSCull)
		{
			m_passSMAAEdgeDetection.SetState(GS_NODEPTHTEST | GS_STENCIL);
			m_passSMAAEdgeDetection.SetStencilState(
				STENC_FUNC(FSS_STENCFUNC_ALWAYS) |
				STENCOP_FAIL(FSS_STENCOP_REPLACE) |
				STENCOP_ZFAIL(FSS_STENCOP_REPLACE) |
				STENCOP_PASS(FSS_STENCOP_REPLACE),
				(uint8)stencilRef);
		}

		m_passSMAAEdgeDetection.BeginConstantUpdate();

		{
			static CCryNameR paramsName("vParams");

			float threshold = CRenderer::CV_r_AntialiasingSMAAThreshold;
			const Vec4 params(max(threshold, 0.0f), 0, 0, 0);
			m_passSMAAEdgeDetection.SetConstant(paramsName, params, eHWSC_Pixel);
		}

		m_passSMAAEdgeDetection.Execute();
	}

	// Pass 2: Generate blend weight map
	{
		if (m_passSMAABlendWeights.IsDirty(pSTexture->GetTextureID(), pBlendWeightsRT->GetTextureID(), CRenderer::CV_r_AntialiasingModeSCull))
		{
			static CCryNameTSCRC techBlendWeights("BlendWeightSMAA");
			m_passSMAABlendWeights.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
			m_passSMAABlendWeights.SetPrimitiveType(CRenderPrimitive::ePrim_ProceduralTriangle);
			m_passSMAABlendWeights.SetTechnique(CShaderMan::s_shPostAA, techBlendWeights, 0);
			m_passSMAABlendWeights.SetTargetClearMask(CPrimitiveRenderPass::eClear_Color0);
			m_passSMAABlendWeights.SetRenderTarget(0, pBlendWeightsRT);
			if (CRenderer::CV_r_AntialiasingModeSCull)
				m_passSMAABlendWeights.SetDepthTarget(pSTexture);
			m_passSMAABlendWeights.SetState(GS_NODEPTHTEST);
			m_passSMAABlendWeights.SetTexture(0, pEdgesRT, EDefaultResourceViews::Linear);
			m_passSMAABlendWeights.SetTexture(1, m_pTexAreaSMAA);
			m_passSMAABlendWeights.SetTexture(2, m_pTexSearchSMAA);
			m_passSMAABlendWeights.SetSampler(0, EDefaultSamplerStates::PointClamp);
			m_passSMAABlendWeights.SetSampler(1, EDefaultSamplerStates::LinearClamp);
		}

		if (CRenderer::CV_r_AntialiasingModeSCull)
		{
			m_passSMAABlendWeights.SetState(GS_NODEPTHTEST | GS_STENCIL);
			m_passSMAABlendWeights.SetStencilState(
				STENC_FUNC(FSS_STENCFUNC_EQUAL) |
				STENCOP_FAIL(FSS_STENCOP_KEEP) |
				STENCOP_ZFAIL(FSS_STENCOP_KEEP) |
				STENCOP_PASS(FSS_STENCOP_KEEP),
				(uint8)stencilRef);
		}

		m_passSMAABlendWeights.BeginConstantUpdate();
		m_passSMAABlendWeights.Execute();
	}

	// Final Pass: Blend neighborhood pixels
	{
		if (m_passSMAANeighborhoodBlending.IsDirty(pCurrRT->GetTextureID(), pBlendWeightsRT->GetTextureID()))
		{
			static CCryNameTSCRC techNeighborhoodBlending("NeighborhoodBlendingSMAA");
			m_passSMAANeighborhoodBlending.SetPrimitiveFlags(CRenderPrimitive::eFlags_None);
			m_passSMAANeighborhoodBlending.SetPrimitiveType(CRenderPrimitive::ePrim_ProceduralTriangle);
			m_passSMAANeighborhoodBlending.SetTechnique(CShaderMan::s_shPostAA, techNeighborhoodBlending, 0);
			m_passSMAANeighborhoodBlending.SetRenderTarget(0, pDestRT);
			m_passSMAANeighborhoodBlending.SetState(GS_NODEPTHTEST);
			m_passSMAANeighborhoodBlending.SetTexture(0, pBlendWeightsRT, EDefaultResourceViews::Linear);
			m_passSMAANeighborhoodBlending.SetTexture(1, pCurrRT, EDefaultResourceViews::Linear);
			m_passSMAANeighborhoodBlending.SetSampler(0, EDefaultSamplerStates::PointClamp);
			m_passSMAANeighborhoodBlending.SetSampler(1, EDefaultSamplerStates::LinearClamp);
		}

		m_passSMAANeighborhoodBlending.Execute();
	}

#if DURANGO_USE_ESRAM
	pBlendWeightsRT->ForfeitESRAMResidency(CDeviceResource::eResCoherence_Abandon);
#endif

	std::swap(pCurrRT, pDestRT);
}

void CPostAAStage::ApplyTemporalAA(CTexture*& pCurrRT, CTexture*& pMgpuRT, uint32 aaMode)
{
	CTexture* pDestRT = GetAARenderTarget(RenderView(), true);
	CTexture* pPrevRT = ((SPostEffectsUtils::m_iFrameCounter - m_lastFrameID) < 10) ? GetAARenderTarget(RenderView(), false) : pCurrRT;

	CRY_ASSERT(pDestRT && pPrevRT, "PostAA rendertargets do not exist!");
	if (!pDestRT || !pPrevRT)
		return;

	uint64 rtMask = 0;
	if (aaMode & (eAT_SMAA_1TX_MASK))
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE2];
	else if (aaMode & eAT_TSAA_MASK)
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE3];

	{
		static CCryNameTSCRC techTemporalAA("PostAA");
		m_passTemporalAA.SetPrimitiveFlags(CRenderPrimitive::eFlags_None);
		m_passTemporalAA.SetTechnique(CShaderMan::s_shPostAA, techTemporalAA, rtMask);
		m_passTemporalAA.SetRenderTarget(0, pDestRT);
		m_passTemporalAA.SetState(GS_NODEPTHTEST);
		m_passTemporalAA.SetRequireWorldPos(true);
		m_passTemporalAA.SetRequirePerViewConstantBuffer(true);
		m_passTemporalAA.SetFlags(CPrimitiveRenderPass::ePassFlags_RequireVrProjectionConstants);

		m_passTemporalAA.SetTexture(0, pCurrRT, EDefaultResourceViews::Linear);
		m_passTemporalAA.SetTexture(1, pPrevRT, EDefaultResourceViews::Linear);
		m_passTemporalAA.SetTexture(2, m_graphicsPipelineResources.m_pTexLinearDepth);
		m_passTemporalAA.SetTexture(3, GetUtils().GetVelocityObjectRT(RenderView()));

		m_passTemporalAA.SetSampler(0, EDefaultSamplerStates::LinearClamp);
		m_passTemporalAA.SetSampler(1, EDefaultSamplerStates::PointClamp);
		m_passTemporalAA.SetTexture(16, RenderView()->GetDepthTarget());
	}

	(pMgpuRT = pDestRT)->MgpuResourceUpdate(true);
	m_passTemporalAA.BeginConstantUpdate();

	{
		size_t viewInfoCount = RenderView()->GetViewInfoCount();

		auto constants = m_passTemporalAA.BeginTypedConstantUpdate<PostAAConstants>(eConstantBufferShaderSlot_PerPrimitive, EShaderStage_Pixel);

		auto screenResolution = Vec2i(m_graphicsPipeline.GetRenderResolution().x, m_graphicsPipeline.GetRenderResolution().y);
		const float rcpWidth = 1.0f / (float)screenResolution.x;
		const float rcpHeight = 1.0f / (float)screenResolution.y;
		constants->screenSize = Vec4((float)screenResolution.x, (float)screenResolution.y, rcpWidth, rcpHeight);

		constants->params = Vec4(max(CRenderer::CV_r_AntialiasingTAASharpening + 1.0f, 1.0f), 0.0f, CRenderer::CV_r_AntialiasingTAAFalloffLowFreq + 1e-6f, CRenderer::CV_r_AntialiasingTAAFalloffHiFreq + 1e-6f);
		if (aaMode & eAT_TSAA_MASK)
			constants->params = Vec4(CRenderer::CV_r_AntialiasingTSAASubpixelDetection, CRenderer::CV_r_AntialiasingTSAASmoothness, 0, 0);

		constants->matReprojection = RenderView()->GetViewInfo(CCamera::eEye_Left).GetReprojection();

		if (viewInfoCount > 1)
		{
			constants.BeginStereoOverride(true);
			constants->matReprojection = RenderView()->GetViewInfo(CCamera::eEye_Right).GetReprojection();
		}

		m_passTemporalAA.EndTypedConstantUpdate(constants);
	}

	m_passTemporalAA.Execute();

	pCurrRT = pDestRT;
	m_lastFrameID = SPostEffectsUtils::m_iFrameCounter;
}

void CPostAAStage::DoFinalComposition(CTexture*& pCurrRT, CTexture* pDestRT, uint32 aaMode)
{
	PROFILE_LABEL_SCOPE("FLARES, GRAIN");

	CTexture* pTexLensOptics = m_graphicsPipelineResources.m_pTexSceneTargetR11G11B10F[0];
	CRY_ASSERT(pCurrRT != pDestRT);

	Vec4 hdrSetupParams[5];
	gEnv->p3DEngine->GetHDRSetupParams(hdrSetupParams);

	// D9 (SceneReferredSpec.md): the three stock LDR operations that live inside this composition
	// pass are inert while the scene-referred pipeline runs. The pass itself is NOT skippable -
	// with the post-effect stage active it is the only path from the display target to the back
	// buffer - so what is gated here is the work inside it, not the pass.
	//
	//   film grain  - an overlay blend pivoting on 0.5, only defined on [0,1]. Comes back before
	//                 the ODT as a sensor effect in S7.
	//   sharpening  - sqrt(saturate(lerp(taps^2, c^2))), a gamma-space operator by construction.
	//   flare chart - the same 8-bit display-referred chart the tone map no longer applies.
	//
	// The viewfinder overlays, the lens-optics composite, the squeeze/letterbox remap and the
	// lateral chromatic aberration all stay: they are ours, or they are geometry rather than
	// grading.
	const bool bSceneReferred = CRendererResources::IsSceneReferredStage(5);
	// ------------------------------------------------------------------------------------------
	// Capture-side film grain (FilmGrainSpec.md). The cinematic camera publishes the whole block
	// on the post-effect bus and this pass is its only consumer: a grain with a size in
	// micrometres on the negative, a response over tone, colour structure per layer and a fresh
	// deterministic pattern per CAPTURED frame, in place of the engine's overlay of a white-noise
	// volume addressed in frame coordinates and animated off the wall clock.
	//
	// Three things have to be true before any of it happens - the camera asked (Active), the kill
	// switch allows it (r_FilmGrain), and there is an amount to apply - so a stock frame, a frame
	// with no cinematic camera, and r_FilmGrain 0 all fall through to the engine's own path with
	// the permutation, the constants and the picture untouched.
	// ------------------------------------------------------------------------------------------
	const bool bCineGrainRequested = CRenderer::CV_r_FilmGrain != 0
	                                 && PostEffectMgr()->GetByNameF("Grain_User_Active") >= 0.5f;

	Vec4 grainAmountVec(ZERO), grainSizeVec(ZERO), grainSensorVec(ZERO), grainSeedVec(ZERO);
	Vec4 grainDigital0Vec(ZERO), grainDigital1Vec(ZERO), grainDigital2Vec(ZERO), grainDigital3Vec(ZERO);
	int  grainFamily = 0;
	bool bCineGrain = false;
	if (bCineGrainRequested)
	{
		grainAmountVec   = PostEffectMgr()->GetByNameVec4("Grain_User_Amount");
		grainSizeVec     = PostEffectMgr()->GetByNameVec4("Grain_User_Size");
		grainSensorVec   = PostEffectMgr()->GetByNameVec4("Grain_User_Sensor");
		grainSeedVec     = PostEffectMgr()->GetByNameVec4("Grain_User_Seed");
		grainDigital0Vec = PostEffectMgr()->GetByNameVec4("Grain_User_Digital0");
		grainDigital1Vec = PostEffectMgr()->GetByNameVec4("Grain_User_Digital1");
		grainDigital2Vec = PostEffectMgr()->GetByNameVec4("Grain_User_Digital2");
		grainDigital3Vec = PostEffectMgr()->GetByNameVec4("Grain_User_Digital3");
		grainFamily      = (int)(PostEffectMgr()->GetByNameF("Grain_User_Family") + 0.5f);

		// An all-zero amount is the camera saying "no grain this frame" (grain switched off, ISO
		// at base with strength 0). Taking the permutation for it would cost a shader switch to
		// compute nothing, so it counts as inactive.
		bCineGrain = (grainAmountVec.x + grainAmountVec.y + grainAmountVec.z) > 0.0f;
	}

	// The freeze holds the capture-frame index the shader seeds from. The CAMERA keeps counting -
	// only what is published to the shader is held - so releasing the freeze does not rewind the
	// grain to where it was pinned. Latched on the transition so the frozen frame is the one that
	// was on screen when the freeze was thrown, not frame 0.
	const bool bFreezeRequested = CRenderer::CV_r_FilmGrainFreeze != 0 || grainSeedVec.z >= 0.5f;
	if (bCineGrain)
	{
		if (bFreezeRequested && !m_bFilmGrainFrozen)
			m_fFilmGrainFrozenIndex = grainSeedVec.y;
		m_bFilmGrainFrozen = bFreezeRequested;
		if (bFreezeRequested)
			grainSeedVec.y = m_fFilmGrainFrozenIndex;
	}
	else
	{
		m_bFilmGrainFrozen = false;
	}

	// Calculate grain amount
	CEffectParam* pParamGrainAmount = PostEffectMgr()->GetByName("FilterGrain_Amount");
	CEffectParam* pParamArtifactsGrain = PostEffectMgr()->GetByName("FilterArtifacts_Grain");
	const float paramGrainAmount = max(pParamGrainAmount->GetParam(), pParamArtifactsGrain->GetParam());
	const float environmentGrainAmount = hdrSetupParams[1].w * CRenderer::CV_r_HDRGrainAmount;
	// While the capture-side block runs, the stock amount is forced to 0: two grains on one
	// frame is not a look, it is a bug, and the camera is the authority over what the negative
	// records. The plugin still writes FilterGrain_Amount every frame, so r_FilmGrain 0 lands
	// straight back on the engine's grain at the amount the camera asked for. The scene-referred
	// gate of D9 is unchanged and independent: on that path the stock overlay grain is inert
	// whether or not a cinematic camera is driving, and our block is what takes the slot.
	const float grainAmount = (bSceneReferred || bCineGrain) ? 0.0f : max(paramGrainAmount, environmentGrainAmount);

	uint64 rtMask = 0;
	if ((aaMode & (eAT_SMAA_2TX_MASK | eAT_TSAA_MASK)) && !bSceneReferred)
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE2];
	if (CRenderer::CV_r_colorRangeCompression)
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE4];
	if (grainAmount && CRenderer::CV_r_GrainEnableExposureThreshold) // enable legacy grain/exposure interaction
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE0];
	// SAMPLE6 was the one free runtime flag on this technique (0 legacy grain exposure, 1 lens
	// optics, 2 sharpen, 3 chroma shift, 4 range compression, 5 flare colour chart). It selects
	// the capture-side grain block and nothing else, so it adds exactly one permutation and only
	// when a cinematic camera is driving the frame.
	if (bCineGrain)
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE6];

	auto* pLensOpticStage = m_graphicsPipeline.GetStage<CLensOpticsStage>();
	if (pLensOpticStage && pLensOpticStage->HasContent())
	{
		rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE1];
		if (CRenderer::CV_r_FlaresChromaShift > 0.5f / (float)m_graphicsPipeline.GetRenderResolution().x)  // Only relevant if bigger than half pixel
			rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE3];
	}

	CTexture* pColorChartTex = CRendererResources::s_ptexBlack;
	auto* pColorGradingStage = m_graphicsPipeline.GetStage<CColorGradingStage>();
	if (CRenderer::CV_r_FlaresEnableColorGrading && pColorGradingStage && !bSceneReferred)
	{
		if (CTexture* pColorChartTexTentative = pColorGradingStage->GetColorChart())
		{
			pColorChartTex = pColorChartTexTentative;
			rtMask |= g_HWSR_MaskBit[HWSR_SAMPLE5];
		}
	}

	int lumID = CRendererResources::s_ptexCurLumTexture ? CRendererResources::s_ptexCurLumTexture->GetTextureID() : 0;
	if (m_passComposition.IsDirty(pCurrRT->GetID(), pDestRT->GetID(), pColorChartTex->GetID(), lumID, rtMask))
	{
		static CCryNameTSCRC techComposition("PostAAComposites");

		m_passComposition.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
		m_passComposition.SetPrimitiveType(CRenderPrimitive::ePrim_ProceduralTriangle);
		m_passComposition.SetTechnique(CShaderMan::s_shPostAA, techComposition, rtMask);
		m_passComposition.SetRenderTarget(0, pDestRT);
		m_passComposition.SetState(GS_NODEPTHTEST);

		m_passComposition.SetTexture(0, pCurrRT, EDefaultResourceViews::Linear);
		// Linear depth for the focus peaking overlay. Bound unconditionally (cheaper than making
		// the pass dirty every time peaking toggles); the shader only reads it when peaking is on.
		// It is read with a NORMALISED coordinate through the pass' PointClamp sampler (s1), not
		// with Load(): this pass runs at DISPLAY resolution while $ZTarget is at RENDER resolution,
		// so pixel coordinates derived from PS_ScreenSize address the wrong texels (or none at
		// all) the moment the two differ.
		m_passComposition.SetTexture(4, m_graphicsPipelineResources.m_pTexLinearDepth);
		m_passComposition.SetTexture(5, pTexLensOptics);
		m_passComposition.SetTexture(6, CRendererResources::s_ptexFilmGrainMap);
		m_passComposition.SetTexture(7, CRendererResources::s_ptexCurLumTexture ? CRendererResources::s_ptexCurLumTexture : CRendererResources::s_ptexBlack);
		m_passComposition.SetTexture(8, pColorChartTex);

		m_passComposition.SetSampler(0, EDefaultSamplerStates::LinearClamp);
		m_passComposition.SetSampler(1, EDefaultSamplerStates::PointClamp);
		m_passComposition.SetSampler(2, EDefaultSamplerStates::PointWrap);
	}

	m_passComposition.BeginConstantUpdate();

	{
		static CCryNameR paramsName("vParams");
		static CCryNameR lensOpticsParamsName("vLensOpticsParams");
		static CCryNameR hdrParamsName("HDRParams");
		static CCryNameR hdrEyeAdaptationName("HDREyeAdaptation");
		static CCryNameR lensDistortionName("vLensDistortionParams");
		static CCryNameR lensGuideName("vLensGuideParams");
		static CCryNameR lensPeakingName("vLensPeakingParams");
		static CCryNameR lensPeakingDebugName("vLensPeakingDebugParams");

		float sharpening = CRenderer::CV_r_AntialiasingTAASharpening;
		const Vec4 params(max(1.0f + sharpening, 1.0f), 0, 0, 0);
		m_passComposition.SetConstant(paramsName, params, eHWSC_Pixel);

		const Vec4 lensOpticsParams(1.0f, 1.0f, 1.0f, CRenderer::CV_r_FlaresChromaShift);
		m_passComposition.SetConstant(lensOpticsParamsName, lensOpticsParams, eHWSC_Pixel);

		// Apply grain (final luminance texture doesn't get its final value baked, so we have to replicate the entire hdr eye adaption)
		const Vec4 v = Vec4(0, 0, 0, grainAmount);

		m_passComposition.SetConstant(hdrParamsName, v, eHWSC_Pixel);
		// This sets exposure clamping max,min, causing weird interaction between between Exp. min/max params and grain (CE-13325)
		// it will be ignored if CV_r_GrainEnableExposureThreshold is 0
		m_passComposition.SetConstant(hdrEyeAdaptationName, hdrSetupParams[4], eHWSC_Pixel);

		// The capture-side grain block. Set only when the permutation that reads it is bound: the
		// constants do not exist in the stock shader, and a reflected SetConstant for a name the
		// shader does not declare is work for nothing. They travel in the pass' existing reflected
		// per-primitive constant buffer, like every other constant here - a typed buffer would
		// mean converting the whole pass, and there is nothing about seven vectors that needs one.
		if (bCineGrain)
		{
			static CCryNameR grainAmountName("vGrainAmount");
			static CCryNameR grainSizeName("vGrainSize");
			static CCryNameR grainSensorName("vGrainSensor");
			static CCryNameR grainSeedName("vGrainSeed");
			static CCryNameR grainDigital0Name("vGrainDigital0");
			static CCryNameR grainDigital1Name("vGrainDigital1");
			static CCryNameR grainDigital2Name("vGrainDigital2");
			static CCryNameR grainDigital3Name("vGrainDigital3");
			static CCryNameR grainDebugName("vGrainDebug");

			// The output width the film-space mapping divides by is the DISPLAY resolution, which
			// is what this pass runs at and what PS_ScreenSize carries in it.
			const int outputWidthPx = max(pDestRT->GetWidth(), 1);
			const float sensorWidthMm = max(grainSensorVec.x, 1.0f);
			const float fpUm = (grainSensorVec.w > 0.0f) ? grainSensorVec.w
			                                             : (sensorWidthMm * 1000.0f / (float)outputWidthPx);

			// The eye index decorrelates the two fields in stereo. Identical grain in both eyes
			// fuses onto the screen plane instead of onto the negative, which is the one thing
			// grain must never do.
			const float viewIndex = (float)(int)RenderView()->GetCurrentEye();
			// z carries the FAMILY. It rides in this vector rather than in a static flag because
			// there was exactly one free %_RT_ bit on this technique and the block itself spent it;
			// a second family flag would double the permutation count of the composition pass for a
			// branch on a value that is uniform over the whole frame. The shader reads it as
			// 0 Film / 1 CMOS / 2 CCD / 3 Phone.
			const Vec4 grainDebug((float)CRenderer::CV_r_FilmGrainDebug, viewIndex, (float)grainFamily, 0.0f);

			m_passComposition.SetConstant(grainAmountName, grainAmountVec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainSizeName, grainSizeVec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainSensorName, grainSensorVec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainSeedName, grainSeedVec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainDigital0Name, grainDigital0Vec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainDigital1Name, grainDigital1Vec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainDigital2Name, grainDigital2Vec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainDigital3Name, grainDigital3Vec, eHWSC_Pixel);
			m_passComposition.SetConstant(grainDebugName, grainDebug, eHWSC_Pixel);

			if (CRenderer::CV_r_FilmGrainDebug == 3)
			{
				const float fNow = gEnv->pTimer->GetAsyncCurTime();
				if ((fNow - m_fFilmGrainReportTime) >= 1.0f)
				{
					m_fFilmGrainReportTime = fNow;
					PrintFilmGrainReport(grainAmountVec, grainSizeVec, grainSensorVec, grainSeedVec,
					                     grainDigital0Vec, grainDigital1Vec, grainDigital2Vec, grainDigital3Vec,
					                     grainFamily, fpUm, outputWidthPx, bFreezeRequested);
				}
			}
		}

		{
			// Radial lens distortion (r_LensDistortion / lens character): x = k1, y = aspect,
			// z = letterbox aspect (r_LensLetterbox; 0 = off, negative = squeezed presentation),
			// w = lateral chromatic aberration. CA is distortion done per wavelength, so it
			// belongs in this constant and in this coordinate chain: it is applied after the
			// remap and therefore follows barrel/pincushion and the squeeze for free.
			const float kChromaticAberration = 0.008f;   // radial scale split at amount 1: ~4-6 px in a 1080p corner
			const float aspect = (float)CRendererResources::s_displayWidth / (float)max(CRendererResources::s_displayHeight, 1);
			const Vec4 distortionParams(clamp_tpl(CRenderer::CV_r_LensDistortion, -0.9f, 0.9f), aspect, CRenderer::CV_r_LensLetterbox,
			                            kChromaticAberration * clamp_tpl(CRenderer::CV_r_LensChromaticAberration, 0.0f, 1.0f));
			m_passComposition.SetConstant(lensDistortionName, distortionParams, eHWSC_Pixel);

			// Viewfinder overlays: frame guides and focus peaking. Never part of the picture, so
			// r_LensOverlays 0 zeroes both drivers and the shader falls back to the untouched image.
			const bool bOverlays = CRenderer::CV_r_LensOverlays != 0;
			const Vec4 guideParams(bOverlays ? (float)CRenderer::CV_r_LensGuides : 0.0f,
			                       max(CRenderer::CV_r_LensGuideAspect, 0.01f),
			                       clamp_tpl(CRenderer::CV_r_LensGuideOpacity, 0.0f, 1.0f),
			                       aspect);
			m_passComposition.SetConstant(lensGuideName, guideParams, eHWSC_Pixel);

			// The peaking band goes to the shader in METRES; the shader converts the linear depth
			// target with the engine's own PS_NearFarClipDist.y, exactly like CompositeDofPS does.
			// Normalising on the CPU with a far plane fetched here proved unsafe: it did not match
			// the value $ZTarget was normalised with, and the band landed in front of the true
			// depth of field (user saw only the near out-of-focus zone tinted).
			const float farPlane = max(RenderView()->GetViewInfo(CCamera::eEye_Left).farClipPlane, 1.0f);
			const float peakNear = max(CRenderer::CV_r_LensPeakingNear, 0.0f);
			// Both limits still at 0 means "no band pushed yet", not "nothing is in focus": fall
			// back to the whole depth range so console-driven peaking still shows something.
			const float peakFar = (CRenderer::CV_r_LensPeakingFar > CRenderer::CV_r_LensPeakingNear)
			                      ? CRenderer::CV_r_LensPeakingFar
			                      : farPlane;
			const Vec4 peakingParams(bOverlays ? max(CRenderer::CV_r_LensPeaking, 0.0f) : 0.0f,
			                         peakNear,
			                         peakFar,
			                         (float)CRenderer::CV_r_LensPeakingColor);
			m_passComposition.SetConstant(lensPeakingName, peakingParams, eHWSC_Pixel);

			// x = r_LensPeakingDebug: 0 off, 1 = solid in-focus mask, 2 = depth ramp.
			const Vec4 peakingDebugParams(bOverlays ? (float)CRenderer::CV_r_LensPeakingDebug : 0.0f, 0, 0, 0);
			m_passComposition.SetConstant(lensPeakingDebugName, peakingDebugParams, eHWSC_Pixel);
		}
	}

	m_passComposition.Execute();
}

// The report r_FilmGrainDebug 3 prints once a second. Everything the block decided this frame,
// said in the units the camera's own controls are in - micrometres on the negative, output pixels,
// stops - and ending in the one sentence the mode exists to produce: whether the grain that is
// being asked for is large enough to survive the 8-bit write at the end of this pass.
//
// It is several short lines rather than one long one, for the same reason the wave-glare report is.
void CPostAAStage::PrintFilmGrainReport(const Vec4& amount, const Vec4& size, const Vec4& sensor,
                                        const Vec4& seed, const Vec4& digital0, const Vec4& digital1,
                                        const Vec4& digital2, const Vec4& digital3, int family,
                                        float fpUm, int outputWidthPx, bool bFrozen)
{
	static const char* const s_szFamily[] = { "Film", "CMOS", "CCD", "Phone" };
	const char* szFamily = (family >= 0 && family < 4) ? s_szFamily[family] : "unknown";

	// A grain cell in OUTPUT pixels. Below 1 the cell is smaller than a pixel and the pixel sees
	// the mean of several of them, which is where the footprint integration takes the amplitude
	// down; above 1 the cell is resolved and the grain has a visible texture.
	const float cellPxR = size.x / max(fpUm, 1e-4f);
	const float cellPxG = size.y / max(fpUm, 1e-4f);
	const float cellPxB = size.z / max(fpUm, 1e-4f);

	// What actually lands on the frame at the response's peak, as a fraction of the value, and
	// then in 8-bit code values at mid grey - the number that decides whether it is visible at all.
	// sigma(x') / x' = ln2 * g for small g; at x = 0.18 the sRGB slope carries it to code values.
	const float gPeak = max(max(amount.x, amount.y), amount.z);
	const float integration = min(1.0f, ((size.x + size.y + size.z) / 3.0f) / max(fpUm, 1e-4f));
	const float sigmaRel = 0.6931472f * gPeak * integration;
	// d(sRGB)/d(linear) at linear 0.18 is 1.055/2.4 * x^(1/2.4 - 1); times 255 for code values.
	const float slope255 = 255.0f * (1.055f / 2.4f) * powf(0.18f, 1.0f / 2.4f - 1.0f);
	const float codeValues = sigmaRel * 0.18f * slope255;

	const char* szVerdict =
	  (gPeak <= 0.0f)        ? "NOTHING is being asked for - the camera published an amount of 0" :
	  (codeValues >= 3.0f)   ? "plainly visible" :
	  (codeValues >= 1.0f)   ? "visible - about one to three code values, which is what film grain on a good transfer measures" :
	  (codeValues >= 0.4f)   ? "faint: under one code value, so the 8-bit write eats most of it" :
	                           "invisible: far below one code value at the 8-bit write - raise Grain Strength or the ISO";

	CryLog("[POSTAA] ---- capture-side film grain, once a second (r_FilmGrainDebug 3) ----");
	CryLog("[POSTAA]   FAMILY: %s%s", szFamily, bFrozen ? "  (capture frame index FROZEN by r_FilmGrainFreeze)" : "");
	CryLog("[POSTAA]   AMOUNT: R %.3f  G %.3f  B %.3f   channel correlation %.2f (1 = one monochrome grain, 0 = three independent layers)",
	       amount.x, amount.y, amount.z, amount.w);
	CryLog("[POSTAA]   SIZE:   R %.2f  G %.2f  B %.2f um on the negative"
	       " -- %.2f / %.2f / %.2f output pixels per grain cell",
	       size.x, size.y, size.z, cellPxR, cellPxG, cellPxB);
	CryLog("[POSTAA]   FILM:   sensor %.2f mm wide, squeeze %.2fx, output %d px"
	       " -- one output pixel covers %.2f um of negative, so %.2f grain cells fall under it",
	       sensor.x, sensor.y, outputWidthPx, fpUm, max(fpUm / max(size.y, 1e-4f), 0.0f));
	CryLog("[POSTAA]   SEED:   shot %d, capture frame %d, algorithm version %d"
	       " -- the same three numbers always give the same grain",
	       (int)seed.x, (int)seed.y, (int)seed.w);
	CryLog("[POSTAA]   RESULT: about %.2f%% of the value at the response peak, ~%.2f code values of 255 on a mid grey card -- %s",
	       sigmaRel * 100.0f, codeValues, szVerdict);

	if (family == 0)
		return;

	// The digital families run a sensor instead of an emulsion, and none of the numbers above says
	// anything about it: the size that matters is the photosite, the amount is emergent from the
	// electron count, and the interesting question is always "how many electrons is a mid grey
	// here" - which is what decides everything else. So the same four questions are answered again
	// in the sensor's own units, ending in the code values the shader will actually produce.
	const float fullWell  = max(digital0.x, 1.0f);
	const float isoBase   = max(digital0.y, 1.0f);
	const float iso       = max(digital0.z, 1.0f);
	const float sigmaRead = max(digital0.w, 0.0f);
	const float sensorPx  = clamp_tpl(sensor.z, 64.0f, 32768.0f);
	const float pitchUm   = sensor.x * 1000.0f / max(sensorPx, 1.0f);
	const float photositesPerPx = max(sensorPx / (float)max(outputWidthPx, 1), 1e-3f);
	const float nPix      = max(photositesPerPx * photositesPerPx, 1.0f);
	const float stackN    = max(digital2.z, 1.0f);

	// The sensor's sigma in DISPLAY-LINEAR units at a given value, green channel (white-balance
	// gain 1 by definition, so this is the honest middle of the three; red and blue are louder by
	// their gains, which is where the colour in the noise is). This is the shader's own
	// FilmGrainSensorField arithmetic, analytic branch, written out once and evaluated twice.
	//
	// Since 2026-09-10 the digital families apply this ADDITIVELY (x' = x + amount*delta), so the
	// number that matters is sigma in CODE VALUES, and it has to be quoted at more than one tone:
	// sensor noise is a roughly constant number of electrons, so it is a roughly constant number of
	// linear units, which the sRGB encoding then turns into MANY code values in the shadows and few
	// in the highlights. One number at mid grey says nothing about where the noise actually lives.
	const float wellScale = isoBase / iso;
	const float k         = 1.0f / max(fullWell * wellScale, 1e-6f);
	const float kPrnu     = max(digital1.x, 0.0f);
	const float kDsnu     = max(digital1.y, 0.0f);
	const float kRow      = max(digital1.z, 0.0f);
	const float smear     = max(digital2.w, 0.0f);
	// The exponent is a preset value on the bus now (Grain_User_Digital3.x); negative means "use
	// the shader's compile-time fallback", which is the same 0.7.
	const float integExp  = (digital3.x >= 0.0f) ? min(digital3.x, 2.0f) : 0.7f;
	const float attenuation = 1.0f / sqrtf(powf(nPix, integExp) * stackN);
	const float amountG   = max(amount.y, 0.0f);

	// d(sRGB)/d(linear): the linear segment below the sRGB break, the power law above it.
	const auto slopeAt = [](float xLin)
	{
		return (xLin <= 0.0031308f) ? 12.92f : ((1.055f / 2.4f) * powf(max(xLin, 1e-8f), 1.0f / 2.4f - 1.0f));
	};
	// The column / smear gate, exactly as the shader computes it: zero in a normally exposed scene,
	// opening only as the picture approaches clipping. And the amplitude, as a fraction of the
	// SATURATING COLUMN'S CHARGE at this ISO - fullWell * wellScale, not the bare full well.
	const auto colGateAt = [](float xLin)
	{
		const float t = clamp_tpl((xLin - 0.75f) / 0.25f, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	const auto smearEAt = [&](float xLin) { return smear * 0.01f * fullWell * wellScale * colGateAt(xLin); };
	const auto eAt      = [&](float xLin) { return xLin * fullWell * wellScale; };

	// ONE TERM, in 8-bit code values. Each term is a coefficient in ELECTRONS multiplying a
	// unit-variance draw, so what it puts on the frame is coeff * k * attenuation * amount in
	// display-linear units, and 255 * that * d(sRGB)/d(linear) in code values at that tone. Quoted
	// on GREEN, whose raw white-balance gain is 1 by definition; red is louder by digital2.x and
	// blue by digital2.y, which is where the colour of the noise comes from.
	//
	// This breakdown exists because the terms were once tuned blind against a multiplicative path
	// that clamped them, and when the path went additive their true magnitudes turned out to be
	// orders of magnitude apart. One line per term, at two tones, so that can never happen again.
	const auto cvOf = [&](float sigmaE, float xLin)
	{
		return 255.0f * (sigmaE * k * attenuation * amountG) * slopeAt(xLin);
	};

	const auto sigmaLinAt = [&](float xLin)
	{
		const float e = eAt(xLin);
		const float smearE = smearEAt(xLin);
		const float var = k * k * (e + (e * kPrnu) * (e * kPrnu) + sigmaRead * sigmaRead
		                           + kDsnu * kDsnu + kRow * kRow + smearE * smearE);
		return sqrtf(max(var, 1e-24f)) * attenuation * amountG;
	};

	const float xMid    = 0.18f;
	const float xShadow = 0.18f / 16.0f;                 // four stops under the card: a real shadow
	const float xHot    = 0.90f;                         // just under the clip point: where a streak lives
	const float sigmaMid    = sigmaLinAt(xMid);
	const float sigmaShadow = sigmaLinAt(xShadow);
	const float cvMid    = 255.0f * sigmaMid * slopeAt(xMid);
	const float cvShadow = 255.0f * sigmaShadow * slopeAt(xShadow);

	const float eMid      = eAt(xMid);
	const float sigmaShot = k * sqrtf(eMid);
	const float sigmaReadRel = k * sigmaRead;

	const char* szDigitalVerdict =
	  (amountG <= 0.0f)     ? "NOTHING is being asked for - the camera published an amount of 0" :
	  (cvShadow >= 8.0f)    ? "loud: the shadows are visibly grainy, which is what a pushed sensor looks like" :
	  (cvShadow >= 2.5f)    ? "right: shadows plainly textured, mid grey almost clean - a normal high-ISO frame" :
	  (cvShadow >= 0.8f)    ? "quiet: about one code value in the shadows, at the edge of visibility" :
	                          "invisible: under one code value even in the shadows - raise Grain Strength or the ISO";

	CryLog("[POSTAA]   SENSOR: %.0f photosites across %.1f mm, effective pitch %.2f um (geometric %.2f um)"
	       " -- %.1f photosites under one output pixel, so the noise is divided by %.2f"
	       " (integration exponent %.2f, %.0f frames stacked)",
	       sensorPx, sensor.x, (digital3.y > 0.0f) ? digital3.y : pitchUm, pitchUm,
	       nPix, 1.0f / max(attenuation, 1e-6f), integExp, stackN);
	CryLog("[POSTAA]   LIGHT:  ISO %.0f on a base of %.0f, full well %.0f e- "
	       "-- a mid grey pixel collects %.0f e-, so shot noise alone is %.1f e- (%.2f%% of it)",
	       iso, isoBase, fullWell, eMid, sqrtf(eMid), 100.0f / max(sqrtf(eMid), 1e-6f));
	CryLog("[POSTAA]   NOISE:  shot %.4f + read %.4f (%.1f e-) + fixed pattern, in display-linear units"
	       " -- PRNU %.3f%%, DSNU %.1f e-, row %.1f e-, chroma NR %.2f, WB gains R %.2f / B %.2f, smear %.2f",
	       sigmaShot, sigmaReadRel, sigmaRead, digital1.x * 100.0f, digital1.y, digital1.z, digital1.w,
	       digital2.x, digital2.y, digital2.w);

	// The per-term table. sigma in CODE VALUES of 255, green channel, at mid grey and four stops
	// under it. Anything at or above ~1 CV is visible; a STRUCTURED term (row, column) is visible
	// well below that, because a coherent band or streak is far easier to see than random grit.
	CryLog("[POSTAA]   PER TERM (green, code values of 255):   mid grey 0.18    -4 stops 0.01125");
	CryLog("[POSTAA]     shot   (sqrt e)          %8.3f          %8.3f", cvOf(sqrtf(eAt(xMid)), xMid), cvOf(sqrtf(eAt(xShadow)), xShadow));
	CryLog("[POSTAA]     read   (%5.1f e-)        %8.3f          %8.3f", sigmaRead, cvOf(sigmaRead, xMid), cvOf(sigmaRead, xShadow));
	CryLog("[POSTAA]     PRNU   (%5.2f%% of e)     %8.3f          %8.3f", kPrnu * 100.0f, cvOf(eAt(xMid) * kPrnu, xMid), cvOf(eAt(xShadow) * kPrnu, xShadow));
	CryLog("[POSTAA]     DSNU   (%5.1f e-)        %8.3f          %8.3f", kDsnu, cvOf(kDsnu, xMid), cvOf(kDsnu, xShadow));
	CryLog("[POSTAA]     row    (%5.1f e-)        %8.3f          %8.3f   [structured: horizontal band]", kRow, cvOf(kRow, xMid), cvOf(kRow, xShadow));
	CryLog("[POSTAA]     column (%5.1f e- at 0.9) %8.3f          %8.3f   [structured: vertical streak; %.3f CV at 0.9, gate %.2f]",
	       smearEAt(xHot), cvOf(smearEAt(xMid), xMid), cvOf(smearEAt(xShadow), xShadow),
	       cvOf(smearEAt(xHot), xHot), colGateAt(xHot));
	CryLog("[POSTAA]     TOTAL  (quadrature)      %8.3f          %8.3f", cvMid, cvShadow);

	CryLog("[POSTAA]   DIGITAL RESULT: green sigma is ADDED, not multiplied"
	       " -- %.5f linear = %.2f code values on the mid grey card,"
	       " and %.5f linear = %.2f code values four stops down in a shadow. %s",
	       sigmaMid, cvMid, sigmaShadow, cvShadow, szDigitalVerdict);
	CryLog("[POSTAA]                   (as a fraction of the value that is %.2f stops at the card and"
	       " %.2f stops in the shadow - which is why it used to be applied in stops, and why doing so"
	       " blew up on near-black pixels.)",
	       sigmaMid / (xMid * 0.6931472f), sigmaShadow / (xShadow * 0.6931472f));
}

void CPostAAStage::Execute()
{
	// TODO: Handle rapid camera position changes in a better way

	PROFILE_LABEL_SCOPE("POST_AA");

	// TODO: CPostEffectContext::GetDstBackBufferTexture() pre-EnableAltBackBuffer()
	CTexture* pCurrRT = m_graphicsPipelineResources.m_pTexDisplayTargetDst;
	CTexture* pTempRT = m_graphicsPipelineResources.m_pTexDisplayTargetSrc;
	CTexture* pMgpuRT = NULL;

	// TODO: Support temporal AA in the editor
	uint32 aaMode = CRenderer::FX_GetAntialiasingType();

	if (aaMode && gcpRendD3D->IsEditorMode())
		aaMode = 1U << (eAT_SMAA_1X * CRenderer::CV_r_AntialiasingModeEditor);

	if (aaMode & eAT_SMAA_MASK)
		ApplySMAA(pCurrRT, pTempRT);

	if (aaMode & eAT_REQUIRES_PREVIOUSFRAME_MASK)
		ApplyTemporalAA(pCurrRT, pMgpuRT, aaMode);

	// TODO: Un-jitter depth buffer for AuxGeom depth tests (alternative: jitter aux)
	// TODO: Don't do anything and throw away depth when no depth-test/aux is used
	{
		// TODO: CPostEffectContext::GetDstBackBufferTexture() post-EnableAltBackBuffer()
		CTexture* pDestRT = RenderView()->GetColorTarget();
		DoFinalComposition(pCurrRT, pDestRT, aaMode);

#ifndef _RELEASE
		if (CRenderer::CV_r_AntialiasingModeDebug > 0)
			ExecuteDebug(pCurrRT, pDestRT);
#endif
	}

	if (pMgpuRT)
	{
		pMgpuRT->MgpuResourceUpdate(false);
	}
}

#ifndef _RELEASE
void CPostAAStage::ExecuteDebug(CTexture* pZoomRT, CTexture* pDestRT)
{
	auto& pass = m_passAntialiasingDebug;

	if (pass.IsDirty(pZoomRT->GetID(), pDestRT->GetID()))
	{
		static CCryNameTSCRC pszTechName("DebugPostAA");
		pass.SetRequirePerViewConstantBuffer(true);
		pass.SetPrimitiveFlags(CRenderPrimitive::eFlags_ReflectShaderConstants_PS);
		pass.SetPrimitiveType(CRenderPrimitive::ePrim_ProceduralTriangle);
		pass.SetTechnique(CShaderMan::s_shPostAA, pszTechName, 0);
		pass.SetRenderTarget(0, pDestRT);
		pass.SetState(GS_NODEPTHTEST);

		pass.SetTexture(0, pZoomRT, EDefaultResourceViews::Linear);
		pass.SetSampler(0, EDefaultSamplerStates::PointClamp);
	}

	pass.BeginConstantUpdate();

	float mx = static_cast<float>(pZoomRT->GetWidth() >> 1);
	float my = static_cast<float>(pZoomRT->GetHeight() >> 1);
#if CRY_PLATFORM_WINDOWS
	gEnv->pHardwareMouse->GetHardwareMouseClientPosition(&mx, &my);
#endif

	const Vec4 vDebugParams(mx, my, 1.f, max(1.0f, (float)CRenderer::CV_r_AntialiasingModeDebug));
	static CCryNameR pszDebugParams("vDebugParams");
	pass.SetConstant(pszDebugParams, vDebugParams);

	pass.Execute();
}
#endif

void CPostAAStage::Resize(int renderWidth, int renderHeight)
{
	if (CRenderer::CV_r_AntialiasingMode)
	{
		const uint32 renderTargetFlags = FT_NOMIPS | FT_DONT_STREAM | FT_USAGE_RENDERTARGET;
		ETEX_Format accumulatorFormat = eTF_R16G16B16A16;
		if (CRenderer::CV_r_AntialiasingMode <= eAT_SMAA_2TX && CRendererCVars::CV_r_HDRTexFormat == 0)
			accumulatorFormat = eTF_R10G10B10A2;

		if (m_pPrevBackBuffers[CCamera::eEye_Left][0] && m_pPrevBackBuffers[CCamera::eEye_Left][0]->GetDstFormat() != accumulatorFormat)
		{
			SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Left][0]);
			SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Left][1]);
			SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Right][0]);
			SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Right][1]);
		}

		std::string prevBackBuffer0texName = "$PrevBackBuffer0" + m_graphicsPipeline.GetUniqueIdentifierName();
		std::string prevBackBuffer1texName = "$PrevBackBuffer1" + m_graphicsPipeline.GetUniqueIdentifierName();
		m_pPrevBackBuffers[CCamera::eEye_Left][0] = CTexture::GetOrCreateRenderTarget(prevBackBuffer0texName.c_str(), renderWidth, renderHeight, Clr_Unknown, eTT_2D, renderTargetFlags, accumulatorFormat);
		m_pPrevBackBuffers[CCamera::eEye_Left][1] = CTexture::GetOrCreateRenderTarget(prevBackBuffer1texName.c_str(), renderWidth, renderHeight, Clr_Unknown, eTT_2D, renderTargetFlags, accumulatorFormat);

		if (gRenDev->IsStereoEnabled())
		{
			prevBackBuffer0texName = "$PrevBackBuffer0_R" + m_graphicsPipeline.GetUniqueIdentifierName();
			prevBackBuffer1texName = "$PrevBackBuffer1_R" + m_graphicsPipeline.GetUniqueIdentifierName();
			m_pPrevBackBuffers[CCamera::eEye_Right][0] = CTexture::GetOrCreateRenderTarget(prevBackBuffer0texName.c_str(), renderWidth, renderHeight, Clr_Unknown, eTT_2D, renderTargetFlags, accumulatorFormat);
			m_pPrevBackBuffers[CCamera::eEye_Right][1] = CTexture::GetOrCreateRenderTarget(prevBackBuffer1texName.c_str(), renderWidth, renderHeight, Clr_Unknown, eTT_2D, renderTargetFlags, accumulatorFormat);
		}
		else
		{
			SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Right][0]);
			SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Right][1]);
		}
	}
	else
	{
		SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Left][0]);
		SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Left][1]);
		SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Right][0]);
		SAFE_RELEASE(m_pPrevBackBuffers[CCamera::eEye_Right][1]);
	}

	oldStereoEnabledState = gRenDev->IsStereoEnabled();
	oldAAState = CRenderer::CV_r_AntialiasingMode;
}

void CPostAAStage::Update()
{
	// Check if Stereo or AA settings have been updated, if so we might need to recreate prevBackBuffer rendertarget
	if (oldStereoEnabledState != gRenDev->IsStereoEnabled() ||
		oldAAState != CRenderer::CV_r_AntialiasingMode)
		Resize(m_graphicsPipeline.GetRenderResolution().x, m_graphicsPipeline.GetRenderResolution().y);
}

CTexture* CPostAAStage::GetAARenderTarget(const CRenderView* pRenderView, bool bCurrentFrame) const
{
	int eye = static_cast<int>(pRenderView->GetCurrentEye());
	int index = (bCurrentFrame ? SPostEffectsUtils::m_iFrameCounter : (SPostEffectsUtils::m_iFrameCounter + 1)) % 2;

	CRY_ASSERT(eye == CCamera::eEye_Left || eye == CCamera::eEye_Right);

	return m_pPrevBackBuffers[eye][index];
}
