// Copyright 2016-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include "Common/GraphicsPipelineStage.h"
#include "Common/ComputeRenderPass.h"
#include "Common/PrimitiveRenderPass.h"

struct ShadowMapFrustum;

// Light Propagation Volumes (nested cascades, sun only).
// Pipeline: sun RSM -> compute scatter injection into an int accumulator -> resolve to SH grid ->
//           iterative 6-neighbour gather propagation -> full resolution screen space irradiance.
class CLPVStage : public CGraphicsPipelineStage
{
public:
	static const EGraphicsPipelineStage StageID = eStage_LPV;

	// Number of spectral channels (R/G/B), each one storing 4 SH L1 coefficients.
	static const int32 SHChannelNum = 3;
	// Number of ints per grid cell in the injection accumulator:
	// 3 channels * 4 SH coefficients + average surface albedo (RGB) + surfel count
	// + geometry volume blocking potential (4 SH coefficients)
	// + average surface normal (xyz) + one pad element.
	static const int32 AccumulatorStride = SHChannelNum * 4 + 4 + 4 + 4;
	// Fixed point scale used by the injection accumulator (SH L1 coefficients can be negative).
	static const int32 AccumulatorFixedPointScale = 4096;
	// Upper bound for the RSM sample grid used by the injection pass.
	static const int32 MaxRsmSampleCount = 1024;
	// Nested cascades: cascade 0 is the detail volume (r_LPVSize), every further cascade covers
	// r_LPVCascadeScale times the extent of the previous one at the same grid resolution
	// (LOD chain like the cascaded shadow maps - range up, resolution down).
	static const int32 MaxCascadeNum = 3;
	// Upper bound for the local lights injected after the propagation (CE3 LPVPostinjectLight
	// equivalent, folded into the temporal pass). Must match LPV_MAX_POINT_LIGHTS in LPV.cfi.
	static const int32 MaxPointLights = 16;

public:
	CLPVStage(CGraphicsPipeline& graphicsPipeline);

	bool IsStageActive(EShaderRenderingFlags flags) const final
	{
		return CRendererCVars::CV_r_LPV > 0;
	}

	void Init() final;
	void Resize(int renderWidth, int renderHeight) final;
	void OnCVarsChanged(const CCVarUpdateRecorder& cvarUpdater) final;

	void Execute();

	// Screen space apply, split out of Execute(): it reads the scene normals as SRV, and at the
	// Execute() position in the pipeline the deferred decal pass may still have them bound as a
	// render target - on D3D11 the runtime then silently nulls the SRV and the shader reads zero
	// normals whenever a decal is visible (a binary function of the camera pose). Called right
	// before the tiled shading combine, after ClipVolumes/ShadowMask have rebound the OM.
	void ExecuteApplyToScreen();

	// 3D probe visualization (r_LPVDebug 10). Has to run after the opaque scene has been shaded,
	// otherwise the tiled shading overwrites the probes - so this is not part of Execute().
	void ExecuteDebugProbes(CTexture* pColorTarget, CTexture* pDepthTarget);

	// RSM provider, mirrors CSvoRenderer::GetRsmColorMap()/GetRsmNormlMap(). Returns nullptr when LPV
	// does not want to own the sun RSM for the given frustum. Every LPV cascade owns its own RSM
	// view fitted to its own volume (like the cascaded shadow maps), routed by
	// ShadowMapFrustum::nLpvCascadeIndex.
	CTexture* GetRsmColorMap(const ShadowMapFrustum& rFr, bool bCheckUpdate = false);
	CTexture* GetRsmNormalMap(const ShadowMapFrustum& rFr, bool bCheckUpdate = false);

	// Depth target of the dedicated RSM view.
	CTexture* GetRsmDepthMap(const ShadowMapFrustum& rFr, bool bCheckUpdate = false);

	// Texture objects handed to CShadowMapStage::Init() before any device resource exists (the pass
	// group defaults; the actual per-frustum targets are routed per cascade at pass preparation).
	CTexture* GetRsmColorTexture() const;
	CTexture* GetRsmNormalTexture() const;

	// True when the RSM content of the given cascade actually needs a re-render, queried by
	// CShadowMapStage during the shadow prep: on every other frame that cascade's RSM pass is
	// dropped entirely and the LPV keeps injecting from the last rendered RSM through the matrices
	// stored at its render time. Re-renders trigger on sun changes, camera movement past a small
	// anchor distance (per cascade: the far cascade's bigger cells re-render less often), invalid
	// history, and a slow content-refresh fallback.
	bool WantsRsmRender(const ShadowMapFrustum& rFr);

	// Called by CShadowMapStage when it actually hands out the RSM depth target for this frame -
	// the same frame's Execute() then re-pins the injection matrices to the fresh content.
	void NotifyRsmRender(const ShadowMapFrustum& rFr);

	// GI combine, mirrors CSvoRenderer::GetDiffuseFinRT(). Only valid after a successful Execute().
	CTexture* GetIrradianceRT() const;

	// Specular GI (short voxel march along the reflected eye vector), mirrors
	// CSvoRenderer::GetSpecularFinRT(). Null while r_LPVSpecular is 0.
	CTexture* GetSpecularRT() const;

private:
	// All state that exists once per cascade. The compute shaders are shared - every pass runs per
	// cascade with the cascade's own constants, only the screen space apply reads both volumes.
	struct SCascade
	{
		// Propagation ping-pong pair, each entry holds the R/G/B SH textures of one iteration.
		_smart_ptr<CTexture> pGridSH[2][SHChannelNum];
		// Running propagation sum ping-pong pair (gather-only scheme, explicit sum).
		_smart_ptr<CTexture> pGridSum[2][SHChannelNum];
		// Temporal accumulation ping-pong pair (exponential moving average of the propagated grid).
		_smart_ptr<CTexture> pGridAcc[2][SHChannelNum];
		// Geometry volume: per cell directional blocking potential (SH L1, achromatic).
		_smart_ptr<CTexture> pGridGV;

