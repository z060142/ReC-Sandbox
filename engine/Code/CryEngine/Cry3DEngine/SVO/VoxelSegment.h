// Copyright 2013-2021 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#if defined(FEATURE_SVO_GI)

#include <3rdParty/concqueue/concqueue.hpp>
#include <CryThreading/IThreadManager.h>	

	#pragma pack(push,4)

//#pragma optimize("",off)

	#define SVO_NODE_BRICK_SIZE        2
	#define SVO_VOX_BRICK_MAX_SIZE     16 // maximum size of voxel brick 3d array data
	#define SVO_BRICK_ALLOC_CHUNK_SIZE 16 // size of allocation granularity
	#define SVO_ATLAS_DIM_MAX_XY       (CVoxelSegment::m_voxTexPoolDimXY / SVO_BRICK_ALLOC_CHUNK_SIZE)
	#define SVO_ATLAS_DIM_MAX_Z        (CVoxelSegment::m_voxTexPoolDimZ / SVO_BRICK_ALLOC_CHUNK_SIZE)
	#define SVO_ATLAS_DIM_BRICKS_XY    (CVoxelSegment::m_voxTexPoolDimXY / SVO_VOX_BRICK_MAX_SIZE)
	#define SVO_ATLAS_DIM_BRICKS_Z     (CVoxelSegment::m_voxTexPoolDimZ / SVO_VOX_BRICK_MAX_SIZE)
	#define SVO_ROOTLESS_PARENT_SIZE   512.f
	#define SVO_STREAM_QUEUE_MAX_SIZE  12

// Mesh ray tracing record pool (rt decision 02 - the CPU/GPU contract).
// One record is 4 texels of the R32G32B32A32F record pool; records never cross a texture row.
	#define SVO_RT_RECORD_TEXELS       4
	#define SVO_RT_SEG_DYN_MESH        0      // global dynamic mesh BVH (stage 3)
	#define SVO_RT_SEG_DYN_MATS        30720  // dynamic material records (stage 3)
	#define SVO_RT_SEG_PARTICLES       32768  // sprite BVH (stage 4)
	#define SVO_RT_SEG_STATIC          49152  // per SVO cell static blocks - record 0 is never a valid static root
	#define SVO_RT_CHUNK_GRANULARITY   64     // chunk allocator granularity, in records
	#define SVO_RT_DYN_MESH_COUNT      (SVO_RT_SEG_DYN_MATS - SVO_RT_SEG_DYN_MESH)       // 30720 records
	#define SVO_RT_DYN_MATS_COUNT      (SVO_RT_SEG_PARTICLES - SVO_RT_SEG_DYN_MATS)      // 2048 records
// Depth budget of the two level dynamic tree: the consumer stack is sized for e_svoTI_RT_MaxDepth + 2,
// so the top level over objects and the cached per object subtrees have to share those 18 levels.
	#define SVO_RT_DYN_TOP_MAX_DEPTH   6      // <= 64 objects before the overflow leaf policy kicks in

// Shading tags written into the material record's matInfo3.z (rt decision 10). Float valued small
// ints; 0.25 is the legacy vegetation-leaves value and is KEPT so the current consumer still works.
	#define SVO_RT_TAG_ILLUM           0.f
	#define SVO_RT_TAG_VEG_LEAVES      0.25f
	#define SVO_RT_TAG_HUMAN_SKIN      2.f
	#define SVO_RT_TAG_GLASS           3.f
	#define SVO_RT_TAG_WATER           4.f    // ocean plane / water volume surface (decision 09 9.4)
	#define SVO_RT_TAG_TERRAIN         5.f
	#define SVO_RT_TAG_EMISSIVE_ONLY   6.f    // reserved - CE has no usable "emissive but unlit" flag

