// Copyright 2016-2021 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "LPV.h"

#include "D3DPostProcess.h"
#include "HeightMapAO.h"
#include "Common/RenderView.h"

#if defined(FEATURE_SVO_GI)
	#include "D3D_SVO.h"
#endif

namespace
{
// Thread group sizes, must match the ones declared in Engine/Shaders/HWScripts/CryFX/LPV.cfi.
const int32 LPVGridTileSize = 4;
const int32 LPVInjectTileSize = 8;
const int32 LPVApplyTileSize = 8;

const ETEX_Format LPVGridFormat = eTF_R16G16B16A16F;
const ETEX_Format LPVRsmFormat = eTF_R8G8B8A8;
const ETEX_Format LPVIrradianceFormat = eTF_R16G16B16A16F;

uint32 lpvDispatchSize(int32 size, int32 tileSize)
{
	return (uint32)((size / tileSize) + ((size % tileSize) > 0 ? 1 : 0));
}

// Has to match CShadowMapStage::GetShadowTexFormat(ePass_DirectionalLightRSM), otherwise the render
// pass of the pre-allocated RSM pass group is incompatible with the dedicated depth target.
ETEX_Format lpvRsmDepthFormat()
{
	return CRendererResources::s_hwTexFormatSupport.GetClosestFormatSupported(
	  CRendererCVars::CV_r_shadowtexformat == 0 ? eTF_D32F :
	  (CRendererCVars::CV_r_shadowtexformat == 1 ? eTF_D16 : eTF_D24S8));
}
}

CLPVStage::CLPVStage(CGraphicsPipeline& graphicsPipeline)
	: CGraphicsPipelineStage(graphicsPipeline)
	, m_pIrradiance(nullptr)
	, m_passApply(&graphicsPipeline, CComputeRenderPass::eFlags_ReflectConstantBuffersFromShader)
	, m_gridSize(0)
	, m_lastSunDir(ZERO)
	, m_lastSunColor(ZERO)
	, m_framesSinceRelight(0)
	, m_bForceRelight(true)
	, m_bResultValid(false)
{
	for (int32 n = 0; n < MaxCascadeNum; ++n)
	{
		SCascade& cascade = m_cascades[n];

		auto configurePass = [&graphicsPipeline](CComputeRenderPass& pass)
		{
			pass.SetFlags(CComputeRenderPass::eFlags_ReflectConstantBuffersFromShader);
			pass.SetGraphicsPipeline(&graphicsPipeline);
		};

		configurePass(cascade.passClear);
		configurePass(cascade.passInject);
		configurePass(cascade.passResolve);
		for (auto& pass : cascade.passPropagate)
			configurePass(pass);
		for (auto& pass : cascade.passTemporal)
			configurePass(pass);

		cascade.rsmToWorld.SetIdentity();
		cascade.worldToRsm.SetIdentity();

		// The RSM texture objects have to exist before CShadowMapStage::Init() runs, which happens
		// before CLPVStage::Init(). The device resources are created on demand in GetRsmColorMap().
		const std::string suffix = std::to_string(n) + m_graphicsPipeline.GetUniqueIdentifierName();
		std::string texName = "LPV_SUN_RSM_COLOR" + suffix;
		cascade.pRsmColor = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 1, eTT_2D, FT_STATE_CLAMP, LPVRsmFormat);

		texName = "LPV_SUN_RSM_NORMAL" + suffix;
		cascade.pRsmNormal = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 1, eTT_2D, FT_STATE_CLAMP, LPVRsmFormat);
	}
}

CTexture* CLPVStage::GetRsmColorTexture() const
{
	return m_cascades[0].pRsmColor.get();
}

CTexture* CLPVStage::GetRsmNormalTexture() const
{
	return m_cascades[0].pRsmNormal.get();
}

CLPVStage::SCascade& CLPVStage::CascadeForFrustum(const ShadowMapFrustum& rFr)
{
	return m_cascades[min<int32>(rFr.nLpvCascadeIndex, MaxCascadeNum - 1)];
}

const CLPVStage::SCascade& CLPVStage::CascadeForFrustum(const ShadowMapFrustum& rFr) const
{
	return m_cascades[min<int32>(rFr.nLpvCascadeIndex, MaxCascadeNum - 1)];
}

void CLPVStage::NotifyRsmRender(const ShadowMapFrustum& rFr)
{
	CascadeForFrustum(rFr).bRsmRenderedThisFrame = true;
}

int32 CLPVStage::GetValidGridSize()
{
	int32 size = CRendererCVars::CV_r_LPVGridSize;

	// The volume depth is limited by CTexture::CreateTextureArray and has to be a multiple of 4.
	size = clamp_tpl(size, 8, 64);
	size = size - (size % 4);

	return size;
}

int32 CLPVStage::GetActiveCascadeCount()
{
	return clamp_tpl<int32>(CRendererCVars::CV_r_LPVCascades, 1, MaxCascadeNum);
}

void CLPVStage::Init()
{
	const uint32 commonFlags = FT_NOMIPS | FT_DONT_STREAM;
	const uint32 uavFlags = commonFlags | FT_USAGE_UNORDERED_ACCESS;

	// Only create the texture objects here, the device resources are allocated by ResizeGrid()/Resize().
	for (int32 n = 0; n < MaxCascadeNum; ++n)
	{
		SCascade& cascade = m_cascades[n];
		const std::string suffix = "C" + std::to_string(n) + m_graphicsPipeline.GetUniqueIdentifierName();

		for (int32 i = 0; i < 2; ++i)
		{
			for (int32 c = 0; c < SHChannelNum; ++c)
			{
				CRY_ASSERT(cascade.pGridSH[i][c] == nullptr);
				std::string texName = "$LPVGridSH" + std::to_string(i) + std::to_string(c) + suffix;
				cascade.pGridSH[i][c] = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 0, eTT_3D, uavFlags, LPVGridFormat);

				CRY_ASSERT(cascade.pGridSum[i][c] == nullptr);
				texName = "$LPVGridSum" + std::to_string(i) + std::to_string(c) + suffix;
				cascade.pGridSum[i][c] = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 0, eTT_3D, uavFlags, LPVGridFormat);

				CRY_ASSERT(cascade.pGridAcc[i][c] == nullptr);
				texName = "$LPVGridAcc" + std::to_string(i) + std::to_string(c) + suffix;
				cascade.pGridAcc[i][c] = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 0, eTT_3D, uavFlags, LPVGridFormat);
			}
		}

		CRY_ASSERT(cascade.pGridGV == nullptr);
		std::string gvName = "$LPVGridGV" + suffix;
		cascade.pGridGV = CTexture::GetOrCreateTextureObjectPtr(gvName.c_str(), 0, 0, 0, eTT_3D, uavFlags, LPVGridFormat);
	}

	CRY_ASSERT(m_pIrradiance == nullptr);
	std::string texName = "$LPVIrradiance" + m_graphicsPipeline.GetUniqueIdentifierName();
	m_pIrradiance = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 1, eTT_2D, uavFlags, LPVIrradianceFormat);

	CRY_ASSERT(m_pSpecular == nullptr);
	texName = "$LPVSpecular" + m_graphicsPipeline.GetUniqueIdentifierName();
	m_pSpecular = CTexture::GetOrCreateTextureObjectPtr(texName.c_str(), 0, 0, 1, eTT_2D, uavFlags, LPVIrradianceFormat);

	ResizeGrid(GetValidGridSize());
}

void CLPVStage::ResizeGrid(int32 gridSize)
{
	const uint32 commonFlags = FT_NOMIPS | FT_DONT_STREAM;
	const uint32 uavFlags = commonFlags | FT_USAGE_UNORDERED_ACCESS;

	auto createTexture3D = [=](CTexture* pTex) -> void
	{
		if (pTex != nullptr
		    && (gridSize != pTex->GetDepth()
		        || !CTexture::IsTextureExist(pTex)
		        || pTex->Invalidate(gridSize, gridSize, LPVGridFormat)))
		{
			pTex->Create3DTexture(gridSize, gridSize, gridSize, 1, uavFlags, nullptr, LPVGridFormat);
			if (pTex->GetFlags() & FT_FAILED)
				CryFatalError("Couldn't allocate texture.");
		}
	};

	for (auto& cascade : m_cascades)
	{
		for (auto& gridSet : cascade.pGridSH)
		{
			for (auto& pTex : gridSet)
				createTexture3D(pTex);
		}
		for (auto& gridSet : cascade.pGridSum)
		{
			for (auto& pTex : gridSet)
				createTexture3D(pTex);
		}
		for (auto& gridSet : cascade.pGridAcc)
		{
			for (auto& pTex : gridSet)
				createTexture3D(pTex);
		}
		createTexture3D(cascade.pGridGV);

		const uint32 elementCount = gridSize * gridSize * gridSize * AccumulatorStride;
		if (elementCount != cascade.injectionBuffer.GetElementCount() || cascade.injectionBuffer.GetDevBuffer() == nullptr)
		{
			cascade.injectionBuffer.Create(elementCount, sizeof(int32), DXGI_FORMAT_R32_SINT,
			                               CDeviceObjectFactory::BIND_SHADER_RESOURCE | CDeviceObjectFactory::BIND_UNORDERED_ACCESS, nullptr);
		}

		cascade.bHistoryValid = false;
	}

	m_gridSize = gridSize;
	m_bResultValid = false;
}

