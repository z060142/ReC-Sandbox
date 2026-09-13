// Copyright 2013-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#if defined(FEATURE_SVO_GI)

#pragma pack(push,4)

typedef std::unordered_map<uint64, std::pair<byte, byte>> PvsMap;
typedef std::set<class CVoxelSegment*>                    VsSet;

template<class T, int maxElemsInChunk> class CCustomSVOPoolAllocator
{
public:
	CCustomSVOPoolAllocator() { m_numElements = 0; }

	~CCustomSVOPoolAllocator()
	{
		Reset();
	}

	void Reset()
	{
		for (int i = 0; i < m_chunksPool.Count(); i++)
		{
			delete[](byte*)m_chunksPool[i];
			m_chunksPool[i] = NULL;
		}
		m_numElements = 0;
	}

	void ReleaseElement(T* pElem)
	{
		if (pElem)
			m_freeElements.Add(pElem);
	}

	T* GetNewElement()
	{
		if (m_freeElements.Count())
		{
			T* pPtr = m_freeElements.Last();
			m_freeElements.DeleteLast();
			return pPtr;
		}

		int poolId = m_numElements / maxElemsInChunk;
		int elemId = m_numElements - poolId * maxElemsInChunk;
		m_chunksPool.PreAllocate(poolId + 1, poolId + 1);
		if (!m_chunksPool[poolId])
			m_chunksPool[poolId] = (T*)new byte[maxElemsInChunk * sizeof(T)];
		m_numElements++;
		return &m_chunksPool[poolId][elemId];
	}

	int GetCount()         { return m_numElements - m_freeElements.Count(); }
	int GetCapacity()      { return m_chunksPool.Count() * maxElemsInChunk; }
	int GetCapacityBytes() { return GetCapacity() * sizeof(T); }

private:

	int          m_numElements;
	PodArray<T*> m_chunksPool;
	PodArray<T*> m_freeElements;
};

class CSvoNode
{
public:

	CSvoNode(const AABB& box, CSvoNode* pParent);
	~CSvoNode();

	static void*         operator new(size_t);
	static void          operator delete(void* ptr);

	void                 CheckAllocateChilds();
	void                 DeleteChilds();
	void                 Render(PodArray<struct SPvsItem>* pSortedPVS, uint64 nodeKey, int treeLevel, PodArray<SVF_P3F_C4B_T2F>& arrVertsOut,
	                            PodArray<class CVoxelSegment*> arrForStreaming[SVO_STREAM_QUEUE_MAX_SIZE][SVO_STREAM_QUEUE_MAX_SIZE], const AABB& playableArea);
	bool                 IsStreamingInProgress();
	void                 GetTrisInAreaStats(int& trisCount, int& vertCount, int& trisBytes, int& vertBytes, int& maxVertPerArea, int& matsCount);
	void                 GetVoxSegMemUsage(int& allocated);
	AABB                 GetChildBBox(int childId);
	static AABB          GetMagnifiedNodeBox(const AABB& nodeBox);
	void                 CheckAllocateSegment(int lod);
	void                 OnStatLightsChanged(const AABB& objBox);
	class CVoxelSegment* AllocateSegment(int cloudId, int stationId, int lod, EFileStreamingStatus eStreamingStatus, bool bDroppedOnDisk);
	uint32               SaveNode(PodArray<byte>& arrData, uint32& nNodesCounter, ICryArchive* pArchive, uint32& totalSizeCounter, const AABB& playableArea);
	void                 MakeNodeFilePath(char* szFileName);
	bool                 CheckReadyForRendering(int treeLevel, PodArray<CVoxelSegment*> arrForStreaming[SVO_STREAM_QUEUE_MAX_SIZE][SVO_STREAM_QUEUE_MAX_SIZE]);
	CSvoNode*            FindNodeByPosition(const Vec3& vPosWS, int treeLevelToFind, int treeLevelCur);
	void                 UpdateNodeRenderDataPtrs();
	void                 RegisterMovement(const AABB& objBox);
	Vec3i                GetStatGeomCheckSumm();
	CSvoNode*            FindNodeByPoolAffset(int allocatedAtlasOffset);
	static bool          IsStreamingActive();