//! One triangle as handed to the static BVH builder (world space, already resolved from the soup).
struct SRTBuildTri
{
	Vec3 v[3];         //!< world space positions
	Vec3 n[3];         //!< per vertex normals, already flipped into the face normal hemisphere
	Vec2 t[3];         //!< per vertex UVs, already normalised into [0, 16)
	Vec3 faceNorm;     //!< plane normal
	int  matRecord;    //!< absolute record index of the material record
};

//! One material as the pool stores it (rt decision 10): the 4 texel base record, an optional 4 texel
//! "extras" record carrying everything the base record has no lane for (detail map, blend layer,
//! transmittance, glass / skin parameters) and, for a %BLENDLAYER material, a second full base record
//! describing the blend layer's own texture set. The three are written consecutively:
//!     base at R, extras at R + 1, blend base at R + 2.
//! base[0].z's LOW ExtractUint2 field holds the extras offset + 1 (0 = no extras, 1 = extras at R + 1);
//! extras[1].x holds the ABSOLUTE record index of the blend base (0 = none).
struct SRTMatRecordSet
{
	Vec4 base[SVO_RT_RECORD_TEXELS];
	Vec4 extras[SVO_RT_RECORD_TEXELS];
	Vec4 blend[SVO_RT_RECORD_TEXELS];
	bool bExtras;
	bool bBlend;

	//! Records this material occupies in the pool (1, 2 or 3).
	//! A blend record only exists behind an extras record, so the count follows the writer exactly.
	int  RecordCount() const { return 1 + (bExtras ? 1 : 0) + ((bExtras && bBlend) ? 1 : 0); }
};

//! Statistics of one static BVH build, accumulated over the level in CSvoEnv.
struct SRTBuildStats
{
	void Reset() { ZeroStruct(*this); }

	int   cells;        //!< number of cells with a BVH
	int   tris;         //!< triangles stored
	int   nodes;        //!< BVH nodes written
	int   leaves;       //!< leaf nodes written
	int   maxDepth;     //!< deepest leaf
	int   maxLeafTris;  //!< biggest leaf
	int   records;      //!< records written (nodes + triangles + materials)
	int   mats;         //!< material records written
	int   uvClamped;    //!< triangles whose UV span did not fit [0, 16)
	int   trisSkipped;  //!< triangles rejected (degenerate or out of the quantisation range)
	int   extras;       //!< material extras records written (rt decision 10)
	int   blends;       //!< blend layer material records written (rt decision 10)
	float buildMs;      //!< accumulated build time
};

//! Statistics of the per frame dynamic BVH (rt decision 07, stage 3A).
struct SRTDynStats
{
	void Reset() { ZeroStruct(*this); }

	int   objs;        //!< objects emitted into the dynamic tree
	int   objsFound;   //!< objects that qualified before the budget was applied
	int   tris;        //!< triangles emitted
	int   records;     //!< records written into DYN_MESH
	int   mats;        //!< material records written into DYN_MATS (base + extras + blend layer)
	int   overflowed;  //!< objects merged into an overflow leaf because the top level hit its depth cap
	int   cached;      //!< per object BVHs held in the cache
	int   waterObjs;   //!< water surfaces emitted (ocean ring + water volumes, rt decision 09 9.4)
	int   waterTris;   //!< triangles of those water surfaces (a subset of tris)

	// character skinning (rt decision 07, stage 3B)
	int   skinnedChars;//!< skin attachments CPU skinned this frame
	int   skinnedVerts;//!< vertices those attachments skinned
	int   skinReused;  //!< attachments that kept an older pose because their skinning data was not ready

	float skinMs;      //!< CPU cost of the skinning alone; part of ms
	float ms;          //!< CPU cost of the last frame
};
	
typedef uint16 ObjectLayerIdType;
const ObjectLayerIdType kInvalidObjectLayerId = 0;
const ObjectLayerIdType kAllObjectLayersId = kInvalidObjectLayerId;