		// The cascade's own sun RSM, fitted to the cascade volume.
		_smart_ptr<CTexture> pRsmColor;
		_smart_ptr<CTexture> pRsmNormal;
		_smart_ptr<CTexture> pRsmDepth;

		CGpuBuffer           injectionBuffer;

		CComputeRenderPass   passClear;
		CComputeRenderPass   passInject;
		CComputeRenderPass   passResolve;
		CComputeRenderPass   passPropagate[3]; // first iteration / odd / even bindings
		CComputeRenderPass   passTemporal[2];

		Vec3                 gridOrigin = Vec3(ZERO);
		// Grid origin of the previous relight, used for the integer cell reprojection.
		Vec3                 prevGridOrigin = Vec3(ZERO);
		float                cellSize = 1.0f;
		// Index of the accumulation set holding the temporal result of the current frame.
		int32                accIndex = 0;
		// Textures holding the final propagated result of the last relight (points into pGridSum,
		// or pGridSH[0] when the iteration count is zero).
		CTexture*            pFinalGrid[SHChannelNum] = { nullptr, nullptr, nullptr };
		bool                 bHistoryValid = false;

		// RSM view state, pinned at RSM render time so matrices and content always agree.
		Matrix44A            rsmToWorld;
		Matrix44A            worldToRsm;
		// Depth range of the pinned RSM view: the depth buffer stores (Z - near) / (far - near)
		// (linear), the matrices work in z_ndc - the shaders convert via these (LPVRsmDepthRange).
		float                rsmNearDist = 1.0f;
		float                rsmFarDist = 2.0f;
		float                rsmTexelSize = 0.0f;
		int32                rsmSize = 0;
		int32                rsmSampleCount = 0;
		int32                rsmSampleStride = 1;
		// RSM re-render gating.
		Vec3                 rsmRenderCamPos = Vec3(-1e9f);
		int32                framesSinceRsmRender = 1000000;
		bool                 bRsmRenderedThisFrame = false;
	};

	static int32 GetValidGridSize();
	static int32 GetActiveCascadeCount();

	bool IsSvoProvidingRsm() const;
	bool IsRsmFrustum(const ShadowMapFrustum& rFr) const;

	SCascade&       CascadeForFrustum(const ShadowMapFrustum& rFr);
	const SCascade& CascadeForFrustum(const ShadowMapFrustum& rFr) const;

	void ResizeGrid(int32 gridSize);
	void CheckCreateUpdateRsmTarget(_smart_ptr<CTexture>& pTex, int32 size, const char* szName);

	const ShadowMapFrustum* FindRsmFrustum(int32 cascadeIndex) const;
	bool                    UpdateFrameParameters(SCascade& cascade, const ShadowMapFrustum& frustum, const CTexture* pRsmColor);
	void                    UpdateCascadeParameters(SCascade& cascade, int32 cascadeIndex);

	void ExecuteClear(SCascade& cascade, const SScopedComputeCommandList& commandList);
	void ExecuteInject(SCascade& cascade, const SScopedComputeCommandList& commandList);
	void ExecuteResolve(SCascade& cascade, const SScopedComputeCommandList& commandList);
	void ExecutePropagate(SCascade& cascade, const SScopedComputeCommandList& commandList);
	void ExecuteTemporal(SCascade& cascade, const SScopedComputeCommandList& commandList);
	void ExecuteApply(const SScopedComputeCommandList& commandList, CTexture* pRsmDepth);

	void SetGridConstants(CComputeRenderPass& pass, const SCascade& cascade);

private:
	SCascade             m_cascades[MaxCascadeNum];

	// Screen space output (irradiance, never pre-multiplied with albedo).
	_smart_ptr<CTexture> m_pIrradiance;
	// Screen space specular GI output (radiance along the reflected eye vector).
	_smart_ptr<CTexture> m_pSpecular;

	CComputeRenderPass   m_passApply;

	// Debug probe spheres, one instanced impostor quad per grid cell (near cascade only).
	CPrimitiveRenderPass m_passDebugProbes;
	CRenderPrimitive     m_primDebugProbes;

	int32                m_gridSize;
	// RSM depth of the frame's Execute() (near cascade), consumed by ExecuteApplyToScreen().
	CTexture*            m_pApplyRsmDepth = nullptr;
	// Set by Execute() when the grid is ready, consumed (and reset) by ExecuteApplyToScreen().
	bool                 m_bGridReadyForApply = false;
	// Number of cascades that ran in the frame's Execute(), consumed by ExecuteApplyToScreen().
	int32                m_activeCascadeCount = 1;
	// Local lights of the frame (gathered in Execute, injected by the temporal pass).
	Vec4                 m_lightPos[MaxPointLights];
	Vec4                 m_lightColor[MaxPointLights];
	// xyz: emission direction (projector axis / area light normal), w: cos outer cone half-angle
	// (-1.5 marks an omnidirectional point light).
	Vec4                 m_lightDir[MaxPointLights];
	int32                m_lightCount = 0;
	// Relight gating state: the grid is only re-injected on discrete input changes or on the heartbeat
	// interval, a frozen grid stays exactly correct for a static scene and cannot flicker.
	Vec3                 m_lastSunDir;
	Vec4                 m_lastSunColor;
	int32                m_framesSinceRelight;
	bool                 m_bForceRelight;
	uint32               m_lastLightsHash = 0;
	bool                 m_bResultValid;
};