	AABB                       m_nodeBox;
	CSvoNode**                 m_ppChilds;
	std::pair<uint32, uint32>* m_pChildFileOffsets;
	CSvoNode*                  m_pParent;
	CVoxelSegment*             m_pSeg;
	uint                       m_requestSegmentUpdateFrametId;
	bool                       m_arrChildNotNeeded[8];
	bool                       m_bForceRecreate;
};

class CPointTreeNode
{
public:
	bool TryInsertPoint(int pointId, const Vec3& vPos, const AABB& nodeBox, int recursionLevel = 0);
	bool IsThereAnyPointInTheBox(const AABB& testBox, const AABB& nodeBox);
	bool GetAllPointsInTheBox(const AABB& testBox, const AABB& nodeBox, PodArray<int>& arrIds);
	AABB GetChildBBox(int childId, const AABB& nodeBox);
	void Clear();
	CPointTreeNode() { m_ppChilds = 0; m_pPoints = 0; }
	~CPointTreeNode() { Clear(); }

	struct SPointInfo
	{ Vec3 vPos; int id; };
	PodArray<SPointInfo>* m_pPoints;
	CPointTreeNode**      m_ppChilds;
};

struct SBrickSubSet
{
	ColorB arrData[SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE];
};

class CSvoEnv : public Cry3DEngineBase
{
public:

	CSvoEnv(const AABB& worldBox);
	~CSvoEnv();
	bool GetSvoStaticTextures(I3DEngine::SSvoStaticTexInfo& svoInfo, PodArray<I3DEngine::SLightTI>* pLightsTI_S, PodArray<I3DEngine::SLightTI>* pLightsTI_D);
	void GetSvoBricksForUpdate(PodArray<I3DEngine::SSvoNodeInfo>& arrNodeInfo, float nodeSize, PodArray<SVF_P3F_C4B_T2F>* pVertsOut);
	bool Render();
	void ProcessSvoRootTeleport();
	void CheckUpdateMeshPools();
	int  GetWorstPointInSubSet(const int start, const int end);
	void StartupStreamingTimeTest(bool bDone);
	void OnLevelGeometryChanged();
	void ReconstructTree(bool bMultiPoint);
	void AllocateRootNode();
	int  ExportSvo(ICryArchive* pArchive);
	AABB GetPlayableArea();
	void DetectMovement_StaticGeom();
	void DetectMovement_StatLights();
	void CollectLights();
	void CollectAnalyticalOccluders();
	void AddAnalyticalOccluder(IRenderNode* pRN, Vec3 camPos);
	void GetGlobalEnvProbeProperties(_smart_ptr<ITexture>& specEnvCM, float& mult);

	// Mesh ray tracing pools (rt decision 02). All dimensions come from here - single source of truth.
	void RTCachePoolDims();
	int  GetRTPoolXY() const    { return m_rtPoolXY; }
	int  GetRTPoolZ() const     { return m_rtPoolZ; }
	int  GetRTTexRes() const    { return m_rtTexRes; }
	int  GetRTTexPoolZ() const  { return m_rtTexPoolZ; }
	int  GetRTPoolRecords() const { return m_rtPoolXY * m_rtPoolXY * m_rtPoolZ / SVO_RT_RECORD_TEXELS; }

	//! Chunk allocator over the STATIC segment; call holds m_arrRTPoolTris.m_Lock in modify mode.
	int  RTAllocChunk(int& records);
	void RTFreeChunkLocked(int start, int count);
	void RTQueueFreeChunk(int start, int count);
	void RTQueueFreeTexSlice(int slice);
	void RTProcessPendingFrees();
	void RTMarkTrisDirty(int firstRecord, int records);

	//! Material atlas slice allocator; caller holds m_arrRTPoolTexs.m_Lock in modify mode.
	int  RTAllocTexSlice(int* pOwner);
	void RTAddTexSliceRef(int slice);
	void RTReleaseTexSlice(int slice);
	void RTMarkTexsDirty(int firstSlice, int slices);

	void RTUploadDirtySlices();

	//! Known gap 7.2 - one atlas UV scale per material. Counted and warned about once per level.
	void RTCountUvScaleMismatch(const char* szMatName);

	//! The two RT status lines, shared by the r_DisplayInfo HUD and the log (2B).
	void RTFormatPoolLine(char* szOut, size_t bufSize) const;
	void RTFormatBvhLine(char* szOut, size_t bufSize) const;
	//! Prints both lines to the log once per completed voxelization pass.
	void RTLogStatsWhenReady();