struct SVoxBrick
{
	enum ESubSetType
	{
		OPA3D,
		COLOR,
		NORML,
		RTRIS,
		MAX_NUM,
	};

	ColorB* pData[MAX_NUM] = { 0 };
};

template<class T>
class PodArrayRT : public PodArray<T>
{
public:
	CryReadModifyLock m_Lock;
	int               m_writeOffsetReady = 0;
	int               m_textureId = 0;
	int               m_writeOffset = 0;
};

template<class Key, class T>
class CLockedMap : public std::map<Key, T>
{
public:
	CryReadModifyLock m_Lock;
};

struct SObjInfo
{
	SObjInfo() { ZeroStruct(*this); }

	static int32 Compare(const void* v1, const void* v2)
	{
		SObjInfo* p[2] = { (SObjInfo*)v1, (SObjInfo*)v2 };

		if (p[0]->maxViewDist < p[1]->maxViewDist)
			return 1;
		if (p[0]->maxViewDist > p[1]->maxViewDist)
			return -1;

		return 0;
	}

	Matrix34          matObjInv;
	Matrix34          matObj;
	float             objectScale;
	IMaterial*        pMat;
	CStatObj*         pStatObj;
	bool              bIndoor;
	bool              bVegetation;
	ObjectLayerIdType objectLayerId;
	float             maxViewDist;
};

struct SVoxSegmentFileHeader
{
	Vec4_tpl<byte> cropTexSize; // and objLayersNum in w
	Vec4_tpl<byte> cropBoxMin;  // and subSetsNum in w
	Vec4_tpl<byte> dummy;
};

// SSuperMesh index type
	#if CRY_PLATFORM_WINDOWS
typedef uint32 SMINDEX;
	#else
typedef uint16 SMINDEX;
	#endif

struct SRayHitTriangleIndexed
{
	SRayHitTriangleIndexed() { ZeroStruct(*this); arrVertId[0] = arrVertId[1] = arrVertId[2] = (SMINDEX) ~0; }
	Vec3              vFaceNorm;
	uint              globalId;
	uint8             triArea;
	uint8             opacity;
	uint8             hitObjectType;
	SMINDEX           arrVertId[3];
	uint16            materialID;
	ObjectLayerIdType objectLayerId;
};

struct SRayHitVertex
{
	Vec3   v;
	Vec2   t;
	ColorB c;
	Vec3   n;
};

struct SSuperMesh
{
	SSuperMesh();
	~SSuperMesh();

	struct SSvoMatInfo
	{
		SSvoMatInfo(){ ZeroStruct(*this); }
		IMaterial* pMat;
		ColorB*    pTexRgb;
		int        texSlotId;
		uint16     textureWidth;
		uint16     textureHeight;
		inline bool operator==(const SSvoMatInfo& other) const { return pMat == other.pMat; }
	};

	static const int hashDim = 8;
	void AddSuperTriangle(SRayHitTriangle& htIn, PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim], ObjectLayerIdType nObjLayerId);
	void AddSuperMesh(SSuperMesh& smIn, float vertexOffset);
	void Clear(PodArray<SMINDEX>* parrVertHash);

	PodArrayRT<SRayHitTriangleIndexed>* m_pTrisInArea;
	PodArrayRT<Vec3>*                   m_pFaceNormals;
	PodArrayRT<SRayHitVertex>*          m_pVertInArea;
	PodArrayRT<SSvoMatInfo>*            m_pMatsInArea;
	AABB                                m_boxTris;

protected:
	int FindVertex(const Vec3& rPos, const Vec2 rTC, const Vec3& rNor, PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim], PodArrayRT<SRayHitVertex>& vertsInArea);
	int AddVertex(const SRayHitVertex& rVert, PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim], PodArrayRT<SRayHitVertex>& vertsInArea);
	bool m_bExternalData;
};

struct SBuildVoxelsParams
{
	int                  X0;
	int                  X1;
	PodArray<int>*       pNodeTrisXYZ;
	PodArray<CVisArea*>* arrPortals;
	PodArray<int>*       pTrisInt;
};

