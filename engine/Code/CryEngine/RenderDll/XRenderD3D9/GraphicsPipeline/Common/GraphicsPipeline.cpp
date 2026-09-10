// Copyright 2017-2021 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "GraphicsPipeline.h"
#include "Common/RendererResources.h"
#include "Common/Textures/TextureHelpers.h"
#include "XRenderD3D9/D3DPostProcess.h"
#include "../SceneReferredExport.h"
#include "../PostAA.h"
#include "../ShadowMap.h"
#include "../SceneGBuffer.h"
#include "../SceneForward.h"
#include "../SceneCustom.h"
#include "../TiledLightVolumes.h"
#include "../ClipVolumes.h"
#include "../Fog.h"
#include "../Sky.h"
#include "../VolumetricFog.h"
#include "../DebugRenderTargets.h"

void CGraphicsPipelineResources::Init()
{
	// Default Template textures
	int nRTFlags = FT_DONT_RELEASE | FT_DONT_STREAM | FT_STATE_CLAMP | FT_USAGE_RENDERTARGET;

	m_pTexVelocityObjects[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityObjects").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET | FT_USAGE_UNORDERED_ACCESS, eTF_Unknown, -1);
	// Only used for VR, but we need to support runtime switching
 	m_pTexVelocityObjects[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityObjects_R").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET | FT_USAGE_UNORDERED_ACCESS, eTF_Unknown, -1);

	m_pTexSceneNormalsMap = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneNormalsMap").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8, TO_SCENE_NORMALMAP);
	m_pTexSceneDiffuse = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDiffuse").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8);
	m_pTexSceneSpecular = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSpecular").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8);

	m_pTexLinearDepth = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTarget").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown);
	m_pTexHDRTarget = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTarget").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_Unknown);
	m_pTexShadowMask = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ShadowMask").c_str(), 0, 0, 1, eTT_2DArray, nRTFlags, eTF_R8, TO_SHADOWMASK);
	m_pTexSceneNormalsBent = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneNormalsBent").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8);

	m_pTexSceneDepthScaled[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDepthScaled").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_DEPTHSTENCIL, eTF_Unknown);
	m_pTexSceneDepthScaled[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDepthScaled2").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_DEPTHSTENCIL, eTF_Unknown);
	m_pTexSceneDepthScaled[2] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDepthScaled3").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_DEPTHSTENCIL, eTF_Unknown);

	m_pTexLinearDepthScaled[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetScaled").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_DOWNSCALED_ZTARGET_FOR_AO);
	m_pTexLinearDepthScaled[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetScaled2").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_QUARTER_ZTARGET_FOR_AO);
	m_pTexLinearDepthScaled[2] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetScaled3").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown);

	m_pTexClipVolumes = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ClipVolumes").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8);

	m_pTexSceneDiffuseTmp = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDiffuseTmp").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8, TO_SCENE_DIFFUSE_ACC);
	m_pTexSceneSpecularTmp[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSpecularTmp").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8, TO_SCENE_SPECULAR_ACC);
	m_pTexSceneSpecularTmp[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSpecularTmp 1/2").c_str(), 0, 0, 1, eTT_2D, nRTFlags, eTF_R8G8B8A8, -1);

	m_pTexDisplayTargetScaled[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/2a").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_BACKBUFFERSCALED_D2);
	m_pTexDisplayTargetScaled[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/4a").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_BACKBUFFERSCALED_D4);
	m_pTexDisplayTargetScaled[2] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/8").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_BACKBUFFERSCALED_D8);

	m_pTexDisplayTargetScaledTemp[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/2b").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);
	m_pTexDisplayTargetScaledTemp[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/4b").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);

	m_pTexDisplayTargetSrc = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_BACKBUFFERMAP);
	m_pTexDisplayTargetDst = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTargetDst").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown);

	m_pTexSceneSelectionIDs = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSelectionIDs").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_R32F);
	
	m_pTexSceneTargetR11G11B10F[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneTargetR11G11B10F_0").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);
	m_pTexSceneTargetR11G11B10F[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneTargetR11G11B10F_1").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);

	m_pTexLinearDepthFixup = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetFixup").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown);
	m_pTexSceneTarget = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneTarget").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, TO_SCENE_TARGET);

	if (RainOcclusionMapsEnabled())
		PrepareRainOcclusionMaps();

	m_pTexVelocity = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$Velocity").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);
	m_pTexVelocityTiles[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityTilesTmp0").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);
	m_pTexVelocityTiles[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityTilesTmp1").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);
	m_pTexVelocityTiles[2] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityTiles").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);

	m_pTexWaterVolumeRefl[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$WaterVolumeRefl").c_str(), 64, 64, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET | FT_FORCE_MIPS, eTF_Unknown, TO_WATERVOLUMEREFLMAP);
	m_pTexWaterVolumeRefl[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$WaterVolumeReflPrev").c_str(), 64, 64, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET | FT_FORCE_MIPS, eTF_Unknown, TO_WATERVOLUMEREFLMAPPREV);

	m_pTexHUD3D[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$Cached3DHUD").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_NOMIPS | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);
	m_pTexHUD3D[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$Cached3DHUD 1/4").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_NOMIPS | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown, -1);

	CreateResources(0, 0);
}

void CGraphicsPipelineResources::CreateResources(int resourceWidth, int resourceHeight)
{
	if (!resourceWidth) resourceWidth = m_resourceWidth;
	if (!resourceHeight) resourceHeight = m_resourceHeight;

	if (!resourceWidth) resourceWidth = m_graphicsPipeline.GetRenderResolution().x;
	if (!resourceHeight) resourceHeight = m_graphicsPipeline.GetRenderResolution().y;

	CRY_ASSERT(resourceWidth);
	CRY_ASSERT(resourceHeight);

	CreateDepthMaps(resourceWidth, resourceHeight);
	CreateDeferredMaps(resourceWidth, resourceHeight);
	CreateHDRMaps(resourceWidth, resourceHeight);
	CreatePostFXMaps(resourceWidth, resourceHeight);
	CreateSceneMaps(resourceWidth, resourceHeight);
	CreateHUDMaps(resourceWidth, resourceHeight);

	m_resourceWidth = resourceWidth;
	m_resourceHeight = resourceHeight;
}

void CGraphicsPipelineResources::Resize(int renderWidth, int renderHeight)
{
	int resourceWidth = m_resourceWidth;
	int resourceHeight = m_resourceHeight;

	if (resourceWidth != renderWidth ||
		resourceHeight != renderHeight)
	{
		resourceWidth = renderWidth;
		resourceHeight = renderHeight;

		CreateDepthMaps(resourceWidth, resourceHeight);
		CreateDeferredMaps(resourceWidth, resourceHeight);
		CreateHDRMaps(resourceWidth, resourceHeight);
		CreatePostFXMaps(resourceWidth, resourceHeight);
		CreateSceneMaps(resourceWidth, resourceHeight);
		CreateHUDMaps(resourceWidth, resourceHeight);
	}

	m_resourceWidth = resourceWidth;
	m_resourceHeight = resourceHeight;
}

void CGraphicsPipelineResources::CreateDeferredMaps(int resourceWidth, int resourceHeight)
{
	Vec2i resolution = Vec2i(resourceWidth, resourceHeight);
	const int width = resolution.x, width_r2 = (width + 1) / 2, width_r4 = (width_r2 + 1) / 2, width_r8 = (width_r4 + 1) / 2;
	const int height = resolution.y, height_r2 = (height + 1) / 2, height_r4 = (height_r2 + 1) / 2, height_r8 = (height_r4 + 1) / 2;

	ETEX_Format preferredDepthFormat = CRendererResources::GetDepthFormat();
	ETEX_Format fmtZScaled = eTF_R16G16B16A16F;

	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneNormalsMap").c_str(), m_pTexSceneNormalsMap, width, height, Clr_Unknown, true, false, eTF_R8G8B8A8, TO_SCENE_NORMALMAP, FT_USAGE_ALLOWREADSRGB);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDiffuse").c_str(), m_pTexSceneDiffuse, width, height, Clr_Empty, true, false, eTF_R8G8B8A8, -1, FT_USAGE_ALLOWREADSRGB);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSpecular").c_str(), m_pTexSceneSpecular, width, height, Clr_Empty, true, false, eTF_R8G8B8A8, -1, FT_USAGE_ALLOWREADSRGB);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityObjects").c_str(), m_pTexVelocityObjects[0], width, height, Clr_Transparent, true, false, eTF_R16G16F, -1, FT_USAGE_UNORDERED_ACCESS);
	if (gRenDev->IsStereoEnabled())
	{
		SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityObjects_R").c_str(), m_pTexVelocityObjects[1], width, height, Clr_Transparent, true, false, eTF_R16G16F, -1, FT_USAGE_UNORDERED_ACCESS);
	}

	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneNormalsBent").c_str(), m_pTexSceneNormalsBent, width, height, Clr_Median, true, false, eTF_R8G8B8A8);

	SD3DPostEffectsUtils::GetOrCreateDepthStencil(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDepthScaled").c_str(), m_pTexSceneDepthScaled[0], width_r2, height_r2, Clr_FarPlane_Rev, false, false, preferredDepthFormat, -1, FT_USAGE_DEPTHSTENCIL);
	SD3DPostEffectsUtils::GetOrCreateDepthStencil(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDepthScaled2").c_str(), m_pTexSceneDepthScaled[1], width_r4, height_r4, Clr_FarPlane_Rev, false, false, preferredDepthFormat, -1, FT_USAGE_DEPTHSTENCIL);
	SD3DPostEffectsUtils::GetOrCreateDepthStencil(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDepthScaled3").c_str(), m_pTexSceneDepthScaled[2], width_r8, height_r8, Clr_FarPlane_Rev, false, false, preferredDepthFormat, -1, FT_USAGE_DEPTHSTENCIL);

	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetScaled").c_str(), m_pTexLinearDepthScaled[0], width_r2, height_r2, ColorF(1.0f, 1.0f, 1.0f, 1.0f), 1, 0, fmtZScaled, TO_DOWNSCALED_ZTARGET_FOR_AO);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetScaled2").c_str(), m_pTexLinearDepthScaled[1], width_r4, height_r4, ColorF(1.0f, 1.0f, 1.0f, 1.0f), 1, 0, fmtZScaled, TO_QUARTER_ZTARGET_FOR_AO);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ZTargetScaled3").c_str(), m_pTexLinearDepthScaled[2], width_r8, height_r8, ColorF(1.0f, 1.0f, 1.0f, 1.0f), 1, 0, fmtZScaled);

	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ClipVolumes").c_str(), m_pTexClipVolumes, width, height, Clr_Empty, false, false, eTF_R8G8);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$AOColorBleed").c_str(), m_pTexAOColorBleed, width_r8, height_r8, Clr_Unknown, true, false, eTF_R8G8B8A8);

	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneDiffuseTmp").c_str(), m_pTexSceneDiffuseTmp, width, height, Clr_Empty, true, false, eTF_R8G8B8A8);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSpecularTmp").c_str(), m_pTexSceneSpecularTmp[0], width, height, Clr_Median, true, false, eTF_R8G8B8A8);
	SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSpecularTmp 1/2").c_str(), m_pTexSceneSpecularTmp[1], width_r2, height_r2, Clr_Median, true, false, eTF_R8G8B8A8);

	// shadow mask
	if (m_pTexShadowMask)
		m_pTexShadowMask->Invalidate(resolution.x, resolution.y, eTF_R8);

	if (!CTexture::IsTextureExist(m_pTexShadowMask))
	{
		const int nArraySize = gcpRendD3D->CV_r_ShadowCastingLightsMaxCount;
		m_pTexShadowMask = CTexture::GetOrCreateTextureArray(m_graphicsPipeline.MakeUniqueTexIdentifierName("$ShadowMask").c_str(), resolution.x, resolution.y, nArraySize, 1, eTT_2DArray, FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_R8, TO_SHADOWMASK);
	}

	// Pre-create shadow pool
	IF(gcpRendD3D->m_pRT->IsRenderThread() && gEnv->p3DEngine, 1)
	{
		//init shadow pool size
		gcpRendD3D->m_nShadowPoolHeight = CRendererCVars::CV_e_ShadowsPoolSize;
		gcpRendD3D->m_nShadowPoolWidth = CRendererCVars::CV_e_ShadowsPoolSize; //square atlas
	}
}

void CGraphicsPipelineResources::CreateDepthMaps(int resourceWidth, int resourceHeight)
{
	const uint32 nRTFlags = FT_DONT_STREAM | FT_DONT_RELEASE | FT_USAGE_RENDERTARGET;
	uint32 nUAFlags = FT_DONT_STREAM | FT_DONT_RELEASE | FT_USAGE_RENDERTARGET | FT_USAGE_UNORDERED_ACCESS | FT_USAGE_UAV_RWTEXTURE;

	m_pTexLinearDepth->SetFlags(nRTFlags);
	m_pTexLinearDepth->SetWidth(resourceWidth);
	m_pTexLinearDepth->SetHeight(resourceHeight);
	m_pTexLinearDepth->CreateRenderTarget(CRendererResources::s_eTFZ, ColorF(1.0f, 1.0f, 1.0f, 1.0f));

	if (!CRendererCVars::CV_r_HDRTexFormat && !CTexture::IsTextureExist(m_pTexLinearDepthFixup))
	{
		m_pTexLinearDepthFixup->SetFlags(nUAFlags);
		m_pTexLinearDepthFixup->SetWidth(resourceWidth);
		m_pTexLinearDepthFixup->SetHeight(resourceHeight);
		m_pTexLinearDepthFixup->CreateRenderTarget(eTF_R32F, ColorF(1.0f, 1.0f, 1.0f, 1.0f));

		SResourceView typedUAV = SResourceView::UnorderedAccessView(DXGI_FORMAT_R32_UINT, 0, -1, 0, SResourceView::eUAV_ReadWrite);
		m_pTexLinearDepthFixupUAV = m_pTexLinearDepthFixup->GetDevTexture()->GetOrCreateResourceViewHandle(typedUAV);
	}
}

// Scene-referred pipeline (SceneReferredSpec.md, S0 fp16 forcing).
//
// Seven colour-path targets take the "low quality" HDR alias and are therefore R11G11B10F at
// r_HDRTexFormat 0 and 1: $HDRTargetPrev[0..1], $SceneTargetR11G11B10F[0..1], $HDRFinalBloom
// and $WaterVolumeRefl[0..1]. That format has no sign bit and 5-6 mantissa bits, so on the
// scene-referred path it would silently clip every negative channel the wide-gamut working
// space produces - the worst kind of failure: a wrong picture with no error anywhere.
//
// The cvar stays a floor and gains a second one: effective = max(cvar, sceneReferred ? 2 : 0).
// The decision is made HERE and not inside CRendererResources::GetHDRFormat(), which is
// engine-wide - value 0 there also selects the depth-fixup / thin-hair forward path and the
// PostAA accumulator format, none of which may move. With the switch off this function is a
// straight pass-through to the stock helper, so the stock formats are bit-for-bit unchanged.
static ETEX_Format ResolveHDRSatelliteFormat()
{
	const int effective = std::max(CRenderer::CV_r_HDRTexFormat, CRendererResources::IsSceneReferredStage(1) ? 2 : 0);

	if (effective >= 2)
		return eTF_R16G16B16A16F;

	return CRendererResources::GetHDRFormat(false, true);
}

// The same floor for the MAIN HDR target and its three scaled siblings, which take the "high
// quality" alias and are therefore fp16 already at the registered default r_HDRTexFormat 1 - but
// R11G11B10F at r_HDRTexFormat 0. That target IS the working space: the 709 -> AP1 conversion
// writes it in place and every post effect reads it from there, so a user typing
// "r_HDRTexFormat 0" would put the whole scene-referred chain in an unsigned 11:11:10 buffer.
// 709 -> AP1 alone produces no negatives (Rec.709's primaries lie inside AP1), so S3 by itself
// would survive it - but S4's CDL and any LMT legitimately can, and then the sign bit matters.
//
// Same shape as the satellites: effective = max(cvar, sceneReferred ? 2 : 0), decided HERE rather
// than inside CRendererResources::GetHDRFormat(), which is engine-wide. With the switch off this
// is a straight pass-through to the stock helper, and at the default cvar value it is a no-op on
// both paths, which is why this costs nothing in practice and closes the hole anyway.
static ETEX_Format ResolveHDRMainFormat()
{
	const int effective = std::max(CRenderer::CV_r_HDRTexFormat, CRendererResources::IsSceneReferredStage(1) ? 2 : 0);

	if (effective >= 1)
		return eTF_R16G16B16A16F;

	return CRendererResources::GetHDRFormat(false, false);
}

// The five scaled DISPLAY targets - $DisplayTarget 1/2a, 1/4a, 1/8 and the two Temp siblings
// (SceneReferredSpec.md S2, decisions/s2-sun-shafts.md approach A). They are R10G10B10A2 UNORM in
// stock CE - hard clamped to [0,1] with 2 bits of alpha - and they are the working set the sun
// shaft stage borrows, which is why the whole shaft effect is display-referred: the mask
// (linearDepth * 1.32 * (R+G+B), Sunshafts.cfx) clips to white for any pixel above about a quarter
// of a mid-grey sunlit surface, so the shaft source is a binary silhouette rather than a luminance
// image. On the scene-referred path the shafts carry the sun's real exposed radiance, which needs
// a target that can hold it.
//
// All five or none: they are ping-ponged AND addressable by name from content-side shader scripts
// as $backbufferscaled_d2 / _d4 / _d8, so a mixed set would be a silent precision cliff that
// content could stumble into.
//
// $DisplayTarget and $DisplayTargetDst are deliberately NOT in this set. Those are the tone map's
// output, still display-referred by design, and moving them is S4's problem. With the switch off
// this is a straight pass-through to CRendererResources::GetLDRFormat(true), so every one of these
// targets is bit-for-bit stock, including the SSAO albedo downsample and the snow velocity passes
// that share them.
static ETEX_Format ResolveDisplayTargetScaledFormat()
{
	if (CRendererResources::IsSceneReferredStage(3))
		return eTF_R16G16B16A16F;

	return CRendererResources::GetLDRFormat(true);
}

void CGraphicsPipelineResources::CreateHDRMaps(int resourceWidth, int resourceHeight)
{
	m_renderTargetPool.ClearRenderTargetList();

	const int width = resourceWidth, width_r2 = (width + 1) / 2, width_r4 = (width_r2 + 1) / 2, width_r8 = (width_r4 + 1) / 2, width_r16 = (width_r8 + 1) / 2;
	const int height = resourceHeight, height_r2 = (height + 1) / 2, height_r4 = (height_r2 + 1) / 2, height_r8 = (height_r4 + 1) / 2, height_r16 = (height_r8 + 1) / 2;
	uint32 nHDRTargetFlags = FT_DONT_RELEASE;
	uint32 nHDRTargetFlagsUAV = nHDRTargetFlags | (FT_USAGE_UNORDERED_ACCESS);  // UAV required for tiled deferred shading

	const ETEX_Format nHDRFormat = ResolveHDRMainFormat();                         // No alpha, default is HiQ, can be downgraded - forced to fp16 on the scene-referred path
	const ETEX_Format nHDRQFormat = ResolveHDRSatelliteFormat();                   // No alpha, default is LoQ, can be upgraded - forced to fp16 on the scene-referred path
	const ETEX_Format nHDRAFormat = CRendererResources::GetHDRFormat(true, false); // With alpha
	
	m_renderTargetPool.AddRenderTarget(width, height, Clr_Unknown, nHDRFormat, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTarget").c_str(), &m_pTexHDRTarget, nHDRTargetFlagsUAV);

	// Scaled versions of the HDR scene texture
	m_renderTargetPool.AddRenderTarget(width_r2, height_r2, Clr_Unknown, nHDRFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTarget 1/2a").c_str(), &m_pTexHDRTargetScaled[0][0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r4, height_r4, Clr_Unknown, nHDRFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTarget 1/4a").c_str(), &m_pTexHDRTargetScaled[1][0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r4, height_r4, Clr_Unknown, nHDRFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTarget 1/4b").c_str(), &m_pTexHDRTargetScaled[1][1], FT_DONT_RELEASE);

	// Scaled versions of compositions of the HDR scene texture (with alpha)
	m_renderTargetPool.AddRenderTarget(width_r2, height_r2, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/2a").c_str(), &m_pTexHDRTargetMaskedScaled[0][0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r2, height_r2, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/2b").c_str(), &m_pTexHDRTargetMaskedScaled[0][1], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r2, height_r2, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/2c").c_str(), &m_pTexHDRTargetMaskedScaled[0][2], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r2, height_r2, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/2d").c_str(), &m_pTexHDRTargetMaskedScaled[0][3], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r4, height_r4, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/4a").c_str(), &m_pTexHDRTargetMaskedScaled[1][0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r4, height_r4, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/4b").c_str(), &m_pTexHDRTargetMaskedScaled[1][1], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r8, height_r8, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/8a").c_str(), &m_pTexHDRTargetMaskedScaled[2][0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r8, height_r8, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/8b").c_str(), &m_pTexHDRTargetMaskedScaled[2][1], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r16, height_r16, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/16a").c_str(), &m_pTexHDRTargetMaskedScaled[3][0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width_r16, height_r16, Clr_Transparent, nHDRAFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked 1/16b").c_str(), &m_pTexHDRTargetMaskedScaled[3][1], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(width, height, Clr_Unknown, nHDRQFormat, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetPrev_Left").c_str(), &m_pTexHDRTargetPrev[0]);
	m_renderTargetPool.AddRenderTarget(width, height, Clr_Unknown, nHDRQFormat, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetPrev_Right").c_str(), &m_pTexHDRTargetPrev[1]);
	m_renderTargetPool.AddRenderTarget(width, height, Clr_Transparent, nHDRAFormat, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRTargetMasked").c_str(), &m_pTexHDRTargetMasked, nHDRTargetFlags);
	
	m_renderTargetPool.AddRenderTarget(width, height, Clr_Unknown, nHDRQFormat, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneTargetR11G11B10F_0").c_str(), &m_pTexSceneTargetR11G11B10F[0], nHDRTargetFlagsUAV);
	m_renderTargetPool.AddRenderTarget(width, height, Clr_Unknown, nHDRQFormat, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneTargetR11G11B10F_1").c_str(), &m_pTexSceneTargetR11G11B10F[1], nHDRTargetFlags);

	m_renderTargetPool.AddRenderTarget(width_r4, height_r4, Clr_Unknown, nHDRQFormat, 0.9f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRFinalBloom").c_str(), &m_pTexHDRFinalBloom, FT_DONT_RELEASE);

	m_renderTargetPool.AddRenderTarget(width, height, Clr_Unknown, eTF_R8G8B8A8, 0.1f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$Velocity").c_str(), &m_pTexVelocity, FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(20, height, Clr_Unknown, eTF_R8G8B8A8, 0.1f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityTilesTmp0").c_str(), &m_pTexVelocityTiles[0], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(20, 20, Clr_Unknown, eTF_R8G8B8A8, 0.1f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityTilesTmp1").c_str(), &m_pTexVelocityTiles[1], FT_DONT_RELEASE);
	m_renderTargetPool.AddRenderTarget(20, 20, Clr_Unknown, eTF_R8G8B8A8, 0.1f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$VelocityTiles").c_str(), &m_pTexVelocityTiles[2], FT_DONT_RELEASE);

#if RENDERER_ENABLE_FULL_PIPELINE
	m_renderTargetPool.AddRenderTarget(width_r2, height_r2, Clr_Unknown, eTF_R16G16F, 1.0f, m_graphicsPipeline.MakeUniqueTexIdentifierName("$MinCoC_0_Temp").c_str(), &m_pTexSceneCoCTemp, FT_DONT_RELEASE);
	for (int i = 0; i < MIN_DOF_COC_K; i++)
	{
		char szName[256];
		cry_sprintf(szName, m_graphicsPipeline.MakeUniqueTexIdentifierName("$MinCoC_%d").c_str(), i);
		m_renderTargetPool.AddRenderTarget(width_r2 / (i + 1), height_r2 / (i + 1), Clr_Unknown, eTF_R16G16F, 0.1f, szName, &m_pTexSceneCoC[i], FT_DONT_RELEASE, -1, true);
	}

	for (int i = 0; i < MAX_GPU_NUM; ++i)
	{
		char szName[256];
		cry_sprintf(szName, m_graphicsPipeline.MakeUniqueTexIdentifierName("$HDRMeasuredLum_%d").c_str(), i);
		m_pTexHDRMeasuredLuminance[i] = CTexture::GetOrCreate2DTexture(szName, 1, 1, 0, FT_DONT_RELEASE | FT_DONT_STREAM, NULL, eTF_R16G16F);
	}
#endif

	m_renderTargetPool.CreateRenderTargetList();
}

void CGraphicsPipelineResources::CreatePostFXMaps(int resourceWidth, int resourceHeight)
{
#if RENDERER_ENABLE_FULL_PIPELINE
	const int width = resourceWidth, width_r2 = (width + 1) / 2, width_r4 = (width_r2 + 1) / 2, width_r8 = (width_r4 + 1) / 2;
	const int height = resourceHeight, height_r2 = (height + 1) / 2, height_r4 = (height_r2 + 1) / 2, height_r8 = (height_r4 + 1) / 2;

	CRY_DISABLE_WARN_UNUSED_VARIABLES();
	const ETEX_Format nHDRFormat = CRendererResources::GetHDRFormat(false, false); // No alpha, default is HiQ, can be downgraded
	const ETEX_Format nHDRAFormat = CRendererResources::GetHDRFormat(true, false); // With alpha
	const ETEX_Format nHDRQFormat = ResolveHDRSatelliteFormat();                   // No alpha, default is LoQ, can be upgraded - forced to fp16 on the scene-referred path
	const ETEX_Format nLDRPFormat = CRendererResources::GetLDRFormat(true);         // With more than 8 mantissa bits for calculations
	const ETEX_Format nLDRSFormat = ResolveDisplayTargetScaledFormat();            // The SCALED display set - forced to fp16 on the scene-referred path
	CRY_RESTORE_WARN_UNUSED_VARIABLES();

	if (RainOcclusionMapsEnabled())
		CreateRainOcclusionMaps(resourceWidth, resourceHeight);

	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/2a").c_str(), m_pTexDisplayTargetScaled[0], width_r2, height_r2, Clr_Unknown, 1, 0, nLDRSFormat, TO_BACKBUFFERSCALED_D2, FT_DONT_RELEASE);
	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/4a").c_str(), m_pTexDisplayTargetScaled[1], width_r4, height_r4, Clr_Unknown, 1, 0, nLDRSFormat, TO_BACKBUFFERSCALED_D4, FT_DONT_RELEASE);
	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/8").c_str(), m_pTexDisplayTargetScaled[2], width_r8, height_r8, Clr_Unknown, 1, 0, nLDRSFormat, TO_BACKBUFFERSCALED_D8, FT_DONT_RELEASE);

	// Scaled versions of the scene target
	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget").c_str(), m_pTexDisplayTargetSrc, width, height, Clr_Unknown, 1, 0, nLDRPFormat, TO_BACKBUFFERMAP, FT_DONT_RELEASE);
	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTargetDst").c_str(), m_pTexDisplayTargetDst, width, height, Clr_Unknown, 1, 0, nLDRPFormat, TO_BACKBUFFERMAP, FT_DONT_RELEASE);

	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/2b").c_str(), m_pTexDisplayTargetScaledTemp[0], width_r2, height_r2, Clr_Unknown, 1, 0, nLDRSFormat, -1, FT_DONT_RELEASE);
	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$DisplayTarget 1/4b").c_str(), m_pTexDisplayTargetScaledTemp[1], width_r4, height_r4, Clr_Unknown, 1, 0, nLDRSFormat, -1, FT_DONT_RELEASE);

	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$WaterVolumeRefl").c_str(), m_pTexWaterVolumeRefl[0], width_r2, height_r2, Clr_Unknown, 1, true, nHDRQFormat, TO_WATERVOLUMEREFLMAP, FT_DONT_RELEASE);
	SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$WaterVolumeReflPrev").c_str(), m_pTexWaterVolumeRefl[1], width_r2, height_r2, Clr_Unknown, 1, true, nHDRQFormat, TO_WATERVOLUMEREFLMAPPREV, FT_DONT_RELEASE);
#endif
}

void CGraphicsPipelineResources::CreateSceneMaps(int resourceWidth, int resourceHeight)
{
	const int32 nWidth = resourceWidth;
	const int32 nHeight = resourceHeight;
	const ETEX_Format eHDRTF = CRendererResources::GetHDRFormat(false, false);
	uint32 nFlags = FT_DONT_STREAM | FT_USAGE_RENDERTARGET | FT_USAGE_UNORDERED_ACCESS;

	if (!m_pTexSceneTarget)
		m_pTexSceneTarget = CTexture::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneTarget").c_str(), nWidth, nHeight, Clr_Empty, eTT_2D, nFlags, eHDRTF, TO_SCENE_TARGET);
	else
	{
		m_pTexSceneTarget->SetFlags(nFlags);
		m_pTexSceneTarget->SetWidth(nWidth);
		m_pTexSceneTarget->SetHeight(nHeight);
		m_pTexSceneTarget->CreateRenderTarget(eHDRTF, Clr_Empty);
	}

	if (gEnv->IsEditor())
	{
		SD3DPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$SceneSelectionIDs").c_str(), m_pTexSceneSelectionIDs, nWidth, nHeight, Clr_Transparent, false, false, eTF_R32F, -1, nFlags);
	}
}

void CGraphicsPipelineResources::CreateHUDMaps(int resourceWidth, int resourceHeight)
{
	const int width = resourceWidth, width_r2 = (width + 1) / 2, width_r4 = (width_r2 + 1) / 2;
	const int height = resourceHeight, height_r2 = (height + 1) / 2, height_r4 = (height_r2 + 1) / 2;

	const uint32 flags = FT_NOMIPS | FT_DONT_STREAM | FT_USAGE_RENDERTARGET;

	if (!m_pTexHUD3D[0])
	{
		m_pTexHUD3D[0] = CTexture::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$Cached3DHUD").c_str(), width, height, Clr_Empty, eTT_2D, flags, eTF_R8G8B8A8, TO_MODELHUD);
	}
	else
	{
		m_pTexHUD3D[0]->SetFlags(flags);
		m_pTexHUD3D[0]->SetWidth(width);
		m_pTexHUD3D[0]->SetHeight(height);
		m_pTexHUD3D[0]->CreateRenderTarget(eTF_R8G8B8A8, Clr_Empty);
	}

	if (!m_pTexHUD3D[1])
	{
		m_pTexHUD3D[1] = CTexture::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$Cached3DHUD 1/4").c_str(), width_r4, height_r4, Clr_Empty, eTT_2D, flags, eTF_R8G8B8A8, TO_MODELHUD);
	}
	else
	{
		m_pTexHUD3D[1]->SetFlags(flags);
		m_pTexHUD3D[1]->SetWidth(width);
		m_pTexHUD3D[1]->SetHeight(height);
		m_pTexHUD3D[1]->CreateRenderTarget(eTF_R8G8B8A8, Clr_Empty);
	}
}

void CGraphicsPipelineResources::PrepareRainOcclusionMaps()
{
	if (!m_pTexRainOcclusion)
	{
		m_pTexRainOcclusion = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$RainOcclusion").c_str(), RAIN_OCC_MAP_SIZE, RAIN_OCC_MAP_SIZE, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_R8G8B8A8);
		m_pTexRainSSOcclusion[0] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$RainSSOcclusion0").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown);
		m_pTexRainSSOcclusion[1] = CTexture::GetOrCreateTextureObject(m_graphicsPipeline.MakeUniqueTexIdentifierName("$RainSSOcclusion1").c_str(), 0, 0, 1, eTT_2D, FT_DONT_RELEASE | FT_DONT_STREAM | FT_USAGE_RENDERTARGET, eTF_Unknown);
	}
}

void CGraphicsPipelineResources::CreateRainOcclusionMaps(int resourceWidth, int resourceHeight)
{
	PrepareRainOcclusionMaps();

	if (!CTexture::IsTextureExist(m_pTexRainOcclusion))
		SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$RainOcclusion").c_str(), m_pTexRainOcclusion, RAIN_OCC_MAP_SIZE, RAIN_OCC_MAP_SIZE, Clr_Neutral, false, false, eTF_R8, -1, FT_DONT_RELEASE);

	const int width_r8 = (resourceWidth + 7) / 8;
	const int height_r8 = (resourceHeight + 7) / 8;

	if (!m_pTexRainSSOcclusion[0] ||
		m_pTexRainSSOcclusion[0]->GetWidth() != width_r8 ||
		m_pTexRainSSOcclusion[0]->GetHeight() != height_r8)
	{
		SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$RainSSOcclusion0").c_str(), m_pTexRainSSOcclusion[0], width_r8, height_r8, Clr_Unknown, 1, false, eTF_R8);
		SPostEffectsUtils::GetOrCreateRenderTarget(m_graphicsPipeline.MakeUniqueTexIdentifierName("$RainSSOcclusion1").c_str(), m_pTexRainSSOcclusion[1], width_r8, height_r8, Clr_Unknown, 1, false, eTF_R8);
	}
}

void CGraphicsPipelineResources::DestroyRainOcclusionMaps()
{
	SAFE_RELEASE_FORCE(m_pTexRainOcclusion);
	SAFE_RELEASE_FORCE(m_pTexRainSSOcclusion[0]);
	SAFE_RELEASE_FORCE(m_pTexRainSSOcclusion[1]);
}

void CGraphicsPipelineResources::OnCVarsChanged(const CCVarUpdateRecorder& rCVarRecs)
{
	const bool enabled = RainOcclusionMapsEnabled();
	if (enabled != RainOcclusionMapsInitialized())
	{
		if (enabled)
			CreateRainOcclusionMaps(m_resourceWidth, m_resourceHeight);
		else
			DestroyRainOcclusionMaps();
	}

	if (rCVarRecs.GetCVar("r_hdrtexformat"))
	{
		const int hdrTexFormat = rCVarRecs.GetCVar("r_hdrtexformat")->intValue;

		if (hdrTexFormat && CTexture::IsTextureExist(m_pTexLinearDepthFixup))
		{
			m_pTexLinearDepthFixup->ReleaseDeviceTexture(false);
		}
		else if (!hdrTexFormat && !CTexture::IsTextureExist(m_pTexLinearDepthFixup))
		{
			uint32 nUAFlags = FT_DONT_STREAM | FT_DONT_RELEASE | FT_USAGE_RENDERTARGET | FT_USAGE_UNORDERED_ACCESS | FT_USAGE_UAV_RWTEXTURE;

			m_pTexLinearDepthFixup->SetFlags(nUAFlags);
			m_pTexLinearDepthFixup->SetWidth(m_resourceWidth);
			m_pTexLinearDepthFixup->SetHeight(m_resourceHeight);
			m_pTexLinearDepthFixup->CreateRenderTarget(eTF_R32F, ColorF(1.0f, 1.0f, 1.0f, 1.0f));

			SResourceView typedUAV = SResourceView::UnorderedAccessView(DXGI_FORMAT_R32_UINT, 0, -1, 0, SResourceView::eUAV_ReadWrite);
			m_pTexLinearDepthFixupUAV = m_pTexLinearDepthFixup->GetDevTexture()->GetOrCreateResourceViewHandle(typedUAV);
		}
	}
}

// Scene-referred pipeline: does this pipeline's live render-target set still match the format the
// switch asks for? (decisions/s8-flip-safety.md approach A)
//
// Twelve targets can move: the four main HDR targets (a no-op at the registered default
// r_HDRTexFormat 1 - they are already fp16 - so this exists to close the r_HDRTexFormat 0 hole),
// the seven low-quality HDR satellites, and the five scaled display targets the sun shafts borrow.
// All of them now take their format from Resolve*Format() at CREATION time, so this comparison is
// false on every frame except the one in which the switch actually flips underneath a pipeline
// that is already built - and it is false for a level load that starts with the switch already on,
// because the level's clear-and-recreate of the render resources builds them in the right format
// to begin with. That is the whole point: the crash frame no longer exists on the load path.
//
// Sixteen GetDstFormat() reads and three format resolutions per pipeline per frame. Nothing is
// allocated, nothing is bound, and with the switch off every wanted value is the stock one.
bool CGraphicsPipelineResources::SceneReferredFormatsChanged() const
{
	const ETEX_Format wantedMain      = ResolveHDRMainFormat();
	const ETEX_Format wantedSatellite = ResolveHDRSatelliteFormat();
	const ETEX_Format wantedScaled    = ResolveDisplayTargetScaledFormat();

	// Already rebuilt for exactly this triple? Then whatever still disagrees below disagrees
	// because SetClosestFormatSupported() moved the request, not because the switch flipped, and
	// rebuilding again would rebuild again next frame and every frame after that. One rebuild per
	// distinct answer from the switch, not one per frame.
	if (wantedMain      == m_lastBuiltHDRMainFormat &&
	    wantedSatellite == m_lastBuiltHDRSatelliteFormat &&
	    wantedScaled    == m_lastBuiltScaledFormat)
	{
		return false;
	}

	const std::pair<CTexture*, ETEX_Format> targets[] =
	{
		{ m_pTexHDRTarget,                  wantedMain      },
		{ m_pTexHDRTargetScaled[0][0],      wantedMain      },
		{ m_pTexHDRTargetScaled[1][0],      wantedMain      },
		{ m_pTexHDRTargetScaled[1][1],      wantedMain      },

		{ m_pTexHDRTargetPrev[0],           wantedSatellite },
		{ m_pTexHDRTargetPrev[1],           wantedSatellite },
		{ m_pTexSceneTargetR11G11B10F[0],   wantedSatellite },
		{ m_pTexSceneTargetR11G11B10F[1],   wantedSatellite },
		{ m_pTexHDRFinalBloom,              wantedSatellite },
		{ m_pTexWaterVolumeRefl[0],         wantedSatellite },
		{ m_pTexWaterVolumeRefl[1],         wantedSatellite },

		{ m_pTexDisplayTargetScaled[0],     wantedScaled    },
		{ m_pTexDisplayTargetScaled[1],     wantedScaled    },
		{ m_pTexDisplayTargetScaled[2],     wantedScaled    },
		{ m_pTexDisplayTargetScaledTemp[0], wantedScaled    },
		{ m_pTexDisplayTargetScaledTemp[1], wantedScaled    },
	};

	for (const auto& entry : targets)
	{
		// A target that is registered but has no device texture yet has no format to disagree
		// with. Sandbox reaches that state routinely - one CGraphicsPipelineResources per viewport,
		// plus the material editor and the thumbnail renderer - and asking for a rebuild because a
		// target that was never created is "the wrong format" would rebuild every frame forever.
		if (!CTexture::IsTextureExist(entry.first))
			continue;

		if (entry.first->GetDstFormat() != entry.second)
			return true;
	}

	return false;
}

// Scene-referred pipeline: the flip, executed as a REBUILD rather than as a patch
// (decisions/s8-flip-safety.md approach A; supersedes the in-place CTexture::ReformatRenderTarget()
// of decisions/s0-fp16-forcing.md and decisions/s2-sun-shafts.md).
//
// The first implementation swapped the device texture underneath each of the twelve live targets
// from inside Update(). It worked - the log shows all twelve reformatted - but it left the render
// thread dying in nvwgf2umx.dll with a read at -1 on the first frame after a level reached RUNNING
// (research/s8-renderthread-av-diagnosis.md). SetDevTexture()'s eDeviceResourceDirty reaches every
// CDeviceResourceSet and CDeviceRenderPass holding the texture, but it does NOT reach a
// ResourceViewHandle cached in a stage member, a raw D3DUAV*/D3DSurface* looked up earlier in the
// frame, or a stage whose re-bind sits behind an IsDirty() key that does not mention the target
// that moved. Twelve device textures moving underneath a freshly loaded level, with the shaft
// stage becoming the first consumer of $DisplayTargetScaled[0] in the same frame and the shader
// cache cold, is exactly the window for a stale binding to reach the driver.
//
// So do what CE itself does when every pipeline target moves at once - a resolution change:
//
//   1. push the new format into the affected objects with CTexture::Invalidate(), which is CE's own
//      primitive for "this render target's format moved": it assigns m_eSrcFormat, re-runs
//      SetClosestFormatSupported() and releases the device texture. It has to happen first because
//      CTexture::CreateRenderTarget() only assigns m_eSrcFormat while it is still eTF_Unknown - the
//      format argument of an already-formatted texture is silently discarded, which is why
//      r_HDRTexFormat behaves as apply-on-startup in stock CE;
//   2. re-run the two creation functions that own those objects, at the current resource size, the
//      same calls CGraphicsPipelineResources::Resize() makes. The whole HDR and post-FX map sets
//      are re-created, not just the sixteen listed here, which is the point: one code path, the one
//      that is exercised every time anyone drags a viewport edge;
//   3. (in CGraphicsPipeline::Update, our caller) broadcast Resize() to every stage, as
//      CGraphicsPipeline::Resize() does, so no stage keeps a handle into a device texture that no
//      longer exists.
//
// Runs on the render thread from the top of CGraphicsPipeline::Update(), i.e. before
// CompileModifiedRenderObjects() and before Execute(), so nothing is prepared or bound yet.
void CGraphicsPipelineResources::RecreateSceneReferredFormats()
{
	const ETEX_Format wantedMain      = ResolveHDRMainFormat();
	const ETEX_Format wantedSatellite = ResolveHDRSatelliteFormat();
	const ETEX_Format wantedScaled    = ResolveDisplayTargetScaledFormat();

	const std::pair<CTexture*, ETEX_Format> targets[] =
	{
		{ m_pTexHDRTarget,                  wantedMain      },
		{ m_pTexHDRTargetScaled[0][0],      wantedMain      },
		{ m_pTexHDRTargetScaled[1][0],      wantedMain      },
		{ m_pTexHDRTargetScaled[1][1],      wantedMain      },

		{ m_pTexHDRTargetPrev[0],           wantedSatellite },
		{ m_pTexHDRTargetPrev[1],           wantedSatellite },
		{ m_pTexSceneTargetR11G11B10F[0],   wantedSatellite },
		{ m_pTexSceneTargetR11G11B10F[1],   wantedSatellite },
		{ m_pTexHDRFinalBloom,              wantedSatellite },
		{ m_pTexWaterVolumeRefl[0],         wantedSatellite },
		{ m_pTexWaterVolumeRefl[1],         wantedSatellite },

		{ m_pTexDisplayTargetScaled[0],     wantedScaled    },
		{ m_pTexDisplayTargetScaled[1],     wantedScaled    },
		{ m_pTexDisplayTargetScaled[2],     wantedScaled    },
		{ m_pTexDisplayTargetScaledTemp[0], wantedScaled    },
		{ m_pTexDisplayTargetScaledTemp[1], wantedScaled    },
	};

	uint64 sizeBefore = 0;
	int    nMoved = 0;

	for (const auto& entry : targets)
	{
		if (!entry.first)
			continue;

		sizeBefore += entry.first->GetDeviceDataSize();

		// Invalidate() is a no-op when the format already matches, and it returns false without
		// releasing anything for a target that has no device texture - but it still assigns the
		// format fields either way, so a target the pool has not created yet is born in the right
		// format when it finally is created.
		if (entry.first->Invalidate(-1, -1, entry.second))
			++nMoved;
	}

	// The creation functions re-read Resolve*Format() themselves, so the format the targets come
	// back with is decided in exactly one place - where they are created.
	CreateHDRMaps(m_resourceWidth, m_resourceHeight);
	CreatePostFXMaps(m_resourceWidth, m_resourceHeight);

	m_lastBuiltHDRMainFormat      = wantedMain;
	m_lastBuiltHDRSatelliteFormat = wantedSatellite;
	m_lastBuiltScaledFormat       = wantedScaled;

	uint64 sizeAfter = 0;
	for (const auto& entry : targets)
	{
		if (entry.first)
			sizeAfter += entry.first->GetDeviceDataSize();
	}

	// Once per flip, not once per frame: SceneReferredFormatsChanged() is false on every other
	// frame. The numbers are per graphics pipeline - Sandbox pays them again for every viewport.
	CryLogAlways("[SceneReferred] pipeline '%s' rebuilt %d of %d format-dependent targets at %dx%d "
	             "(HDR main %s, satellites %s, scaled display %s); %.1f MiB -> %.1f MiB (%+.1f MiB)",
	             m_graphicsPipeline.GetUniqueIdentifierName().c_str(),
	             nMoved, (int)CRY_ARRAY_COUNT(targets), m_resourceWidth, m_resourceHeight,
	             CTexture::NameForTextureFormat(wantedMain),
	             CTexture::NameForTextureFormat(wantedSatellite),
	             CTexture::NameForTextureFormat(wantedScaled),
	             sizeBefore / (1024.0 * 1024.0), sizeAfter / (1024.0 * 1024.0),
	             (double(sizeAfter) - double(sizeBefore)) / (1024.0 * 1024.0));
}

void CGraphicsPipelineResources::Update(EShaderRenderingFlags renderingFlags)
{
	// Scene-referred pipeline: pick up a flip of the switch. Update() is the sanctioned place
	// for a stage to manage or retire resources - it runs on the render thread, before
	// CompileModifiedRenderObjects() and before Execute(), so nothing is bound yet - and the
	// rain occlusion texture right below is created and released on demand in the same way.
	//
	// The first thing that happens, before any of it: say so. This is the marker that tells the
	// next crash log how far into the flip the frame got, and it is CryLogAlways rather than
	// CryLog so that no verbosity setting can swallow it. Once per pipeline per flip.
	//
	// Since the preview gate (research/s8-editor-pipelines.md) IsSceneReferred() is false for every
	// pipeline that owns no display transform, so a normal editor flip prints "on" for the level
	// viewport's Standard pipeline only, and the three Update*Formats calls below leave every
	// preview, thumbnail and 2D viewport on its stock formats - which is also where the ~42 MiB
	// per pipeline of s8-perf-and-permutations.md section 3.1 stops being paid by widgets that
	// never bind those targets. Which pipelines were considered, and why each was refused, is the
	// separate one-line-per-decision log in CRendererResources::SetSceneReferredPipelineGate().
	{
		const int nState = CRendererResources::IsSceneReferred() ? 1 : 0;
		if (m_nSceneReferredLogged < 0)
		{
			// FIRST Update() of this pipeline's resources - a level load creates a new
			// CGraphicsPipelineResources, so this runs once per level, not once per flip. It is
			// NOT a flip and must not say so: the 2026-09-07 crash log was read as "the request
			// was on during load and got reset to off" purely because the seeding pass printed
			// "request flipped -> off" on the first frame of the level, with the switch never
			// having been on. Seed silently when the answer is "off" (the overwhelmingly common
			// case, and the one that says nothing); say so when a pipeline is born scene-referred,
			// because that IS news.
			m_nSceneReferredLogged = nState;
			if (nState)
			{
				CryLogAlways("[SceneReferred] pipeline '%s' starts scene-referred (stage gate r_SceneReferredStage %d, %dx%d)",
				             m_graphicsPipeline.GetUniqueIdentifierName().c_str(),
				             CRenderer::CV_r_SceneReferredStage, m_resourceWidth, m_resourceHeight);
			}
		}
		else if (nState != m_nSceneReferredLogged)
		{
			m_nSceneReferredLogged = nState;
			CryLogAlways("[SceneReferred] request flipped -> %s, pipeline '%s' (stage gate r_SceneReferredStage %d, %dx%d)",
			             nState ? "on" : "off",
			             m_graphicsPipeline.GetUniqueIdentifierName().c_str(),
			             CRenderer::CV_r_SceneReferredStage, m_resourceWidth, m_resourceHeight);

			// The switch going off retires everything that could still be holding a GPU resource
			// or a readback in flight on the scene-referred path. The passes themselves all
			// early-out on the gate, so nothing new is issued; this is about what was already
			// issued before the gate closed. A capture is the only one that owns its own textures.
			if (!nState)
				CSceneReferredExport::Get().Stop("the scene-referred switch was turned off");
		}
	}

	// NOTE: the format flip itself is NOT done here any more. CGraphicsPipeline::Update() runs
	// SceneReferredFormatsChanged() / RecreateSceneReferredFormats() before it calls us, so by the
	// time this function is reached the targets are already in the format the switch asks for and
	// every stage has been told. decisions/s8-flip-safety.md approach A.

	const auto shouldApplyOcclusion = gcpRendD3D->m_bDeferredRainOcclusionEnabled && CRendererCVars::IsRainEnabled();

	// Create/release the occlusion texture on demand
	if (!shouldApplyOcclusion && CTexture::IsTextureExist(m_pTexRainOcclusion))
		m_pTexRainOcclusion->ReleaseDeviceTexture(false);
	else if (shouldApplyOcclusion && !CTexture::IsTextureExist(m_pTexRainOcclusion))
		m_pTexRainOcclusion->CreateRenderTarget(eTF_R8, Clr_Neutral);

	// Clear texture because it's being reused for TAA
	CClipVolumesStage* pClipVolumeStage = m_graphicsPipeline.GetStage<CClipVolumesStage>();
	if (!pClipVolumeStage || !pClipVolumeStage->IsStageActive(renderingFlags))
	{
		CClearSurfacePass::Execute(m_pTexClipVolumes, m_pTexClipVolumes->GetClearColor());
	}
}

bool CGraphicsPipelineResources::RainOcclusionMapsEnabled()
{
	return CRendererCVars::IsRainEnabled() || CRendererCVars::IsSnowEnabled();
}

void CGraphicsPipelineResources::Shutdown()
{
	DestroyRainOcclusionMaps();

	SAFE_RELEASE_FORCE(m_pTexHDRTarget);
	SAFE_RELEASE_FORCE(m_pTexHDRTargetPrev[0]);
	SAFE_RELEASE_FORCE(m_pTexHDRTargetPrev[1]);
	SAFE_RELEASE_FORCE(m_pTexHDRTargetMasked);
	SAFE_RELEASE_FORCE(m_pTexLinearDepth);
	SAFE_RELEASE_FORCE(m_pTexSceneDiffuse);
	SAFE_RELEASE_FORCE(m_pTexSceneNormalsMap);
	SAFE_RELEASE_FORCE(m_pTexSceneSpecular);
	SAFE_RELEASE_FORCE(m_pTexVelocityObjects[0]);
	SAFE_RELEASE_FORCE(m_pTexVelocityObjects[1]);
	SAFE_RELEASE_FORCE(m_pTexShadowMask);
	SAFE_RELEASE_FORCE(m_pTexSceneNormalsBent);
	SAFE_RELEASE_FORCE(m_pTexLinearDepthScaled[0]);
	SAFE_RELEASE_FORCE(m_pTexLinearDepthScaled[1]);
	SAFE_RELEASE_FORCE(m_pTexLinearDepthScaled[2]);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetScaledTemp[0]);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetScaledTemp[1]);
	SAFE_RELEASE_FORCE(m_pTexSceneDepthScaled[0]);
	SAFE_RELEASE_FORCE(m_pTexSceneDepthScaled[1]);
	SAFE_RELEASE_FORCE(m_pTexSceneDepthScaled[2]);
	SAFE_RELEASE_FORCE(m_pTexClipVolumes);
	SAFE_RELEASE_FORCE(m_pTexAOColorBleed);
	SAFE_RELEASE_FORCE(m_pTexSceneDiffuseTmp);
	SAFE_RELEASE_FORCE(m_pTexSceneSpecularTmp[0]);
	SAFE_RELEASE_FORCE(m_pTexSceneSpecularTmp[1]);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetScaled[0]);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetScaled[1]);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetScaled[2]);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetDst);
	SAFE_RELEASE_FORCE(m_pTexDisplayTargetSrc);
	SAFE_RELEASE_FORCE(m_pTexSceneSelectionIDs);
	SAFE_RELEASE_FORCE(m_pTexSceneTargetR11G11B10F[0]);
	SAFE_RELEASE_FORCE(m_pTexSceneTargetR11G11B10F[1]);
	SAFE_RELEASE_FORCE(m_pTexLinearDepthFixup);
	SAFE_RELEASE_FORCE(m_pTexSceneTarget);
	SAFE_RELEASE_FORCE(m_pTexHDRFinalBloom);
	SAFE_RELEASE_FORCE(m_pTexVelocity);
	SAFE_RELEASE_FORCE(m_pTexVelocityTiles[0]);
	SAFE_RELEASE_FORCE(m_pTexVelocityTiles[1]);
	SAFE_RELEASE_FORCE(m_pTexVelocityTiles[2]);
	SAFE_RELEASE_FORCE(m_pTexWaterVolumeRefl[0]);
	SAFE_RELEASE_FORCE(m_pTexWaterVolumeRefl[1]);
	SAFE_RELEASE_FORCE(m_pTexHUD3D[0]);
	SAFE_RELEASE_FORCE(m_pTexHUD3D[1]);

	SAFE_RELEASE_FORCE(m_pTexSceneCoCTemp);
	for (int i = 0; i < MIN_DOF_COC_K; i++)
	{
		SAFE_RELEASE_FORCE(m_pTexSceneCoC[i]);
	}

	for (int i = 0; i < MAX_GPU_NUM; ++i)
	{
		SAFE_RELEASE_FORCE(m_pTexHDRMeasuredLuminance[i]);
	}

	for (int i = 0; i < 4; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			SAFE_RELEASE_FORCE(m_pTexHDRTargetScaled[i][j]);
			SAFE_RELEASE_FORCE(m_pTexHDRTargetMaskedScaled[i][j]);
		}
	}

	m_resourceWidth  = 0;
	m_resourceHeight = 0;
}

void CGraphicsPipelineResources::Discard()
{
	// DISCARD RESOURCES
	//------------------------------------------------------------------------------
#if (CRY_RENDERER_DIRECT3D >= 111)
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRTarget->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRTargetPrev[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRTargetPrev[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRTargetMasked->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexLinearDepth->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneDiffuse->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneNormalsMap->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneSpecular->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexVelocityObjects[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexVelocityObjects[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexShadowMask->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneNormalsBent->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexLinearDepthScaled[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexLinearDepthScaled[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexLinearDepthScaled[2]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneDepthScaled[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneDepthScaled[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneDepthScaled[2]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexClipVolumes->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexAOColorBleed->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneDiffuseTmp->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneSpecularTmp[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneSpecularTmp[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexDisplayTargetScaled[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexDisplayTargetScaled[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexDisplayTargetScaled[2]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexDisplayTargetDst->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexDisplayTargetSrc-> GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneSelectionIDs->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneTargetR11G11B10F[0]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneTargetR11G11B10F[1]->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexLinearDepthFixup->GetDevTexture(false)->GetNativeResource());
	gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexSceneTarget->GetDevTexture(false)->GetNativeResource());

	for (int i = 0; i < MAX_GPU_NUM; ++i)
	{
		gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRMeasuredLuminance[i]->GetDevTexture(false)->GetNativeResource());
	}

	for (int i = 0; i < 4; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRTargetScaled[i][j]->GetDevTexture(false)->GetNativeResource());
			gcpRendD3D->GetDeviceContext()->DiscardResource(m_pTexHDRTargetMaskedScaled[i][j]->GetDevTexture(false)->GetNativeResource());
		}
	}
#endif
}

void CGraphicsPipelineResources::Clear()
{
	CTexture* clearTextures[] =
	{
		m_pTexSceneNormalsMap,
		m_pTexSceneDiffuse,
		m_pTexSceneSpecular,
		m_pTexSceneDiffuseTmp,
		m_pTexDisplayTargetSrc,
		m_pTexDisplayTargetDst,
		m_pTexLinearDepth,
		m_pTexHDRTarget,
		m_pTexSceneTarget
	};

	for (auto pTex : clearTextures)
	{
		if (CTexture::IsTextureExist(pTex))
		{
			CClearSurfacePass::Execute(pTex, pTex->GetClearColor());
		}
	}
}

//////////////////////////////////////////////////////////////////////////
CGraphicsPipeline::CGraphicsPipeline(const IRenderer::SGraphicsPipelineDescription& desc, const std::string& uniqueIdentifier, const SGraphicsPipelineKey key)
	: m_changedCVars(gEnv->pConsole)
	, m_pipelineResources(*this)
	, m_pipelineDesc(desc)
	, m_uniquePipelineIdentifierName(uniqueIdentifier)
	, m_key(key)
{
	m_renderingFlags = (EShaderRenderingFlags)desc.shaderFlags;
	m_pipelineStages.fill(nullptr);
	m_pVRProjectionManager = nullptr;
}

//////////////////////////////////////////////////////////////////////////
CGraphicsPipeline::~CGraphicsPipeline()
{
	ShutDown();
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::ClearState()
{
	GetDeviceObjectFactory().GetCoreCommandList().Reset();
}

void CGraphicsPipeline::ClearDeviceState()
{
	GetDeviceObjectFactory().GetCoreCommandList().GetGraphicsInterface()->ClearState(false);
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::Init()
{
	// per view constant buffer
	m_mainViewConstantBuffer.CreateDeviceBuffer();

	m_pDeferredShading = new CDeferredShading(this);

	m_pipelineResources.Init();	

	// Register scene stages that make use of the global PSO cache
	RegisterStage<CSceneGBufferStage>();
	RegisterStage<CSceneForwardStage>();
	RegisterStage<CShadowMapStage>();

	// Register all other stages that don't need the global PSO cache
	RegisterStage<CTiledLightVolumesStage>();
	RegisterStage<CClipVolumesStage>();
	RegisterStage<CFogStage>();
	RegisterStage<CDebugRenderTargetsStage>();

	RegisterStage<CSkyStage>();

	m_pVRProjectionManager = gcpRendD3D->m_pVRProjectionManager;

	// Initializes all the pipeline stages.
	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it) (*it)->Init();
	}
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::ShutDown()
{
	m_pipelineResources.Shutdown();

	m_mainViewConstantBuffer.Clear();

	SAFE_DELETE(m_pDeferredShading);

	// destroy stages in reverse order to satisfy data dependencies
	for (auto it = m_pipelineStages.rbegin(); it != m_pipelineStages.rend(); ++it)
	{
		if (*it)
			delete *it;
	}

	m_pipelineStages.fill(nullptr);
}

//////////////////////////////////////////////////////////////////////////

bool CGraphicsPipeline::CreatePipelineStates(DevicePipelineStatesArray* pStateArray, SGraphicsPipelineStateDescription stateDesc, CGraphicsPipelineStateLocalCache* pStateCache)
{
	// NOTE: Please update SDeviceObjectHelpers::CheckTessellationSupport when adding new techniques types here.

	bool bFullyCompiled = true;

	// GBuffer
	{
		stateDesc.technique = TTYPE_Z;
		bFullyCompiled &= GetStage<CSceneGBufferStage>()->CreatePipelineStates(pStateArray, stateDesc, pStateCache);
	}

	// ShadowMap
	{
		stateDesc.technique = TTYPE_SHADOWGEN;
		bFullyCompiled &= GetStage<CShadowMapStage>()->CreatePipelineStates(pStateArray, stateDesc, pStateCache);
	}

	// Forward
	{
		stateDesc.technique = TTYPE_GENERAL;
		bFullyCompiled &= GetStage<CSceneForwardStage>()->CreatePipelineStates(pStateArray, stateDesc, pStateCache);
	}

#if RENDERER_ENABLE_FULL_PIPELINE
	// Custom
	{
		stateDesc.technique = TTYPE_DEBUG;
		bFullyCompiled &= GetStage<CSceneCustomStage>()->CreatePipelineStates(pStateArray, stateDesc, pStateCache);
	}
#endif

	return bFullyCompiled;
}

//////////////////////////////////////////////////////////////////////////
CDeviceResourceLayoutPtr CGraphicsPipeline::CreateScenePassLayout(const CDeviceResourceSetDesc& perPassResources)
{
	SDeviceResourceLayoutDesc layoutDesc;

	layoutDesc.SetConstantBuffer(EResourceLayoutSlot_PerDrawCB, eConstantBufferShaderSlot_PerDraw, EShaderStage_Vertex | EShaderStage_Pixel | EShaderStage_Domain);

	layoutDesc.SetResourceSet(EResourceLayoutSlot_PerDrawExtraRS, CSceneRenderPass::GetDefaultDrawExtraResourceLayout());
	layoutDesc.SetResourceSet(EResourceLayoutSlot_PerMaterialRS, CSceneRenderPass::GetDefaultMaterialBindPoints());
	layoutDesc.SetResourceSet(EResourceLayoutSlot_PerPassRS, perPassResources);

	CDeviceResourceLayoutPtr pResourceLayout = GetDeviceObjectFactory().CreateResourceLayout(layoutDesc);
	assert(pResourceLayout != nullptr);
	return pResourceLayout;
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::Resize(int renderWidth, int renderHeight)
{
	// Sets the current render resolution on all the pipeline stages.
	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it)
			(*it)->Resize(renderWidth, renderHeight);
	}

	m_pipelineResources.Resize(renderWidth, renderHeight);

	m_renderWidth  = renderWidth;
	m_renderHeight = renderHeight;
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::SetCurrentRenderView(CRenderView* pRenderView)
{
	m_pCurrentRenderView = pRenderView;

	// Sets the current render view on all the pipeline stages.
	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it)
			(*it)->SetRenderView(pRenderView);
	}
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::Update(EShaderRenderingFlags renderingFlags)
{
	// Scene-referred pipeline: a flip of the switch changes render-target FORMATS, and this is the
	// first thing that happens in the frame it is noticed - before CGraphicsPipelineResources
	// ::Update(), before any stage's Update(), and (in RT_RenderScene) before
	// CompileModifiedRenderObjects() and Execute(). Nothing is prepared or bound yet.
	//
	// The flip is a REBUILD, not a patch: the pipeline resources go back through the two creation
	// functions a resolution change uses, and then every stage is told with the same Resize() call
	// CGraphicsPipeline::Resize() makes. That second half is what the previous in-place reformat
	// was missing, and it is why the render thread was dying inside the NVIDIA UMD on the first
	// frame after a level reached RUNNING - a stage still holding a ResourceViewHandle or a raw
	// view into a device texture that had just been released. decisions/s8-flip-safety.md
	// approach A, research/s8-renderthread-av-diagnosis.md.
	//
	// Guarded on a non-zero render resolution: Update() can be reached before the pipeline has ever
	// been Resize()d, and broadcasting a 0x0 resize would have every stage allocate nothing and
	// then be asked to draw into it.
	if (m_renderWidth > 0 && m_renderHeight > 0 && m_pipelineResources.SceneReferredFormatsChanged())
	{
		m_pipelineResources.RecreateSceneReferredFormats();

		for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
		{
			if (*it)
				(*it)->Resize(m_renderWidth, m_renderHeight);
		}
	}

	m_pipelineResources.Update(renderingFlags);

	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it && (*it)->IsStageActive(renderingFlags))
			(*it)->Update();
	}
}

#if DURANGO_USE_ESRAM
//////////////////////////////////////////////////////////////////////////
bool CGraphicsPipeline::UpdatePerPassResourceSet()
{
	bool result = true;
	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it && (*it)->IsStageActive(m_renderingFlags))
			result &= (*it)->UpdatePerPassResourceSet();
	}
	return result;
}

//////////////////////////////////////////////////////////////////////////
bool CGraphicsPipeline::UpdateRenderPasses()
{
	bool result = true;
	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it && (*it)->IsStageActive(m_renderingFlags))
			result &= (*it)->UpdateRenderPasses();
	}
	return result;
}
#endif

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::OnCVarsChanged(const CCVarUpdateRecorder& rCVarRecs)
{
	m_pipelineResources.OnCVarsChanged(rCVarRecs);

	for (auto it = m_pipelineStages.begin(); it != m_pipelineStages.end(); ++it)
	{
		if (*it)
			(*it)->OnCVarsChanged(rCVarRecs);
	}
}

//////////////////////////////////////////////////////////////////////////
std::array<SamplerStateHandle, EFSS_MAX> CGraphicsPipeline::GetDefaultMaterialSamplers() const
{
	std::array<SamplerStateHandle, EFSS_MAX> result =
	{
		{
			gcpRendD3D->m_nMaterialAnisoHighSampler,                                                                                                                                         // EFSS_ANISO_HIGH
			gcpRendD3D->m_nMaterialAnisoLowSampler,                                                                                                                                          // EFSS_ANISO_LOW
			CDeviceObjectFactory::GetOrCreateSamplerStateHandle(SSamplerState(FILTER_TRILINEAR, eSamplerAddressMode_Wrap, eSamplerAddressMode_Wrap, eSamplerAddressMode_Wrap, 0x0)),         // EFSS_TRILINEAR
			CDeviceObjectFactory::GetOrCreateSamplerStateHandle(SSamplerState(FILTER_BILINEAR, eSamplerAddressMode_Wrap, eSamplerAddressMode_Wrap, eSamplerAddressMode_Wrap, 0x0)),          // EFSS_BILINEAR
			CDeviceObjectFactory::GetOrCreateSamplerStateHandle(SSamplerState(FILTER_TRILINEAR, eSamplerAddressMode_Clamp, eSamplerAddressMode_Clamp, eSamplerAddressMode_Clamp, 0x0)),      // EFSS_TRILINEAR_CLAMP
			CDeviceObjectFactory::GetOrCreateSamplerStateHandle(SSamplerState(FILTER_BILINEAR, eSamplerAddressMode_Clamp, eSamplerAddressMode_Clamp, eSamplerAddressMode_Clamp, 0x0)),       // EFSS_BILINEAR_CLAMP
			gcpRendD3D->m_nMaterialAnisoSamplerBorder,                                                                                                                                       // EFSS_ANISO_HIGH_BORDER
			CDeviceObjectFactory::GetOrCreateSamplerStateHandle(SSamplerState(FILTER_TRILINEAR, eSamplerAddressMode_Border, eSamplerAddressMode_Border, eSamplerAddressMode_Border, 0x0)),   // EFSS_TRILINEAR_BORDER
		} };

	return result;
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::ExecuteAnisotropicVerticalBlur(CTexture* pTex, int nAmount, float fScale, float fDistribution, bool bAlphaOnly)
{
	m_AnisoVBlurPass->Execute(pTex, nAmount, fScale, fDistribution, bAlphaOnly);
}

//////////////////////////////////////////////////////////////////////////
const SRenderViewInfo& CGraphicsPipeline::GetCurrentViewInfo(CCamera::EEye eye) const
{
	const CRenderView* pRenderView = GetCurrentRenderView();
	if (pRenderView)
	{
		return pRenderView->GetViewInfo(eye);
	}

	static SRenderViewInfo viewInfo;
	return viewInfo;
}

void CGraphicsPipeline::SetParticleBuffers(bool bOnInit, CDeviceResourceSetDesc& resources, ResourceViewHandle hView, EShaderStage shaderStages) const
{
	if (!bOnInit && m_pCurrentRenderView)
	{
		int frameId = m_pCurrentRenderView->GetFrameId();
		const CParticleBufferSet& particleBuffer = gcpRendD3D.GetParticleBufferSet();
		const auto positionStream = particleBuffer.GetPositionStream(frameId);
		const auto axesStream     = particleBuffer.GetAxesStream(frameId);
		const auto colorStream    = particleBuffer.GetColorSTsStream(frameId);
		if (positionStream && axesStream && colorStream)
		{
			resources.SetBuffer(EReservedTextureSlot_ParticlePositionStream, const_cast<CGpuBuffer*>(positionStream), hView, shaderStages);
			resources.SetBuffer(EReservedTextureSlot_ParticleAxesStream,     const_cast<CGpuBuffer*>(axesStream),     hView, shaderStages);
			resources.SetBuffer(EReservedTextureSlot_ParticleColorSTStream,  const_cast<CGpuBuffer*>(colorStream),    hView, shaderStages);
			return;
		}
	}

	auto nullBuffer = CDeviceBufferManager::GetNullBufferStructured();
	resources.SetBuffer(EReservedTextureSlot_ParticlePositionStream, nullBuffer, hView, shaderStages);
	resources.SetBuffer(EReservedTextureSlot_ParticleAxesStream,     nullBuffer, hView, shaderStages);
	resources.SetBuffer(EReservedTextureSlot_ParticleColorSTStream,  nullBuffer, hView, shaderStages);
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::ExecutePostAA()
{
	auto* pStage = GetStage<CPostAAStage>();
	CRY_ASSERT(pStage);
	pStage->Execute();
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::GeneratePerViewConstantBuffer(const SRenderViewInfo* pViewInfo, int viewInfoCount, CConstantBufferPtr pPerViewBuffer, const SRenderViewport* pCustomViewport)
{
	CRY_ASSERT(m_pCurrentRenderView);
	if (!gEnv->p3DEngine || !pPerViewBuffer)
		return;

	const SRenderViewShaderConstants& perFrameConstants = m_pCurrentRenderView->GetShaderConstants();
	CryStackAllocWithSizeVectorCleared(HLSL_PerViewGlobalConstantBuffer, viewInfoCount, bufferData, CDeviceBufferManager::AlignBufferSizeForStreaming);

	const SRenderGlobalFogDescription& globalFog = m_pCurrentRenderView->GetGlobalFog();

	for (int i = 0; i < viewInfoCount; ++i)
	{
		CRY_ASSERT(pViewInfo[i].pCamera);

		const SRenderViewInfo& viewInfo = pViewInfo[i];

		HLSL_PerViewGlobalConstantBuffer& cb = bufferData[i];

		const float animTime = GetAnimationTime().GetSeconds();
		const bool bReverseDepth = (viewInfo.flags & SRenderViewInfo::eFlags_ReverseDepth) != 0;

		cb.CV_HPosScale = viewInfo.downscaleFactor;

		SRenderViewport viewport = pCustomViewport ? *pCustomViewport : viewInfo.viewport;
		cb.CV_ScreenSize = Vec4(float(viewport.width),
		                        float(viewport.height),
		                        0.5f / (viewport.width / viewInfo.downscaleFactor.x),
		                        0.5f / (viewport.height / viewInfo.downscaleFactor.y));

		cb.CV_ViewProjZeroMatr = viewInfo.cameraProjZeroMatrix.GetTransposed();
		cb.CV_ViewProjMatr = viewInfo.cameraProjMatrix.GetTransposed();
		cb.CV_ViewProjNearestMatr = viewInfo.cameraProjNearestMatrix.GetTransposed();
		cb.CV_InvViewProj = viewInfo.invCameraProjMatrix.GetTransposed();
		cb.CV_PrevViewProjMatr = viewInfo.prevCameraProjMatrix.GetTransposed();
		cb.CV_PrevViewProjNearestMatr = viewInfo.prevCameraProjNearestMatrix.GetTransposed();
		cb.CV_ViewMatr = viewInfo.viewMatrix.GetTransposed();
		cb.CV_InvViewMatr = viewInfo.invViewMatrix.GetTransposed();
#ifndef _RELEASE
		cb.CV_ProjMatr = viewInfo.projMatrix.GetTransposed();
		cb.CV_ProjMatrUnjittered = viewInfo.unjitteredProjMatrix.GetTransposed();
#endif

		Vec4r vWBasisX, vWBasisY, vWBasisZ, vCamPos;
		CShadowUtils::ProjectScreenToWorldExpansionBasis(Matrix44(IDENTITY), *viewInfo.pCamera, m_pCurrentRenderView->m_vProjMatrixSubPixoffset,
		                                                 float(viewport.width), float(viewport.height), vWBasisX, vWBasisY, vWBasisZ, vCamPos, true);

		cb.CV_ScreenToWorldBasis.SetColumn(0, Vec3r(vWBasisX));
		cb.CV_ScreenToWorldBasis.SetColumn(1, Vec3r(vWBasisY));
		cb.CV_ScreenToWorldBasis.SetColumn(2, Vec3r(vWBasisZ));
		cb.CV_ScreenToWorldBasis.SetColumn(3, viewInfo.cameraOrigin);

		cb.CV_SunLightDir = Vec4(perFrameConstants.pSunDirection, 1.0f);
		cb.CV_SunColor = Vec4(perFrameConstants.pSunColor, perFrameConstants.sunSpecularMultiplier);
		cb.CV_SkyColor = Vec4(perFrameConstants.pSkyColor, 1.0f);
		cb.CV_FogColor = Vec4(globalFog.bEnable ? globalFog.color.toVec3() : Vec3(0.f, 0.f, 0.f), perFrameConstants.pVolumetricFogParams.z);
		cb.CV_TerrainInfo = Vec4(gEnv->p3DEngine->GetTerrainTextureMultiplier(), 0, 0, 0);

		cb.CV_AnimGenParams = Vec4(animTime * 2.0f, animTime * 0.25f, animTime * 1.0f, animTime * 0.125f);

		// Scene-referred pre-exposure, for every writer that bypasses the deferred light lists:
		// the 16 forward / transparent PS_HDR_RANGE_ADAPT_MAX sites and the emissive term
		// (decisions/s1-writer-coverage.md approach A, the UE View.PreExposure shape). The value
		// is the one CD3D9Renderer::LatchSceneReferredExposure() latched for this render view, so
		// the lights, the sky and the forward passes cannot disagree within a frame.
		//
		// Stored MINUS ONE, and the macro adds it back: off the scene-referred path the exposure
		// is 1.0, so this is 0.0, so a shader whose per-view buffer is not bound still multiplies
		// by exactly 1.0 instead of blacking out. See FXConstantDefs.cfi.
		//
		// .z carries the OTHER scene-referred constant, on exactly the same terms: the BRDF
		// normalisation factor of SceneReferredSpec.md D4 - 1.0 on the stock path, 1/PI on the
		// scene-referred one, where the lights are radiometric and the 1/PI that CE omits from
		// both lobes has to come back somewhere. A per-view CONSTANT and not a static flag,
		// deliberately: a flag would double the permutation count of Illum, Terrain, HumanSkin,
		// Glass, MultiLayeredMaterials, Vegetation and TiledShading to save nothing, because
		// both BRDF sites already multiply by a value at that point. Stored minus one for the
		// unbound-buffer reason above.
		{
			const float sceneExposure = gcpRendD3D.GetSceneReferredExposure();
			const float brdfFactor = gcpRendD3D.GetSceneReferredBRDFFactor();
			// .w is the exposure meter's saturation luminance (SCENE_METER_CEILING). NOT offset
			// by one: 0 is the meaningful "stock" value here, which is what an unbound buffer
			// reads and what the stock path publishes, so the branch in HDRSampleLumInitialPS
			// folds away off the switch.
			const float meterCeiling = CRendererResources::GetSceneReferredMeterCeiling();
			cb.CV_SceneExposure = Vec4(sceneExposure - 1.0f, (1.0f / sceneExposure) - 1.0f, brdfFactor - 1.0f, meterCeiling);
		}

		// The luminance weights of the space the HDR target is in this frame (SceneReferredSpec.md
		// S3). Rec.709's (0.2126, 0.7152, 0.0722) on the stock path; the AP1 Y row once the
		// working-space conversion runs. Compared with 709, red gains about 0.060, green loses
		// 0.041 and blue loses 0.019, so a saturated red source reads roughly 28 % brighter than
		// the 709 weights would say - which is exactly the S3 acceptance case: with the wrong
		// weights a red lamp sits below the glare threshold and the test fails silently.
		//
		// Published as a DELTA from Rec.709 and added back in GetWorkingLuminanceWeights(), for
		// the same reason CV_SceneExposure is stored minus one: not every pass between the
		// conversion point and the tone map binds this buffer, and one that does not must fall
		// back to the stock weights rather than to zero. On the stock path the delta is an exact
		// 0.0 and the addition is bit-identical.
		//
		// Gated on IsSceneReferredWorkingSpace() and not on IsSceneReferred(): this follows what
		// is actually IN the buffer, so r_SceneReferredDebug 1 (skip the conversion) keeps the
		// weights on Rec.709 too and the A/B stays coherent.
		{
			static const Vec3 kRec709Y(0.2126f, 0.7152f, 0.0722f);

			const Vec3 wanted = CRendererResources::GetWorkingLuminanceWeights();
			// .w is the writer clamp (SCENE_WRITER_CLAMP), not a weight. It lives in this float4
			// because it is the only spare slot in the per-view block that reads 0 on an unbound
			// buffer, and 0 is exactly "no clamp" - the same argument that put the meter ceiling
			// in CV_SceneExposure.w. See decisions/s8-fp16-writers.md.
			cb.CV_SceneLuminanceWeightsDelta = Vec4(wanted - kRec709Y, CRendererResources::GetSceneReferredWriterClamp());
		}

		Vec3 pDecalZFightingRemedy;
		{
			const float* mProj = viewInfo.projMatrix.GetData();
			const float s = clamp_tpl(CRendererCVars::CV_r_ZFightingDepthScale, 0.1f, 1.0f);

			pDecalZFightingRemedy.x = s;                                      // scaling factor to pull decal in front
			pDecalZFightingRemedy.y = (float)((1.0f - s) * mProj[4 * 3 + 2]); // correction factor for homogeneous z after scaling is applied to xyzw { = ( 1 - v[0] ) * zMappingRageBias }
			pDecalZFightingRemedy.z = clamp_tpl(CRendererCVars::CV_r_ZFightingExtrude, 0.0f, 1.0f);

			// alternative way the might save a bit precision
			//PF.pDecalZFightingRemedy.x = s; // scaling factor to pull decal in front
			//PF.pDecalZFightingRemedy.y = (float)((1.0f - s) * mProj[4*2+2]);
			//PF.pDecalZFightingRemedy.z = clamp_tpl(CRendererCVars::CV_r_ZFightingExtrude, 0.0f, 1.0f);
		}
		cb.CV_DecalZFightingRemedy = Vec4(pDecalZFightingRemedy, 0);

		cb.CV_CamRightVector = Vec4(viewInfo.cameraVX.GetNormalized(), 0);
		cb.CV_CamFrontVector = Vec4(viewInfo.cameraVZ.GetNormalized(), 0);
		cb.CV_CamUpVector = Vec4(viewInfo.cameraVY.GetNormalized(), 0);
		cb.CV_WorldViewPosition = Vec4(viewInfo.cameraOrigin, 0);

		// CV_NearFarClipDist
		{
			// Note: CV_NearFarClipDist.z is used to put the weapon's depth range into correct relation to the whole scene
			// when generating the depth texture in the z pass (_RT_NEAREST)
			cb.CV_NearFarClipDist = Vec4(
				viewInfo.nearClipPlane,
				viewInfo.farClipPlane,
				viewInfo.farClipPlane / gEnv->p3DEngine->GetMaxViewDistance(),
				1.0f / viewInfo.farClipPlane);
		}

		// CV_ProjRatio
		{
			float zn = viewInfo.nearClipPlane;
			float zf = viewInfo.farClipPlane;
			float hfov = viewInfo.pCamera->GetHorizontalFov();
			cb.CV_ProjRatio.x = bReverseDepth ? zn / (zn - zf) : zf / (zf - zn);
			cb.CV_ProjRatio.y = bReverseDepth ? zn / (zf - zn) : zn / (zn - zf);
			cb.CV_ProjRatio.z = 1.0f / hfov;
			cb.CV_ProjRatio.w = 1.0f;
		}

		// CV_NearestScaled
		{
			float zn = viewInfo.nearClipPlane;
			float zf = viewInfo.farClipPlane;
			float nearZRange = CRendererCVars::CV_r_DrawNearZRange;
			cb.CV_NearestScaled.x = bReverseDepth ? 1.0f - zf / (zf - zn) * nearZRange : zf / (zf - zn) * nearZRange;
			cb.CV_NearestScaled.y = bReverseDepth ? zn / (zf - zn) * nearZRange * nearZRange : zn / (zn - zf) * nearZRange * nearZRange;
			cb.CV_NearestScaled.z = bReverseDepth ? 1.0f - (nearZRange - 0.001f) : nearZRange - 0.001f;
			cb.CV_NearestScaled.w = 1.0f;
		}

		// CV_TessInfo
		{
			// We want to obtain the edge length in pixels specified by CV_r_tessellationtrianglesize
			// Therefore the tess factor would depend on the viewport size and CV_r_tessellationtrianglesize
			static const ICVar* pCV_e_TessellationMaxDistance(gEnv->pConsole->GetCVar("e_TessellationMaxDistance"));
			assert(pCV_e_TessellationMaxDistance);

			const float hfov = viewInfo.pCamera->GetHorizontalFov();
			cb.CV_TessInfo.x = sqrtf(float(viewport.width * viewport.height)) / (hfov * CRendererCVars::CV_r_tessellationtrianglesize);
			cb.CV_TessInfo.y = CRendererCVars::CV_r_displacementfactor;
			cb.CV_TessInfo.z = pCV_e_TessellationMaxDistance->GetFVal();
			cb.CV_TessInfo.w = (float)CRendererCVars::CV_r_ParticlesTessellationTriSize;
		}

		cb.CV_FrustumPlaneEquation.SetRow4(0, (Vec4&)viewInfo.pFrustumPlanes[FR_PLANE_RIGHT]);
		cb.CV_FrustumPlaneEquation.SetRow4(1, (Vec4&)viewInfo.pFrustumPlanes[FR_PLANE_LEFT]);
		cb.CV_FrustumPlaneEquation.SetRow4(2, (Vec4&)viewInfo.pFrustumPlanes[FR_PLANE_TOP]);
		cb.CV_FrustumPlaneEquation.SetRow4(3, (Vec4&)viewInfo.pFrustumPlanes[FR_PLANE_BOTTOM]);

		if (gRenDev->m_pCurWindGrid)
		{
			float fSizeWH = (float)gRenDev->m_pCurWindGrid->m_nWidth * gRenDev->m_pCurWindGrid->m_fCellSize * 0.5f;
			float fSizeHH = (float)gRenDev->m_pCurWindGrid->m_nHeight * gRenDev->m_pCurWindGrid->m_fCellSize * 0.5f;
			cb.CV_WindGridOffset = Vec4(gRenDev->m_pCurWindGrid->m_vCentr.x - fSizeWH, gRenDev->m_pCurWindGrid->m_vCentr.y - fSizeHH, 1.0f / (float)gRenDev->m_pCurWindGrid->m_nWidth, 1.0f / (float)gRenDev->m_pCurWindGrid->m_nHeight);
		}
	}

	pPerViewBuffer->UpdateBuffer(&bufferData[0], sizeof(HLSL_PerViewGlobalConstantBuffer), 0, viewInfoCount);
}

//////////////////////////////////////////////////////////////////////////
void CGraphicsPipeline::ApplyShaderQuality(CDeviceGraphicsPSODesc& psoDesc, const SShaderProfile& shaderProfile)
{
	const uint64 quality = g_HWSR_MaskBit[HWSR_QUALITY];
	const uint64 quality1 = g_HWSR_MaskBit[HWSR_QUALITY1];

	psoDesc.m_ShaderFlags_RT &= ~(quality | quality1);
	switch (psoDesc.m_ShaderQuality = shaderProfile.GetShaderQuality())
	{
	case eSQ_Medium:
		psoDesc.m_ShaderFlags_RT |= quality;
		break;
	case eSQ_High:
		psoDesc.m_ShaderFlags_RT |= quality1;
		break;
	case eSQ_VeryHigh:
		psoDesc.m_ShaderFlags_RT |= (quality | quality1);
		break;
	}

}