	PodArray<I3DEngine::SLightTI>            m_lightsTI_S, m_lightsTI_D;
	PodArray<I3DEngine::SAnalyticalOccluder> m_analyticalOccluders[2];
	Vec4                      m_vSvoOriginAndSize;
	AABB                      m_aabbLightsTI_D;
	SRenderLight*             m_pGlobalEnvProbe;
	CryCriticalSection        m_csLockGlobalEnvProbe;
	CVoxStreamEngine*         m_pStreamEngine;
	CSvoNode*                 m_pSvoRoot;
	bool                      m_bReady;
	bool                      m_bRootTeleportSkipFrame = false;
	PodArray<CVoxelSegment*>  m_arrForStreaming[SVO_STREAM_QUEUE_MAX_SIZE][SVO_STREAM_QUEUE_MAX_SIZE];
	int                       m_debugDrawVoxelsCounter;
	int                       m_nodeCounter;
	int                       m_dynNodeCounter;
	int                       m_dynNodeCounter_DYNL;
	PodArray<CVoxelSegment*>  m_arrForBrickUpdate[16];
	CryCriticalSection        m_csLockTree;
	float                     m_streamingStartTime;
	float                     m_svoFreezeTime;
	int                       m_arrVoxelizeMeshesCounter[2];
	AABB                      m_worldBox;
	PodArray<SVF_P3F_C4B_T2F> m_arrSvoProxyVertices;
	double                    m_prevCheckVal;
	bool                      m_bFirst_SvoFreezeTime;
	bool                      m_bFirst_StartStreaming;
	bool                      m_bStreamingDonePrev;

	int                       m_texOpasPoolId;
	int                       m_texNodePoolId;
	int                       m_texNormPoolId;
	int                       m_texRgb0PoolId;
	int                       m_texRgb1PoolId;
	int                       m_texRgb2PoolId;
	int                       m_texRgb3PoolId;
	int                       m_texRgb4PoolId;
	int                       m_texDynlPoolId;
	int                       m_texAldiPoolId;

	ETEX_Format                                       m_voxTexFormat;
	TDoublyLinkedList<CVoxelSegment>                  m_arrSegForUnload;
	CCustomSVOPoolAllocator<struct SBrickSubSet, 128> m_brickSubSetAllocator;
	CCustomSVOPoolAllocator<CSvoNode, 128>            m_nodeAllocator;

	PodArrayRT<ColorB> m_arrRTPoolTexs;
	PodArrayRT<Vec4>   m_arrRTPoolTris;

	// mesh ray tracing allocator state (rt decision 02 2.4 / 2.7)
	struct SRTChunk       { int start; int count; };
	struct SRTPendingFree { int start; int count; uint frameId; };

	int                      m_rtPoolXY = 0;
	int                      m_rtPoolZ = 0;
	int                      m_rtTexRes = 0;
	int                      m_rtTexPoolZ = 0;
	PodArray<SRTChunk>       m_arrRTFreeChunks;
	PodArray<SRTPendingFree> m_arrRTPendingFree;
	PodArray<uint8>          m_arrRTDirtyTris;    //!< one flag per Z slice of the record pool
	PodArray<uint8>          m_arrRTDirtyTexs;    //!< one flag per Z slice of the material atlas
	PodArray<int>            m_arrRTTexSliceRef;  //!< refcount per atlas slice
	PodArray<int*>           m_arrRTTexSliceOwner;//!< back pointer into the texture object's atlas id
	CryCriticalSection       m_rtPendingLock;
	int                      m_rtRecordsUsed = 0;
	int                      m_rtTexSlicesUsed = 0;
	bool                     m_rtOverflowWarned = false;
	bool                     m_rtTexOverflowWarned = false;
	int                      m_rtUvScaleMismatch = 0;     //!< materials whose non albedo copy has another size
	bool                     m_rtUvScaleWarned = false;
	bool                     m_rtStatsLogged = false;     //!< log line already printed for this voxelization pass
	bool                     m_rtWasReady = true;
	bool                     m_rtSelfTestDone = false;
	SRTBuildStats            m_rtStats;
};

#pragma pack(pop)

#endif