class CVoxelSegment;

class CVoxStreamEngine
{
public:

	struct SVoxStreamItem
	{
		CVoxelSegment* pObj;
		uint           requestFrameId;
	};

	class CVoxStreamEngineThread final : public IThread
	{
	public:
		CVoxStreamEngineThread(CVoxStreamEngine* pStreamingEngine);
		virtual void ThreadEntry();
		void SignalStopWork();

	private:
		CVoxStreamEngine* m_pStreamingEngine;
		bool m_bRun;
	};

	CVoxStreamEngine();
	~CVoxStreamEngine();

	void DecompressVoxStreamItem(const SVoxStreamItem item);
	void ProcessSyncCallBacks();
	bool StartRead(CVoxelSegment* pObj, int64 fileOffset, int bytesToRead);

public:
	BoundMPMC<SVoxStreamItem> m_arrForFileRead;
	BoundMPMC<SVoxStreamItem> m_arrForSyncCallBack;
	CrySemaphore m_fileReadSemaphore;

private:
	std::vector<CVoxStreamEngineThread*> m_workerThreads;	
};

class CVoxelSegment : public Cry3DEngineBase, public SSuperMesh, public IStreamCallback
{
public:

	CVoxelSegment(class CSvoNode* pNode, bool bDumpToDiskInUse = false, EFileStreamingStatus eStreamingStatus = ecss_Ready, bool bDroppedOnDisk = false);
	~CVoxelSegment();
	static int   GetSubSetsNum();
	bool         CheckUpdateBrickRenderData(bool bJustCheck);
	bool         LoadVoxels(byte* pData, int size);
	void         SaveVoxels(PodArray<byte>& arrData);
	bool         StartStreaming(CVoxStreamEngine* pVoxStreamEngine);
	bool         UpdateBrickRenderData();
	ColorF       GetBilinearAt(float iniX, float iniY, const ColorB* pImg, int dimW, int dimH, float multiplier);
	ColorF       GetColorF_255(int x, int y, const ColorB* pImg, int imgSizeW, int imgSizeH);
	ColorF       ProcessMaterial(const SRayHitTriangleIndexed& tr, const Vec3& voxBox);
	const AABB&  GetBoxOS()   { return m_boxOS; }
	float        GetBoxSize() { return (m_boxOS.max.z - m_boxOS.min.z); }
	int32        GetID()      { return m_segmentID; }
	static AABB  GetChildBBox(const AABB& parentBox, int childId);
	static int   GetBrickPoolUsageLoadedMB();
	static int   GetBrickPoolUsageMB();
	static int32 ComparemLastVisFrameID(const void* v1, const void* v2);
	static void  CheckAllocateBrick(ColorB*& pPtr, int elems, bool bClean = false);
	static void  CheckAllocateTexturePool();
	static void  FreeBrick(ColorB*& pPtr);
	static void  MakeFolderName(char szFolder[256], bool bCreateDirectory = false);
	static void  SetVoxCamera(const CCamera& newCam);
	static void  UpdateObjectLayersInfo();
	static void  ErrorTerminate(const char* format, ...);
	Vec3i        GetDxtDim();
	void         AddTriangle(const SRayHitTriangleIndexed& ht, int trId, PodArray<int>*& rpNodeTrisXYZ, PodArrayRT<SRayHitVertex>* pVertInArea);
	//! pDeferredOut, when given, collects the textures whose low resolution system copy is not ready
	//! yet: the slot is emitted as 0 (no texture) and the copy is requested from the prefetch worker
	//! instead of being loaded and decompressed here (report 06d fix 3a).
	static int   CheckStoreTextureInPool(SShaderItem* pShItem, EEfResTextures texSlot, uint16& nTexW, uint16& nTexH, PodArray<int>& arrTexSlicesOut, EEfResTextures eEncodeAs = EFTT_UNKNOWN, EEfResTextures eSmoothnessSlot = EFTT_SMOOTHNESS, PodArray<ITexture*>* pDeferredOut = nullptr);