void CLPVStage::Resize(int renderWidth, int renderHeight)
{
	const uint32 uavFlags = FT_NOMIPS | FT_DONT_STREAM | FT_USAGE_UNORDERED_ACCESS;

	if (m_pIrradiance != nullptr
	    && (!CTexture::IsTextureExist(m_pIrradiance)
	        || m_pIrradiance->Invalidate(renderWidth, renderHeight, LPVIrradianceFormat)))
	{
		m_pIrradiance->Create2DTexture(renderWidth, renderHeight, 1, uavFlags, nullptr, LPVIrradianceFormat);
		if (m_pIrradiance->GetFlags() & FT_FAILED)
			CryFatalError("Couldn't allocate texture.");
	}

	if (m_pSpecular != nullptr
	    && (!CTexture::IsTextureExist(m_pSpecular)
	        || m_pSpecular->Invalidate(renderWidth, renderHeight, LPVIrradianceFormat)))
	{
		m_pSpecular->Create2DTexture(renderWidth, renderHeight, 1, uavFlags, nullptr, LPVIrradianceFormat);
		if (m_pSpecular->GetFlags() & FT_FAILED)
			CryFatalError("Couldn't allocate texture.");
	}

	m_bResultValid = false;
}

void CLPVStage::OnCVarsChanged(const CCVarUpdateRecorder& cvarUpdater)
{
	if (cvarUpdater.GetCVar("r_LPVGridSize"))
	{
		const int32 gridSize = GetValidGridSize();
		if (gridSize != m_gridSize)
			ResizeGrid(gridSize);
	}

	// Any LPV parameter change invalidates the frozen grid of the relight gating - and drops the
	// temporal history: parameters like r_LPVIntensity are compensated at injection time (the sky
	// light divides by the intensity so the display multiply cancels), and blending the old grid
	// into the new one through the EMA makes every such change look temporarily coupled (a raised
	// intensity visibly "boosted" the sky light until the accumulator converged over seconds).
	// A hard cut shows the true steady state immediately.
	for (const auto& record : cvarUpdater.GetCVars())
	{
		if (strncmp(record.name, "r_LPV", 5) == 0)
		{
			m_bForceRelight = true;
			for (auto& cascade : m_cascades)
				cascade.bHistoryValid = false;
			break;
		}
	}
}

CTexture* CLPVStage::GetIrradianceRT() const
{
	// The CVar check is required: when the stage is toggled off Execute() is not called anymore, so
	// m_bResultValid would keep the value of the last active frame and the stale result would stay
	// applied forever.
	return (CRendererCVars::CV_r_LPV > 0 && m_bResultValid && CTexture::IsTextureExist(m_pIrradiance)) ? m_pIrradiance.get() : nullptr;
}

CTexture* CLPVStage::GetSpecularRT() const
{
	return (CRendererCVars::CV_r_LPV > 0 && CRendererCVars::CV_r_LPVSpecular > 0.0f
	        && m_bResultValid && CTexture::IsTextureExist(m_pSpecular)) ? m_pSpecular.get() : nullptr;
}

bool CLPVStage::WantsRsmRender(const ShadowMapFrustum& rFr)
{
	SCascade& cascade = CascadeForFrustum(rFr);

	if (m_bForceRelight || !cascade.bHistoryValid)
		return true;

	// Same sun change thresholds as the relight gate in Execute().
	if (CRenderView* pRenderView = RenderView())
	{
		const Vec3 sunDir = pRenderView->GetSunLightDirection();
		const Vec4 sunColor = pRenderView->GetSunLightColor();
		if (sunDir.Dot(m_lastSunDir) < 0.99995f)
			return true;
		if ((fabsf(sunColor.x - m_lastSunColor.x) + fabsf(sunColor.y - m_lastSunColor.y) + fabsf(sunColor.z - m_lastSunColor.z)) > 0.01f)
			return true;

		// Camera anchor: the RSM view is camera anchored, but the content keeps covering the volume
		// for small movements - re-render only past ~2 cells of THIS cascade (the far cascade's
		// bigger cells re-render less often). A stationary camera never re-renders from this
		// criterion.
		const Vec3 camPos = pRenderView->GetCamera(CCamera::eEye_Left).GetPosition();
		if ((camPos - cascade.rsmRenderCamPos).GetLengthSquared() > sqr(2.0f * max(0.01f, cascade.cellSize)))
			return true;
	}

	// Slow content-refresh fallback (doors opening, parked vehicles, streamed geometry): four
	// relight heartbeats. With r_LPVUpdateInterval 0 the RSM refreshes every frame.
	const int32 interval = max(0, CRendererCVars::CV_r_LPVUpdateInterval);
	if (interval == 0 || cascade.framesSinceRsmRender >= interval * 4)
		return true;

	return false;
}

bool CLPVStage::IsSvoProvidingRsm() const
{
#if defined(FEATURE_SVO_GI)
	return CSvoRenderer::IsActive();
#else
	return false;
#endif
}

bool CLPVStage::IsRsmFrustum(const ShadowMapFrustum& rFr) const
{
	if (CRendererCVars::CV_r_LPV <= 0 || IsSvoProvidingRsm() || rFr.nShadowMapSize <= 0)
		return false;

	// Dedicated view, fitted to the volume itself and set up by Cry3DEngine
	// (ShadowCacheGenerator::InitLPVRsmFrustum). Independent of the camera orientation.
	return rFr.m_eFrustumType == ShadowMapFrustum::e_LPVRsm;
}

void CLPVStage::CheckCreateUpdateRsmTarget(_smart_ptr<CTexture>& pTex, int32 size, const char* szName)
{
	if (!CTexture::IsTextureExist(pTex) || pTex->GetWidth() != size || pTex->GetHeight() != size)
	{
		// NOTE: SPostEffectsUtils::GetOrCreateRenderTarget adds a reference when !CTexture::IsTextureExist
		const bool bNeedsDecRef = !CTexture::IsTextureExist(pTex);

		CTexture* pTexRaw = pTex;
		if (SD3DPostEffectsUtils::GetOrCreateRenderTarget(szName, pTexRaw, size, size, Clr_Transparent, 0, false, LPVRsmFormat))
		{
			pTex = pTexRaw;
			pTex->DisableMgpuSync();

			if (bNeedsDecRef)
				pTex->Release();
		}
	}
}

CTexture* CLPVStage::GetRsmColorMap(const ShadowMapFrustum& rFr, bool bCheckUpdate)
{
#if defined(FEATURE_SVO_GI)
	// Never fight SVOGI over the single sun RSM pass slot.
	if (CSvoRenderer::GetRsmColorMap(m_graphicsPipeline, rFr))
		return nullptr;
#endif

	if (!IsRsmFrustum(rFr))
		return nullptr;

	SCascade& cascade = CascadeForFrustum(rFr);

	if (bCheckUpdate)
	{
		const std::string texName = "LPV_SUN_RSM_COLOR" + std::to_string(rFr.nLpvCascadeIndex) + m_graphicsPipeline.GetUniqueIdentifierName();
		CheckCreateUpdateRsmTarget(cascade.pRsmColor, rFr.nShadowMapSize, texName.c_str());
	}

	return CTexture::IsTextureExist(cascade.pRsmColor) ? cascade.pRsmColor.get() : nullptr;
}

CTexture* CLPVStage::GetRsmNormalMap(const ShadowMapFrustum& rFr, bool bCheckUpdate)
{
#if defined(FEATURE_SVO_GI)
	if (CSvoRenderer::GetRsmNormlMap(m_graphicsPipeline, rFr))
		return nullptr;
#endif

	if (!IsRsmFrustum(rFr))
		return nullptr;

	SCascade& cascade = CascadeForFrustum(rFr);

	if (bCheckUpdate)
	{
		const std::string texName = "LPV_SUN_RSM_NORMAL" + std::to_string(rFr.nLpvCascadeIndex) + m_graphicsPipeline.GetUniqueIdentifierName();
		CheckCreateUpdateRsmTarget(cascade.pRsmNormal, rFr.nShadowMapSize, texName.c_str());
	}

	return CTexture::IsTextureExist(cascade.pRsmNormal) ? cascade.pRsmNormal.get() : nullptr;
}

CTexture* CLPVStage::GetRsmDepthMap(const ShadowMapFrustum& rFr, bool bCheckUpdate)
{
	if (!IsRsmFrustum(rFr))
		return nullptr;

	SCascade& cascade = CascadeForFrustum(rFr);

	if (bCheckUpdate)
	{
		const int32 size = rFr.nShadowMapSize;
		const ETEX_Format depthFormat = lpvRsmDepthFormat();

		if (!cascade.pRsmDepth)
		{
			const std::string texName = "$LPV_SUN_RSM_DEPTH" + std::to_string(rFr.nLpvCascadeIndex) + m_graphicsPipeline.GetUniqueIdentifierName();
			cascade.pRsmDepth = CTexture::GetOrCreateDepthStencilPtr(texName.c_str(), size, size, Clr_FarPlane, eTT_2D, FT_DONT_STREAM, depthFormat);
		}

		if (cascade.pRsmDepth && (!CTexture::IsTextureExist(cascade.pRsmDepth) || cascade.pRsmDepth->Invalidate(size, size, depthFormat)))
			cascade.pRsmDepth->CreateDepthStencil(depthFormat, Clr_FarPlane);
	}

	return CTexture::IsTextureExist(cascade.pRsmDepth) ? cascade.pRsmDepth.get() : nullptr;
}

