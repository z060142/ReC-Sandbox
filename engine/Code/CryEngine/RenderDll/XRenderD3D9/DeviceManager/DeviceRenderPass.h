// Copyright 2017-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include "DeviceResourceSet.h"

////////////////////////////////////////////////////////////////////////////
// Device Render Pass

class TMP_RENDER_API CDeviceRenderPassDesc : NoCopy
{
	friend class CDeviceObjectFactory;

public:
	// rt stage 5B. Was 4. Stock CE never binds a slot above 3, but the SVO ray tracing
	// ConeTracePass does: stage 4A added the HITGI target at slot 4 and stage 5B adds HITMAT at
	// slot 5, and SetRenderTarget indexes m_renderTargets[slot] unconditionally - with the old
	// value that is a write PAST THE END of a 4 element std::array, straight into m_outputUAVs,
	// and GetDeviceRendertargetViews never saw the target at all. 8 is the D3D11 / D3D12 /
	// Vulkan colour attachment limit, so this is the ceiling of the API rather than a new one.
	// Every other use of the constant is derived from it (the view arrays, the format
	// validation array, the hash input, the Vulkan attachment arrays), so raising it costs a
	// few pointers per render pass object and changes no behaviour on any stock pass: slots
	// 4-7 stay null everywhere except here.
	enum { MaxRendertargetCount = 8 };
	enum { MaxOutputUAVCount = 3 };

	struct SHash { uint64 operator() (const CDeviceRenderPassDesc& desc)                                  const; };
	struct SEqual { bool   operator() (const CDeviceRenderPassDesc& lhs, const CDeviceRenderPassDesc& rhs) const; };

public:
	CDeviceRenderPassDesc();
	CDeviceRenderPassDesc(void* pInvalidateCallbackOwner, const SResourceBinding::InvalidateCallbackFunction& invalidateCallback);
	CDeviceRenderPassDesc(const CDeviceRenderPassDesc& other);
	CDeviceRenderPassDesc(const CDeviceRenderPassDesc& other, void* pInvalidateCallbackOwner, const SResourceBinding::InvalidateCallbackFunction& invalidateCallback);
	~CDeviceRenderPassDesc();

	bool HasChanged() const { return m_bResourcesInvalidated; }
	void AcceptAllChanges() { m_bResourcesInvalidated = false; }

	bool SetRenderTarget(uint32 slot, CTexture* pTexture, ResourceViewHandle hView = EDefaultResourceViews::RenderTarget);
	bool SetDepthTarget(CTexture* pTexture, ResourceViewHandle hView = EDefaultResourceViews::DepthStencil);
	bool SetOutputUAV(uint32 slot, CGpuBuffer* pBuffer);
	bool SetResources(const CDeviceRenderPassDesc& other);
	bool ClearResources() threadsafe;

	bool GetDeviceRendertargetViews(std::array<D3DSurface*, MaxRendertargetCount>& views, int& viewCount) const;
	bool GetDeviceDepthstencilView(D3DDepthSurface*& pView) const;

	const std::array<SResourceBinding, MaxRendertargetCount>& GetRenderTargets()           const { return m_renderTargets; }
	const            SResourceBinding&                        GetDepthTarget()             const { return m_depthTarget; }
	const std::array<SResourceBinding, MaxOutputUAVCount>&    GetOutputUAVs()              const { return m_outputUAVs; }

	static bool OnResourceInvalidated(void* pThis, SResourceBindPoint bindPoint, UResourceReference pResource, uint32 flags) threadsafe;

protected:
	bool UpdateResource(SResourceBindPoint bindPoint, SResourceBinding& dstResource, const SResourceBinding& srcResource);

	std::array<SResourceBinding, MaxRendertargetCount> m_renderTargets;
	std::array<SResourceBinding, MaxOutputUAVCount>    m_outputUAVs;
	SResourceBinding                                   m_depthTarget;

	SResourceBinding::InvalidateCallbackFunction       m_invalidateCallback;
	void*                                              m_invalidateCallbackOwner;

	std::atomic<bool>                                  m_bResourcesInvalidated;
};

class CDeviceRenderPass_Base : public NoCopy
{
	friend class CDeviceObjectFactory;

public:
	CDeviceRenderPass_Base();
	virtual ~CDeviceRenderPass_Base() {}

	bool         IsValid() const { return m_bValid; }
	void         Invalidate() { m_bValid = false; }
	uint64       GetHash() const { return m_nHash; }

	bool         Update(const CDeviceRenderPassDesc& passDesc);
	static bool  UpdateWithReevaluation(CDeviceRenderPassPtr& pRenderPass, CDeviceRenderPassDesc& passDesc);

private:
	virtual bool UpdateImpl(const CDeviceRenderPassDesc& passDesc) = 0;

protected:
	uint64                 m_nHash;
	uint32                 m_nUpdateCount;
	bool                   m_bValid;

#if !defined(RELEASE)
	std::array<DXGI_FORMAT, CDeviceRenderPassDesc::MaxRendertargetCount + 1>  m_targetFormats;
#endif
};