	//! Low resolution texture copy prefetch (report 06d fix 3a). One dedicated worker owns the file
	//! reads and the BC decompression; the voxelization jobs only ask and carry on.
	static void  RTStartTexPrefetch();
	static void  RTStopTexPrefetch();
	static bool  RTIsTexCopyReady(ITexture* pTex);
	static bool  RTRequestTexCopy(ITexture* pTex);   //!< true when it is already there
	static void  RTGetTexPrefetchStats(int& done, int& pending);
	ColorB*      ApplyHighPass(uint16& nTexW, uint16& nTexH, const ColorB* pTexRgbOr);
	void         ComputeDistancesFast_MinDistToSurf(ColorB* pTex3dOptRGBA, ColorB* pTex3dOptNorm, ColorB* pTex3dOptOpac, int threadId);
	void         CropVoxTexture(int threadId, bool bCompSurfDist);
	void         DebugDrawVoxels();
	void         FindTrianglesForVoxelization(PodArray<int>*& rpNodeTrisXYZ);
	static bool  CheckCollectObjectsForVoxelization(const AABB& nodeBox, PodArray<SObjInfo>* parrObjects, bool& bThisIsAreaParent, bool& bThisIsLowLodNode, bool bAllowStartStreaming);
	void         FreeAllBrickData();
	void         FreeBrickLayers();
	void         FreeRenderData();
	void         PropagateDirtyFlag();
	void         ReleaseAtlasBlock();
	void         RenderMesh(PodArray<SVF_P3F_C4B_T2F>& arrVertsOut);
	void         SetBoxOS(const AABB& box) { m_boxOS = box; }
	void         SetID(int32 nID)          { m_segmentID = nID; }
	void         BuildStaticBVH();
	void         ReleaseRTChunk();
	void         FillRTMaterialRecord(const SRayHitTriangleIndexed& tr, SRTMatRecordSet& out, PodArray<ITexture*>* pDeferredOut = nullptr);
	static void  RTFillMaterialRecord(IMaterial* pMat, bool bTerrain, PodArray<int>& arrTexSlicesOut, SRTMatRecordSet& out, PodArray<ITexture*>* pDeferredOut = nullptr);
	static bool  BuildStaticBVHRecords(const PodArray<SRTBuildTri>& arrTris, const Vec4& qb, int recordBase, PodArray<Vec4>& arrOut, PodArray<int>* pRelocFloats, SRTBuildStats& stats);
	static void  RunRTSelfTest();

	//! Dynamic mesh BVH (rt decision 07, stage 3A) - main thread, once per frame, from CSvoEnv::Render.
	static void  RTUpdateDynamic();
	static void  RTClearDynamicCache();
	void         StreamAsyncOnComplete(IReadStream* pStream, unsigned nError) override;
	void         StreamOnComplete(IReadStream* pStream, unsigned nError) override;
	void         UnloadStreamableData();
	void         UpdateMeshRenderData();
	void         UpdateNodeRenderData();
	void         UpdateVoxRenderData();
	void         VoxelizeMeshes(int threadId, bool bMT = false);
	static void  CheckAllocateSubSets(SVoxBrick& voxData, int elemsNum, bool bClean = false);
	void         CombineLayers();
	void         BuildVoxels(SBuildVoxelsParams params);
	int          CompressToDxt(ColorB* pImgSource, byte*& pDxtOut, int threadId);
	static void  SaveCompTexture(const void* data, size_t size, void* userData);