const ShadowMapFrustum* CLPVStage::FindRsmFrustum(int32 cascadeIndex) const
{
	const CRenderView* pRenderView = RenderView();
	if (!pRenderView)
		return nullptr;

	for (const auto& pFrustumToRender : pRenderView->GetShadowFrustumsByType(CRenderView::eShadowFrustumRenderType_LPVRsm))
	{
		const ShadowMapFrustum* pFrustum = pFrustumToRender->pFrustum;
		if (pFrustum && pFrustum->m_eFrustumType == ShadowMapFrustum::e_LPVRsm && (int32)pFrustum->nLpvCascadeIndex == cascadeIndex)
		{
			if (CRendererCVars::CV_r_LPVDebug > 0 && cascadeIndex == 0 && pFrustumToRender->pShadowsView)
			{
				// Render item count of the RSM shadow view, logged on change: tells whether casters
				// disappear on the submission side (count drops) or in the collection (count stable
				// while the 3DEngine side count drops).
				const size_t itemCount = reinterpret_cast<CRenderView*>(pFrustumToRender->pShadowsView.get())->GetRenderItems(ERenderListID(0)).size();
				static size_t lastItemCount = ~size_t(0);
				if (itemCount != lastItemCount)
				{
					CryLog("LPV debug: RSM shadow view render items %u -> %u", (uint32)lastItemCount, (uint32)itemCount);
					lastItemCount = itemCount;
				}
			}

			return pFrustum;
		}
	}

	return nullptr;
}

bool CLPVStage::UpdateFrameParameters(SCascade& cascade, const ShadowMapFrustum& frustum, const CTexture* pRsmColor)
{
	// Use the cascade's own persistent depth target, NOT frustum.pDepthTex: on the frames the RSM
	// pass is dropped (WantsRsmRender() gating) the shadow prep stomps the frustum pointer with
	// the empty far plane texture, while our texture still holds the last rendered RSM.
	if (!CTexture::IsTextureExist(cascade.pRsmDepth))
		return false;

	// The injection pass reads depth, albedo and normal at the same texel, so all three RSM targets
	// have to share the resolution of the RSM view.
	cascade.rsmSize = pRsmColor->GetWidth();
	if (cascade.rsmSize <= 0 || cascade.rsmSize != cascade.pRsmDepth->GetWidth())
		return false;

	const int32 wantedSamples = clamp_tpl(CRendererCVars::CV_r_LPVRSMSamples, 64, MaxRsmSampleCount);
	cascade.rsmSampleStride = max(1, cascade.rsmSize / wantedSamples);
	cascade.rsmSampleCount = max(1, cascade.rsmSize / cascade.rsmSampleStride);

	// Build the RSM texture space -> world space transform used to place the VPLs.
	// ConfigShadowTexgen() returns world -> (u, v, z, w). The dedicated RSM view is built by the very
	// same renderer code path as the sun cascades (CShadowUtils::GetShadowMatrixOrtho), so the depth
	// buffer stores the clip space z of exactly this matrix and it inverts directly.
	// Do NOT scale the z row by RecpFarDist: fFarDist of the sun frustums is the (astronomic) distance
	// to the sun light source, the scale would make the matrix numerically singular. SVOGI applies that
	// scale but never reconstructs positions from RSM depth, so it is harmless there.
	CShadowUtils::SShadowsSetupInfo shadowsSetup = gcpRendD3D->ConfigShadowTexgen(RenderView(), &frustum, 0);

	Matrix44A worldToRsm = shadowsSetup.ShadowMat;

	cascade.worldToRsm = worldToRsm;
	cascade.rsmToWorld = worldToRsm.GetInverted();

	// Depth range of the view, pinned together with the matrices: the RSM depth buffer stores a
	// LINEAR normalized depth ((Z - near) / (far - near), CommonShadowGenPass.cfi divides the clip
	// z by fFarDist), the matrix pair above works in post-divide z_ndc. The shaders convert between
	// the two encodings via LPVRsmDepthRange - without the conversion the reconstruction is exact
	// only at the range ends and stretches along the sun axis with an error that grows with the
	// depth range (sub-cell at r_LPVRsmClipRange 0, tens of meters at 700).
	cascade.rsmNearDist = frustum.fNearDist;
	cascade.rsmFarDist = frustum.fFarDist;

	// World size of one sampled RSM texel. The inverse is only defined up to a projective scale,
	// dehomogenize before measuring. Used by the injection weight (surfel footprint / cascade cell
	// footprint) and the geometry volume blocking potential.
	{
		Vec4 rsmCorner0 = cascade.rsmToWorld * Vec4(0.0f, 0.0f, 0.5f, 1.0f);
		Vec4 rsmCorner1 = cascade.rsmToWorld * Vec4(1.0f, 0.0f, 0.5f, 1.0f);
		if (fabsf(rsmCorner0.w) > 1e-12f)
			rsmCorner0 = rsmCorner0 / rsmCorner0.w;
		if (fabsf(rsmCorner1.w) > 1e-12f)
			rsmCorner1 = rsmCorner1 / rsmCorner1.w;
		const Vec3 rsmSpan = Vec3(rsmCorner1.x - rsmCorner0.x, rsmCorner1.y - rsmCorner0.y, rsmCorner1.z - rsmCorner0.z);
		cascade.rsmTexelSize = rsmSpan.GetLength() / (float)cascade.rsmSampleCount;
	}

	if (CRendererCVars::CV_r_LPVDebug > 0 && &cascade == &m_cascades[0])
	{
		// CPU side sanity check of the matrix chain: the RSM centre at mid depth has to land in the
		// vicinity of the volume (and the camera), and the roundtrip has to reproduce the input.
		const Vec4 rsmCenter(0.5f, 0.5f, 0.5f, 1.0f);
		const Vec4 worldH = cascade.rsmToWorld * rsmCenter;
		const Vec3 world = Vec3(worldH.x, worldH.y, worldH.z) / worldH.w;
		// Homogeneous matrices are only defined up to scale, so both results have to be dehomogenized
		// before they are compared (the first version of this log line skipped the divide and made a
		// perfectly fine projective inverse look broken).
		Vec4 roundtripH = worldToRsm * Vec4(world, 1.0f);
		if (fabsf(roundtripH.w) > 1e-12f)
			roundtripH = roundtripH / roundtripH.w;
		const Vec3 camPos = GetCurrentViewInfo().cameraOrigin;
		CryLog("LPV debug: rsm(0.5,0.5,0.5) -> world (%.1f, %.1f, %.1f) w=%.3f | roundtrip (%.3f, %.3f, %.3f) | cam (%.1f, %.1f, %.1f) | RecpFarDist %.5f | lod %d | frustumType %d (LPVRsm=%d) | rsmSize %d | depthTex %s",
		       world.x, world.y, world.z, worldH.w,
		       roundtripH.x, roundtripH.y, roundtripH.z,
		       camPos.x, camPos.y, camPos.z,
		       shadowsSetup.RecpFarDist, frustum.nShadowMapLod,
		       (int)frustum.m_eFrustumType, (int)ShadowMapFrustum::e_LPVRsm,
		       cascade.rsmSize,
		       cascade.pRsmDepth ? cascade.pRsmDepth->GetName() : "null");
	}

	return true;
}

void CLPVStage::UpdateCascadeParameters(SCascade& cascade, int32 cascadeIndex)
{
	// Volume extent, snapped to whole cells to avoid sampling discontinuities while the camera moves.
	// r_LPVCascadeScale is the PER STEP ratio between adjacent cascades (96 / 288 / 864 with the
	// defaults) - has to stay in sync with ShadowCacheGenerator::GetLPVBox().
	float extent = max(1.0f, CRendererCVars::CV_r_LPVSize);
	const float scaleStep = max(1.0f, CRendererCVars::CV_r_LPVCascadeScale);
	for (int32 i = 0; i < cascadeIndex; ++i)
		extent *= scaleStep;

	cascade.cellSize = extent / (float)m_gridSize;

	const Vec3 camPos = GetCurrentViewInfo().cameraOrigin;
	const float halfExtent = extent * 0.5f;
	cascade.gridOrigin.x = floorf(camPos.x / cascade.cellSize) * cascade.cellSize - halfExtent;
	cascade.gridOrigin.y = floorf(camPos.y / cascade.cellSize) * cascade.cellSize - halfExtent;
	cascade.gridOrigin.z = floorf(camPos.z / cascade.cellSize) * cascade.cellSize - halfExtent;
}

void CLPVStage::SetGridConstants(CComputeRenderPass& pass, const SCascade& cascade)
{
	static CCryNameR paramGrid("LPVGridParams");
	static CCryNameR paramGrid2("LPVGridParams2");

	const float fGridSize = (float)m_gridSize;

	pass.SetConstant(paramGrid, Vec4(cascade.gridOrigin.x, cascade.gridOrigin.y, cascade.gridOrigin.z, cascade.cellSize));
	pass.SetConstant(paramGrid2, Vec4(fGridSize, 1.0f / fGridSize, 1.0f / cascade.cellSize, fGridSize * cascade.cellSize));
}