	AABB                                                 m_boxClipped;
	AABB                                                 m_boxOS;
	bool                                                 m_bStatLightsChanged;
	byte                                                 m_bChildOffsetsDirty;
	class CSvoNode*                                      m_pNode;
	SVoxBrick                                            m_voxData;
	CVoxelSegment*                                       m_pParentCloud;
	EFileStreamingStatus                                 m_eStreamingStatus;
	float                                                m_maxAlphaInBrick;
	int                                                  m_allocatedAtlasOffset;
	int                                                  m_fileStreamSize;
	int32                                                m_arrChildOffset[8];
	int32                                                m_segmentID;
	int64                                                m_fileStreamOffset64;
	PodArray<int>                                        m_nodeTrisAllMerged;

	static CCamera                                       m_voxCam;
	static class CBlockPacker3D*                         m_pBlockPacker;
	static CryCriticalSection                            m_csLockBrick;
	static int                                           m_addPolygonToSceneCounter;
	static int                                           m_checkReadyCounter;
	static int                                           m_segmentsCounter;
	static int                                           m_postponedCounter;
	static int                                           m_currPassMainFrameID;
	static int                                           m_maxBrickUpdates;
	static int                                           m_nextSegmentId;
	static int                                           m_poolUsageBytes;
	static int                                           m_poolUsageItems;
	static int                                           m_svoDataPoolsCounter;
	static int                                           m_voxTrisCounter;
	static int                                           m_voxTexPoolDimXY;
	static int                                           m_voxTexPoolDimZ;
	static int32                                         m_streamingTasksInProgress;
	static bool                                          m_bUpdateBrickRenderDataPostponed;
	static int32                                         m_updatesInProgressBri;
	static int32                                         m_updatesInProgressTex;
	static PodArray<CVoxelSegment*>                      m_arrLoadedSegments;
	static SRenderingPassInfo*                           m_pCurrPassInfo;
	static CLockedMap<ITexture*, _smart_ptr<ITexture>>   m_arrLockedTextures;
	static CLockedMap<IMaterial*, _smart_ptr<IMaterial>> m_arrLockedMaterials;
	static std::map<CStatObj*, float>                    m_cgfTimeStats;
	static CryReadModifyLock                             m_cgfTimeStatsLock;
	static bool                                          m_bExportMode;
	static bool                                          m_bExportAbortRequested;
	static int                                           m_exportVisitedAreasCounter;
	static int                                           m_exportVisitedNodesCounter;
	static PodArray<C3DEngine::SLayerActivityInfo>       m_arrObjectLayersInfo;
	static uint                                          m_arrObjectLayersInfoVersion;
	struct SBlockMinMax*                                 m_pBlockInfo;
	SVF_P3F_C4B_T2F                                      m_vertForGS;
	uint                                                 m_lastRendFrameId;
	uint                                                 m_lastTexUpdateFrameId;
	uint16                                               m_solidVoxelsNum;
	uint8                                                m_dwChildTrisTest;
	Vec3i                                                m_vCropBoxMin;
	Vec3                                                 m_vSegOrigin;
	Vec3i                                                m_vCropTexSize;
	Vec3i                                                m_vStaticGeomCheckSumm;
	Vec3i                                                m_vStatLightsCheckSumm;
	CryReadModifyLock                                    m_superMeshLock;
	std::map<ObjectLayerIdType, SVoxBrick>               m_objLayerMap;
	std::unique_ptr<PodArray<SObjInfo>>                  m_areaObjects;
	bool                                                 m_isAreaParent = false;
	bool                                                 m_isLowLodNode = false;

	// static BVH chunk owned by this cell (rt decision 02 2.4); record units, 0 = none
	int                                                  m_rtChunkStart = 0;
	int                                                  m_rtChunkCount = 0;
	int                                                  m_rtRootRecord = 0;
	PodArray<int>                                        m_rtTexSlices;  //!< 0 based atlas slices referenced by this cell
};

inline uint GetCurrPassMainFrameID() { return CVoxelSegment::m_currPassMainFrameID; }

	#pragma pack(pop)

#endif