// Debug instrumentation: log on which frames (and why) the whole LPV execution bails out. A single
// bailed frame makes GetIrradianceRT() return null and the entire GI contribution disappears from
// the tiled shading combine for that frame - visible as a full-frame dark flicker.
static void LpvDebugLogExecuteState(int reason)
{
	if (CRendererCVars::CV_r_LPVDebug <= 0)
		return;

	static int lastReason = -1;
	if (reason != lastReason)
	{
		static const char* const reasonNames[] =
		{
			"ok", "svo active", "no shader", "no textures", "no injection buffer",
			"no rsm frustum", "no rsm targets", "frame parameters failed"
		};
		CryLog("LPV debug: execute state %s -> %s",
		       (lastReason >= 0 && lastReason < 8) ? reasonNames[lastReason] : "startup",
		       reasonNames[reason]);
		lastReason = reason;
	}
}

void CLPVStage::Execute()
{
	FUNCTION_PROFILER_RENDERER();

	// Never run for recursive views (e.g. water reflections): they would overwrite the shared
	// irradiance target and the frozen grid state with the recursion camera, and the main view
	// combine would alternate between the correct and the recursion result. Do not touch
	// m_bResultValid here - the main view result stays valid.
	if (RenderView() && RenderView()->IsRecursive())
		return;

	m_bResultValid = false;

	// SVOGI owns the GI combine point and the sun RSM slot, LPV steps aside completely while it is active.
	if (IsSvoProvidingRsm())
	{
		LpvDebugLogExecuteState(1);
		return;
	}

	if (!CShaderMan::s_shDeferredShading)
	{
		LpvDebugLogExecuteState(2);
		return;
	}

	if (m_gridSize <= 0 || !CTexture::IsTextureExist(m_pIrradiance) || !CTexture::IsTextureExist(m_cascades[0].pGridSH[0][0]))
	{
		LpvDebugLogExecuteState(3);
		return;
	}

	if (!m_cascades[0].injectionBuffer.IsAvailable())
	{
		LpvDebugLogExecuteState(4);
		return;
	}

	PROFILE_LABEL_SCOPE("LPV");

	const bool bAsynchronous = false;
	SScopedComputeCommandList commandList(bAsynchronous);

	const int32 cascadeCount = GetActiveCascadeCount();
	m_activeCascadeCount = cascadeCount;

	// Relight gating: the injection inputs are fully discretized (cell snapped volumes, texel snapped
	// RSM), so between discrete changes a re-injection can only add noise. The grids are world
	// anchored, so a frozen grid stays exactly correct for a static scene while the camera moves or
	// rotates within the current cell.
	const Vec3 sunDir = RenderView()->GetSunLightDirection();
	const Vec4 sunColor = RenderView()->GetSunLightColor();
	const int32 interval = max(0, CRendererCVars::CV_r_LPVUpdateInterval);

	// Local lights of the frame (CE3 LPVPostinjectLight equivalent): every deferred point light
	// adds an outward flowing lobe to the final grid inside the temporal pass - a cheap local
	// ambient glow that follows the scene's real lights into the GI (no bounce, no propagation).
	m_lightCount = 0;
	uint32 lightsHash = 0;
	if (CRendererCVars::CV_r_LPVPointLights > 0.0f)
	{
		const float sceneScale = gcpRendD3D->m_fAdaptedSceneScaleLBuffer;
		const auto& deferredLights = RenderView()->GetLightsArray(eDLT_DeferredLight);
		for (auto itr = deferredLights.begin(); itr != deferredLights.end() && m_lightCount < MaxPointLights; ++itr)
		{
			const SRenderLight& light = *itr;
			if (light.m_Flags & (DLF_FAKE | DLF_VOLUMETRIC_FOG_ONLY | DLF_SUN))
				continue;

			// GI Mode gate (Light entity property): only lights whose GI Mode is Static or
			// Dynamic feed the LPV. CLightEntity translates the mode into DLF_USE_FOR_SVOGI
			// for whichever GI system is active; None and HideIfGiIsActive never set it.
			if (!(light.m_Flags & DLF_USE_FOR_SVOGI))
				continue;
			const Vec3 pos = light.GetPosition();

			const bool bProjector = (light.m_Flags & DLF_PROJECT) != 0;
			const bool bArea = (light.m_Flags & DLF_AREA) != 0;

			// Emission direction (the light's forward X axis) and cone gate: projectors emit into
			// their frustum cone, area lights into the hemisphere in front of the plane, point
			// lights omnidirectionally (-1.5 disables the gate in the shader).
			Vec3 dir(0.0f, 0.0f, 1.0f);
			float cosOuter = -1.5f;
			if (bProjector)
			{
				dir = light.m_ObjMatrix.GetColumn0().GetNormalized();
				cosOuter = cosf(DEG2RAD(clamp_tpl(light.m_fLightFrustumAngle, 1.0f, 89.9f)));
			}
			else if (bArea)
			{
				dir = light.m_ObjMatrix.GetColumn0().GetNormalized();
				cosOuter = 0.0f;
			}

			float radius = light.m_fRadius;
			if (bArea)
				radius += max(light.m_fAreaWidth, light.m_fAreaHeight) * 0.5f;
			if (radius <= 0.01f)
				continue;

			m_lightPos[m_lightCount] = Vec4(pos, radius);
			m_lightColor[m_lightCount] = Vec4(light.m_Color.r * sceneScale, light.m_Color.g * sceneScale, light.m_Color.b * sceneScale, 0.0f);
			m_lightDir[m_lightCount] = Vec4(dir, cosOuter);
			++m_lightCount;

			// Quarter meter / coarse colour quantization: moving or recoloured lights trigger a
			// relight, camera-noise sized changes do not.
			lightsHash = lightsHash * 397u
			             + (uint32)(int32)(pos.x * 4.0f) * 31u
			             + (uint32)(int32)(pos.y * 4.0f) * 17u
			             + (uint32)(int32)(pos.z * 4.0f) * 7u
			             + (uint32)(int32)(dir.x * 8.0f + dir.y * 32.0f + dir.z * 128.0f) * 3u
			             + (uint32)(int32)(cosOuter * 16.0f) * 13u
			             + (uint32)(int32)(light.m_Color.r * 8.0f + light.m_Color.g * 64.0f + light.m_Color.b * 512.0f);
		}
		lightsHash += (uint32)m_lightCount * 2654435761u;

		// GI Mode gate diagnostics: list the deferred light set and the gate verdicts whenever the
		// set changes, so a "light does not feed the LPV" report can be split into "flag never
		// reached the renderer" vs "gate logic wrong" at a glance.
		if (CRendererCVars::CV_r_LPVDebug > 0)
		{
			static uint32 sLastLightLogSig = 0xFFFFFFFFu;
			const uint32 logSig = lightsHash * 31u + (uint32)deferredLights.size();
			if (logSig != sLastLightLogSig)
			{
				sLastLightLogSig = logSig;
				CryLog("LPV debug: deferred lights %d, passed GI gate %d", (int)deferredLights.size(), m_lightCount);
				int32 logged = 0;
				for (auto itr = deferredLights.begin(); itr != deferredLights.end() && logged < 8; ++itr, ++logged)
				{
					const SRenderLight& dbgLight = *itr;
					CryLog("LPV debug:   light '%s' flags 0x%08x giFlag %d radius %.1f",
					       dbgLight.m_sName ? dbgLight.m_sName : "?", (uint32)dbgLight.m_Flags,
					       (dbgLight.m_Flags & DLF_USE_FOR_SVOGI) ? 1 : 0, dbgLight.m_fRadius);
				}
			}
		}
	}

	bool bRelightAll = m_bForceRelight;
	bRelightAll |= sunDir.Dot(m_lastSunDir) < 0.99995f;
	bRelightAll |= (fabsf(sunColor.x - m_lastSunColor.x) + fabsf(sunColor.y - m_lastSunColor.y) + fabsf(sunColor.z - m_lastSunColor.z)) > 0.01f;
	bRelightAll |= (interval > 0) && (++m_framesSinceRelight >= interval);
	bRelightAll |= (interval == 0);
	bRelightAll |= (lightsHash != m_lastLightsHash);

	bool bAnyRelight = false;
	for (int32 i = 0; i < cascadeCount; ++i)
	{
		SCascade& cascade = m_cascades[i];

		UpdateCascadeParameters(cascade, i);

		// Every cascade owns its own RSM view (cascaded shadow maps scheme).
		const ShadowMapFrustum* pFrustum = FindRsmFrustum(i);
		CTexture* pRsmColor = pFrustum ? GetRsmColorMap(*pFrustum) : nullptr;
		CTexture* pRsmNormal = pFrustum ? GetRsmNormalMap(*pFrustum) : nullptr;

		if (i == 0 && (!pFrustum || !pRsmColor || !pRsmNormal))
			LpvDebugLogExecuteState(pFrustum ? 6 : 5);

		++cascade.framesSinceRsmRender;

		if (cascade.bRsmRenderedThisFrame)
		{
			cascade.bRsmRenderedThisFrame = false;

			// Pin the injection matrices to the freshly rendered content. On the frames the RSM
			// pass is dropped the frustum keeps moving with the camera, so recomputing the
			// matrices there would make them disagree with the stored texels - matrices and
			// content have to snapshot together.
			if (pFrustum && pRsmColor && pRsmNormal && UpdateFrameParameters(cascade, *pFrustum, pRsmColor))
			{
				cascade.rsmRenderCamPos = GetCurrentViewInfo().cameraOrigin;
				cascade.framesSinceRsmRender = 0;
			}
		}

		// No RSM has ever been rendered for this cascade - nothing to inject from yet.
		if (cascade.rsmSize <= 0 || !CTexture::IsTextureExist(cascade.pRsmDepth) || !CTexture::IsTextureExist(cascade.pRsmColor) || !CTexture::IsTextureExist(cascade.pRsmNormal))
		{
			if (i == 0)
				LpvDebugLogExecuteState(7);
			continue;
		}

		if (i == 0)
			LpvDebugLogExecuteState(0);

		bool bRelight = bRelightAll || !cascade.bHistoryValid;
		// A cell crossing does NOT trigger a relight anymore: the frozen grid stays anchored at its
		// old origin (which the apply reads, so it remains exactly correct) and the heartbeat
		// catches up. Only a teleport sized jump (a quarter of the volume) forces an immediate
		// relight.
		const float teleportDistSq = sqr(cascade.cellSize * (float)m_gridSize * 0.25f);
		bRelight |= (cascade.gridOrigin - cascade.prevGridOrigin).GetLengthSquared() > teleportDistSq;

		if (bRelight)
		{
			ExecuteClear(cascade, commandList);
			ExecuteInject(cascade, commandList);
			ExecuteResolve(cascade, commandList);
			ExecutePropagate(cascade, commandList);
			ExecuteTemporal(cascade, commandList);

			cascade.prevGridOrigin = cascade.gridOrigin;
			cascade.bHistoryValid = true;
			bAnyRelight = true;
		}
		else
		{
			// The frozen accumulation grid is anchored at the origin of the last relight.
			cascade.gridOrigin = cascade.prevGridOrigin;
		}
	}

	if (bAnyRelight)
	{
		m_lastSunDir = sunDir;
		m_lastSunColor = sunColor;
		m_framesSinceRelight = 0;
		m_bForceRelight = false;
		m_lastLightsHash = lightsHash;
	}

	// The screen space apply runs later in the pipeline (ExecuteApplyToScreen), see LPV.h.
	if (m_cascades[0].bHistoryValid && CTexture::IsTextureExist(m_cascades[0].pRsmDepth))
	{
		m_pApplyRsmDepth = m_cascades[0].pRsmDepth;
		m_bGridReadyForApply = true;
	}
}

void CLPVStage::ExecuteApplyToScreen()
{
	FUNCTION_PROFILER_RENDERER();

	if (!m_bGridReadyForApply)
		return;

	m_bGridReadyForApply = false;

	if (!m_pApplyRsmDepth || !CTexture::IsTextureExist(m_pIrradiance))
		return;

	PROFILE_LABEL_SCOPE("LPV APPLY");

	const bool bAsynchronous = false;
	SScopedComputeCommandList commandList(bAsynchronous);

	ExecuteApply(commandList, m_pApplyRsmDepth);

	m_bResultValid = true;
}

void CLPVStage::ExecuteClear(SCascade& cascade, const SScopedComputeCommandList& commandList)
{
	auto& pass = cascade.passClear;

	if (pass.IsDirty(m_gridSize))
	{
		static CCryNameTSCRC techName("LPVClear");
		pass.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
		pass.SetOutputUAV(0, &cascade.injectionBuffer);
	}

	pass.BeginConstantUpdate();
	SetGridConstants(pass, cascade);

	const uint32 dispatchSize = lpvDispatchSize(m_gridSize, LPVGridTileSize);
	pass.SetDispatchSize(dispatchSize, dispatchSize, dispatchSize);
	pass.PrepareResourcesForUse(commandList);
	pass.Execute(commandList);
}

void CLPVStage::ExecuteInject(SCascade& cascade, const SScopedComputeCommandList& commandList)
{
	auto& pass = cascade.passInject;

	if (pass.IsDirty(m_gridSize, cascade.pRsmColor->GetID(), cascade.pRsmNormal->GetID(), cascade.pRsmDepth->GetID()))
	{
		static CCryNameTSCRC techName("LPVInject");
		pass.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
		pass.SetOutputUAV(0, &cascade.injectionBuffer);
		pass.SetTexture(0, cascade.pRsmDepth);
		pass.SetTexture(1, cascade.pRsmColor);
		pass.SetTexture(2, cascade.pRsmNormal);
	}

	pass.BeginConstantUpdate();
	SetGridConstants(pass, cascade);

	// Geometry volume blocking potential of one sampled RSM texel: the fraction of a cell face its
	// world space footprint covers. The surfels of a wall crossing a cell then sum to ~1 with plain
	// additive accumulation (total surfel area / face area), no max blending or count normalization
	// needed.
	float gvBlocking = 0.0f;
	if (CRendererCVars::CV_r_LPVOcclusion > 0.0f)
		gvBlocking = min(1.0f, (cascade.rsmTexelSize * cascade.rsmTexelSize) / (cascade.cellSize * cascade.cellSize));

	if (CRendererCVars::CV_r_LPVDebug > 0 && &cascade == &m_cascades[0])
	{
		static float lastBlocking = -1.0f;
		if (fabsf(gvBlocking - lastBlocking) > 1e-6f)
		{
			CryLog("LPV debug: GV blocking potential per surfel %.6f (rsm texel %.3f m, cell %.3f m)",
			       gvBlocking, cascade.rsmTexelSize, cascade.cellSize);
			lastBlocking = gvBlocking;
		}
	}

	static CCryNameR paramRsm("LPVRsmParams");
	pass.SetConstant(paramRsm, Vec4((float)cascade.rsmSampleCount, (float)cascade.rsmSampleStride, CRendererCVars::CV_r_LPVInjectionBias, gvBlocking));

	static CCryNameR paramRsmSize("LPVRsmSize");
	pass.SetConstant(paramRsmSize, Vec4((float)cascade.rsmSize, 1.0f / (float)cascade.rsmSize, (float)CRendererCVars::CV_r_LPVDebug, 0.0f));

	static CCryNameR paramSunDir("LPVSunDir");
	const Vec3 sunDir = RenderView()->GetSunLightDirection();
	pass.SetConstant(paramSunDir, Vec4(sunDir.x, sunDir.y, sunDir.z, 0.0f));

	static CCryNameR paramSunColor("LPVSunColor");
	const Vec4 sunColor = RenderView()->GetSunLightColor();
	pass.SetConstant(paramSunColor, Vec4(sunColor.x, sunColor.y, sunColor.z, CRendererCVars::CV_r_LPVIntensity));

	static CCryNameR paramRsmToWorld("LPVRsmToWorld");
	pass.SetConstant(paramRsmToWorld, cascade.rsmToWorld);

	static CCryNameR paramDepthRange("LPVRsmDepthRange");
	const float depthSpan = max(1e-3f, cascade.rsmFarDist - cascade.rsmNearDist);
	pass.SetConstant(paramDepthRange, Vec4(cascade.rsmNearDist, cascade.rsmFarDist, depthSpan, 1.0f / depthSpan));

	const uint32 dispatchSize = lpvDispatchSize(cascade.rsmSampleCount, LPVInjectTileSize);
	pass.SetDispatchSize(dispatchSize, dispatchSize, 1);
	pass.PrepareResourcesForUse(commandList);
	pass.Execute(commandList);
}

void CLPVStage::ExecuteResolve(SCascade& cascade, const SScopedComputeCommandList& commandList)
{
	auto& pass = cascade.passResolve;

	if (pass.IsDirty(m_gridSize, cascade.accIndex, cascade.pRsmDepth->GetID()))
	{
		static CCryNameTSCRC techName("LPVResolve");
		pass.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
		pass.SetOutputUAV(0, cascade.pGridSH[0][0]);
		pass.SetOutputUAV(1, cascade.pGridSH[0][1]);
		pass.SetOutputUAV(2, cascade.pGridSH[0][2]);
		pass.SetOutputUAV(3, cascade.pGridGV);
		pass.SetBuffer(0, &cascade.injectionBuffer);
		// Previous frame accumulated grid: ExecuteResolve runs before ExecuteTemporal flips the index.
		pass.SetTexture(1, cascade.pGridAcc[cascade.accIndex][0]);
		pass.SetTexture(2, cascade.pGridAcc[cascade.accIndex][1]);
		pass.SetTexture(3, cascade.pGridAcc[cascade.accIndex][2]);
		// RSM depth for the sky visibility test (open sky proxy: the cell can see the sun).
		pass.SetTexture(4, cascade.pRsmDepth);
	}

	pass.BeginConstantUpdate();
	SetGridConstants(pass, cascade);

	// The surfel weight compensates for the different footprints of an RSM texel and a grid cell:
	// surfel area / cell face area, using the MEASURED RSM texel size of this cascade's own view.
	// Applied here in float instead of pre-quantization in the inject pass, otherwise high sample
	// counts push the per VPL contribution below the fixed point resolution.
	const float fSampleCount = (float)cascade.rsmSampleCount;
	const float injectionWeight = (cascade.rsmTexelSize > 1e-4f)
	                              ? (cascade.rsmTexelSize * cascade.rsmTexelSize) / (cascade.cellSize * cascade.cellSize)
	                              : ((float)m_gridSize * (float)m_gridSize) / (fSampleCount * fSampleCount);

	static CCryNameR paramResolve("LPVResolveParams");
	pass.SetConstant(paramResolve, Vec4(injectionWeight, 0.0f, 0.0f, 0.0f));

	// Secondary bounce feedback constants: cell offset into the previous accumulation grid (both
	// origins are cell snapped) and the gain. The feedback operates entirely in raw grid units - do
	// NOT scale it by the display intensity, that puts the loop gain far above 1 and the grid energy
	// explodes exponentially within seconds.
	Vec3 deltaCells(ZERO);
	if (cascade.bHistoryValid)
		deltaCells = (cascade.gridOrigin - cascade.prevGridOrigin) / cascade.cellSize;
	// Normalized so that r_LPVSecondaryBounce is directly the feedback loop gain: 0.0705 is the
	// SH round trip of one feedback step (iso receiver transfer 0.25 x emission projection C0),
	// so a cvar value of 1 re-emits exactly the irradiance the cell received (a physical single
	// bounce, marginally stable on white scenes). Empirically confirmed: the pre-normalization
	// equilibrium was found at a raw gain of ~13, and 13 x 0.0705 = 0.92 - right at loop gain 1.
	const float bounceGain = cascade.bHistoryValid ? max(0.0f, CRendererCVars::CV_r_LPVSecondaryBounce) / 0.0705f : 0.0f;

	static CCryNameR paramResolve2("LPVResolveParams2");
	pass.SetConstant(paramResolve2, Vec4(deltaCells.x, deltaCells.y, deltaCells.z, bounceGain));

	static CCryNameR paramRsmSize("LPVRsmSize");
	pass.SetConstant(paramRsmSize, Vec4((float)cascade.rsmSize, 1.0f / (float)cascade.rsmSize, (float)CRendererCVars::CV_r_LPVDebug, 0.0f));

	static CCryNameR paramSunDir("LPVSunDir");
	const Vec3 sunDir = RenderView()->GetSunLightDirection();
	pass.SetConstant(paramSunDir, Vec4(sunDir.x, sunDir.y, sunDir.z, 0.0f));

	static CCryNameR paramWorldToRsm("LPVWorldToRsm");
	pass.SetConstant(paramWorldToRsm, cascade.worldToRsm);

	static CCryNameR paramDepthRange("LPVRsmDepthRange");
	const float depthSpan = max(1e-3f, cascade.rsmFarDist - cascade.rsmNearDist);
	pass.SetConstant(paramDepthRange, Vec4(cascade.rsmNearDist, cascade.rsmFarDist, depthSpan, 1.0f / depthSpan));

	// Sky light, premultiplied so that r_LPVSkyLight 1 gives an upward facing outdoor surface an
	// irradiance of 0.2 x the sun luminance (in the sky hue), independent of the display intensity.
	// The luminance is anchored to the sun instead of the raw TOD sky colour: levels that drive
	// their ambient through environment probes can have a near black GetSkyColor(), which silently
	// disabled the whole feature (observed: r_LPVSkyLight 0 vs 1000 indistinguishable). shTransfer
	// is the apply side SH transfer of the injected lobe mix (0.6 x downward cosine lobe self dot
	// 0.3125 + 0.4 x isotropic transfer 0.0705 = 0.2157).
	Vec3 skyPremult(ZERO);
	const float skyGain = max(0.0f, CRendererCVars::CV_r_LPVSkyLight);
	if (skyGain > 0.0f && gEnv->p3DEngine)
	{
		const Vec3 lumWeights(0.2126f, 0.7152f, 0.0722f);
		// Hue source chain: TOD sky colour, then the TOD fog colour 2. On probe driven levels the
		// sky colour is often black - without the fog fallback the hue silently dropped to the
		// neutral constant and no TOD adjustment had any visible effect. Do NOT query
		// E3DPARAM_FOG_COLOR here: C3DEngine::GetGlobalParameter has no getter case for it and
		// asserts (maps with both sky and fog colour 2 black hit an assert dialog on load).
		Vec3 hueSource = gEnv->p3DEngine->GetSkyColor();
		if (hueSource.Dot(lumWeights) < 1e-4f)
			gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_FOG_COLOR2, hueSource);
		const Vec4 sunColor = RenderView()->GetSunLightColor();
		const float lumSky = hueSource.Dot(lumWeights);
		const float lumSun = Vec3(sunColor.x, sunColor.y, sunColor.z).Dot(lumWeights);
		// Normalized to unit luminance; neutral cool fallback when every source is black.
		const Vec3 tint = (lumSky > 1e-4f) ? hueSource / lumSky : Vec3(0.6f, 0.8f, 1.0f) / 0.7719f;
		const Vec3 skyEffective = tint * (lumSun * 0.2f);
		const float shTransfer = 0.2157f;
		const float displayIntensity = max(1.0f, CRendererCVars::CV_r_LPVIntensity);
		skyPremult = skyEffective * (skyGain / (shTransfer * displayIntensity));
	}

	if (CRendererCVars::CV_r_LPVDebug > 0 && &cascade == &m_cascades[0])
	{
		static float lastSkyGain = -1.0f;
		if (fabsf(skyGain - lastSkyGain) > 1e-4f)
		{
			const Vec3 skyColorLog = gEnv->p3DEngine ? gEnv->p3DEngine->GetSkyColor() : Vec3(ZERO);
			CryLog("LPV debug: sky colour (%.3f, %.3f, %.3f) gain %.3f premult (%.5f, %.5f, %.5f)",
			       skyColorLog.x, skyColorLog.y, skyColorLog.z, skyGain,
			       skyPremult.x, skyPremult.y, skyPremult.z);
			lastSkyGain = skyGain;
		}
	}

	static CCryNameR paramSky("LPVSkyColor");
	pass.SetConstant(paramSky, Vec4(skyPremult.x, skyPremult.y, skyPremult.z, skyGain > 0.0f ? 1.0f : 0.0f));

	const uint32 dispatchSize = lpvDispatchSize(m_gridSize, LPVGridTileSize);
	pass.SetDispatchSize(dispatchSize, dispatchSize, dispatchSize);
	pass.PrepareResourcesForUse(commandList);
	pass.Execute(commandList);
}

void CLPVStage::ExecutePropagate(SCascade& cascade, const SScopedComputeCommandList& commandList)
{
	// Same iteration count for every cascade: a reduced count on the far cascades saved GPU time
	// but made their accumulated energy systematically lower than the near cascade's - visible as
	// a brightness step at the cascade boundaries. Energy consistency across the seams wins.
	const int32 iterations = clamp_tpl<int32>(CRendererCVars::CV_r_LPVIterations, 0, 32);

	const uint32 dispatchSize = lpvDispatchSize(m_gridSize, LPVGridTileSize);

	// Gather-only propagation with an explicit running sum (see LPV.cfi for the face solid angle
	// reprojection details).
	for (int32 c = 0; c < SHChannelNum; ++c)
		cascade.pFinalGrid[c] = cascade.pGridSH[0][c].get();

	for (int32 i = 0; i < iterations; ++i)
	{
		const int32 propSrc = i & 1;
		const int32 propDst = 1 - propSrc;
		const int32 sumDst = i & 1;

		// pass 0: first iteration (sum seed = resolve output), pass 1/2: odd/even iterations
		auto& pass = cascade.passPropagate[(i == 0) ? 0 : (1 + (i & 1))];

		if (pass.IsDirty(m_gridSize))
		{
			static CCryNameTSCRC techName("LPVPropagate");
			pass.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
			pass.SetOutputUAV(0, cascade.pGridSH[propDst][0]);
			pass.SetOutputUAV(1, cascade.pGridSH[propDst][1]);
			pass.SetOutputUAV(2, cascade.pGridSH[propDst][2]);
			pass.SetOutputUAV(3, cascade.pGridSum[sumDst][0]);
			pass.SetOutputUAV(4, cascade.pGridSum[sumDst][1]);
			pass.SetOutputUAV(5, cascade.pGridSum[sumDst][2]);
			pass.SetTexture(0, cascade.pGridSH[propSrc][0]);
			pass.SetTexture(1, cascade.pGridSH[propSrc][1]);
			pass.SetTexture(2, cascade.pGridSH[propSrc][2]);
			// the running sum starts as the un-propagated injection result
			CTexture* pSumSrc0 = (i == 0) ? cascade.pGridSH[0][0].get() : cascade.pGridSum[1 - sumDst][0].get();
			CTexture* pSumSrc1 = (i == 0) ? cascade.pGridSH[0][1].get() : cascade.pGridSum[1 - sumDst][1].get();
			CTexture* pSumSrc2 = (i == 0) ? cascade.pGridSH[0][2].get() : cascade.pGridSum[1 - sumDst][2].get();
			pass.SetTexture(3, pSumSrc0);
			pass.SetTexture(4, pSumSrc1);
			pass.SetTexture(5, pSumSrc2);
			pass.SetTexture(6, cascade.pGridGV);
		}

		pass.BeginConstantUpdate();
		SetGridConstants(pass, cascade);

		// Occlusion strength, pre-divided by the SH transfer of a perpendicular fully covered face:
		// dot(cosineLobe(n), cone90(n)) = ZH_COS0*SH_C0*ZH_CONE0*SH_C0 + ZH_COS1*SH_C1*ZH_CONE1*SH_C1
		// = 0.10194, so r_LPVOcclusion 1 blocks such a face completely. y flags the first iteration:
		// it gates with the destination cell GV only (see the shader comment) instead of running
		// unoccluded - the blanket exemption used before gave every wall one free crossing.
		static CCryNameR paramProp("LPVPropParams");
		const float occStrength = max(0.0f, CRendererCVars::CV_r_LPVOcclusion) / 0.10194f;
		pass.SetConstant(paramProp, Vec4(occStrength, (i == 0) ? 1.0f : 0.0f, 0.0f, 0.0f));

		pass.SetDispatchSize(dispatchSize, dispatchSize, dispatchSize);
		pass.PrepareResourcesForUse(commandList);
		pass.Execute(commandList);

		for (int32 c = 0; c < SHChannelNum; ++c)
			cascade.pFinalGrid[c] = cascade.pGridSum[sumDst][c].get();
	}
}

void CLPVStage::ExecuteTemporal(SCascade& cascade, const SScopedComputeCommandList& commandList)
{
	const float alpha = clamp_tpl(CRendererCVars::CV_r_LPVTemporalAlpha, 0.01f, 1.0f);

	// The reprojection is a pure integer cell shift because both origins are snapped to the cell size.
	Vec3 deltaCells(ZERO);
	if (cascade.bHistoryValid)
		deltaCells = (cascade.gridOrigin - cascade.prevGridOrigin) / cascade.cellSize;

	// Blend weight 1 (or no usable history) means the accumulator is a plain copy of the current frame.
	const float effectiveAlpha = cascade.bHistoryValid ? alpha : 1.0f;

	const int32 src = cascade.accIndex;
	const int32 dst = 1 - cascade.accIndex;
	auto& pass = cascade.passTemporal[src];

	CRY_ASSERT(cascade.pFinalGrid[0] && cascade.pFinalGrid[1] && cascade.pFinalGrid[2]);

	if (pass.IsDirty(m_gridSize, cascade.pFinalGrid[0]->GetID()))
	{
		static CCryNameTSCRC techName("LPVTemporal");
		pass.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
		pass.SetOutputUAV(0, cascade.pGridAcc[dst][0]);
		pass.SetOutputUAV(1, cascade.pGridAcc[dst][1]);
		pass.SetOutputUAV(2, cascade.pGridAcc[dst][2]);
		pass.SetTexture(0, cascade.pFinalGrid[0]);
		pass.SetTexture(1, cascade.pFinalGrid[1]);
		pass.SetTexture(2, cascade.pFinalGrid[2]);
		pass.SetTexture(3, cascade.pGridAcc[src][0]);
		pass.SetTexture(4, cascade.pGridAcc[src][1]);
		pass.SetTexture(5, cascade.pGridAcc[src][2]);
	}

	pass.BeginConstantUpdate();
	SetGridConstants(pass, cascade);

	static CCryNameR paramTemporal("LPVTemporalParams");
	pass.SetConstant(paramTemporal, Vec4(deltaCells.x, deltaCells.y, deltaCells.z, effectiveAlpha));

	// Local light injection, folded into this pass (a separate pass would need a typed RGBA16F UAV
	// read-modify-write, which is an optional feature on D3D11). Same display intensity decoupling
	// as the sky light, 0.25 is the iso receiver transfer: r_LPVPointLights 1 gives a surface at
	// the light centre an ambient of roughly the light colour.
	const float displayIntensity = max(1.0f, CRendererCVars::CV_r_LPVIntensity);
	const float lightPremult = max(0.0f, CRendererCVars::CV_r_LPVPointLights) / (0.25f * displayIntensity);

	static CCryNameR paramLights("LPVLightParams");
	pass.SetConstant(paramLights, Vec4((float)m_lightCount, lightPremult, 0.0f, 0.0f));

	static CCryNameR paramLightPos("LPVLightPos");
	pass.SetConstantArray(paramLightPos, m_lightPos, MaxPointLights);

	static CCryNameR paramLightColor("LPVLightColor");
	pass.SetConstantArray(paramLightColor, m_lightColor, MaxPointLights);

	static CCryNameR paramLightDir("LPVLightDir");
	pass.SetConstantArray(paramLightDir, m_lightDir, MaxPointLights);

	const uint32 dispatchSize = lpvDispatchSize(m_gridSize, LPVGridTileSize);
	pass.SetDispatchSize(dispatchSize, dispatchSize, dispatchSize);
	pass.PrepareResourcesForUse(commandList);
	pass.Execute(commandList);

	cascade.accIndex = dst;
}

void CLPVStage::ExecuteApply(const SScopedComputeCommandList& commandList, CTexture* pRsmDepth)
{
	auto& pass = m_passApply;

	const int32 screenWidth = m_pIrradiance->GetWidth();
	const int32 screenHeight = m_pIrradiance->GetHeight();

	SCascade& nearCascade = m_cascades[0];
	SCascade& midCascade = m_cascades[1];
	SCascade& outerCascade = m_cascades[2];
	const bool bMidActive = m_activeCascadeCount > 1 && midCascade.bHistoryValid;
	const bool bOuterActive = m_activeCascadeCount > 2 && outerCascade.bHistoryValid;

	// The linear depth and the scene normals are not stable texture objects, key the bindings on
	// their IDs so a swap rebinds.
	CTexture* pLinearDepth = m_graphicsPipelineResources.m_pTexLinearDepth;
	CTexture* pSceneNormals = m_graphicsPipelineResources.m_pTexSceneNormalsMap;

	// Height map AO mask (openness to the sky), used to damp the coarse cascades indoors. Falls
	// back to the white texture whenever the stage did not run this frame - a natural no-op.
	CTexture* pHeightMapAO = CRendererResources::s_ptexWhite;
	if (auto* pHmaoStage = m_graphicsPipeline.GetStage<CHeightMapAOStage>())
	{
		if (pHmaoStage->IsValid())
			pHeightMapAO = pHmaoStage->GetHeightMapAOTex();
	}

	if (pass.IsDirty(m_gridSize, screenWidth, screenHeight, nearCascade.accIndex, midCascade.accIndex,
	                 outerCascade.accIndex, m_activeCascadeCount, pRsmDepth->GetID(), pLinearDepth->GetID(),
	                 pSceneNormals->GetID(), pHeightMapAO->GetID()))
	{
		static CCryNameTSCRC techName("LPVApply");
		pass.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
		pass.SetOutputUAV(0, m_pIrradiance);
		pass.SetOutputUAV(1, m_pSpecular);
		pass.SetTexture(0, pLinearDepth);
		pass.SetTexture(1, pSceneNormals);
		pass.SetTexture(2, nearCascade.pGridAcc[nearCascade.accIndex][0]);
		pass.SetTexture(3, nearCascade.pGridAcc[nearCascade.accIndex][1]);
		pass.SetTexture(4, nearCascade.pGridAcc[nearCascade.accIndex][2]);
		pass.SetBuffer(5, &nearCascade.injectionBuffer);
		pass.SetTexture(6, pRsmDepth);
		pass.SetTexture(7, midCascade.pGridAcc[midCascade.accIndex][0]);
		pass.SetTexture(8, midCascade.pGridAcc[midCascade.accIndex][1]);
		pass.SetTexture(9, midCascade.pGridAcc[midCascade.accIndex][2]);
		pass.SetTexture(10, outerCascade.pGridAcc[outerCascade.accIndex][0]);
		pass.SetTexture(11, outerCascade.pGridAcc[outerCascade.accIndex][1]);
		pass.SetTexture(12, outerCascade.pGridAcc[outerCascade.accIndex][2]);
		pass.SetTexture(13, pHeightMapAO);
		pass.SetSampler(0, EDefaultSamplerStates::TrilinearClamp);
	}

	pass.SetInlineConstantBuffer(eConstantBufferShaderSlot_PerView, m_graphicsPipeline.GetMainViewConstantBuffer());

	pass.BeginConstantUpdate();
	SetGridConstants(pass, nearCascade);

	// Far cascade volumes, y of the second constant doubles as the enable flag.
	static CCryNameR paramFar("LPVGridParamsFar");
	pass.SetConstant(paramFar, Vec4(midCascade.gridOrigin.x, midCascade.gridOrigin.y, midCascade.gridOrigin.z, midCascade.cellSize));
	static CCryNameR paramFar2("LPVGridParams2Far");
	pass.SetConstant(paramFar2, Vec4((float)m_gridSize * midCascade.cellSize, bMidActive ? 1.0f : 0.0f, 0.0f, 0.0f));

	static CCryNameR paramOuter("LPVGridParamsFar2");
	pass.SetConstant(paramOuter, Vec4(outerCascade.gridOrigin.x, outerCascade.gridOrigin.y, outerCascade.gridOrigin.z, outerCascade.cellSize));
	static CCryNameR paramOuter2("LPVGridParams2Far2");
	pass.SetConstant(paramOuter2, Vec4((float)m_gridSize * outerCascade.cellSize, bOuterActive ? 1.0f : 0.0f, 0.0f, 0.0f));

	// x: height map AO damping strength for the coarse cascades (0 disables the sampling).
	// y: translucent surface irradiance gain (r_LPVTranslucentBrightness). 0 = off; the neutral 1 is
	//    sent as 0 as well so that the default skips the shader branch.
	static CCryNameR paramHmao("LPVHmaoParams");
	const float hmaoStrength = (pHeightMapAO != CRendererResources::s_ptexWhite)
	                           ? clamp_tpl(CRendererCVars::CV_r_LPVHeightMapOcclusion, 0.0f, 1.0f)
	                           : 0.0f;
	const float translucentGain = max(0.0f, CRendererCVars::CV_r_LPVTranslucentBrightness);
	pass.SetConstant(paramHmao, Vec4(hmaoStrength, (translucentGain != 1.0f) ? translucentGain : 0.0f, 0.0f, 0.0f));

	static CCryNameR paramScreenSize("LPVScreenSize");
	const float fScreenWidth = (float)screenWidth;
	const float fScreenHeight = (float)screenHeight;
	pass.SetConstant(paramScreenSize, Vec4(fScreenWidth, fScreenHeight, 1.0f / fScreenWidth, 1.0f / fScreenHeight));

	// Explicit view ray reconstruction, see LPV.cfi. m_frustumCorners are the world space corner rays
	// of the camera frustum, the same data the engine uses for its WPOS fullscreen reconstruction.
	const SRenderViewInfo& viewInfo = GetCurrentViewInfo();

	static CCryNameR paramCamPos("LPVCamPos");
	pass.SetConstant(paramCamPos, Vec4(viewInfo.cameraOrigin, 0.0f));

	static CCryNameR paramFrustumLT("LPVFrustumLT");
	pass.SetConstant(paramFrustumLT, Vec4(viewInfo.m_frustumCorners[SRenderViewInfo::eFrustum_LT], 0.0f));
	static CCryNameR paramFrustumRT("LPVFrustumRT");
	pass.SetConstant(paramFrustumRT, Vec4(viewInfo.m_frustumCorners[SRenderViewInfo::eFrustum_RT], 0.0f));
	static CCryNameR paramFrustumLB("LPVFrustumLB");
	pass.SetConstant(paramFrustumLB, Vec4(viewInfo.m_frustumCorners[SRenderViewInfo::eFrustum_LB], 0.0f));
	static CCryNameR paramFrustumRB("LPVFrustumRB");
	pass.SetConstant(paramFrustumRB, Vec4(viewInfo.m_frustumCorners[SRenderViewInfo::eFrustum_RB], 0.0f));

	static CCryNameR paramApply("LPVApplyParams");
	pass.SetConstant(paramApply, Vec4(CRendererCVars::CV_r_LPVIntensity, (float)CRendererCVars::CV_r_LPVDebug,
	                                  max(0.0f, CRendererCVars::CV_r_LPVMaxIrradiance),
	                                  max(0.0f, CRendererCVars::CV_r_LPVSpecular)));

	// The apply pass debug views (mode 8) inspect the near cascade's RSM.
	const SCascade& nearRsm = m_cascades[0];

	static CCryNameR paramRsmSizeApply("LPVRsmSize");
	pass.SetConstant(paramRsmSizeApply, Vec4((float)nearRsm.rsmSize, 1.0f / (float)max(1, nearRsm.rsmSize), (float)CRendererCVars::CV_r_LPVDebug, 0.0f));

	// Alignment probes of debug mode 8 (content vs matrix investigation).
	static CCryNameR paramRsmToWorldApply("LPVRsmToWorld");
	pass.SetConstant(paramRsmToWorldApply, nearRsm.rsmToWorld);
	static CCryNameR paramWorldToRsmApply("LPVWorldToRsm");
	pass.SetConstant(paramWorldToRsmApply, nearRsm.worldToRsm);
	static CCryNameR paramDepthRangeApply("LPVRsmDepthRange");
	const float depthSpanApply = max(1e-3f, nearRsm.rsmFarDist - nearRsm.rsmNearDist);
	pass.SetConstant(paramDepthRangeApply, Vec4(nearRsm.rsmNearDist, nearRsm.rsmFarDist, depthSpanApply, 1.0f / depthSpanApply));

	pass.SetDispatchSize(lpvDispatchSize(screenWidth, LPVApplyTileSize), lpvDispatchSize(screenHeight, LPVApplyTileSize), 1);
	pass.PrepareResourcesForUse(commandList);
	pass.Execute(commandList);
}

void CLPVStage::ExecuteDebugProbes(CTexture* pColorTarget, CTexture* pDepthTarget)
{
	// m_bResultValid also covers the grid transform and the accumulation index: they are only up to
	// date when Execute() ran all the way through for this frame.
	// 10 = probe balls only, 11 = balls + surface mosaic (the mosaic branch lives in LPVApplyCS).
	// The balls visualize the near cascade.
	if ((CRendererCVars::CV_r_LPVDebug != 10 && CRendererCVars::CV_r_LPVDebug != 11) || !m_bResultValid || m_gridSize <= 0)
		return;

	if (!CShaderMan::s_shDeferredShading)
		return;

	if (!CTexture::IsTextureExist(pColorTarget) || !CTexture::IsTextureExist(pDepthTarget))
		return;

	SCascade& cascade = m_cascades[0];

	CTexture* pGridR = cascade.pGridAcc[cascade.accIndex][0];
	CTexture* pGridG = cascade.pGridAcc[cascade.accIndex][1];
	CTexture* pGridB = cascade.pGridAcc[cascade.accIndex][2];
	if (!CTexture::IsTextureExist(pGridR) || !CTexture::IsTextureExist(pGridG) || !CTexture::IsTextureExist(pGridB))
		return;

	CRenderView* pRenderView = RenderView();
	if (!pRenderView)
		return;

	PROFILE_LABEL_SCOPE("LPV_DEBUG_PROBES");

	auto& pass = m_passDebugProbes;
	auto& prim = m_primDebugProbes;

	pass.SetRenderTarget(0, pColorTarget);
	pass.SetDepthTarget(pDepthTarget);
	pass.SetViewport(pRenderView->GetViewport());
	pass.BeginAddingPrimitives();

	static CCryNameTSCRC techName("LPVDebugProbes");
	prim.SetFlags(CRenderPrimitive::eFlags_ReflectShaderConstants);
	prim.SetTechnique(CShaderMan::s_shDeferredShading, techName, 0);
	// Reverse depth is always on in this pipeline: test against the opaque scene, never write depth.
	prim.SetRenderState(GS_DEPTHFUNC_GEQUAL);
	prim.SetCullMode(eCULL_None);
	prim.SetTexture(0, pGridR);
	prim.SetTexture(1, pGridG);
	prim.SetTexture(2, pGridB);

	// One instanced impostor quad per cell. The vertex stream stays empty (the vertex shader builds
	// the quad from SV_VertexID), and ePrim_Custom is required so that CRenderPrimitive::Compile()
	// does not overwrite the instance count with the shared primitive geometry cache.
	prim.SetCustomVertexStream(~0u, EDefaultInputLayouts::Empty, 0);
	prim.SetDrawInfo(eptTriangleStrip, 0, 0, 4, (uint32)(m_gridSize * m_gridSize * m_gridSize));
	prim.SetInlineConstantBuffer(eConstantBufferShaderSlot_PerView, m_graphicsPipeline.GetMainViewConstantBuffer(), EShaderStage_Vertex);
	prim.Compile(pass);

	auto& constantManager = prim.GetConstantManager();
	constantManager.BeginNamedConstantUpdate();

	static CCryNameR paramGrid("LPVGridParams");
	static CCryNameR paramGrid2("LPVGridParams2");
	static CCryNameR paramProbe("LPVProbeParams");

	const float fGridSize = (float)m_gridSize;
	const Vec4 gridParams(cascade.gridOrigin.x, cascade.gridOrigin.y, cascade.gridOrigin.z, cascade.cellSize);
	const Vec4 gridParams2(fGridSize, 1.0f / fGridSize, 1.0f / cascade.cellSize, fGridSize * cascade.cellSize);
	// x: probe radius, y: colour scale, z: floor that keeps unlit probes visible as dark grey balls.
	const Vec4 probeParams(cascade.cellSize * 0.1f, CRendererCVars::CV_r_LPVIntensity, 0.05f, 0.0f);

	constantManager.SetNamedConstant(paramGrid, gridParams, eHWSC_Vertex);
	constantManager.SetNamedConstant(paramGrid2, gridParams2, eHWSC_Vertex);
	constantManager.SetNamedConstant(paramProbe, probeParams, eHWSC_Vertex);
	constantManager.SetNamedConstant(paramProbe, probeParams, eHWSC_Pixel);

	constantManager.EndNamedConstantUpdate(&pass.GetViewport(), pRenderView);

	pass.AddPrimitive(&prim);
	pass.Execute();
}
