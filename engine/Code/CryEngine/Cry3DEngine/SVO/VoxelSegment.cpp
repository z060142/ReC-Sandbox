// Copyright 2014-2021 Crytek GmbH / Crytek Group. All rights reserved.

// -------------------------------------------------------------------------
//  File name:   VoxelSegment.cpp
//  Created:     2012 by Vladimir Kajalin.
//  Description: SVO brick implementation
// -------------------------------------------------------------------------
//  History:
//
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"

#if defined(FEATURE_SVO_GI)

	#include <CryCore/Platform/CryWindows.h>
	#include "VoxelSegment.h"
	#include "BlockPacker.h"
	#include "visareas.h"
	#include "brush.h"
	#include "SceneTree.h"
	#include <CryThreading/IJobManager_JobDelegator.h>
	#include <algorithm>
	#include <CryAnimation/ICryAnimation.h>
	#include <CryAnimation/IAttachment.h>
	#include "terrain.h"
	#include "terrain_water.h"
	#include "TimeOfDay.h"
	#include "WaterVolumeRenderNode.h"

	#pragma warning(push)
	#pragma warning(disable: 4244) //conversion' conversion from 'type1' to 'type2', possible loss of data

	#define SVO_OFFSET_MESH        0
	#define SVO_OFFSET_TERRAIN     -0.04f
	#define SVO_OFFSET_VISAREA     (Cry3DEngineBase::GetCVars()->e_svoMinNodeSize / (float)SVO_VOX_BRICK_MAX_SIZE)
	#define SVO_POOL_SIZE_MB       (12 * 1024)
	#define SVO_AREA_SCALE         200.f
	#define SVO_DIST_TO_SURF_RANGE 4
	#define SVO_NODES_POOL_DIM_XY  (SVO_NODE_BRICK_SIZE * SVO_ATLAS_DIM_MAX_XY)
	#define SVO_NODES_POOL_DIM_Z   (SVO_NODE_BRICK_SIZE * SVO_ATLAS_DIM_MAX_Z)
	#define SVO_MAX_TRIS_PER_VOXEL 512
	#define SVO_PACK_TO_16_BIT     true

CBlockPacker3D* CVoxelSegment::m_pBlockPacker = 0;
CCamera CVoxelSegment::m_voxCam;
extern CSvoEnv* gSvoEnv;
int CVoxelSegment::m_addPolygonToSceneCounter = 0;
int CVoxelSegment::m_checkReadyCounter = 0;
int CVoxelSegment::m_segmentsCounter = 0;
int CVoxelSegment::m_postponedCounter = 0;
int CVoxelSegment::m_currPassMainFrameID = 0;
int CVoxelSegment::m_maxBrickUpdates = 128;
int CVoxelSegment::m_nextSegmentId = 0;
int CVoxelSegment::m_poolUsageBytes = 0;
int CVoxelSegment::m_poolUsageItems = 0;
int CVoxelSegment::m_streamingTasksInProgress = 0;
int CVoxelSegment::m_svoDataPoolsCounter = 0;
bool CVoxelSegment::m_bUpdateBrickRenderDataPostponed = 0;
int CVoxelSegment::m_updatesInProgressBri = 0;
int CVoxelSegment::m_updatesInProgressTex = 0;
int CVoxelSegment::m_voxTrisCounter = 0;
int CVoxelSegment::m_voxTexPoolDimXY = 0;
int CVoxelSegment::m_voxTexPoolDimZ = 0;
bool CVoxelSegment::m_bExportMode = false;
int CVoxelSegment::m_exportVisitedAreasCounter = 0;
int CVoxelSegment::m_exportVisitedNodesCounter = 0;
bool CVoxelSegment::m_bExportAbortRequested = false;
PodArray<C3DEngine::SLayerActivityInfo> CVoxelSegment::m_arrObjectLayersInfo;
uint CVoxelSegment::m_arrObjectLayersInfoVersion = 0;

std::map<CStatObj*, float> CVoxelSegment::m_cgfTimeStats;
CryReadModifyLock CVoxelSegment::m_cgfTimeStatsLock;

CLockedMap<ITexture*, _smart_ptr<ITexture>> CVoxelSegment::m_arrLockedTextures;
CLockedMap<IMaterial*, _smart_ptr<IMaterial>> CVoxelSegment::m_arrLockedMaterials;

PodArray<CVoxelSegment*> CVoxelSegment::m_arrLoadedSegments;
SRenderingPassInfo* CVoxelSegment::m_pCurrPassInfo = 0;

DECLARE_JOB("VoxelSegmentFileDecompress", TDecompressVoxStreamItemJob, CVoxStreamEngine::DecompressVoxStreamItem);
DECLARE_JOB("VoxelSegmentBuildVoxels", TBuildVoxelsJob, CVoxelSegment::BuildVoxels);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Voxel Stream Engine Thread
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CVoxStreamEngine::CVoxStreamEngineThread::CVoxStreamEngineThread(CVoxStreamEngine* pStreamingEngine) : m_pStreamingEngine(pStreamingEngine)
	, m_bRun(true)
{}

void CVoxStreamEngine::CVoxStreamEngineThread::ThreadEntry()
{
	MEMSTAT_CONTEXT(EMemStatContextType::Other, "VoxStreamEngine");

	while (m_bRun)
	{
		m_pStreamingEngine->m_fileReadSemaphore.Acquire();

		if (!m_bRun)
		{
			break;
		}

		SVoxStreamItem pItem;
		if (m_pStreamingEngine->m_arrForFileRead.dequeue(pItem))
		{
			if (Cry3DEngineBase::GetCVars()->e_svoMaxStreamRequests > 4)
			{
				TDecompressVoxStreamItemJob job(pItem);
				job.SetClassInstance(m_pStreamingEngine);
				job.SetPriorityLevel(JobManager::eStreamPriority);
				job.Run();
			}
			else
			{
				m_pStreamingEngine->DecompressVoxStreamItem(pItem);
			}
		}
	}
}

void CVoxStreamEngine::CVoxStreamEngineThread::SignalStopWork()
{
	m_bRun = false;
	m_pStreamingEngine->m_fileReadSemaphore.Release();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Voxel Stream Engine
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CVoxStreamEngine::CVoxStreamEngine() : m_arrForFileRead(512)
	, m_arrForSyncCallBack(512)
	, m_fileReadSemaphore(512)
{
	// The RT path no longer serialises voxelization: the static BVH build runs entirely in a local
	// buffer and takes the pool lock only to reserve a chunk and memcpy the finished records.
	const int numThreads = gEnv->pConsole->GetCVar("e_svoTI_NumStreamingThreads")->GetIVal();

	for (int i = 0; i < numThreads; ++i)
	{
		CVoxStreamEngineThread* pStreamingThread = new CVoxStreamEngineThread(this);
		if (!gEnv->pThreadManager->SpawnThread(pStreamingThread, "VoxelStreamingWorker_%u", i))
		{
			CryFatalError("Error spawning \"VoxelStreamingWorker_%u\" thread.", i);
			delete pStreamingThread;
		}
		else
		{
			m_workerThreads.push_back(pStreamingThread);
		}
	}
}

CVoxStreamEngine::~CVoxStreamEngine()
{
	for (CVoxStreamEngineThread* pWorker : m_workerThreads)
	{
		pWorker->SignalStopWork();
	}

	for (CVoxStreamEngineThread* pWorker : m_workerThreads)
	{
		gEnv->pThreadManager->JoinThread(pWorker, eJM_Join);
		delete pWorker;
	}
	m_workerThreads.clear();

	ProcessSyncCallBacks();
}

void CVoxStreamEngine::DecompressVoxStreamItem(const SVoxStreamItem item)
{
	item.pObj->StreamAsyncOnComplete(0, 0);

#if defined(USE_CRY_ASSERT)
	const bool ret = m_arrForSyncCallBack.enqueue(item);
	CRY_ASSERT(ret, "CVoxStreamEngine::m_arrForSyncCallBack is not big enough.");
#else
	m_arrForSyncCallBack.enqueue(item);
#endif
}

void CVoxStreamEngine::ProcessSyncCallBacks()
{

	SVoxStreamItem item;
	while (m_arrForSyncCallBack.dequeue(item))
	{
		item.pObj->StreamOnComplete(0, 0);
	}
}

bool CVoxStreamEngine::StartRead(CVoxelSegment* pObj, int64 fileOffset, int bytesToRead)
{
	FUNCTION_PROFILER_3DENGINE;

	if (m_arrForFileRead.enqueue({ pObj, GetCurrPassMainFrameID() / 10 }))
	{
		m_fileReadSemaphore.Release();
		return true;
	}

	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Voxel Segment
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CryCriticalSection CVoxelSegment::m_csLockBrick;

CVoxelSegment::CVoxelSegment(class CSvoNode* pNode, bool bDumpToDiskInUse, EFileStreamingStatus eStreamingStatus, bool bDroppedOnDisk)
{
	m_boxTris.Reset();
	m_boxOS.Reset();
	m_boxClipped.Reset();
	m_dwChildTrisTest = 0;
	m_eStreamingStatus = eStreamingStatus;
	m_maxAlphaInBrick = 0;
	m_allocatedAtlasOffset = -2;
	m_lastTexUpdateFrameId = m_lastRendFrameId = 0;
	m_fileStreamOffset64 = m_fileStreamSize = -1;
	m_bChildOffsetsDirty = 0;
	m_segmentsCounter++;
	m_segmentID = -1;
	m_bStatLightsChanged = 0;
	m_solidVoxelsNum = 0;
	m_pBlockInfo = 0;
	m_pNode = pNode;
	m_pParentCloud = 0;
	m_vCropBoxMin.zero();
	m_vCropTexSize.zero();
	m_vSegOrigin = Vec3(0, 0, 0);
	m_vStaticGeomCheckSumm.zero();
	m_vStatLightsCheckSumm.zero();
	ZeroStruct(m_arrChildOffset);
}

int32 CVoxelSegment::ComparemLastVisFrameID(const void* v1, const void* v2)
{
	CVoxelSegment* p[2] = { *(CVoxelSegment**)v1, *(CVoxelSegment**)v2 };

	uint arrNodeSize[2] =
	{
		uint((p[0]->m_boxOS.max.x - p[0]->m_boxOS.min.x) * 4),
		uint((p[1]->m_boxOS.max.x - p[1]->m_boxOS.min.x) * 4)
	};

	if ((p[0]->m_lastRendFrameId + arrNodeSize[0]) > (p[1]->m_lastRendFrameId + arrNodeSize[1]))
		return 1;
	if ((p[0]->m_lastRendFrameId + arrNodeSize[0]) < (p[1]->m_lastRendFrameId + arrNodeSize[1]))
		return -1;

	if (p[0] > p[1])
		return 1;
	if (p[0] < p[1])
		return -1;

	return 0;
}

int CVoxelSegment::GetBrickPoolUsageMB()
{
	return gSvoEnv->m_brickSubSetAllocator.GetCapacity() * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * sizeof(ColorB) / 1024 / 1024;
}

int CVoxelSegment::GetBrickPoolUsageLoadedMB()
{
	return gSvoEnv->m_brickSubSetAllocator.GetCount() * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * sizeof(ColorB) / 1024 / 1024;
}

void CVoxelSegment::CheckAllocateBrick(ColorB*& pPtr, int elemsNum, bool bClean)
{
	if (pPtr)
	{
		if (bClean)
			memset(pPtr, 0, SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * sizeof(ColorB));

		return;
	}

	{
		AUTO_LOCK(m_csLockBrick);

		pPtr = (ColorB*)gSvoEnv->m_brickSubSetAllocator.GetNewElement();
	}

	memset(pPtr, 0, SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * sizeof(ColorB));
}

void CVoxelSegment::FreeBrick(ColorB*& pPtr)
{
	if (pPtr)
	{
		AUTO_LOCK(m_csLockBrick);

		gSvoEnv->m_brickSubSetAllocator.ReleaseElement((SBrickSubSet*)pPtr);

		pPtr = 0;
	}
}

int GetBrickDataSize(ColorB*& pPtr)
{
	return pPtr ? SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * sizeof(ColorB) : 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CVoxelSegment
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CVoxelSegment::~CVoxelSegment()
{
	m_arrLoadedSegments.Delete(this);
	m_segmentsCounter--;

	if (gSvoEnv)
		ReleaseRTChunk();

	FreeAllBrickData();
	FreeRenderData();
}

void CVoxelSegment::RenderMesh(PodArray<SVF_P3F_C4B_T2F>& arrVertsOut)
{
	m_lastRendFrameId = GetCurrPassMainFrameID();

	if (m_eStreamingStatus == ecss_Ready)
	{
		if (!CheckUpdateBrickRenderData(true))
			return;

		// accumulate DVR proxy mesh
		if ((GetCVars()->e_svoDVR == 10 && m_maxAlphaInBrick > 0.05) || GetCVars()->e_svoTI_Active)
			if (GetBoxSize() <= Cry3DEngineBase::GetCVars()->e_svoMaxNodeSize)
			{
				arrVertsOut.Add(m_vertForGS);
				m_addPolygonToSceneCounter++;
			}

		DebugDrawVoxels();
	}
}

bool CVoxelSegment::LoadVoxels(byte* pDataRead, int dataSize)
{
	MEMSTAT_CONTEXT(EMemStatContextType::Other, "LoadVoxels");

	byte* pData = (byte*)pDataRead;

	SVoxSegmentFileHeader* pHeader = (SVoxSegmentFileHeader*)pData;

	m_boxOS = m_pNode->m_nodeBox;
	m_boxOS.min -= m_vSegOrigin;
	m_boxOS.max -= m_vSegOrigin;

	pData += sizeof(SVoxSegmentFileHeader);

	FreeAllBrickData();

	m_vCropTexSize.x = pHeader->cropTexSize.x;
	m_vCropTexSize.y = pHeader->cropTexSize.y;
	m_vCropTexSize.z = pHeader->cropTexSize.z;

	m_vCropBoxMin.x = pHeader->cropBoxMin.x;
	m_vCropBoxMin.y = pHeader->cropBoxMin.y;
	m_vCropBoxMin.z = pHeader->cropBoxMin.z;

	int texDataSize = (m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z * sizeof(ColorB));

	if (SVO_PACK_TO_16_BIT)
	{
		texDataSize /= 2;
	}

	if (gSvoEnv->m_voxTexFormat == eTF_BC3)
	{
		Vec3i vDxtDim = GetDxtDim();
		texDataSize = (vDxtDim.x * vDxtDim.y * vDxtDim.z * sizeof(ColorB)) / 4;
	}

	if (int dataSize = texDataSize)
	{
		assert(m_objLayerMap.empty());

		// for every layer
		for (int s = 0; s < pHeader->cropTexSize.w; s++)
		{
			uint32 nLayerId = *((uint32*)pData);
			pData += sizeof(uint32);

			SVoxBrick voxData;

			assert(GetSubSetsNum() == pHeader->cropBoxMin.w); // number of subset in file must be compatible with current GI settings

			CheckAllocateSubSets(voxData, dataSize / sizeof(ColorB));

			// for every subset
			for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
			{
				if (voxData.pData[s])
				{
					if (SVO_PACK_TO_16_BIT)
					{
						byte* pDataOut = (byte*)voxData.pData[s];

						for (int i = 0; i < texDataSize; i++)
						{
							(*pDataOut) = (((*pData) >> 0) & 15) << 4;
							(*pDataOut) = SATURATEB(int(powf(float((*pDataOut)) / 255.f, 2.f) * 255.f));
							pDataOut++;

							(*pDataOut) = (((*pData) >> 4) & 15) << 4;
							(*pDataOut) = SATURATEB(int(powf(float((*pDataOut)) / 255.f, 2.f) * 255.f));
							pDataOut++;

							pData++;
						}
					}
					else
					{
						memcpy(voxData.pData[s], pData, dataSize);

						pData += dataSize;
					}

					if (gSvoEnv->m_voxTexFormat == eTF_R8G8B8A8)
					{
						uint8 nMaxAlphaInBrick = 0;

						// swap r and b
						int pixNum = m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z;

						for (int p = 0; p < pixNum; p++)
						{
							if (voxData.pData[s])
							{
								ColorB& col = voxData.pData[s][p];
								std::swap(col.r, col.b);

								if (s == 0)
								{
									nMaxAlphaInBrick = max(nMaxAlphaInBrick, col.a);
								}
							}
						}

						m_maxAlphaInBrick = 1.f / 255.f * (float)nMaxAlphaInBrick;
					}
				}
			}

			m_objLayerMap[nLayerId] = voxData;
		}

		CombineLayers();
	}

	assert((pData - pDataRead) == dataSize);

	return true;
}

Vec3i CVoxelSegment::GetDxtDim()
{
	if (m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z == 0)
		return Vec3i(0, 0, 0);

	Vec3i vDxtSize = m_vCropTexSize;

	// adjust X and Y sizes for DXT
	for (int n = 0; n < 2; n++)
	{
		while (1)
		{
			if (((vDxtSize[n]) % 4) == 0)
				break;

			vDxtSize[n]++;
		}
	}

	return vDxtSize;
}

void CVoxelSegment::FreeAllBrickData()
{
	FreeBrickLayers();

	for (int s = 0; s < SVoxBrick::RTRIS; s++)
	{
		FreeBrick(m_voxData.pData[s]);
	}

	m_nodeTrisAllMerged.Reset();
	m_boxTris.Reset();

	if (m_pTrisInArea && (!m_pParentCloud || !m_pParentCloud->m_pTrisInArea))
	{
		m_pTrisInArea->Reset();
		m_pVertInArea->Reset();
		m_pMatsInArea->Reset();
	}
}

void CVoxelSegment::FreeBrickLayers()
{
	for (auto& it : m_objLayerMap)
	{
		SVoxBrick& voxData = it.second;

		for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
		{
			if (voxData.pData[s])
			{
				if (voxData.pData[s] != m_voxData.pData[s])
				{
					FreeBrick(voxData.pData[s]);
				}
				else
				{
					voxData.pData[s] = nullptr;
				}
			}
		}
	}

	m_objLayerMap.clear();
}

void CVoxelSegment::FreeRenderData()
{
	FreeAllBrickData();

	ReleaseAtlasBlock();

	m_eStreamingStatus = ecss_NotLoaded;
}

void CVoxelSegment::MakeFolderName(char szFolder[256], bool bCreateDirectory)
{
	char szLevelFolder[MAX_PATH_LENGTH];
	cry_strcpy(szLevelFolder, Cry3DEngineBase::Get3DEngine()->GetLevelFolder());
	cry_sprintf(szFolder, 256, "%s", szLevelFolder);
}

void CVoxelSegment::StreamAsyncOnComplete(IReadStream* pStream, unsigned nError)
{
	FUNCTION_PROFILER_3DENGINE;

	if (m_fileStreamSize < 0)
	{
		VoxelizeMeshes(0);
		return;
	}

	if (pStream->IsError())
	{
		m_eStreamingStatus = ecss_Ready;
		return;
	}

	byte* pDataRead = (byte*)pStream->GetBuffer();
	uint bytesRead = pStream->GetBytesRead();

	byte* pData = (byte*)pDataRead;

	int compressedSize = *(int*)pData;
	pData += sizeof(int);

	if (compressedSize)
	{
		if ((compressedSize < 0) || (compressedSize > SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * 4 * 2))
		{
			ErrorTerminate("%s: Data corruption detected", __FUNCTION__);
		}

		CMemoryBlock mbZip;
		mbZip.Allocate(compressedSize);
		memcpy(mbZip.GetData(), pData, mbZip.GetSize());

		pData += mbZip.GetSize();
		while ((pData - pDataRead) & 3)
		{
			pData++;
		}

		CMemoryBlock* pUnpacked = CMemoryBlock::DecompressFromMemBlock(&mbZip, GetSystem());

		if (!pUnpacked || !pUnpacked->GetSize())
		{
			ErrorTerminate("%s: DecompressFromMemBlock error", __FUNCTION__);
		}
		else
		{
			LoadVoxels((byte*)pUnpacked->GetData(), pUnpacked->GetSize());
		}

		delete pUnpacked;
	}

	// load child info if available
	if (ptrdiff_t(pData - pDataRead + (sizeof(uint32) * 2 * 8)) <= ptrdiff_t(bytesRead))
	{
		bool bChildsExist = false;

		for (int childId = 0; childId < 8; childId++)
		{
			if (((uint32*)pData)[childId * 2 + 1])
			{
				bChildsExist = true;
				break;
			}
		}

		if (bChildsExist)
		{
			m_pNode->m_pChildFileOffsets = new std::pair<uint32, uint32>[8];

			for (int childId = 0; childId < 8; childId++)
			{
				m_pNode->m_pChildFileOffsets[childId].first = *(uint32*)pData; // file offset
				pData += sizeof(uint32);

				m_pNode->m_pChildFileOffsets[childId].second = *(uint32*)pData; // data size
				pData += sizeof(uint32);

				if (m_pNode->m_pChildFileOffsets[childId].second)
				{
					m_dwChildTrisTest |= (1 << childId);
				}
			}
		}
		else
		{
			pData += sizeof(uint32) * 2 * 8;
		}
	}

	assert(ptrdiff_t(pData - pDataRead) <= ptrdiff_t(bytesRead));

	if (ptrdiff_t(pData - pDataRead) > ptrdiff_t(bytesRead))
	{
		ErrorTerminate("%s: Data size error, nBytesRead = %d, (pData - pDataRead) = %td", __FUNCTION__, bytesRead, ptrdiff_t(pData - pDataRead));
	}
}

void CVoxelSegment::StreamOnComplete(IReadStream* pStream, unsigned nError)
{
	FUNCTION_PROFILER_3DENGINE;

	m_eStreamingStatus = ecss_Ready;

	m_streamingTasksInProgress--;

	if (m_arrLoadedSegments.Find(this) < 0)
		m_arrLoadedSegments.Add(this);

	if (m_vCropTexSize.GetVolume() == 0 || !m_voxData.pData[SVoxBrick::OPA3D])
	{
		if (GetBoxSize() <= GetCVars()->e_svoMaxNodeSize && m_pParentCloud && !Cry3DEngineBase::GetCVars()->e_svoTI_Troposphere_Subdivide)
		{
			CSvoNode** ppChilds = m_pParentCloud->m_pNode->m_ppChilds;

			for (int childId = 0; childId < 8; childId++)
			{
				if (ppChilds[childId] == m_pNode)
				{
					m_pParentCloud->m_pNode->m_arrChildNotNeeded[childId] = true;
				}
			}
		}
	}
}

void CVoxelSegment::UnloadStreamableData()
{
	FreeRenderData();

	m_arrLoadedSegments.Delete(this);

	m_eStreamingStatus = ecss_NotLoaded;
}

bool CVoxelSegment::StartStreaming(CVoxStreamEngine* pVoxStreamEngine)
{
	if (m_eStreamingStatus != ecss_NotLoaded)
	{
		return true;
	}

	if (CSvoNode::IsStreamingActive() && m_pNode->m_nodeBox.GetSize().x <= SVO_ROOTLESS_PARENT_SIZE / 2)
	{
		// stream voxels from disk

		assert(m_fileStreamOffset64 >= 0 && m_fileStreamSize);

		StreamReadParams params;
		params.nOffset = m_fileStreamOffset64;
		params.nSize = m_fileStreamSize;
		params.ePriority = estpAboveNormal;

		CSvoNode* pAreaNode = m_pNode;

		while (pAreaNode->m_nodeBox.GetSize().x < SVO_ROOTLESS_PARENT_SIZE / 2)
		{
			pAreaNode = pAreaNode->m_pParent;
		}

		char szAreaFileName[256];
		pAreaNode->MakeNodeFilePath(szAreaFileName);

		GetSystem()->GetStreamEngine()->StartRead(eStreamTaskTypeGeometry, szAreaFileName, this, &params);
	}
	else
	{
		// generate voxels on CPU
		if (!pVoxStreamEngine->StartRead(this, m_fileStreamOffset64, m_fileStreamSize))
		{
			return false;
		}
	}

	m_eStreamingStatus = ecss_InProgress;

	m_streamingTasksInProgress++;

	return true;
}

void CVoxelSegment::CropVoxTexture(int threadId, bool bCompSurfDist)
{
	m_vCropTexSize.Set(0, 0, 0);

	if (!GetBrickDataSize(m_voxData.pData[SVoxBrick::OPA3D]))
	{
		return;
	}

	Vec3i vMin(SVO_VOX_BRICK_MAX_SIZE, SVO_VOX_BRICK_MAX_SIZE, SVO_VOX_BRICK_MAX_SIZE);
	Vec3i vMax(0, 0, 0);

	for (auto& it : m_objLayerMap)
	{
		SVoxBrick& voxData = it.second;

		for (int x = 0; x < SVO_VOX_BRICK_MAX_SIZE; x++)
			for (int y = 0; y < SVO_VOX_BRICK_MAX_SIZE; y++)
				for (int z = 0; z < SVO_VOX_BRICK_MAX_SIZE; z++)
				{
					const int id = z * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE + y * SVO_VOX_BRICK_MAX_SIZE + x;

					ColorB& opaOut = voxData.pData[SVoxBrick::OPA3D][id];

					if ((opaOut.r || opaOut.g || opaOut.b || opaOut.a) || Cry3DEngineBase::GetCVars()->e_svoTI_Troposphere_Subdivide)
					{
						vMin.CheckMin(Vec3i(x, y, z));
						vMax.CheckMax(Vec3i(x + 1, y + 1, z + 1));
					}

					if (voxData.pData[SVoxBrick::NORML] && (opaOut.r || opaOut.g || opaOut.b))
					{
						voxData.pData[SVoxBrick::NORML][id].a = 255;
					}
				}
	}

	m_vCropTexSize = vMax - vMin;

	for (int d = 0; d < 3; d++)
	{
		if (vMax[d] < SVO_VOX_BRICK_MAX_SIZE)
			vMax[d]++;

		if (vMin[d] > 0)
			vMin[d]--;
	}

	m_vCropTexSize = vMax - vMin;

	if (m_vCropTexSize.x > 0 && m_vCropTexSize.y > 0 && m_vCropTexSize.z > 0)
	{
		for (auto& it : m_objLayerMap)
		{
			SVoxBrick& voxDataIn = it.second;

			SVoxBrick voxTemp;
			for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
			{
				if (voxDataIn.pData[s])
				{
					voxTemp.pData[s] = new ColorB[m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z];
					memset(voxTemp.pData[s], 0, m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z * sizeof(ColorB));
				}
			}

			// copy cropped data into temp
			for (int x = 0; x < m_vCropTexSize.x; x++)
			{
				for (int y = 0; y < m_vCropTexSize.y; y++)
				{
					for (int z = 0; z < m_vCropTexSize.z; z++)
					{
						int x_in = x + vMin.x;
						int y_in = y + vMin.y;
						int z_in = z + vMin.z;

						int id_in = z_in * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE + y_in * SVO_VOX_BRICK_MAX_SIZE + x_in;

						ColorB& opaIn = voxDataIn.pData[SVoxBrick::OPA3D][id_in];

						if (opaIn.r || opaIn.g || opaIn.b)
						{
							int id_out = z * m_vCropTexSize.x * m_vCropTexSize.y + y * m_vCropTexSize.x + x;

							for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
							{
								if (voxDataIn.pData[s])
								{
									voxTemp.pData[s][id_out] = voxDataIn.pData[s][id_in];
								}
							}
						}
					}
				}
			}

			// copy back from temp
			for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
			{
				if (voxDataIn.pData[s])
				{
					memcpy(voxDataIn.pData[s], voxTemp.pData[s], sizeof(ColorB) * m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z);
					SAFE_DELETE_ARRAY(voxTemp.pData[s]);
				}
			}
		}

		m_vCropBoxMin = vMin;
	}
	else
	{
		FreeAllBrickData();

		m_vCropTexSize.zero();
		m_vCropBoxMin.zero();
	}
}

void CVoxelSegment::ComputeDistancesFast_MinDistToSurf(ColorB* pTex3dOptRGBA, ColorB* pTex3dOptNorm, ColorB* pTex3dOptOpac, int threadId)
{
	for (int Z = 0; Z < m_vCropTexSize.z; Z++)
	{
		for (int Y = 0; Y < m_vCropTexSize.y; Y++)
		{
			for (int X = 0; X < m_vCropTexSize.x; X++)
			{
				int idOut_ = Z * m_vCropTexSize.x * m_vCropTexSize.y + Y * m_vCropTexSize.x + X;

				ColorB& colOut_ = pTex3dOptRGBA[idOut_];
				ColorB& norOut_ = pTex3dOptNorm[idOut_];

				if (colOut_.a != 255) // if empty
				{
					norOut_.a = 0;

					Vec3 v1((float)X, (float)Y, (float)Z);

					float minDistSq = 255 * 255 * 2;

					int _nNearestId = 0;

					int X1 = CLAMP(X - SVO_DIST_TO_SURF_RANGE, 0, m_vCropTexSize.x - 1);
					int X2 = CLAMP(X + SVO_DIST_TO_SURF_RANGE, 0, m_vCropTexSize.x - 1);
					int Y1 = CLAMP(Y - SVO_DIST_TO_SURF_RANGE, 0, m_vCropTexSize.y - 1);
					int Y2 = CLAMP(Y + SVO_DIST_TO_SURF_RANGE, 0, m_vCropTexSize.y - 1);
					int Z1 = CLAMP(Z - SVO_DIST_TO_SURF_RANGE, 0, m_vCropTexSize.z - 1);
					int Z2 = CLAMP(Z + SVO_DIST_TO_SURF_RANGE, 0, m_vCropTexSize.z - 1);

					// find nearest voxel
					for (int _x = X1; _x <= X2; _x++)
						for (int _y = Y1; _y <= Y2; _y++)
							for (int _z = Z1; _z <= Z2; _z++)
							{
								int _idOut = (_z) * m_vCropTexSize.x * m_vCropTexSize.y + (_y) * m_vCropTexSize.x + (_x);

								const ColorB& colOut = pTex3dOptRGBA[_idOut];

								if (colOut.a == 255)
								{
									Vec3 v0((float)_x, (float)_y, (float)_z);
									float distSq = v1.GetSquaredDistance(v0);

									if (distSq < minDistSq)
									{
										minDistSq = distSq;
										_nNearestId = _idOut;
									}
								}
							}

					minDistSq = sqrt(minDistSq);

					if (minDistSq <= 4)
						norOut_.a = SATURATEB(255 - (int)minDistSq);
				}
			}
		}
	}
}

bool CVoxelSegment::UpdateBrickRenderData()
{
	FUNCTION_PROFILER_3DENGINE;

	ReleaseAtlasBlock();

	if (!m_pBlockPacker)
		m_pBlockPacker = new CBlockPacker3D(SVO_ATLAS_DIM_MAX_XY, SVO_ATLAS_DIM_MAX_XY, SVO_ATLAS_DIM_MAX_Z, true);

	int blockW = max(1, (m_vCropTexSize.x + SVO_BRICK_ALLOC_CHUNK_SIZE - 1) / SVO_BRICK_ALLOC_CHUNK_SIZE);
	int blockH = max(1, (m_vCropTexSize.y + SVO_BRICK_ALLOC_CHUNK_SIZE - 1) / SVO_BRICK_ALLOC_CHUNK_SIZE);
	int blockD = max(1, (m_vCropTexSize.z + SVO_BRICK_ALLOC_CHUNK_SIZE - 1) / SVO_BRICK_ALLOC_CHUNK_SIZE);

	const float minNodeSize = Cry3DEngineBase::GetCVars()->e_svoMinNodeSize;

	const int dataSizeStatsScale = 4;

	#ifndef _RELEASE
	{
		// count stats
		m_poolUsageItems = 0;
		m_poolUsageBytes = 0;

		const uint numBlocks = m_pBlockPacker->GetNumBlocks();
		for (uint blockId = 0; blockId < numBlocks; blockId++)
		{
			if (SBlockMinMax* pInfo = m_pBlockPacker->GetBlockInfo(blockId))
			{
				m_poolUsageItems++;
				m_poolUsageBytes += dataSizeStatsScale * pInfo->m_nDataSize;
			}
		}
	}
	#endif

	// TODO: pack multiple bricks into single compressed block and upload to GPU only once

	for (int passId = 0; passId < 16; passId++)
	{
		m_pBlockInfo = m_pBlockPacker->AddBlock(blockW, blockH, blockD,
		                                        this, GetCurrPassMainFrameID(), m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z * (gSvoEnv->m_voxTexFormat == eTF_BC3 ? 1 : 4) / dataSizeStatsScale);
		if (m_pBlockInfo)
		{
			bool bIsMultiThreadedRenderer = false;
			gEnv->pRenderer->EF_Query(EFQ_RenderMultithreaded, bIsMultiThreadedRenderer);

			if (bIsMultiThreadedRenderer)
				m_pNode->m_requestSegmentUpdateFrametId = max(m_pNode->m_requestSegmentUpdateFrametId, GetCurrPassMainFrameID() + 1);
			else
				m_pNode->m_requestSegmentUpdateFrametId = max(m_pNode->m_requestSegmentUpdateFrametId, GetCurrPassMainFrameID() + 0);

			m_vStatLightsCheckSumm.zero();
			break;
		}

		// remove 4 oldest blocks
		SBlockMinMax* pOldestBlockInfo[4] = { 0, 0, 0, 0 };
		uint arrOldestVisFrameId[4] = { ~0U, ~0U, ~0U, ~0U };
		const uint maxAllowedFrameId = GetCurrPassMainFrameID() - 16;
		const uint numBlocks = m_pBlockPacker->GetNumBlocks();
		for (uint blockId = 0; blockId < numBlocks; blockId++)
		{
			if (SBlockMinMax* pInfo = m_pBlockPacker->GetBlockInfo(blockId))
			{
				int newestSlotId = 0;
				for (int i = 0; i < 4; i++)
					if (arrOldestVisFrameId[i] > arrOldestVisFrameId[newestSlotId])
						newestSlotId = i;

				if (pInfo->m_nLastVisFrameId < arrOldestVisFrameId[newestSlotId] && pInfo->m_nLastVisFrameId < maxAllowedFrameId)
				{
					CVoxelSegment* pSeg = (CVoxelSegment*)pInfo->m_pUserData;
					pInfo->m_nLastVisFrameId = pSeg->m_lastRendFrameId;

					uint frameIdWeighted = pSeg->m_lastRendFrameId + int(pSeg->GetBoxSize() / minNodeSize);

					if ((frameIdWeighted < arrOldestVisFrameId[newestSlotId])
					    && (pSeg->m_lastTexUpdateFrameId < maxAllowedFrameId)
					    && (pSeg->m_lastRendFrameId < maxAllowedFrameId))
					{
						pOldestBlockInfo[newestSlotId] = pInfo;
						arrOldestVisFrameId[newestSlotId] = frameIdWeighted;
					}
				}
			}
		}

		int numRemovedBlocks = 0;

		for (int i = 0; i < 4; i++)
			if (pOldestBlockInfo[i])
			{
				CVoxelSegment* pSeg = (CVoxelSegment*)pOldestBlockInfo[i]->m_pUserData;
				if (pSeg->m_pBlockInfo != pOldestBlockInfo[i])
					ErrorTerminate("pSeg->m_pBlockInfo != pOldestBlockInfo[i]");
				pSeg->ReleaseAtlasBlock();
				numRemovedBlocks++;
			}

		if (!numRemovedBlocks)
		{
			break;
		}
	}

	if (m_pBlockInfo == 0)
	{
#ifdef _DEBUG
		Cry3DEngineBase::PrintMessage("UpdateBrickRenderData postponed %d", GetCurrPassMainFrameID());
#endif
		gSvoEnv->m_svoFreezeTime = -1; // prevent hang in case of full sync update
		CVoxelSegment::m_bUpdateBrickRenderDataPostponed = 1;
		return false;
	}

	CVoxelSegment::m_bUpdateBrickRenderDataPostponed = 0;
	m_lastTexUpdateFrameId = GetCurrPassMainFrameID();
	m_lastRendFrameId = GetCurrPassMainFrameID();

	assert(m_pBlockInfo->m_pUserData == this);
	Vec3i vOffset(m_pBlockInfo->m_dwMinX, m_pBlockInfo->m_dwMinY, m_pBlockInfo->m_dwMinZ);
	m_allocatedAtlasOffset = vOffset.z * SVO_ATLAS_DIM_MAX_XY * SVO_ATLAS_DIM_MAX_XY + vOffset.y * SVO_ATLAS_DIM_MAX_XY + vOffset.x;

	if (GetCVars()->e_svoMaxBrickUpdates > 0)
	{
		if (m_vCropTexSize.GetVolume() && m_voxData.pData[SVoxBrick::OPA3D])
		{
			UpdateVoxRenderData();
		}

		UpdateNodeRenderData();

		UpdateMeshRenderData();
	}

	return true;
}

bool CVoxelSegment::CheckUpdateBrickRenderData(bool bJustCheck)
{
	bool bRes = true;

	if (m_allocatedAtlasOffset < 0)
	{
		if (bJustCheck)
			bRes = false;
		else
			bRes = UpdateBrickRenderData();
	}

	return bRes;
}

void CVoxelSegment::UpdateObjectLayersInfo()
{
	// Copy only if modified
	if (m_arrObjectLayersInfoVersion == Get3DEngine()->m_objectLayersModificationId)
	{
		return;
	}

	m_arrObjectLayersInfoVersion = Get3DEngine()->m_objectLayersModificationId;
	m_arrObjectLayersInfo = Get3DEngine()->m_arrObjectLayersActivity;
}

void CVoxelSegment::CheckAllocateTexturePool()
{
	int flagsReadOnly = FT_DONT_STREAM;
	int flagsReadWrite = FT_DONT_STREAM | FT_USAGE_UNORDERED_ACCESS | FT_USAGE_UAV_RWTEXTURE;

	if (GetSubSetsNum() > 1)
	{
		if (!gSvoEnv->m_texRgb0PoolId)
		{
			gSvoEnv->m_texRgb0PoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
			                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
			m_svoDataPoolsCounter++;
		}

		if (Cry3DEngineBase::GetCVars()->e_svoTI_Active && Cry3DEngineBase::GetCVars()->e_svoTI_IntegrationMode)
		{
			// direct lighting
			if (!gSvoEnv->m_texRgb1PoolId)
			{
				gSvoEnv->m_texRgb1PoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
				m_svoDataPoolsCounter++;
			}

			// dyn direct lighting
			if (!gSvoEnv->m_texDynlPoolId && Cry3DEngineBase::GetCVars()->e_svoTI_DynLights)
			{
				gSvoEnv->m_texDynlPoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
				m_svoDataPoolsCounter++;
			}

			// propagation
			if (!gSvoEnv->m_texRgb2PoolId && Cry3DEngineBase::GetCVars()->e_svoTI_NumberOfBounces > 1)
			{
				gSvoEnv->m_texRgb2PoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
				m_svoDataPoolsCounter++;
			}

			// propagation
			if (!gSvoEnv->m_texRgb3PoolId && Cry3DEngineBase::GetCVars()->e_svoTI_NumberOfBounces > 2)
			{
				gSvoEnv->m_texRgb3PoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
				m_svoDataPoolsCounter++;
			}

			// snow
			if (!gSvoEnv->m_texRgb4PoolId && Cry3DEngineBase::GetCVars()->e_svoTI_Troposphere_Snow_Height)
			{
				gSvoEnv->m_texRgb4PoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
				m_svoDataPoolsCounter++;
			}
		}

		{
			if (!gSvoEnv->m_texNormPoolId)
			{
				gSvoEnv->m_texNormPoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadOnly);
				m_svoDataPoolsCounter++;
			}

			if (!gSvoEnv->m_texAldiPoolId && Cry3DEngineBase::GetCVars()->e_svoTI_Diffuse_Cache)
			{
				gSvoEnv->m_texAldiPoolId = gEnv->pRenderer->UploadToVideoMemory3D(NULL,
				                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadWrite);
				m_svoDataPoolsCounter++;
			}
		}
	}

	if (!gSvoEnv->m_texOpasPoolId)
	{
	#ifndef _RELEASE
		int voxDataSize = m_voxTexPoolDimXY * m_voxTexPoolDimXY * m_voxTexPoolDimZ * sizeof(ColorB);
		byte* pDataZero = new byte[voxDataSize];
		memset(pDataZero, 0, voxDataSize);
	#else
		byte* pDataZero = 0;
	#endif

		gSvoEnv->m_texOpasPoolId = gEnv->pRenderer->UploadToVideoMemory3D(pDataZero,
		                                                                  m_voxTexPoolDimXY, m_voxTexPoolDimXY, m_voxTexPoolDimZ, gSvoEnv->m_voxTexFormat, gSvoEnv->m_voxTexFormat, 1, false, FILTER_LINEAR, 0, 0, flagsReadOnly);
		m_svoDataPoolsCounter++;

		gSvoEnv->m_texNodePoolId = gEnv->pRenderer->UploadToVideoMemory3D(pDataZero,
		                                                                  SVO_NODES_POOL_DIM_XY, SVO_NODES_POOL_DIM_XY, SVO_NODES_POOL_DIM_Z, eTF_R32G32B32A32F, eTF_R32G32B32A32F, 1, false, FILTER_POINT, 0, 0, flagsReadOnly);

	#ifndef _RELEASE
		delete[] pDataZero;
	#endif
	}

}

void CVoxelSegment::UpdateNodeRenderData()
{
	FUNCTION_PROFILER_3DENGINE;

	assert(m_pBlockInfo->m_pUserData == this);
	Vec3i vOffset(m_pBlockInfo->m_dwMinX, m_pBlockInfo->m_dwMinY, m_pBlockInfo->m_dwMinZ);

	static Vec4 voxNodeData[SVO_NODE_BRICK_SIZE * SVO_NODE_BRICK_SIZE * SVO_NODE_BRICK_SIZE];
	ZeroStruct(voxNodeData);

	voxNodeData[0] = Vec4(m_boxOS.min + m_vSegOrigin, 0);
	voxNodeData[1] = Vec4(m_boxOS.max + m_vSegOrigin, 0);
	voxNodeData[0] = voxNodeData[0] + Vec4(Vec3((float)m_vCropBoxMin.x / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropBoxMin.y / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropBoxMin.z / SVO_VOX_BRICK_MAX_SIZE) * GetBoxSize(), 0);
	voxNodeData[1] = voxNodeData[0] + Vec4(Vec3((float)m_vCropTexSize.x / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropTexSize.y / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropTexSize.z / SVO_VOX_BRICK_MAX_SIZE) * GetBoxSize(), 0);
	voxNodeData[0].w = GetBoxSize();
	if (GetCVars()->e_svoTI_RT_Active)
	{
		// the consumer reads this as the cell's static BVH root record (0 = no BVH); off flag the
		// original (write only) parent atlas offset is written, so the texture stays byte identical
		voxNodeData[1].w = (float)m_rtRootRecord;
	}
	else
	{
		voxNodeData[1].w = m_pParentCloud ? (0.1f + (float)m_pParentCloud->m_allocatedAtlasOffset) : -2.f;
	}

	for (int c = 0; c < 4; c++)
	{
		if (m_arrChildOffset[c + 0] >= 0)
			voxNodeData[2][c] = 0.1f + (float)m_arrChildOffset[c + 0];
		else
			voxNodeData[2][c] = -0.1f + (float)m_arrChildOffset[c + 0];

		if (m_arrChildOffset[c + 4] >= 0)
			voxNodeData[3][c] = 0.1f + (float)m_arrChildOffset[c + 4];
		else
			voxNodeData[3][c] = -0.1f + (float)m_arrChildOffset[c + 4];
	}

	voxNodeData[4][0] = 0.1f + (float)GetRenderer()->GetFrameID(false);

	gEnv->pRenderer->UpdateTextureInVideoMemory(
	  gSvoEnv->m_texNodePoolId,
	  (byte*)&voxNodeData[0],
	  vOffset.x * SVO_NODE_BRICK_SIZE,
	  vOffset.y * SVO_NODE_BRICK_SIZE,
	  SVO_NODE_BRICK_SIZE,
	  SVO_NODE_BRICK_SIZE,
	  eTF_R32G32B32A32F,
	  vOffset.z * SVO_NODE_BRICK_SIZE,
	  SVO_NODE_BRICK_SIZE);
	CVoxelSegment::m_updatesInProgressTex++;

	m_boxClipped.min = Vec3(voxNodeData[0].x, voxNodeData[0].y, voxNodeData[0].z);
	m_boxClipped.max = Vec3(voxNodeData[1].x, voxNodeData[1].y, voxNodeData[1].z);
}

void CVoxelSegment::UpdateMeshRenderData()
{
	FUNCTION_PROFILER_3DENGINE;

	assert(m_pBlockInfo->m_pUserData == this);

	// define single vertex for GS
	{
		// set box origin
		m_vertForGS.xyz = m_boxClipped.min;

		// set pinter to brick data
		m_vertForGS.st.x = (0.5f + (float)m_allocatedAtlasOffset);

		// pack box size
		float nodeSize = m_vertForGS.st.y = GetBoxSize();
		Vec3 vBoxSize = m_boxClipped.GetSize();
		m_vertForGS.color.bcolor[0] = SATURATEB(int(vBoxSize.x / nodeSize * 255.f));
		m_vertForGS.color.bcolor[1] = SATURATEB(int(vBoxSize.y / nodeSize * 255.f));
		m_vertForGS.color.bcolor[2] = SATURATEB(int(vBoxSize.z / nodeSize * 255.f));
		m_vertForGS.color.bcolor[3] = SATURATEB(int(m_maxAlphaInBrick * 255.f));
	}
}

void CVoxelSegment::UpdateVoxRenderData()
{
	FUNCTION_PROFILER_3DENGINE;

	assert(m_pBlockInfo->m_pUserData == this);

	Vec3i vOffset(m_pBlockInfo->m_dwMinX, m_pBlockInfo->m_dwMinY, m_pBlockInfo->m_dwMinZ);

	Vec3i vSizeFin = m_vCropTexSize;

	if (gSvoEnv->m_voxTexFormat == eTF_BC3)
	{
		vSizeFin = GetDxtDim();
	}

	// SVoxBrick::RTRIS is never allocated any more (rt decision 02: the per-voxel triangle list path is retired)
	int arrTexId[SVoxBrick::MAX_NUM] = { gSvoEnv->m_texOpasPoolId, gSvoEnv->m_texRgb0PoolId, gSvoEnv->m_texNormPoolId, 0 };

	for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
	{
		if (m_voxData.pData[s])
		{
			gEnv->pRenderer->UpdateTextureInVideoMemory(
			  arrTexId[s],
			  (byte*)m_voxData.pData[s],
			  vOffset.x * SVO_BRICK_ALLOC_CHUNK_SIZE, vOffset.y * SVO_BRICK_ALLOC_CHUNK_SIZE,
			  vSizeFin.x, vSizeFin.y,
			  gSvoEnv->m_voxTexFormat,
			  vOffset.z * SVO_BRICK_ALLOC_CHUNK_SIZE,
			  vSizeFin.z);

			CVoxelSegment::m_updatesInProgressTex++;
		}
	}
}

void CVoxelSegment::ReleaseAtlasBlock()
{
	if (m_pBlockInfo)
		m_pBlockPacker->RemoveBlock(m_pBlockInfo);
	m_pBlockInfo = 0;
	m_allocatedAtlasOffset = -2;

	m_pNode->m_requestSegmentUpdateFrametId = 0;
	m_vStatLightsCheckSumm.zero();

	PropagateDirtyFlag();
}

void CVoxelSegment::PropagateDirtyFlag()
{
	if (CVoxelSegment* pParent = m_pParentCloud)
	{
		pParent->m_bChildOffsetsDirty = 2;

		if (pParent->m_pParentCloud)
			pParent->m_pParentCloud->m_bChildOffsetsDirty = 2;

		while (pParent->m_pParentCloud)
		{
			pParent = pParent->m_pParentCloud;
			pParent->m_bChildOffsetsDirty = max(pParent->m_bChildOffsetsDirty, (byte)1);
		}
	}
}

AABB CVoxelSegment::GetChildBBox(const AABB& parentBox, int childId)
{
	int x = (childId / 4);
	int y = (childId - x * 4) / 2;
	int z = (childId - x * 4 - y * 2);
	Vec3 vSize = parentBox.GetSize() * 0.5f;
	Vec3 vOffset = vSize;
	vOffset.x *= x;
	vOffset.y *= y;
	vOffset.z *= z;
	AABB childBox;
	childBox.min = parentBox.min + vOffset;
	childBox.max = childBox.min + vSize;
	return childBox;
}

bool GetBarycentricTC(const Vec3& a, const Vec3& b, const Vec3& c, float& u, float& v, float& w, const Vec3& p, const float& border)
{
	Vec3 v0 = b - a, v1 = c - a, v2 = p - a;
	float d00 = v0.Dot(v0);
	float d01 = v0.Dot(v1);
	float d11 = v1.Dot(v1);
	float d20 = v2.Dot(v0);
	float d21 = v2.Dot(v1);
	float d = d00 * d11 - d01 * d01;
	float invDenom = d ? (1.0f / d) : 1000000.f;
	v = (d11 * d20 - d01 * d21) * invDenom;
	w = (d00 * d21 - d01 * d20) * invDenom;
	u = 1.0f - v - w;
	return (u >= -border) && (v >= -border) && (w >= -border);
}

ColorF CVoxelSegment::GetColorF_255(int x, int y, const ColorB* pImg, int imgSizeW, int imgSizeH)
{
	const ColorB& colB = pImg[x + y * imgSizeW];
	ColorF colF;
	colF.r = colB.r;
	colF.g = colB.g;
	colF.b = colB.b;
	colF.a = colB.a;

	return colF;
}

ColorF CVoxelSegment::GetBilinearAt(float iniX, float iniY, const ColorB* pImg, int dimW, int dimH, float multiplier)
{
	int imgSizeW = dimW;
	int imgSizeH = dimH;

	iniX *= imgSizeW;
	iniY *= imgSizeH;

	//  iniX -= .5f;
	//  iniY -= .5f;

	int x = (int)floor(iniX);
	int y = (int)floor(iniY);

	float rx = iniX - x;    // fractional part
	float ry = iniY - y;    // fractional part

	int maskW = imgSizeW - 1;
	int maskH = imgSizeH - 1;

	//  return GetColorF_255(nMaskW&(x  ),nMaskH&(y  ), pImg, nImgSizeW, nImgSizeH) * fBr;

	ColorF top = GetColorF_255(maskW & (x), maskH & (y), pImg, imgSizeW, imgSizeH) * (1.f - rx)     // left top
	             + GetColorF_255(maskW & (x + 1), maskH & (y), pImg, imgSizeW, imgSizeH) * rx;      // right top
	ColorF bot = GetColorF_255(maskW & (x), maskH & (y + 1), pImg, imgSizeW, imgSizeH) * (1.f - rx) // left bottom
	             + GetColorF_255(maskW & (x + 1), maskH & (y + 1), pImg, imgSizeW, imgSizeH) * rx;  // right bottom

	return (top * (1.f - ry) + bot * ry) * multiplier;
}

void CVoxelSegment::VoxelizeMeshes(int threadId, bool bUseMT)
{
	MEMSTAT_CONTEXT(EMemStatContextType::Other, "VoxelizeMeshes");

	m_vCropBoxMin.Set(0, 0, 0);
	m_vCropTexSize.Set(SVO_VOX_BRICK_MAX_SIZE, SVO_VOX_BRICK_MAX_SIZE, SVO_VOX_BRICK_MAX_SIZE);

	PodArray<int>* pNodeTrisXYZ = 0;

	if (GetBoxSize() <= GetCVars()->e_svoMaxNodeSize)
	{
		pNodeTrisXYZ = new PodArray<int>[SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE];
		FindTrianglesForVoxelization(pNodeTrisXYZ);

		gSvoEnv->m_arrVoxelizeMeshesCounter[0]++;
		if (GetBoxSize() == GetCVars()->e_svoMaxNodeSize)
			gSvoEnv->m_arrVoxelizeMeshesCounter[1]++;
	}

	CheckAllocateSubSets(m_voxData, m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z, true);

	// voxelize node tris
	if ((m_nodeTrisAllMerged.Count() || Cry3DEngineBase::GetCVars()->e_svoTI_Troposphere_Subdivide) && m_voxData.pData[SVoxBrick::OPA3D] && m_pTrisInArea && pNodeTrisXYZ)
	{
		PodArray<CVisArea*> arrPortals;

		// collect overlapping portals
		if (GetSubSetsNum() > 1)
		{
			for (int v = 0;; v++)
			{
				CVisArea* pVisArea = (CVisArea*)GetVisAreaManager()->GetVisAreaById(v);
				if (!pVisArea)
					break;

				if (pVisArea->IsPortal() && Overlap::AABB_AABB(*pVisArea->GetAABBox(), m_pNode->m_nodeBox))
					arrPortals.Add(pVisArea);
			}
		}

		if (m_objLayerMap.empty())
		{
			m_objLayerMap[kAllObjectLayersId] = m_voxData;
		}

		/*if (bUseMT) // commented because causes not enough occlusion error
		   {
		   const int VOX_THREADS_NUM = 8;

		   JobManager::SJobState jobState[VOX_THREADS_NUM];
		   PodArray<int> arrTrisInt[VOX_THREADS_NUM];

		   for (int t = 0; t < VOX_THREADS_NUM; t++)
		   {
		    int x0 = t * SVO_VOX_BRICK_MAX_SIZE / VOX_THREADS_NUM;
		    int x1 = x0 + SVO_VOX_BRICK_MAX_SIZE / VOX_THREADS_NUM;

		    SBuildVoxelsParams params;
		    params.X0 = x0;
		    params.X1 = x1;
		    params.pNodeTrisXYZ = pNodeTrisXYZ;
		    params.arrPortals = &arrPortals;

		    arrTrisInt[t].PreAllocate(SVO_MAX_TRIS_PER_VOXEL);
		    params.pTrisInt = &arrTrisInt[t];

		    TBuildVoxelsJob job(params);
		    job.SetClassInstance(this);
		    job.SetPriorityLevel(JobManager::eHighPriority);
		    job.RegisterJobState(&jobState[t]);
		    job.Run();
		   }

		   for (int t = 0; t < VOX_THREADS_NUM; t++)
		   {
		    gEnv->GetJobManager()->WaitForJob(jobState[t]);
		   }
		   }
		   else*/
		{
			SBuildVoxelsParams params;
			params.X0 = 0;
			params.X1 = SVO_VOX_BRICK_MAX_SIZE;
			params.pNodeTrisXYZ = pNodeTrisXYZ;
			params.arrPortals = &arrPortals;

			PodArray<int> trisInt;
			trisInt.PreAllocate(SVO_MAX_TRIS_PER_VOXEL);
			params.pTrisInt = &trisInt;

			BuildVoxels(params);
		}

		if (!Cry3DEngineBase::GetCVars()->e_svoTI_Troposphere_Subdivide)
		{
			CropVoxTexture(threadId, true);
		}

		if (!m_bExportMode)
		{
			CombineLayers();
		}
	}
	else
	{
		m_vCropTexSize.zero();
		m_vCropBoxMin.zero();
	}

	if (Cry3DEngineBase::GetCVars()->e_svoTI_Troposphere_Subdivide)
	{
		// allocate all nodes
		m_vCropTexSize.zero();
		m_vCropTexSize.Set(SVO_VOX_BRICK_MAX_SIZE, SVO_VOX_BRICK_MAX_SIZE, SVO_VOX_BRICK_MAX_SIZE);
	}

	SAFE_DELETE_ARRAY(pNodeTrisXYZ);
}

void CVoxelSegment::CombineLayers()
{
	if (m_objLayerMap.size() != 1 || m_objLayerMap.begin()->second.pData[SVoxBrick::OPA3D] != m_voxData.pData[SVoxBrick::OPA3D])
	{
		CheckAllocateSubSets(m_voxData, m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z, true);

		for (auto& it : m_objLayerMap)
		{
			ObjectLayerIdType nLayerId = it.first;

			SVoxBrick& layerVoxData = it.second;

			if (nLayerId && ((nLayerId >= m_arrObjectLayersInfo.Count()) || !m_arrObjectLayersInfo[nLayerId].bActive))
			{
				continue;
			}

			int pixNum = m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z;

			for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
			{
				if (m_voxData.pData[s])
				{
					for (int p = 0; p < pixNum; p++)
					{
						for (int c = 0; c < 4; c++)
						{
							m_voxData.pData[s][p][c] = max(m_voxData.pData[s][p][c], layerVoxData.pData[s][p][c]);
						}
					}
				}
			}
		}
	}

	FreeBrickLayers();
}

void CVoxelSegment::BuildVoxels(SBuildVoxelsParams params)
{
	int X0 = params.X0;
	int X1 = params.X1;
	PodArray<int>* pNodeTrisXYZ = params.pNodeTrisXYZ;
	PodArray<CVisArea*>* arrPortals = params.arrPortals;

	Vec4 voxNodeData[SVO_NODE_BRICK_SIZE * SVO_NODE_BRICK_SIZE * SVO_NODE_BRICK_SIZE];
	ZeroStruct(voxNodeData);
	voxNodeData[0] = Vec4(m_boxOS.min + m_vSegOrigin, 0);
	voxNodeData[1] = Vec4(m_boxOS.max + m_vSegOrigin, 0);

	const int dimSS = 4;
	const int dimS = dimSS / 2;

	PodArray<int> arrSubOpa[dimS][dimS][dimS];

	for (int X = X0; X < X1; X++)
	{
		for (int Y = 0; Y < SVO_VOX_BRICK_MAX_SIZE; Y++)
		{
			for (int Z = 0; Z < SVO_VOX_BRICK_MAX_SIZE; Z++)
			{
				Vec4 vMin = voxNodeData[0] + (voxNodeData[1] - voxNodeData[0]) * Vec4((float) X / SVO_VOX_BRICK_MAX_SIZE, (float) Y / SVO_VOX_BRICK_MAX_SIZE, (float) Z / SVO_VOX_BRICK_MAX_SIZE, 1);
				Vec4 vMax = voxNodeData[0] + (voxNodeData[1] - voxNodeData[0]) * Vec4((float) (X + 1) / SVO_VOX_BRICK_MAX_SIZE, (float) (Y + 1) / SVO_VOX_BRICK_MAX_SIZE, (float) (Z + 1) / SVO_VOX_BRICK_MAX_SIZE, 1);

				// safety border support
				//      Vec4 vCenter(m_vOrigin.x,m_vOrigin.y,m_vOrigin.z,0);
				//        vMin += (vMin - vCenter)/(kVoxTexMaxDim/2)/2;
				//        vMax += (vMax - vCenter)/(kVoxTexMaxDim/2)/2;

				AABB voxBox;
				voxBox.min.Set(vMin.x, vMin.y, vMin.z);
				voxBox.max.Set(vMax.x, vMax.y, vMax.z);

				if (GetCVars()->e_svoTI_RT_Active)
					voxBox.Expand(Vec3(vMax.z - vMin.z) * GetCVars()->e_svoTI_RT_SafetyBorder);

				if (Overlap::AABB_AABB(voxBox, m_boxTris))
				{
					// loop through found layers
					for (auto& it : m_objLayerMap)
					{
						ObjectLayerIdType nLayerId = it.first;

						SVoxBrick& voxData = it.second;

						const int id = Z * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE + Y * SVO_VOX_BRICK_MAX_SIZE + X;

						ColorB& opaOutFin = voxData.pData[SVoxBrick::OPA3D][id];

						PodArray<int>& trisInt = *params.pTrisInt;
						trisInt.Clear();

						bool bVisAreaTrisDetected = false;

						AUTO_READLOCK(m_superMeshLock);

						// collect tris only for this voxel; TODO: check maybe pNodeTrisXYZ[id] already have only what is needed and trisInt can be dropped
						for (int d = 0; (d < pNodeTrisXYZ[id].Count()) && (trisInt.Count() < SVO_MAX_TRIS_PER_VOXEL); d++)
						{
							int trId = pNodeTrisXYZ[id].GetAt(d);

							SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[trId];

							if (tr.objectLayerId == nLayerId)
							{
								Vec3 arrV[3] = { (*m_pVertInArea)[tr.arrVertId[0]].v, (*m_pVertInArea)[tr.arrVertId[1]].v, (*m_pVertInArea)[tr.arrVertId[2]].v };

								if (Overlap::AABB_Triangle(voxBox, arrV[0], arrV[1], arrV[2])) // 14s
								{
									if (tr.hitObjectType == HIT_OBJ_TYPE_VISAREA)
										bVisAreaTrisDetected = true;

									trisInt.Add(trId);
								}
							}
						}

						// OPA
						{
							{
								// Fill nSDim x nSDim x nSDim super voxel

								Vec3 vSubVoxSize = voxBox.GetSize() / (float)dimS;

								for (int x = 0; x < dimS; x++)
								{
									for (int y = 0; y < dimS; y++)
									{
										for (int z = 0; z < dimS; z++)
										{
											PodArray<int>& opaOutSub = arrSubOpa[x][y][z];

											opaOutSub.Clear();

											AABB SubBox;
											SubBox.min = voxBox.min + vSubVoxSize.CompMul(Vec3((float)x, (float)y, (float)z));
											SubBox.max = SubBox.min + vSubVoxSize;

											AABB SubBoxT = SubBox;
											SubBoxT.max.z++;

											for (int c = 0; c < trisInt.Count(); c++)
											{
												int triangleId = trisInt.GetAt(c);

												SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[triangleId];

												Vec3 arrV[3] = { (*m_pVertInArea)[tr.arrVertId[0]].v, (*m_pVertInArea)[tr.arrVertId[1]].v, (*m_pVertInArea)[tr.arrVertId[2]].v };

												if (Overlap::AABB_Triangle(tr.materialID ? SubBox : SubBoxT, arrV[0], arrV[1], arrV[2])) // 40 ms
												{
													opaOutSub.Add(triangleId);
												}
											}
										}
									}
								}
							}

							uint8 arrSubSubOpa[dimSS][dimSS][dimSS];
							Vec4 arrSubSubNor[dimSS][dimSS][dimSS];
							ColorF arrSubSubCol[dimSS][dimSS][dimSS];
							float arrSubSubEmi[dimSS][dimSS][dimSS];

							ZeroStruct(arrSubSubOpa);

							if (GetSubSetsNum() > 1)
							{
								ZeroStruct(arrSubSubNor);
								ZeroStruct(arrSubSubCol);
								ZeroStruct(arrSubSubEmi);
							}

							// Fill nSSDim x nSSDim x nSSDim super voxel

							Vec3 vSubSubVoxSize = voxBox.GetSize() / (float)dimSS;

							for (int x = 0; x < dimSS; x++)
							{
								for (int y = 0; y < dimSS; y++)
								{
									for (int z = 0; z < dimSS; z++)
									{
										PodArray<int>& opaOutSub = arrSubOpa[x / 2][y / 2][z / 2];

										uint8& opaOutSubSub = arrSubSubOpa[x][y][z];
										Vec4& norOutSubSub = arrSubSubNor[x][y][z];
										ColorF& colOutSubSub = arrSubSubCol[x][y][z];
										float& EmiOutSubSub = arrSubSubEmi[x][y][z];

										AABB SubSubBox;
										SubSubBox.min = voxBox.min + vSubSubVoxSize.CompMul(Vec3((float)x, (float)y, (float)z));
										SubSubBox.max = SubSubBox.min + vSubSubVoxSize;

										AABB SubSubBoxT = SubSubBox;
										SubSubBoxT.max.z++;

										AABB SubSubBoxP = SubSubBox;
										SubSubBoxP.Expand(Vec3(.25, .25, .25));

										if (GetSubSetsNum() > 1)
										{
											bool bVoxelInPortal = false;

											for (int v = 0; v < arrPortals->Count(); v++)
											{
												if (Overlap::AABB_AABB(*(*arrPortals)[v]->GetAABBox(), SubSubBoxP))
												{
													bVoxelInPortal = true;
													break;
												}
											}

											if (bVoxelInPortal) // skip sub-voxels in the portal
												continue;
										}

										for (int c = 0; c < opaOutSub.Count(); c++)
										{
											if (opaOutSub.GetAt(c) >= (*m_pTrisInArea).Count())
											{
												PrintMessage("%s warning: trId>=(*m_pTrisInArea).Count()", __FUNC__);
												break;
											}

											SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[opaOutSub.GetAt(c)];

											Vec3 arrV[3] = { (*m_pVertInArea)[tr.arrVertId[0]].v, (*m_pVertInArea)[tr.arrVertId[1]].v, (*m_pVertInArea)[tr.arrVertId[2]].v };

											if (Overlap::AABB_Triangle(tr.materialID ? SubSubBox : SubSubBoxT, arrV[0], arrV[1], arrV[2])) // 8ms
											{
												ColorF colTraced = ProcessMaterial(tr, SubSubBox.GetCenter());
												if (colTraced.a)
												{
													opaOutSubSub = max(opaOutSubSub, min(tr.opacity, (uint8)SATURATEB(colTraced.a * 255.f)));

													if (GetSubSetsNum() > 1)
													{
														norOutSubSub += Vec4(tr.vFaceNorm, 1);

														colOutSubSub.r += colTraced.r;
														colOutSubSub.g += colTraced.g;
														colOutSubSub.b += colTraced.b;
														colOutSubSub.a += 1.f;

														SSvoMatInfo& rMI = m_pMatsInArea->GetAt(tr.materialID);
														if (rMI.pMat)
														{
															EmiOutSubSub = +rMI.pMat->GetShaderItem(0).m_pShaderResources->GetFinalEmittance().Luminance();
														}
													}
													else
													{
														if (opaOutSubSub == 255)
														{
															break;
														}
													}
												}
											}
										}
									}
								}
							}

							// Compute tri-planar opacity

							uint8 arrQuad[3][dimSS][dimSS];
							ZeroStruct(arrQuad);
							Vec4 vNorFin(0, 0, 0, 0);
							ColorF vColFin(0, 0, 0, 0);
							float emiFin(0);

							for (int _x = 0; _x < dimSS; _x++)
							{
								for (int _y = 0; _y < dimSS; _y++)
								{
									for (int _z = 0; _z < dimSS; _z++)
									{
										uint8& opaCh = arrSubSubOpa[_x][_y][_z];
										Vec4& norCh = arrSubSubNor[_x][_y][_z];
										ColorF& colCh = arrSubSubCol[_x][_y][_z];
										float& EmiCh = arrSubSubEmi[_x][_y][_z];

										arrQuad[0][_y][_z] = max(arrQuad[0][_y][_z], opaCh);
										arrQuad[1][_x][_z] = max(arrQuad[1][_x][_z], opaCh);
										arrQuad[2][_x][_y] = max(arrQuad[2][_x][_y], opaCh);

										if (GetSubSetsNum() > 1)
										{
											vNorFin += norCh;
											vColFin += colCh;
											emiFin += EmiCh;
										}
									}
								}
							}

							// we do not normalize normal, length of the normal says how reliable the normal is
							if (vNorFin.w)
							{
								vNorFin = vNorFin / vNorFin.w;
							}

							if (vColFin.a)
							{
								emiFin /= vColFin.a;
								vColFin /= vColFin.a;
							}

							ColorF opaSummF(0, 0, 0, 0);

							for (int k = 0; k < dimSS; k++)
							{
								for (int m = 0; m < dimSS; m++)
								{
									opaSummF.r += arrQuad[0][k][m];
									opaSummF.g += arrQuad[1][k][m];
									opaSummF.b += arrQuad[2][k][m];
								}
							}

							opaSummF.r /= dimSS * dimSS;
							opaSummF.g /= dimSS * dimSS;
							opaSummF.b /= dimSS * dimSS;

							opaOutFin.r = SATURATEB((int)opaSummF.b);
							opaOutFin.g = SATURATEB((int)opaSummF.g);
							opaOutFin.b = SATURATEB((int)opaSummF.r);

							bool bTerrainTrisDetected;
							Vec3 vH = voxBox.GetCenter();
							if (vH.z > (GetTerrain()->GetZ((int)vH.x, (int)vH.y, 0) + 1.5f))
								bTerrainTrisDetected = false;
							else
								bTerrainTrisDetected = true;

							opaOutFin.a = bTerrainTrisDetected ? 0 : 1; // reserved for opacity of dynamic voxels or [0 = triangle is missing in RSM]

							if (bVisAreaTrisDetected && (opaOutFin.r || opaOutFin.g || opaOutFin.b))
							{
								opaOutFin.r = opaOutFin.g = opaOutFin.b = 255; // full opaque
								vColFin.r = vColFin.g = vColFin.b = 0;         // full black
							}

							if (GetSubSetsNum() > 1)
							{
								ColorB& norOut = voxData.pData[SVoxBrick::NORML][id];

								norOut.a = (opaOutFin.r || opaOutFin.g || opaOutFin.b) ? 255 : 0; // contains 255 for any geometry and 0 for empty space

								for (int c = 0; c < 3; c++)
								{
									norOut[c] = SATURATEB(int(vNorFin[2 - c] * 127.5f + 127.5f));
								}

								ColorB& colOut = voxData.pData[SVoxBrick::COLOR][id];

								colOut.r = SATURATEB((int)(vColFin.r * 255.f));
								colOut.g = SATURATEB((int)(vColFin.g * 255.f));
								colOut.b = SATURATEB((int)(vColFin.b * 255.f));
								colOut.a = SATURATEB((int)(emiFin * 255.f));

								if (opaOutFin.r || opaOutFin.g || opaOutFin.b)
								{
									m_solidVoxelsNum++;
								}
							}
						}
					}
				}
			}
		}
	}
}

static int32 CompareTriArea(const void* v1, const void* v2)
{
	SRayHitTriangle* t1 = (SRayHitTriangle*)v1;
	SRayHitTriangle* t2 = (SRayHitTriangle*)v2;

	if (t1->nTriArea > t2->nTriArea)
		return -1;
	if (t1->nTriArea < t2->nTriArea)
		return 1;

	return 0;
};

bool CVoxelSegment::CheckCollectObjectsForVoxelization(const AABB& cloudBoxWS, PodArray<SObjInfo>* parrObjects, bool& bThisIsAreaParent, bool& bThisIsLowLodNode, bool bAllowStartStreaming)
{
	FUNCTION_PROFILER_3DENGINE;

	bool bSuccess = true;

	bThisIsAreaParent = (cloudBoxWS.GetSize().z == GetCVars()->e_svoMaxAreaSize);
	bThisIsLowLodNode = (cloudBoxWS.GetSize().z > GetCVars()->e_svoMaxAreaSize);

	AABB cloudBoxEX = cloudBoxWS;

	if (GetCVars()->e_svoTI_RT_Active)
		cloudBoxEX.Expand(Vec3(cloudBoxWS.GetSize().z / SVO_VOX_BRICK_MAX_SIZE * GetCVars()->e_svoTI_RT_SafetyBorder));

	if (bThisIsAreaParent || bThisIsLowLodNode)
	{
		if (bAllowStartStreaming && bThisIsAreaParent && gEnv->IsEditor())
		{
			// check or activate or build procedural vegetation in the area
			if (!GetTerrain()->CheckUpdateProcObjectsInArea(AABB(cloudBoxWS.min - Vec3(2.f, 2.f, 32.f), cloudBoxWS.max + Vec3(2.f)), m_bExportMode))
				bSuccess = false;
		}

		for (int objType = 0; objType < eERType_TypesNum; objType++)
		{
			if ((bThisIsAreaParent && (objType == eERType_Brush || objType == eERType_MovableBrush)) || (objType == eERType_Vegetation))
			{
				PodArray<IRenderNode*> arrRenderNodes;

				// TODO: query for non-hidden objects only
				Get3DEngine()->GetObjectsByTypeGlobal(arrRenderNodes, (EERType)objType, &cloudBoxEX, bAllowStartStreaming ? &bSuccess : 0, ERF_GI_MODE_BIT0);
				if (Get3DEngine()->GetVisAreaManager())
					Get3DEngine()->GetVisAreaManager()->GetObjectsByType(arrRenderNodes, (EERType)objType, &cloudBoxEX, bAllowStartStreaming ? &bSuccess : 0, ERF_GI_MODE_BIT0);

				if (!arrRenderNodes.Count())
					continue;

				int culledNum = 0;

				for (int d = 0; d < arrRenderNodes.Count(); d++)
				{
					IRenderNode* pNode = arrRenderNodes[d];

					if (pNode->GetRndFlags() & (ERF_COLLISION_PROXY | ERF_RAYCAST_PROXY | ERF_PENDING_DELETE))
						continue;

					if (!GetCVars()->e_svoTI_VoxelizeHiddenObjects && pNode->IsHidden())
						continue;

					if (bThisIsLowLodNode)
						if (!(pNode->GetRndFlags() & ERF_CASTSHADOWMAPS))
							continue;

					if (pNode->GetGIMode() != IRenderNode::eGM_StaticVoxelization)
						continue;

					// skip run-time procedural vegetation (voxelize only offline-procedural)
					if ((pNode->GetRenderNodeType() == eERType_Vegetation) && (pNode->GetRndFlags() & ERF_PROCEDURAL) && !((CVegetation*)pNode)->GetStatObjGroup().offlineProcedural)
						continue;

					float maxViewDist = pNode->GetBBox().GetRadius() * GetCVars()->e_ViewDistRatio;

					float minAllowedViewDist = (pNode->GetRenderNodeType() == eERType_Vegetation) ? (GetCVars()->e_svoTI_ObjectsMaxViewDistance * 2) : GetCVars()->e_svoTI_ObjectsMaxViewDistance;
					minAllowedViewDist *= GetCVars()->e_svoTI_ObjectsMaxViewDistanceScale;

					if (bThisIsLowLodNode)
					{
						if (pNode->GetBBox().GetSize().z < cloudBoxWS.GetSize().z * 0.25f)
							continue;

						minAllowedViewDist *= 4.f;
					}

					if (maxViewDist < minAllowedViewDist)
					{
						culledNum++;
						continue;
					}

					if (pNode->GetBBox().GetSize().z > 256.f)
					{
						//            const T_ObjectLayerId layerId = pNode->GetLayerId();
						//
						//            CStatObj* pStatObj = (CStatObj*)pNode->GetEntityStatObj();
						//            gEnv->pLog->LogWarning("%s: Warning: Too big object skipped at position (%.1f, %.1f, %.1f), name: '%s', layer: %d, CGF name: '%s'",
						//                                   __FUNC__, pNode->GetBBox().GetCenter().x, pNode->GetBBox().GetCenter().y, pNode->GetBBox().GetCenter().z
						//                                   , pNode->GetName(), layerId, pStatObj ? pStatObj->GetFilePath() : "NONE");
						culledNum++;
						continue;
					}

					Matrix34A nodeTM;
					CStatObj* pStatObj = (CStatObj*)pNode->GetEntityStatObj(0, &nodeTM);

					IMaterial* pMaterial = pNode->GetMaterial();
					if (!pMaterial && pStatObj)
						pMaterial = pStatObj->GetMaterial();

					if (pMaterial)
					{
						SObjInfo info;
						info.matObjInv = nodeTM.GetInverted();
						info.matObj = nodeTM;
						info.pStatObj = pStatObj;

						if (!info.pStatObj)
							continue;

						int lodId = GetCVars()->e_svoTI_ObjectsLod;

						if (bThisIsLowLodNode)
							lodId++;

						info.pStatObj = (CStatObj*)info.pStatObj->GetLodObject(lodId, true);

						CStatObj* pParent = info.pStatObj->GetParentObject() ? ((CStatObj*)info.pStatObj->GetParentObject()) : (CStatObj*)info.pStatObj;
						EFileStreamingStatus eStreamingStatusParent = pParent->m_eStreamingStatus;
						bool bUnloadable = pParent->IsUnloadable();

						if (pNode->GetRenderNodeType() == eERType_Vegetation)
						{
							info.objectScale = ((CVegetation*)pNode)->GetScale();
						}
						else if (pNode->GetRenderNodeType() == eERType_MovableBrush || pNode->GetRenderNodeType() == eERType_Brush)
						{
							Vec3 vScaleAbs = info.matObj.TransformVector(Vec3(1, 1, 1)).abs();
							info.objectScale = min(min(vScaleAbs.x, vScaleAbs.y), vScaleAbs.z);
						}
						else
							assert(!"Undefined object type");

						info.pMat = pMaterial;

						if (info.pStatObj->m_nFlags & STATIC_OBJECT_HIDDEN)
							continue;

						info.bIndoor = pNode->GetEntityVisArea() != 0 || (pNode->GetRndFlags() & ERF_REGISTER_BY_BBOX);

						info.bVegetation = (objType == eERType_Vegetation);

						info.maxViewDist = maxViewDist;

						if (parrObjects)
						{
							AUTO_MODIFYLOCK(CVoxelSegment::m_arrLockedMaterials.m_Lock);
							CVoxelSegment::m_arrLockedMaterials[info.pMat] = info.pMat;

							parrObjects->Add(info);
						}

						if (eStreamingStatusParent != ecss_Ready && bUnloadable)
						{
							// request streaming of missing meshes
							if (Cry3DEngineBase::GetCVars()->e_svoTI_VoxelizationPostpone == 2)
							{
								info.pStatObj->UpdateStreamableComponents(0.5f, false, lodId);
							}

							if (Cry3DEngineBase::GetCVars()->e_svoDebug == 7)
							{
								Cry3DEngineBase::Get3DEngine()->DrawBBox(pNode->GetBBox(), Col_Red);
								IRenderAuxText::DrawLabel(pNode->GetBBox().GetCenter(), 1.3f, info.pStatObj->GetFilePath());
							}
							bSuccess = false;
						}
					}
				}
			}
		}
	}

	return bSuccess;
}

void CVoxelSegment::FindTrianglesForVoxelization(PodArray<int>*& rpNodeTrisXYZ)
{
	MEMSTAT_CONTEXT(EMemStatContextType::Other, "FindTrianglesForVoxelization");

	AABB cloudBoxWS;
	cloudBoxWS.min = m_boxOS.min + m_vSegOrigin;
	cloudBoxWS.max = m_boxOS.max + m_vSegOrigin;

	m_nodeTrisAllMerged.Reset();
	m_boxTris.Reset();

	// safety border support
	//  Vec3 vCloudSize = cloudBoxWS.GetSize();
	//    cloudBoxWS.min -= (vCloudSize/kVoxTexMaxDim)/2;
	//    cloudBoxWS.max += (vCloudSize/kVoxTexMaxDim)/2;

	AABB cloudBoxEX = cloudBoxWS;

	if (GetCVars()->e_svoTI_RT_Active)
		cloudBoxEX.Expand(Vec3(cloudBoxWS.GetSize().z / SVO_VOX_BRICK_MAX_SIZE * GetCVars()->e_svoTI_RT_SafetyBorder));

	//if(!m_pParentCloud)
	if (m_isAreaParent || m_isLowLodNode)
	{
		// get tris from real level geometry

		//float startTimeAll = GetCurAsyncTimeSec();
		//		PrintMessage("VoxelizeMeshes: starting triangle search for node id %d (size=%d)", m_nId, (int)GetBoxSize());

		SSuperMesh superMesh;
		PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim];

		//      if(nCulled)
		//        PrintMessage("  %d objects culled", nCulled);

		// add terrain
		if (Get3DEngine()->m_bShowTerrainSurface)
		{
			//float startTime = GetCurAsyncTimeSec();

			CTerrain* pTerrain = GetTerrain();
			int S = (int)max(1.f, pTerrain->GetHeightMapUnitSize());

			if (m_isLowLodNode)
				S *= 4;

			int halfStep = S / 2;

			SRayHitTriangle ht;
			ZeroStruct(ht);
			ht.c[0] = ht.c[1] = ht.c[2] = Col_White;
			ht.nOpacity = 255;
			ht.nHitObjType = HIT_OBJ_TYPE_TERRAIN;
			ht.vn[0] = ht.vn[1] = ht.vn[2] = ht.n = Vec3(0, 0, 1);
			Plane pl;

			int I = 0, X = 0, Y = 0;

			superMesh.Clear(&arrVertHash[0][0][0]);

			for (int x = (int)cloudBoxWS.min.x; x < (int)cloudBoxWS.max.x; x += S)
			{
				for (int y = (int)cloudBoxWS.min.y; y < (int)cloudBoxWS.max.y; y += S)
				{
					if (!pTerrain->GetHole(x + halfStep, y + halfStep))
					{
						// prevent surface interpolation over long edge
						bool bFlipTris = false;
						int type10 = pTerrain->GetSurfaceTypeID(x + S, y);
						int type01 = pTerrain->GetSurfaceTypeID(x, y + S);
						if (type10 != type01)
						{
							int type00 = pTerrain->GetSurfaceTypeID(x, y);
							int type11 = pTerrain->GetSurfaceTypeID(x + S, y + S);
							if ((type10 == type00 && type10 == type11) || (type01 == type00 && type01 == type11))
								bFlipTris = true;
						}

						if (bFlipTris)
						{
							I = 0;
							X = x + S, Y = y + 0;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + S, Y = y + S;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + 0, Y = y + 0;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;

							if (Overlap::AABB_Triangle(cloudBoxWS, ht.v[0], ht.v[1], ht.v[2]))
							{
								ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
								pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
								ht.n = pl.n;

								superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
							}

							I = 0;
							X = x + 0, Y = y + 0;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + S, Y = y + S;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + 0, Y = y + S;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;

							if (Overlap::AABB_Triangle(cloudBoxWS, ht.v[0], ht.v[1], ht.v[2]))
							{
								ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
								pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
								ht.n = pl.n;

								superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
							}
						}
						else
						{
							I = 0;
							X = x + 0, Y = y + 0;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + S, Y = y + 0;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + 0, Y = y + S;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;

							if (Overlap::AABB_Triangle(cloudBoxWS, ht.v[0], ht.v[1], ht.v[2]))
							{
								ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
								pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
								ht.n = pl.n;

								superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
							}

							I = 0;
							X = x + S, Y = y + 0;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + S, Y = y + S;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;
							X = x + 0, Y = y + S;
							ht.v[I].Set((float)X, (float)Y, pTerrain->GetZ(X, Y));
							I++;

							if (Overlap::AABB_Triangle(cloudBoxWS, ht.v[0], ht.v[1], ht.v[2]))
							{
								ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
								pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
								ht.n = pl.n;

								superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
							}
						}
					}
				}
			}

			AUTO_MODIFYLOCK(m_superMeshLock);

			AddSuperMesh(superMesh, SVO_OFFSET_TERRAIN);
		}

		PodArray<SRayHitTriangle> arrTris;

		// sort objects by size
		if (m_areaObjects->Count())
		{
			qsort(m_areaObjects->GetElements(), m_areaObjects->Count(), sizeof((*m_areaObjects)[0]), SObjInfo::Compare);
		}

		for (int d = 0; d < m_areaObjects->Count(); d++)
		{
			SRayHitInfo nodeHitInfo;
			nodeHitInfo.bInFirstHit = true;
			nodeHitInfo.bUseCache = false;
			nodeHitInfo.bGetVertColorAndTC = true;

			SObjInfo& info = (*m_areaObjects)[d];

			nodeHitInfo.nHitTriID = HIT_UNKNOWN;
			nodeHitInfo.nHitMatID = HIT_UNKNOWN;
			nodeHitInfo.inRay.origin = info.matObjInv.TransformPoint(m_vSegOrigin);
			nodeHitInfo.inRay.direction = Vec3(0, 0, 0);
			nodeHitInfo.inReferencePoint = nodeHitInfo.inRay.origin + nodeHitInfo.inRay.direction * 0.5f;
			nodeHitInfo.fMaxHitDistance = GetBoxSize() / 2.f / info.objectScale * sqrt(3.f);

			arrTris.Clear();
			nodeHitInfo.pHitTris = &arrTris;
			nodeHitInfo.useBoxIntersection = true;

			int minVoxelOpacity = (int)(GetCVars()->e_svoTI_MinVoxelOpacity * 255.f);

			float timeRayIntersection = Cry3DEngineBase::GetTimer()->GetAsyncCurTime();

			info.pStatObj->RayIntersection(nodeHitInfo, info.pMat);

			if (arrTris.Count())
			{
				{
					AUTO_MODIFYLOCK(CVoxelSegment::m_arrLockedMaterials.m_Lock);
					CVoxelSegment::m_arrLockedMaterials[info.pMat] = info.pMat;

					for (int s = 0; s < info.pMat->GetSubMtlCount(); s++)
					{
						IMaterial* pSubMtl = info.pMat->GetSafeSubMtl(s);
						CVoxelSegment::m_arrLockedMaterials[pSubMtl] = pSubMtl;
					}
				}

				superMesh.Clear(&arrVertHash[0][0][0]);

				float epsilon = GetCVars()->e_svoTI_ObjectsMaxViewDistance ? (VEC_EPSILON / 2) : (VEC_EPSILON / 10);

				for (int t = 0; t < arrTris.Count(); t++)
				{
					SRayHitTriangle ht = arrTris[t];

					// Workaround for over occlusion from vegetation; TODO: make thin geometry produce less occlusion
					if (ht.pMat)
					{
						SShaderItem& rSI = ht.pMat->GetShaderItem();

						if (rSI.m_pShaderResources && rSI.m_pShader && (rSI.m_pShader->GetShaderType() == eST_Vegetation || info.bVegetation))
						{
							bool vegetationLeaves = (rSI.m_pShaderResources->GetAlphaRef() > 0.05f && rSI.m_pShader->GetShaderType() == eST_Vegetation);

							if (vegetationLeaves)
							{
								ht.nOpacity = min(ht.nOpacity, uint8(SATURATEB(GetCVars()->e_svoTI_VegetationMaxOpacity * 255.f)));
							}
							else
							{
								float midZ = 0;
								for (int v = 0; v < 3; v++)
								{
									midZ += ht.v[v].z;
								}
								midZ *= 0.333f;

								ht.nOpacity = min(ht.nOpacity, uint8(SATURATEB(LERP(255.f, GetCVars()->e_svoTI_VegetationMaxOpacity * 255.f, SATURATE(midZ * .5f)))));
							}
						}
					}

					if (ht.nOpacity < minVoxelOpacity)
						continue;

					bool isFaceValid = true;

					// transform vertices into world space
					for (int v = 0; v < 3; v++)
					{
						ht.v[v] = info.matObj.TransformPoint(ht.v[v]);

						const float length = ht.vn[v].GetLength();
						if (length < 0.9f || length > 1.1f)
							isFaceValid = false;

						ht.vn[v] = info.matObj.TransformVector(ht.vn[v]).GetNormalized();
					}

					if (!isFaceValid)
						continue;

					ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
					Plane pl;
					pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
					ht.n = pl.n;

					if (!ht.v[0].IsEquivalent(ht.v[1], epsilon) && !ht.v[1].IsEquivalent(ht.v[2], epsilon) && !ht.v[2].IsEquivalent(ht.v[0], epsilon))
						if ((ht.nTriArea || !GetCVars()->e_svoTI_ObjectsMaxViewDistance) && Overlap::AABB_Triangle(cloudBoxEX, ht.v[0], ht.v[1], ht.v[2]))
						{
							bool bSkipUnderTerrain = Get3DEngine()->m_bShowTerrainSurface && !info.bIndoor && (!GetCVars()->e_svoTI_VoxelizeUnderTerrain || info.bVegetation);

							if (bSkipUnderTerrain)
							{
								for (int h = 0; h < 3; h++)
								{
									Vec3 vH = ht.v[h];
									vH.CheckMax(cloudBoxWS.min);
									vH.CheckMin(cloudBoxWS.max);
									if (vH.z > (GetTerrain()->GetZ((int)vH.x, (int)vH.y) - 1.f) || GetTerrain()->GetHole((int)vH.x, (int)vH.y))
									{
										bSkipUnderTerrain = false;
										break;
									}
								}
							}

							if (!bSkipUnderTerrain)
							{
								superMesh.AddSuperTriangle(ht, arrVertHash, info.objectLayerId);
							}
						}
				}

				{
					AUTO_MODIFYLOCK(m_superMeshLock);

					int meshMemoryAllocated = m_pTrisInArea ? (m_pTrisInArea->ComputeSizeInMemory() + m_pVertInArea->ComputeSizeInMemory()) : 0;
					int meshMemoryToAppend = superMesh.m_pTrisInArea ? (superMesh.m_pTrisInArea->ComputeSizeInMemory() + superMesh.m_pVertInArea->ComputeSizeInMemory()) : 0;

					if ((meshMemoryAllocated + meshMemoryToAppend) > GetCVars()->e_svoMaxAreaMeshSizeKB * 1024)
					{
						// skip low importance objects
						break;
					}

					AddSuperMesh(superMesh, SVO_OFFSET_MESH);
				}
			}

			timeRayIntersection = Cry3DEngineBase::GetTimer()->GetAsyncCurTime() - timeRayIntersection;

			if (GetCVars()->e_svoDebug)
			{
				AUTO_MODIFYLOCK(CVoxelSegment::m_cgfTimeStatsLock);
				m_cgfTimeStats[info.pStatObj] += timeRayIntersection;
			}
		}

		m_areaObjects = nullptr;

		if (GetSubSetsNum() > 1)
		{
			AABB cloudBoxWS_VisAreaEx = cloudBoxWS;
			cloudBoxWS_VisAreaEx.Expand(Vec3(SVO_OFFSET_VISAREA, SVO_OFFSET_VISAREA, SVO_OFFSET_VISAREA));

			// add visarea shapes
			for (int v = 0; !m_isLowLodNode; v++)
			{
				superMesh.Clear(&arrVertHash[0][0][0]);

				CVisArea* pVisArea = (CVisArea*)GetVisAreaManager()->GetVisAreaById(v);
				if (!pVisArea)
					break;

				if (pVisArea->IsPortal() || !Overlap::AABB_AABB(*pVisArea->GetAABBox(), cloudBoxWS_VisAreaEx))
					continue;

				size_t nPoints = 0;
				const Vec3* pPoints = 0;
				pVisArea->GetShapePoints(pPoints, nPoints);
				float height = pVisArea->GetHeight();

				SRayHitTriangle ht;
				ZeroStruct(ht);
				ht.c[0] = ht.c[1] = ht.c[2] = Col_Black;
				ht.nOpacity = 255;
				ht.nHitObjType = HIT_OBJ_TYPE_VISAREA;
				Plane pl;

				// sides
				for (size_t i = 0; i < nPoints; i++)
				{
					const Vec3& v0 = (pPoints)[i];
					const Vec3& v1 = (pPoints)[(i + 1) % nPoints];

					ht.v[0] = v0;
					ht.v[1] = v0 + Vec3(0, 0, height);
					ht.v[2] = v1;

					if (Overlap::AABB_Triangle(cloudBoxWS_VisAreaEx, ht.v[0], ht.v[1], ht.v[2]))
					{
						ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
						pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
						ht.n = pl.n;

						superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
					}

					ht.v[0] = v1;
					ht.v[1] = v0 + Vec3(0, 0, height);
					ht.v[2] = v1 + Vec3(0, 0, height);

					if (Overlap::AABB_Triangle(cloudBoxWS_VisAreaEx, ht.v[0], ht.v[1], ht.v[2]))
					{
						ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
						pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
						ht.n = pl.n;

						superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
					}
				}

				// top and bottom
				for (float h = 0; fabs(h) <= fabs(height); h += height)
				{
					for (int p = 0; p < ((int)nPoints - 2l); p++)
					{
						ht.v[0] = (pPoints)[0 + 0] + Vec3(0, 0, h);
						ht.v[1] = (pPoints)[p + 1] + Vec3(0, 0, h);
						ht.v[2] = (pPoints)[p + 2] + Vec3(0, 0, h);

						if (Overlap::AABB_Triangle(cloudBoxWS_VisAreaEx, ht.v[0], ht.v[1], ht.v[2]))
						{
							ht.nTriArea = SATURATEB(int(SVO_AREA_SCALE * 0.5f * (ht.v[1] - ht.v[0]).Cross(ht.v[2] - ht.v[0]).GetLength()));
							pl.SetPlane(ht.v[0], ht.v[1], ht.v[2]);
							ht.n = pl.n;

							superMesh.AddSuperTriangle(ht, arrVertHash, kAllObjectLayersId);
						}
					}
				}

				AUTO_MODIFYLOCK(m_superMeshLock);

				AddSuperMesh(superMesh, SVO_OFFSET_VISAREA);
			}
		}

		m_voxTrisCounter += m_pTrisInArea ? m_pTrisInArea->Count() : 0;

		//    if(m_nodeTrisAllMerged.Count())
		//      PrintMessage("VoxelizeMeshes: %d tris found for node id %d (size=%d) in %.2f sec", m_nodeTrisAllMerged.Count(), m_nId, (int)GetBoxSize(), GetCurAsyncTimeSec() - fStartTimeAll);

		// one static BVH per area parent, straight into the STATIC segment (rt decisions 02, 03)
		BuildStaticBVH();

		{
			AUTO_READLOCK(m_superMeshLock);

			if (m_pTrisInArea)
			{
				for (int t = 0; t < m_pTrisInArea->Count(); t++)
				{
					AddTriangle(m_pTrisInArea->GetAt(t), t, rpNodeTrisXYZ, m_pVertInArea);
				}
			}
		}
	}
	else if (m_pParentCloud->m_pTrisInArea)
	{
		// copy some tris from parent
		//		FUNCTION_PROFILER_3DENGINE;

		m_pTrisInArea = m_pParentCloud->m_pTrisInArea;
		m_pVertInArea = m_pParentCloud->m_pVertInArea;
		m_pMatsInArea = m_pParentCloud->m_pMatsInArea;
		m_bExternalData = true;

		AUTO_READLOCK(m_superMeshLock);

		if (gEnv->IsEditor() && (gSvoEnv->m_streamingStartTime < 0) && !m_bExportMode)
		{
			// slow but reliable for editing
			for (int trId = 0; trId < (*m_pTrisInArea).Count(); trId++)
			{
				const SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[trId];

				Vec3 arrV[3] = { (*m_pVertInArea)[tr.arrVertId[0]].v, (*m_pVertInArea)[tr.arrVertId[1]].v, (*m_pVertInArea)[tr.arrVertId[2]].v };

				if (Overlap::AABB_Triangle(cloudBoxEX, arrV[0], arrV[1], arrV[2])) // 20ms
				{
					AddTriangle(tr, trId, rpNodeTrisXYZ, m_pVertInArea);
				}
			}
		}
		else
		{
			// fast but not reliable for editing - content of m_pTrisInArea may not much m_nodeTrisAllMerged
			for (int d = 0; d < m_pParentCloud->m_nodeTrisAllMerged.Count(); d++)
			{
				int trId = m_pParentCloud->m_nodeTrisAllMerged.GetAt(d);

				if (trId >= (*m_pTrisInArea).Count())
				{
					PrintMessage("%s warning: trId>=(*m_pTrisInArea).Count()", __FUNC__);
					break;
				}

				const SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[trId];

				Vec3 arrV[3] = { (*m_pVertInArea)[tr.arrVertId[0]].v, (*m_pVertInArea)[tr.arrVertId[1]].v, (*m_pVertInArea)[tr.arrVertId[2]].v };

				if (Overlap::AABB_Triangle(cloudBoxEX, arrV[0], arrV[1], arrV[2])) // 20ms
				{
					AddTriangle(tr, trId, rpNodeTrisXYZ, m_pVertInArea);
				}
			}
		}
	}
}

void CVoxelSegment::AddTriangle(const SRayHitTriangleIndexed& tr, int trId, PodArray<int>*& rpNodeTrisXYZ, PodArrayRT<SRayHitVertex>* pVertInArea)
{
	Vec3 arrV[3] = { (*pVertInArea)[tr.arrVertId[0]].v, (*pVertInArea)[tr.arrVertId[1]].v, (*pVertInArea)[tr.arrVertId[2]].v };

	for (int v = 0; v < 3; v++)
		m_boxTris.Add(arrV[v]);

	AABB triBox(arrV[0], arrV[0]);
	triBox.Add(arrV[2]);
	triBox.Add(arrV[1]);

	const float voxSizeExpand = GetBoxSize() / (float)SVO_VOX_BRICK_MAX_SIZE / 8.f;
	triBox.Expand(Vec3(voxSizeExpand, voxSizeExpand, voxSizeExpand)); // for RT

	AABB nodeBoxWS = m_boxOS;
	nodeBoxWS.min += m_vSegOrigin;
	nodeBoxWS.max += m_vSegOrigin;

	// safety border support
	Vec3 vBoxSize = nodeBoxWS.GetSize();
	//    nodeBoxWS.min -= (vBoxSize/kVoxTexMaxDim)/2;
	//    nodeBoxWS.max += (vBoxSize/kVoxTexMaxDim)/2;

	int x0 = clamp_tpl<int>((int)(((triBox.min.x - nodeBoxWS.min.x) / vBoxSize.x * SVO_VOX_BRICK_MAX_SIZE)), (int)0, SVO_VOX_BRICK_MAX_SIZE - 1);
	int x1 = clamp_tpl<int>((int)(((triBox.max.x - nodeBoxWS.min.x) / vBoxSize.x * SVO_VOX_BRICK_MAX_SIZE)), (int)0, SVO_VOX_BRICK_MAX_SIZE - 1);

	int y0 = clamp_tpl<int>((int)(((triBox.min.y - nodeBoxWS.min.y) / vBoxSize.y * SVO_VOX_BRICK_MAX_SIZE)), (int)0, SVO_VOX_BRICK_MAX_SIZE - 1);
	int y1 = clamp_tpl<int>((int)(((triBox.max.y - nodeBoxWS.min.y) / vBoxSize.y * SVO_VOX_BRICK_MAX_SIZE)), (int)0, SVO_VOX_BRICK_MAX_SIZE - 1);

	int z0 = clamp_tpl<int>((int)(((triBox.min.z - nodeBoxWS.min.z) / vBoxSize.z * SVO_VOX_BRICK_MAX_SIZE)), (int)0, SVO_VOX_BRICK_MAX_SIZE - 1);
	int z1 = clamp_tpl<int>((int)(((triBox.max.z - nodeBoxWS.min.z) / vBoxSize.z * SVO_VOX_BRICK_MAX_SIZE)), (int)0, SVO_VOX_BRICK_MAX_SIZE - 1);

	for (int z = z0; z <= z1; z++)
	{
		for (int y = y0; y <= y1; y++)
		{
			for (int x = x0; x <= x1; x++)
			{
				rpNodeTrisXYZ[z * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE + y * SVO_VOX_BRICK_MAX_SIZE + x].Add(trId);
			}
		}
	}

	m_nodeTrisAllMerged.Add(trId);

	AABB boxWS = m_boxOS;
	boxWS.min += m_vSegOrigin;
	boxWS.max += m_vSegOrigin;

	for (int childId = 0; childId < 8; childId++)
	{
		if (m_dwChildTrisTest & (1 << childId))
			continue;

		AABB childBox = CVoxelSegment::GetChildBBox(boxWS, childId);

		childBox.min -= (vBoxSize / (float)SVO_VOX_BRICK_MAX_SIZE) / 4.0f;
		childBox.max += (vBoxSize / (float)SVO_VOX_BRICK_MAX_SIZE) / 4.0f;

		if (Overlap::AABB_Triangle(childBox, arrV[0], arrV[1], arrV[2]))
			m_dwChildTrisTest |= (1 << childId);
	}

	if (m_objLayerMap.find(tr.objectLayerId) == m_objLayerMap.end())
	{
		//    if (tr.nObjLayerId)
		//    {
		//      PrintMessage("Allocating layer id %d", tr.nObjLayerId);
		//    }

		SVoxBrick voxData;
		CheckAllocateSubSets(voxData, SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE);

		m_objLayerMap[tr.objectLayerId] = voxData;
	}
}

void CVoxelSegment::SetVoxCamera(const CCamera& newCam)
{
	CVoxelSegment::m_voxCam = newCam;

	if (gSvoEnv)
	{
		gSvoEnv->m_debugDrawVoxelsCounter = 0;
	}
}

ColorF CVoxelSegment::ProcessMaterial(const SRayHitTriangleIndexed& tr, const Vec3& vHitPos)
{
	ColorF colVert = Col_White;
	Vec2 vHitTC(0, 0);

	SSvoMatInfo& rMI = m_pMatsInArea->GetAt(tr.materialID);

	SShaderItem* pShItem = rMI.pMat ? &rMI.pMat->GetShaderItem() : nullptr;

	Vec3 arrV[3] = { (*m_pVertInArea)[tr.arrVertId[0]].v, (*m_pVertInArea)[tr.arrVertId[1]].v, (*m_pVertInArea)[tr.arrVertId[2]].v };

	float w0 = 0, w1 = 0, w2 = 0;
	if (GetBarycentricTC(arrV[0], arrV[1], arrV[2], w0, w1, w2, vHitPos, 2.f))
	{
		Vec2 arrT[3] = { (*m_pVertInArea)[tr.arrVertId[0]].t, (*m_pVertInArea)[tr.arrVertId[1]].t, (*m_pVertInArea)[tr.arrVertId[2]].t };

		vHitTC = arrT[0] * w0 + arrT[1] * w1 + arrT[2] * w2;

		if (GetSubSetsNum() > 1 && (!(pShItem && pShItem->m_pShader) || pShItem->m_pShader->GetFlags2() & EF2_VERTEXCOLORS))
		{
			ColorB arrC[3] = { (*m_pVertInArea)[tr.arrVertId[0]].c, (*m_pVertInArea)[tr.arrVertId[1]].c, (*m_pVertInArea)[tr.arrVertId[2]].c };

			if (!(pShItem && pShItem->m_pShader) || pShItem->m_pShader->GetShaderType() != eST_Vegetation)
			{
				Vec4 c0 = arrC[0].toVec4();
				Vec4 c1 = arrC[1].toVec4();
				Vec4 c2 = arrC[2].toVec4();

				Vec4 colInter = c0 * w0 + c1 * w1 + c2 * w2;

				if (pShItem)
				{
					// swap r and b
					colVert.r = 1.f / 255.f * colInter.z;
					colVert.g = 1.f / 255.f * colInter.y;
					colVert.b = 1.f / 255.f * colInter.x;
				}
				else
				{
					colVert.r = 1.f / 255.f * colInter.x;
					colVert.g = 1.f / 255.f * colInter.y;
					colVert.b = 1.f / 255.f * colInter.z;
				}
			}
		}
	}
	else
	{
		colVert = Col_DimGray;
	}

	ColorF colTex = Col_Gray;

	ColorB* pTexRgb = 0;
	int texWidth = 0, texHeight = 0;

	if (rMI.pMat)
	{
		// objects
		pTexRgb = rMI.pTexRgb;
		texWidth = rMI.textureWidth;
		texHeight = rMI.textureHeight;
	}
	else if (const PodArray<ColorB>* pTerrLowResTex = GetTerrain()->GetTerrainRgbLowResSystemCopy())
	{
		// terrain
		texWidth = texHeight = (int)sqrt((float)pTerrLowResTex->Count());
		pTexRgb = (ColorB*)pTerrLowResTex->GetElements();
	}

	if (pTexRgb)
	{
		if (rMI.pMat)
		{
			Vec4 vTextureAtlasInfo(0, 0, 1, 1);
			//      vTextureAtlasInfo.x = pResTexture->GetOffset(0);
			//      vTextureAtlasInfo.y = pResTexture->GetOffset(1);
			//      vTextureAtlasInfo.z = pResTexture->GetTiling(0);
			//      vTextureAtlasInfo.w = pResTexture->GetTiling(1);

			colTex = GetBilinearAt(
				vHitTC.x * vTextureAtlasInfo.z + vTextureAtlasInfo.x,
				vHitTC.y * vTextureAtlasInfo.w + vTextureAtlasInfo.y,
				pTexRgb, texWidth, texHeight, 1.f / 255.f);

			// ignore alpha if material do not use it
			if (pShItem && pShItem->m_pShaderResources && (pShItem->m_pShaderResources->GetAlphaRef() == 0.f) && (pShItem->m_pShaderResources->GetStrengthValue(EFTT_OPACITY) == 1.f))
				colTex.a = 1;
		}
		else
		{
			// terrain tex-gen
			int worldSize = GetTerrain()->GetTerrainSize();
			colTex = GetBilinearAt(
				vHitPos.y / worldSize,
				vHitPos.x / worldSize,
				pTexRgb, texWidth, texHeight, 1.f / 255.f);

			colTex *= GetTerrain()->GetTerrainTextureMultiplier();

			colTex.r = max(colTex.r, .02f);
			colTex.g = max(colTex.g, .02f);
			colTex.b = max(colTex.b, .02f);
			colTex.a = 1;
		}
	}

	ColorF colMat = (pShItem && pShItem->m_pShaderResources) ? pShItem->m_pShaderResources->GetColorValue(EFTT_DIFFUSE) : Col_White;

	ColorF colRes = colTex * colMat * colVert;

	return colRes;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - record packing (rt decision 02, 2.5)
//
// Every helper below is the exact inverse of the shader side helper named in its comment
// (CommonSVO_RT.cfi). The record pool is R32G32B32A32F and is read with Load() only, so the
// bit patterns produced by RT_PackU16x2 (which may be denormals or NaNs) survive untouched.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static ILINE float RT_AsFloat(uint32 v)
{
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static ILINE uint32 RT_AsUint(float f)
{
	uint32 v;
	memcpy(&v, &f, sizeof(v));
	return v;
}

//! Inverse of ExtractPosXY's asuint split: low 16 bits = x, high 16 bits = y.
static ILINE float RT_PackU16x2(uint32 a, uint32 b)
{
	return RT_AsFloat((a & 0xffff) | ((b & 0xffff) << 16));
}

//! Quantise one position component; the consumer computes pos.xy = (q / 65536 - 0.5) * qb.w + qb.xy
static ILINE uint32 RT_QuantPos(float value, float center, float size)
{
	const float q = ((value - center) / size + 0.5f) * 65536.f;
	return (uint32)CLAMP((int)floor(q + 0.5f), 0, 65535);
}

static ILINE float RT_UnquantPos(uint32 q, float center, float size)
{
	return ((float)q / 65536.f - 0.5f) * size + center;
}

//! Inverse of ExtractDir: n = qx + qy*256 + qz*65536, q = round((v*0.5+0.5)*255); the sign carries handedness.
static ILINE float RT_PackDir(const Vec3& vIn, float handedness)
{
	Vec3 v = vIn;
	if (v.GetLengthSquared() < 1e-12f)
		v = Vec3(0, 0, 1);
	else
		v.Normalize();

	const int qx = CLAMP((int)floor((v.x * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
	const int qy = CLAMP((int)floor((v.y * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
	const int qz = CLAMP((int)floor((v.z * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);

	int n = qx + qy * 256 + qz * 65536;

	// the consumer recovers the sign as "f > 0 ? +1 : -1", so zero must never be emitted
	if (n == 0)
		n = 1;

	return (handedness >= 0.f) ? (float)n : -((float)n);
}

//! Largest value PackTC16 can represent without wrapping.
#define RT_TC16_MAX (16.f - 1.f / 4096.f)

//! Inverse of ExtractTC16: two 16 bit fixed point fields over [0, 16), packed like positions.
static ILINE float RT_PackTC16(float u, float v)
{
	const int qu = CLAMP((int)floor(u / 16.f * 65535.f + 0.5f), 0, 65535);
	const int qv = CLAMP((int)floor(v / 16.f * 65535.f + 0.5f), 0, 65535);
	return RT_PackU16x2((uint32)qu, (uint32)qv);
}

//! Inverse of ExtractUint2: base 4096, high field first.
static ILINE float RT_PackUint2(int high, int low)
{
	return (float)(CLAMP(high, 0, 4095) * 4096 + CLAMP(low, 0, 4095));
}

//! One 4 texel triangle record (rt decision 02, 2.5). Shared by the static per cell builder and the
//! per frame dynamic builder, so both produce byte identical triangles for the same input.
static void RT_WriteTriRecord(const SRTBuildTri& tr, const Vec4& qb, int matRecord, Vec4* pTri)
{
	// per triangle tangent frame from the UV gradient; the consumer rebuilds the
	// geometric normal as normalize(cross(tangent, bitangent) * handedness)
	const Vec3  e1 = tr.v[1] - tr.v[0];
	const Vec3  e2 = tr.v[2] - tr.v[0];
	const Vec2  d1 = tr.t[1] - tr.t[0];
	const Vec2  d2 = tr.t[2] - tr.t[0];
	const float det = d1.x * d2.y - d2.x * d1.y;

	Vec3 vT, vB;
	if (fabs(det) > 1e-12f)
	{
		const float r = 1.f / det;
		vT = (e1 * d2.y - e2 * d1.y) * r;
		vB = (e2 * d1.x - e1 * d2.x) * r;
	}
	else
	{
		vT = e1;
		vB = e2;
	}

	for (int v = 0; v < 3; v++)
	{
		const Vec3 vN = tr.n[v].GetNormalizedSafe(tr.faceNorm.GetNormalizedSafe(Vec3(0, 0, 1)));

		Vec3 vTan = vT - vN * vN.Dot(vT);
		if (vTan.GetLengthSquared() < 1e-12f)
			vTan = vN.GetOrthogonal();
		vTan.Normalize();

		const float handedness = (vN.Cross(vTan).Dot(vB) < 0.f) ? -1.f : 1.f;
		const Vec3  vBit = vN.Cross(vTan) * handedness;

		const uint32 qx = RT_QuantPos(tr.v[v].x, qb.x, qb.w);
		const uint32 qy = RT_QuantPos(tr.v[v].y, qb.y, qb.w);

		pTri[v] = Vec4(RT_PackU16x2(qx, qy), RT_PackDir(vTan, handedness), RT_PackDir(vBit, 1.f), tr.v[v].z);
	}

	pTri[3] = Vec4((float)matRecord,
	               RT_PackTC16(tr.t[0].x, tr.t[0].y),
	               RT_PackTC16(tr.t[1].x, tr.t[1].y),
	               RT_PackTC16(tr.t[2].x, tr.t[2].y));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - binned SAH BVH builder (rt decision 03)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
struct SRTRef
{
	AABB box;
	Vec3 centroid;
	int  triId;
};

struct SRTNode
{
	AABB box;
	int  first;      //!< first reference of this node
	int  count;      //!< number of references
	int  child0;     //!< index into the node array, -1 on leaves
	int  child1;
	int  splitAxis;  //!< 0/1/2, -1 on leaves
	int  depth;
	int  record;     //!< absolute record index, filled by the layout pass
	int  subtreeEnd; //!< first record after this subtree
};

const int kRTSahBins = 16;

//! Top down binned SAH builder: 3 axes x 16 bins over the centroid bounds, Ct = Ci = 1,
//! exact child bounds, median fallback. Deterministic - no randomness, stable partition.
struct SRTBuilder
{
	PodArray<SRTRef>  refs;
	PodArray<SRTNode> nodes;
	int               leafTris;
	int               maxDepth;
	int               maxDepthSeen;
	int               maxLeafTris;
	int               leafCount;

	static bool CompareRefX(const SRTRef& a, const SRTRef& b) { return (a.centroid.x != b.centroid.x) ? (a.centroid.x < b.centroid.x) : (a.triId < b.triId); }
	static bool CompareRefY(const SRTRef& a, const SRTRef& b) { return (a.centroid.y != b.centroid.y) ? (a.centroid.y < b.centroid.y) : (a.triId < b.triId); }
	static bool CompareRefZ(const SRTRef& a, const SRTRef& b) { return (a.centroid.z != b.centroid.z) ? (a.centroid.z < b.centroid.z) : (a.triId < b.triId); }

	static float SurfaceArea(const AABB& box)
	{
		if (box.min.x > box.max.x)
			return 0.f;
		const Vec3 d = box.max - box.min;
		return 2.f * (d.x * d.y + d.y * d.z + d.z * d.x);
	}

	int AddNode(int first, int count, int depth)
	{
		SRTNode n;
		n.box.Reset();
		for (int i = first; i < first + count; i++)
			n.box.Add(refs[i].box);
		n.first = first;
		n.count = count;
		n.child0 = n.child1 = -1;
		n.splitAxis = -1;
		n.depth = depth;
		n.record = 0;
		n.subtreeEnd = 0;
		nodes.Add(n);
		return nodes.Count() - 1;
	}

	void MakeLeaf(int nodeId)
	{
		SRTNode& n = nodes[nodeId];
		n.child0 = n.child1 = -1;
		n.splitAxis = -1;
		leafCount++;
		maxDepthSeen = max(maxDepthSeen, n.depth);
		maxLeafTris = max(maxLeafTris, n.count);
	}

	void Split(int nodeId)
	{
		const int first = nodes[nodeId].first;
		const int count = nodes[nodeId].count;
		const int depth = nodes[nodeId].depth;

		if (count <= leafTris || depth >= maxDepth || count < 2)
		{
			MakeLeaf(nodeId);
			return;
		}

		AABB centroidBox;
		centroidBox.Reset();
		for (int i = first; i < first + count; i++)
			centroidBox.Add(refs[i].centroid);

		const Vec3  cExtent = centroidBox.max - centroidBox.min;
		const float parentArea = SurfaceArea(nodes[nodeId].box);

		int   bestAxis = -1;
		int   bestBin = -1;
		float bestCost = (float)count;   // leaf cost with Ct = Ci = 1

		if (parentArea > 0.f)
		{
			for (int axis = 0; axis < 3; axis++)
			{
				const float lo = centroidBox.min[axis];
				const float extent = cExtent[axis];
				if (extent <= 0.f)
					continue;

				const float k = (float)kRTSahBins / extent;

				AABB binBox[kRTSahBins];
				int  binCount[kRTSahBins];
				for (int b = 0; b < kRTSahBins; b++)
				{
					binBox[b].Reset();
					binCount[b] = 0;
				}

				for (int i = first; i < first + count; i++)
				{
					const int b = CLAMP((int)((refs[i].centroid[axis] - lo) * k), 0, kRTSahBins - 1);
					binBox[b].Add(refs[i].box);
					binCount[b]++;
				}

				// accumulate the right side first
				AABB rightBox[kRTSahBins];
				int  rightCount[kRTSahBins];
				AABB acc;
				acc.Reset();
				int accCount = 0;
				for (int b = kRTSahBins - 1; b >= 1; b--)
				{
					if (binCount[b])
						acc.Add(binBox[b]);
					accCount += binCount[b];
					rightBox[b] = acc;
					rightCount[b] = accCount;
				}

				// sweep left to right and evaluate every plane
				AABB leftAcc;
				leftAcc.Reset();
				int leftCount = 0;
				for (int b = 0; b < kRTSahBins - 1; b++)
				{
					if (binCount[b])
						leftAcc.Add(binBox[b]);
					leftCount += binCount[b];

					if (leftCount == 0 || rightCount[b + 1] == 0)
						continue;

					const float cost = 1.f + (SurfaceArea(leftAcc) * leftCount + SurfaceArea(rightBox[b + 1]) * rightCount[b + 1]) / parentArea;

					if (cost < bestCost)
					{
						bestCost = cost;
						bestAxis = axis;
						bestBin = b;
					}
				}
			}
		}

		int mid = -1;
		int splitAxis = bestAxis;

		if (bestAxis >= 0)
		{
			const float lo = centroidBox.min[bestAxis];
			const float k = (float)kRTSahBins / cExtent[bestAxis];

			// stable partition; child0 keeps the lower half along the split axis
			PodArray<SRTRef> tmp;
			tmp.PreAllocate(count, 0);
			for (int i = first; i < first + count; i++)
			{
				const int b = CLAMP((int)((refs[i].centroid[bestAxis] - lo) * k), 0, kRTSahBins - 1);
				if (b <= bestBin)
					tmp.Add(refs[i]);
			}
			const int leftCount = tmp.Count();
			for (int i = first; i < first + count; i++)
			{
				const int b = CLAMP((int)((refs[i].centroid[bestAxis] - lo) * k), 0, kRTSahBins - 1);
				if (b > bestBin)
					tmp.Add(refs[i]);
			}
			memcpy(refs.GetElements() + first, tmp.GetElements(), sizeof(SRTRef) * count);
			mid = first + leftCount;
		}
		else
		{
			// median fallback: coincident centroids, or no plane was cheaper than the leaf
			if (count <= leafTris)
			{
				MakeLeaf(nodeId);
				return;
			}

			splitAxis = 0;
			if (cExtent.y > cExtent[splitAxis])
				splitAxis = 1;
			if (cExtent.z > cExtent[splitAxis])
				splitAxis = 2;

			SRTRef* pBegin = refs.GetElements() + first;
			if (splitAxis == 0)
				std::stable_sort(pBegin, pBegin + count, CompareRefX);
			else if (splitAxis == 1)
				std::stable_sort(pBegin, pBegin + count, CompareRefY);
			else
				std::stable_sort(pBegin, pBegin + count, CompareRefZ);

			mid = first + count / 2;
		}

		if (mid <= first || mid >= first + count)
		{
			MakeLeaf(nodeId);
			return;
		}

		const int child0 = AddNode(first, mid - first, depth + 1);
		const int child1 = AddNode(mid, first + count - mid, depth + 1);

		nodes[nodeId].child0 = child0;
		nodes[nodeId].child1 = child1;
		nodes[nodeId].splitAxis = splitAxis;

		Split(child0);
		Split(child1);
	}

	//! Assigns record indices depth first; a leaf's triangle records follow its node record immediately.
	int Layout(int nodeId, int rec)
	{
		nodes[nodeId].record = rec;
		rec++;

		if (nodes[nodeId].child0 < 0)
		{
			rec += nodes[nodeId].count;
		}
		else
		{
			rec = Layout(nodes[nodeId].child0, rec);
			rec = Layout(nodes[nodeId].child1, rec);
		}

		nodes[nodeId].subtreeEnd = rec;
		return rec;
	}
};
}

bool CVoxelSegment::BuildStaticBVHRecords(const PodArray<SRTBuildTri>& arrTris, const Vec4& qb, int recordBase, PodArray<Vec4>& arrOut, PodArray<int>* pRelocFloats, SRTBuildStats& stats)
{
	if (!arrTris.Count())
		return false;

	SRTBuilder bld;
	bld.leafTris = max(1, Cry3DEngineBase::GetCVars()->e_svoTI_RT_LeafTris);
	bld.maxDepth = CLAMP(Cry3DEngineBase::GetCVars()->e_svoTI_RT_MaxDepth, 1, 18);
	bld.maxDepthSeen = 0;
	bld.maxLeafTris = 0;
	bld.leafCount = 0;

	bld.refs.PreAllocate(arrTris.Count(), 0);
	for (int i = 0; i < arrTris.Count(); i++)
	{
		const SRTBuildTri& tr = arrTris[i];
		SRTRef ref;
		ref.box.Reset();
		ref.box.Add(tr.v[0]);
		ref.box.Add(tr.v[1]);
		ref.box.Add(tr.v[2]);
		ref.centroid = (tr.v[0] + tr.v[1] + tr.v[2]) / 3.f;
		ref.triId = i;
		bld.refs.Add(ref);
	}

	bld.nodes.PreAllocate(arrTris.Count() * 2 + 8, 0);
	const int rootNode = bld.AddNode(0, bld.refs.Count(), 0);
	bld.Split(rootNode);

	const int recordsEnd = bld.Layout(rootNode, recordBase);
	const int recordCount = recordsEnd - recordBase;

	const int texelBase = arrOut.Count();
	arrOut.PreAllocate(texelBase + recordCount * SVO_RT_RECORD_TEXELS, texelBase + recordCount * SVO_RT_RECORD_TEXELS);

	for (int n = 0; n < bld.nodes.Count(); n++)
	{
		const SRTNode& node = bld.nodes[n];
		const bool    isLeaf = (node.child0 < 0);
		const int     nodeTexel = texelBase + (node.record - recordBase) * SVO_RT_RECORD_TEXELS;

		Vec4* pRec = arrOut.GetElements() + nodeTexel;

		pRec[0] = Vec4(node.box.min, isLeaf ? (float)node.count : 0.f);
		pRec[1] = Vec4(node.box.max, (float)node.subtreeEnd);
		pRec[2] = qb;
		pRec[3] = Vec4(isLeaf ? 0.f : (float)bld.nodes[node.child0].record,
		               isLeaf ? 0.f : (float)bld.nodes[node.child1].record,
		               0.f,
		               isLeaf ? -1.f : (float)node.splitAxis);

		if (pRelocFloats)
		{
			pRelocFloats->Add(nodeTexel * 4 + 7);          // texel 1 .w = subtreeEnd
			if (!isLeaf)
			{
				pRelocFloats->Add(nodeTexel * 4 + 12);     // texel 3 .x = child0
				pRelocFloats->Add(nodeTexel * 4 + 13);     // texel 3 .y = child1
			}
		}

		if (!isLeaf)
			continue;

		for (int t = 0; t < node.count; t++)
		{
			const SRTBuildTri& tr = arrTris[bld.refs[node.first + t].triId];
			const int          triTexel = nodeTexel + (1 + t) * SVO_RT_RECORD_TEXELS;
			Vec4*              pTri = arrOut.GetElements() + triTexel;

			RT_WriteTriRecord(tr, qb, tr.matRecord, pTri);

			if (pRelocFloats)
				pRelocFloats->Add(triTexel * 4 + 12);      // texel 3 .x = matRecord
		}
	}

	stats.tris += arrTris.Count();
	stats.nodes += bld.nodes.Count();
	stats.leaves += bld.leafCount;
	stats.records += recordCount;
	stats.maxDepth = max(stats.maxDepth, bld.maxDepthSeen);
	stats.maxLeafTris = max(stats.maxLeafTris, bld.maxLeafTris);

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - low resolution texture copy prefetch (report 06c S3, report 06d fix 3a)
//
// CTexture::GetLowResSystemCopy is a SYNCHRONOUS EF_LoadImage (a DDS read off disk) plus a BC
// decompress on the calling thread. Stock voxelization asked for one 32x32 copy per material; the
// ray tracing producer asks for up to six roles per material at 256x256, from every job worker at
// once, inside the soup read lock. That is what turned a re-voxelization into minutes of saturated
// disk queue and starved cores.
//
// Here one dedicated worker owns every miss. A producer that asks for a texture whose copy is not
// cached yet gets "not ready", emits the material with no texture for that slot, and carries on; the
// cell's material records are patched in place (CSvoEnv::RTApplyMatPatches) once the copies land.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
class CRTTexPrefetchThread final : public IThread
{
public:
	CRTTexPrefetchThread() : m_semaphore(4096), m_bRun(true) {}

	virtual void ThreadEntry() override
	{
		MEMSTAT_CONTEXT(EMemStatContextType::Other, "SvoRTTexPrefetch");

		while (m_bRun)
		{
			m_semaphore.Acquire();

			if (!m_bRun)
				break;

			_smart_ptr<ITexture> pTex;

			{
				CryAutoLock<CryCriticalSection> lock(m_lock);

				if (m_queue.empty())
					continue;

				pTex = m_queue.front();
				m_queue.pop_front();
			}

			if (pTex)
			{
				uint16 w = 0, h = 0;
				int*   pSlotId = nullptr;

				// the only place in the RT path that is allowed to pay for a miss
				pTex->GetLowResSystemCopy(w, h, &pSlotId, gSvoEnv ? gSvoEnv->GetRTTexRes() : 256);
			}

			{
				CryAutoLock<CryCriticalSection> lock(m_lock);

				// "ready" means ATTEMPTED: a texture that cannot produce a copy at all must not be
				// requested again every frame for the rest of the level
				m_ready.insert(pTex.get());
				m_pending.erase(pTex.get());
				m_done++;
			}
		}
	}

	void SignalStopWork()
	{
		m_bRun = false;
		m_semaphore.Release();
	}

	bool IsReady(ITexture* pTex)
	{
		CryAutoLock<CryCriticalSection> lock(m_lock);
		return m_ready.find(pTex) != m_ready.end();
	}

	//! true when it is already there; otherwise the request is queued (once) and false is returned
	bool Request(ITexture* pTex)
	{
		{
			CryAutoLock<CryCriticalSection> lock(m_lock);

			if (m_ready.find(pTex) != m_ready.end())
				return true;

			if (m_pending.find(pTex) != m_pending.end())
				return false;

			m_pending.insert(pTex);
			m_queue.push_back(_smart_ptr<ITexture>(pTex));
		}

		m_semaphore.Release();

		return false;
	}

	void GetStats(int& done, int& pending)
	{
		CryAutoLock<CryCriticalSection> lock(m_lock);
		done = m_done;
		pending = (int)m_pending.size();
	}

private:
	CryCriticalSection              m_lock;
	std::list<_smart_ptr<ITexture>> m_queue;
	std::set<ITexture*>             m_pending;
	std::set<ITexture*>             m_ready;
	CrySemaphore                    m_semaphore;
	volatile bool                   m_bRun;
	int                             m_done = 0;
};

CRTTexPrefetchThread* g_pRTTexPrefetch = nullptr;
}

void CVoxelSegment::RTStartTexPrefetch()
{
	if (g_pRTTexPrefetch)
		return;

	CRTTexPrefetchThread* pThread = new CRTTexPrefetchThread();

	if (!gEnv->pThreadManager->SpawnThread(pThread, "SvoRTTexPrefetch"))
	{
		delete pThread;
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
		           "SVO RT: could not spawn the texture prefetch worker, falling back to synchronous copies");
		return;
	}

	g_pRTTexPrefetch = pThread;
}

void CVoxelSegment::RTStopTexPrefetch()
{
	if (!g_pRTTexPrefetch)
		return;

	g_pRTTexPrefetch->SignalStopWork();
	gEnv->pThreadManager->JoinThread(g_pRTTexPrefetch, eJM_Join);

	delete g_pRTTexPrefetch;
	g_pRTTexPrefetch = nullptr;
}

bool CVoxelSegment::RTIsTexCopyReady(ITexture* pTex)
{
	if (!pTex || !g_pRTTexPrefetch || !GetCVars()->e_svoTI_RT_TexPrefetch)
		return true;

	return g_pRTTexPrefetch->IsReady(pTex);
}

bool CVoxelSegment::RTRequestTexCopy(ITexture* pTex)
{
	if (!pTex)
		return true;

	if (!g_pRTTexPrefetch || !GetCVars()->e_svoTI_RT_TexPrefetch)
		return true;   // the old synchronous behaviour: the caller loads it itself

	return g_pRTTexPrefetch->Request(pTex);
}

void CVoxelSegment::RTGetTexPrefetchStats(int& done, int& pending)
{
	done = pending = 0;

	if (g_pRTTexPrefetch)
		g_pRTTexPrefetch->GetStats(done, pending);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - material texture atlas (rt decision 04)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//! Nearest neighbour index when a source map of another size is written into a dst of dstSize.
//! Factored out (and self tested) because getting it wrong shifts a whole gloss map by half a texel.
static ILINE int RT_ResampleIndex(int dst, int dstSize, int srcSize)
{
	if (srcSize == dstSize || dstSize <= 0)
		return CLAMP(dst, 0, max(0, srcSize - 1));

	return CLAMP((int)(((int64)dst * srcSize) / dstSize), 0, max(0, srcSize - 1));
}

//! Copies one low resolution texture into a free atlas slice (or reuses the one it already owns)
//! and returns the 1 based slice id the material record stores; 0 means "no texture".
//!
//! eEncodeAs selects how the bytes are written into the slice and defaults to texSlot; it is passed
//! explicitly for the %BLENDLAYER set (rt decision 10), whose second normal map lives in
//! EFTT_CUSTOM_SECONDARY but has to be encoded exactly like an EFTT_NORMALS _ddna slice, with its own
//! smoothness donor (EFTT_DECAL_OVERLAY = Illum's smoothness2Tex) instead of EFTT_SMOOTHNESS.
int CVoxelSegment::CheckStoreTextureInPool(SShaderItem* pShItem, EEfResTextures texSlot, uint16& nTexW, uint16& nTexH, PodArray<int>& arrTexSlicesOut, EEfResTextures eEncodeAs, EEfResTextures eSmoothnessSlot, PodArray<ITexture*>* pDeferredOut)
{
	const int maxTexSizeXY = gSvoEnv->GetRTTexRes();

	const EEfResTextures eEnc = (eEncodeAs == EFTT_UNKNOWN) ? texSlot : eEncodeAs;

	const ColorB* pTexRgbOr = nullptr;
	int*          pSysTexId = nullptr;

	nTexW = nTexH = 0;

	// The normals encoding folds a second texture (the smoothness donor) into this slice, so both
	// have to be there before the slice is written - otherwise the gloss map would be baked in as a
	// flat 255 and the patch pass could not tell the difference afterwards.
	ITexture* pSmoothITex = nullptr;

	if (eEnc == EFTT_NORMALS && pShItem && pShItem->m_pShaderResources)
		if (SEfResTexture* pSmoothResTex = pShItem->m_pShaderResources->GetTexture(eSmoothnessSlot))
			pSmoothITex = pSmoothResTex->m_Sampler.m_pITex;

	if (pShItem)
	{
		if (pShItem->m_pShaderResources)
		{
			if (SEfResTexture* pResTexture = pShItem->m_pShaderResources->GetTexture(texSlot))
			{
				if (ITexture* pITex = pResTexture->m_Sampler.m_pITex)
				{
					{
						AUTO_MODIFYLOCK(m_arrLockedTextures.m_Lock);
						m_arrLockedTextures[pITex] = pITex;

						if (pSmoothITex)
							m_arrLockedTextures[pSmoothITex] = pSmoothITex;
					}

					// Ask the prefetch worker instead of loading and decompressing here. Both have to
					// be requested, not just the first missing one, or the two waits would serialise.
					bool bReady = RTRequestTexCopy(pITex);

					if (pSmoothITex && !RTRequestTexCopy(pSmoothITex))
						bReady = false;

					if (!bReady)
					{
						if (pDeferredOut)
						{
							pDeferredOut->Add(pITex);

							if (pSmoothITex)
								pDeferredOut->Add(pSmoothITex);
						}

						return 0;
					}

					pTexRgbOr = pITex->GetLowResSystemCopy(nTexW, nTexH, &pSysTexId, maxTexSizeXY);
				}
			}
		}
	}
	else if (texSlot == EFTT_DIFFUSE)
	{
		// terrain: the whole terrain diffuse is one square texture carrying its own atlas id slot
		if (const PodArray<ColorB>* pTerrLowResTex = GetTerrain()->GetTerrainRgbLowResSystemCopy(&pSysTexId))
		{
			nTexW = nTexH = (uint16)(int)sqrt((float)pTerrLowResTex->Count());
			pTexRgbOr = pTerrLowResTex->GetElements();
		}
	}

	if (!pTexRgbOr || !pSysTexId || !nTexW || !nTexH)
	{
		nTexW = nTexH = 0;
		return 0;
	}

	if (nTexW > maxTexSizeXY || nTexH > maxTexSizeXY)
	{
		// never overrun a slice; the material falls back to its tint
		nTexW = nTexH = 0;
		return 0;
	}

	AUTO_MODIFYLOCK(gSvoEnv->m_arrRTPoolTexs.m_Lock);

	if (*pSysTexId == 0)
	{
		const int newSlice = gSvoEnv->RTAllocTexSlice(pSysTexId);
		if (newSlice < 0)
		{
			nTexW = nTexH = 0;
			return 0;
		}

		ColorB* pDst = gSvoEnv->m_arrRTPoolTexs.GetElements() + (size_t)newSlice * maxTexSizeXY * maxTexSizeXY;

		// Gloss at a ray traced hit (report 02d 3, route 1). CE ships _ddna as BC5U and
		// DXTDecompressRow writes a LITERAL 255 into alpha for a <= 3 channel source
		// (Renderer.cpp), so this slice's alpha carried no smoothness at all and the hit only ever
		// saw the material's scalar. CE's real gloss is a separate texture: Illum.cfx
		// GetSurfaceAttributes (:479) computes
		//     attribs.Smoothness = MatSpecColor.w * GetSmoothnessTex(smoothnessTex, baseTC)
		// and GetSmoothnessTex (Common.cfi:146) returns the map's .r - a MULTIPLY, not a replace.
		// The consumer already forms matInfo1.z * saturate(vNor.w) with matInfo1.z = MatSpecColor.w,
		// so writing the smoothness map's red channel into this alpha reproduces the primary
		// surface exactly, and 255 (no map) degrades to the scalar, i.e. to today's behaviour.
		// Terrain.cfx and Vegetation.cfx use the same product, so this holds for them too.
		const ColorB* pSmoothSrc = nullptr;
		uint16        smoothW = 0, smoothH = 0;

		if (eEnc == EFTT_NORMALS && pShItem && pShItem->m_pShaderResources)
		{
			if (SEfResTexture* pSmoothResTex = pShItem->m_pShaderResources->GetTexture(eSmoothnessSlot))
			{
				if (ITexture* pSmoothITex = pSmoothResTex->m_Sampler.m_pITex)
				{
					{
						AUTO_MODIFYLOCK(m_arrLockedTextures.m_Lock);
						m_arrLockedTextures[pSmoothITex] = pSmoothITex;
					}

					int* pSmoothSlotId = nullptr;
					pSmoothSrc = pSmoothITex->GetLowResSystemCopy(smoothW, smoothH, &pSmoothSlotId, maxTexSizeXY);

					// no slice is allocated for it - it only donates one channel to this one
					if (!smoothW || !smoothH)
						pSmoothSrc = nullptr;
				}
			}
		}

		for (int lineId = 0; lineId < nTexH; lineId++)
		{
			const ColorB* pSrcLine = pTexRgbOr + lineId * nTexW;
			ColorB*       pDstLine = pDst + lineId * maxTexSizeXY;

			if (eEnc == EFTT_NORMALS)
			{
				// CE _ddna normals are BC5; DXTDecompressRow only fills the channels the source has
				// (Renderer.cpp, "sourceChannels"), so the blue channel is reconstructed here.
				for (int x = 0; x < nTexW; x++)
				{
					const float nx = (float)pSrcLine[x].r / 255.f * 2.f - 1.f;
					const float ny = (float)pSrcLine[x].g / 255.f * 2.f - 1.f;
					const float nz = sqrt_tpl(max(0.f, 1.f - nx * nx - ny * ny));

					pDstLine[x].r = pSrcLine[x].r;
					pDstLine[x].g = pSrcLine[x].g;
					pDstLine[x].b = SATURATEB((int)(nz * 127.5f + 127.5f));

					// the gloss map's red, resampled if the two low res copies snapped to
					// different sizes; 255 (no map) leaves the consumer with the scalar alone
					pDstLine[x].a = pSmoothSrc
					                ? pSmoothSrc[(size_t)RT_ResampleIndex(lineId, nTexH, smoothH) * smoothW + RT_ResampleIndex(x, nTexW, smoothW)].r
					                : (uint8)255;
				}
			}
			else if (eEnc == EFTT_EMITTANCE)
			{
				// Illum.cfx GetEmittanceMask uses "emittanceMap.rgb * emittanceMap.a" as the mask and
				// the consumer only has one channel for it (vEmm.a), so the mask luminance is folded
				// into the alpha here. rgb is kept untouched for a later coloured-emissive stage.
				// EmittanceMapGamma is NOT applied - it is a per material shader constant the record
				// has no room for, so a strongly gamma-shaped emissive mask reflects slightly flatter
				// than it shades (documented gap, stage 4).
				for (int x = 0; x < nTexW; x++)
				{
					const ColorB  s = pSrcLine[x];
					const ColorF  f((float)s.r / 255.f, (float)s.g / 255.f, (float)s.b / 255.f, 1.f);
					const float   mask = f.Luminance() * ((float)s.a / 255.f);

					pDstLine[x].r = s.r;
					pDstLine[x].g = s.g;
					pDstLine[x].b = s.b;
					pDstLine[x].a = SATURATEB((int)(mask * 255.f + 0.5f));
				}
			}
			else
			{
				memcpy(pDstLine, pSrcLine, nTexW * sizeof(ColorB));
			}
		}

		*pSysTexId = newSlice + 1;
		gSvoEnv->RTMarkTexsDirty(newSlice, 1);
	}

	const int slice = (*pSysTexId) - 1;

	gSvoEnv->RTAddTexSliceRef(slice);
	arrTexSlicesOut.Add(slice);

	return slice + 1;
}

//! Reads one of the material's public shader parameters (the "tweakables" a .mtl overrides) by name.
//! CE only stores the parameters a material actually overrides, so an absent name must fall back to
//! the shader's own default - which is why every call site below passes one explicitly.
//! Same access pattern as CBreakableGlassRenderNode::SetMaterial (BreakableGlassRenderNode.cpp:718).
static bool RT_GetMatParam(IRenderShaderResources* pRes, const char* szName, float* pOut, int numOut)
{
	if (!pRes || !szName)
		return false;

	DynArrayRef<SShaderParam>& params = pRes->GetParameters();

	for (int i = 0; i < params.size(); i++)
	{
		const SShaderParam& sp = params[i];

		if (stricmp(sp.m_Name, szName))
			continue;

		switch (sp.m_Type)
		{
		case eType_HALF:
		case eType_FLOAT:
			pOut[0] = sp.m_Value.m_Float;
			return true;

		case eType_BYTE:
			pOut[0] = (float)sp.m_Value.m_Byte;
			return true;

		case eType_SHORT:
			pOut[0] = (float)sp.m_Value.m_Short;
			return true;

		case eType_INT:
			pOut[0] = (float)sp.m_Value.m_Int;
			return true;

		case eType_BOOL:
			pOut[0] = sp.m_Value.m_Bool ? 1.f : 0.f;
			return true;

		case eType_FCOLOR:
			for (int k = 0; k < min(numOut, 4); k++)
				pOut[k] = sp.m_Value.m_Color[k];
			return true;

		case eType_VECTOR:
			for (int k = 0; k < min(numOut, 3); k++)
				pOut[k] = sp.m_Value.m_Vector[k];
			return true;

		default:
			return false;
		}
	}

	return false;
}

//! Texture modificator tiling of one texture slot. CShaderResources::RT_UpdateConstants fills
//! CM_DetailTilingAndAlphaRef.xy from exactly this (ShaderResources.cpp:622-629).
static Vec2 RT_GetTexTiling(IRenderShaderResources* pRes, EEfResTextures texSlot)
{
	if (pRes)
	{
		if (SEfResTexture* pResTex = pRes->GetTexture(texSlot))
		{
			if (pResTex->m_Ext.m_pTexModifier)
				return Vec2(pResTex->m_Ext.m_pTexModifier->m_Tiling[0], pResTex->m_Ext.m_pTexModifier->m_Tiling[1]);
		}
	}

	return Vec2(1.f, 1.f);
}

//! True when the slot holds a texture that could reach the atlas at all.
static bool RT_HasTexture(IRenderShaderResources* pRes, EEfResTextures texSlot)
{
	if (!pRes)
		return false;

	SEfResTexture* pResTex = pRes->GetTexture(texSlot);

	return pResTex && pResTex->m_Sampler.m_pITex;
}

//! Shading tag for matInfo3.z (rt decision 10).
//!
//! Tagging is by SHADER, not by material flags, because the shader is what actually selects the
//! lighting code on the primary surface:
//!   * terrain         -> 5    (the terrain hit type, or a shader of type eST_Terrain)
//!   * Vegetation.cfx  -> 0.25 when the material is alpha tested (leaves / grass planes: two sided +
//!                       LIGHTINGMODEL_TRANSMITTANCE, Vegetation.cfx:504-512). A Vegetation trunk
//!                       (no alpha test) stays 0 - exactly the stage 1/2 rule, kept so the current
//!                       consumer sees byte identical tags for every material it already handled.
//!   * HumanSkin.cfx   -> 2    BY NAME: HumanSkin declares "ShaderType = General" (HumanSkin.cfx:21),
//!                       so eST_* cannot tell it apart from Illum.
//!   * Glass.cfx       -> 3    (eST_Glass, Glass.cfx:21)
//!   * everything else -> 0    (Illum and friends)
//! Tags 4 (Water) and 6 (EmissiveOnly) are reserved and never produced: water is decision 09's, and
//! CE has no usable "emissive but unlit" material flag - MTL_FLAG_LIGHTING exists (IMaterial.h:52)
//! but is set by no shipped .mtl and read by no renderer code, so keying a tag off it would mis-tag
//! nearly every material in the level.
static float RT_ShadingTag(SShaderItem* pShItem, bool bTerrain, float alphaRef)
{
	if (bTerrain)
		return SVO_RT_TAG_TERRAIN;

	if (!pShItem || !pShItem->m_pShader)
		return SVO_RT_TAG_ILLUM;

	IShader*          pShader = pShItem->m_pShader;
	const EShaderType eType = pShader->GetShaderType();
	const char*       szName = pShader->GetName();

	if (eType == eST_Terrain)
		return SVO_RT_TAG_TERRAIN;

	if (eType == eST_Glass)
		return SVO_RT_TAG_GLASS;

	if (eType == eST_Vegetation)
		return (alphaRef > 0.05f) ? SVO_RT_TAG_VEG_LEAVES : SVO_RT_TAG_ILLUM;

	if (szName && !stricmp(szName, "HumanSkin"))
		return SVO_RT_TAG_HUMAN_SKIN;

	return SVO_RT_TAG_ILLUM;
}

//! True for the two shaders that implement Illum's %BLENDLAYER with the same parameter names and the
//! same texture slots (Illum.cfx:38-64 / :507-533, Vegetation.cfx:465). EFTT_CUSTOM is NOT a blend
//! layer on other shaders - HumanSkin uses it for the wrinkle map and Glass for the tint colour map -
//! so the slot alone must never be read as "this material has a blend layer".
static bool RT_HasBlendLayerShader(SShaderItem* pShItem)
{
	if (!pShItem || !pShItem->m_pShader)
		return false;

	if (pShItem->m_pShader->GetShaderType() == eST_Vegetation)
		return true;

	const char* szName = pShItem->m_pShader->GetName();

	return szName && !strnicmp(szName, "Illum", 5);
}

//! One material as the pool stores it (rt decision 02 2.5 for the base record, decision 10 for the
//! extras and the blend layer record). See SRTMatRecordSet for the placement rules.
void CVoxelSegment::FillRTMaterialRecord(const SRayHitTriangleIndexed& tr, SRTMatRecordSet& out, PodArray<ITexture*>* pDeferredOut)
{
	SSvoMatInfo& rMI = m_pMatsInArea->GetAt(tr.materialID);

	RTFillMaterialRecord(rMI.pMat, tr.hitObjectType == HIT_OBJ_TYPE_TERRAIN, m_rtTexSlices, out, pDeferredOut);
}

//! The material record itself, independent of where the triangle came from: the static soup and the
//! per frame dynamic collection (stage 3A) fill identical records from the same leaf material.
void CVoxelSegment::RTFillMaterialRecord(IMaterial* pInMat, bool bTerrain, PodArray<int>& arrTexSlicesOut, SRTMatRecordSet& out, PodArray<ITexture*>* pDeferredOut)
{
	ZeroStruct(out);

	const int maxTexSizeXY = gSvoEnv->GetRTTexRes();

	IMaterial*   pMat = pInMat;
	SShaderItem* pShItem = (pMat && !bTerrain) ? &pMat->GetShaderItem() : nullptr;

	uint16 texW = 0, texH = 0, norW = 0, norH = 0, spcW = 0, spcH = 0, emiW = 0, emiH = 0;
	int    albSlot = 0, norSlot = 0, spcSlot = 0, emiSlot = 0;

	ColorF tint = Col_White;
	float  opacity = 1.f;
	float  alphaRef = 0.f;
	float  smoothness = 1.f;
	float  normalStrength = 1.f;
	float  specRefl = 0.04f;
	float  emissive = 0.f;

	if (bTerrain)
	{
		albSlot = CheckStoreTextureInPool(nullptr, EFTT_DIFFUSE, texW, texH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);

		const float mul = GetTerrain() ? GetTerrain()->GetTerrainTextureMultiplier() : 1.f;
		tint = ColorF(mul, mul, mul, 1.f);
		smoothness = 0.f;
		specRefl = 0.02f;
	}
	else if (pShItem)
	{
		albSlot = CheckStoreTextureInPool(pShItem, EFTT_DIFFUSE, texW, texH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);
		norSlot = CheckStoreTextureInPool(pShItem, EFTT_NORMALS, norW, norH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);
		spcSlot = CheckStoreTextureInPool(pShItem, EFTT_SPECULAR, spcW, spcH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);
		emiSlot = CheckStoreTextureInPool(pShItem, EFTT_EMITTANCE, emiW, emiH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);

		if (IRenderShaderResources* pShRes = pShItem->m_pShaderResources)
		{
			tint = pShRes->GetColorValue(EFTT_DIFFUSE);
			opacity = SATURATE(pShRes->GetStrengthValue(EFTT_OPACITY));
			alphaRef = SATURATE(pShRes->GetAlphaRef());

			normalStrength = pShRes->GetStrengthValue(EFTT_NORMALS);
			if (normalStrength <= 0.f)
				normalStrength = 1.f;

			// MatSpecColor.w - exactly what Illum.cfx uses for attribs.Smoothness
			const float gloss = pShRes->GetStrengthValue(EFTT_SMOOTHNESS);
			smoothness = SATURATE(gloss > 1.f ? gloss / 255.f : gloss);

			// Reflectance source. Illum.cfx (GetSurfaceAttributes) computes
			//     attribs.Reflectance = MatSpecColor.rgb * GetSpecularTex(specularTex).rgb
			// and CShaderResources::GetColorValue(EFTT_SPECULAR) returns exactly MatSpecColor.rgb
			// (REG_PM_SPECULAR_COL - ShaderResources.cpp GetColorValue). The consumer forms
			// matInfo1.y * vSpc.xyz, so matInfo1.y is the scalar stand-in for MatSpecColor.rgb and
			// vSpc is the atlas copy of the same specular map the primary surface samples.
			// The 4 texel record has no room for a coloured F0, so a coloured metal loses its tint
			// here; the unweighted channel average is used (not a photometric luminance) because
			// reflectance is an energy ratio per channel, not a perceived brightness.
			const ColorF spec = pShRes->GetColorValue(EFTT_SPECULAR);
			specRefl = SATURATE((spec.r + spec.g + spec.b) / 3.f);

			// Emissive source. Illum.cfx IlluminationPS computes
			//     emittance = MatEmissiveColor.rgb * MatEmissiveColor.w * MAT_EMISSIVE_UNIT_SCALE * mask
			// where MatEmissiveColor.w is the material's "Emissive Intensity (kcd/m2)" and
			// MAT_EMISSIVE_UNIT_SCALE (FXConstantDefs.cfi) is the literal 1000/10000.
			// SShaderResources::GetFinalEmittance() is rgb * w * (1000 / RENDERER_LIGHT_UNIT_SCALE),
			// i.e. the same product with the unit scale already applied. The consumer
			// (CommonSVO_RT.cfi RT_ProcessBestHit) applies MAT_EMISSIVE_UNIT_SCALE itself, so the
			// record must store the RAW kcd/m2 value - divide the unit scale back out. Going through
			// GetFinalEmittance keeps this tied to the one place CE defines the unit.
			const float matEmissiveUnitScale = 1000.f / RENDERER_LIGHT_UNIT_SCALE;   // == MAT_EMISSIVE_UNIT_SCALE
			emissive = max(0.f, pShRes->GetFinalEmittance().Luminance() / matEmissiveUnitScale);
		}
	}

	const float tag = RT_ShadingTag(pShItem, bTerrain, alphaRef);

	// ---------------------------------------------------------------------------------------------
	// Extras record (rt decision 10). Only materials that actually need one get one; an Illum
	// material with no detail map, no blend layer, no transmittance and no alpha blending stays a
	// single 4 texel record, exactly as in stage 2B.
	// ---------------------------------------------------------------------------------------------

	IRenderShaderResources* pRes = pShItem ? pShItem->m_pShaderResources : nullptr;

	int    detailSlot = 0;
	Vec2   detailTiling(1.f, 1.f);
	float  detailDiffuse = 0.f, detailBump = 0.f;
	uint16 detW = 0, detH = 0;

	int    opacitySlot = 0;
	uint16 opaW = 0, opaH = 0;

	float blendFactor = 0.f, blendFalloff = 0.f, blendMaskTiling = 1.f, blendLayer2Tiling = 1.f;

	Vec3  transmit(0.f, 0.f, 0.f);
	float transmitStrength = 0.f;
	float typeParam0 = 0.f, typeParam1 = 0.f;

	const bool bBlendShader = RT_HasBlendLayerShader(pShItem);
	const bool bHasBlend = bBlendShader && RT_HasTexture(pRes, EFTT_CUSTOM);
	const bool bHasDetail = RT_HasTexture(pRes, EFTT_DETAIL_OVERLAY);

	// %_RT_ALPHABLEND is a runtime flag the renderer raises for a transparent material; the CPU sees
	// the same condition as IRenderShaderResources::IsTransparent (IShader.h:1371) plus the two
	// material flags that force a forward blended pass.
	const uint32 matFlags = pMat ? (uint32)pMat->GetFlags() : 0;
	const bool   bAlphaBlend = !bTerrain && pRes &&
	                           (opacity < 0.999f || (matFlags & (uint32)(MTL_FLAG_ADDITIVE | MTL_FLAG_REQUIRE_FORWARD_RENDERING)) != 0);

	if (bHasDetail)
	{
		// The detail map is ONE texture. Illum.cfx:551 reads it through
		//     GetDetailTex(tex, uv) = tex.Sample(uv).garb * 2 - 1     (Common.cfi:149)
		// so the source channels are g,a = tangent normal xy, r = albedo delta, b = gloss delta.
		// It is therefore copied into the atlas RAW (no _ddna reconstruction, no alpha rewrite) and
		// the consumer applies the .garb swizzle and the *2-1 itself.
		detailSlot = CheckStoreTextureInPool(pShItem, EFTT_DETAIL_OVERLAY, detW, detH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);

		// CM_DetailTilingAndAlphaRef.xy, filled from the detail slot's texture modificator
		// (ShaderResources.cpp RT_UpdateConstants, :622-629)
		detailTiling = RT_GetTexTiling(pRes, EFTT_DETAIL_OVERLAY);

		// Illum.cfx:550 detailScales = (DetailBumpScale, DetailBumpScale, DetailDiffuseScale, DetailGlossScale)
		detailBump = 0.5f;        // shader defaults (Illum.cfx:187, :198; Vegetation.cfx:225, :236)
		detailDiffuse = 0.5f;
		RT_GetMatParam(pRes, "DetailBumpScale", &detailBump, 1);
		RT_GetMatParam(pRes, "DetailDiffuseScale", &detailDiffuse, 1);
	}

	if (bHasBlend || tag == SVO_RT_TAG_VEG_LEAVES || tag == SVO_RT_TAG_HUMAN_SKIN)
	{
		// One slot, two meanings - exactly as in CE, where Illum's BlendTex and Vegetation's /
		// HumanSkin's opacityTex are both TM_Opacity (Illum.cfx:58, Vegetation.cfx:73, the OPACITYMAP
		// macro). The tag says which: blend mask for tag 0, translucency mask for 0.25 and 2.
		opacitySlot = CheckStoreTextureInPool(pShItem, EFTT_OPACITY, opaW, opaH, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);
	}

	if (bHasBlend)
	{
		blendFactor = 8.f;          // Illum.cfx:280, Vegetation.cfx:260
		blendFalloff = 32.f;        // Illum.cfx:302, Vegetation.cfx:282
		blendMaskTiling = 1.f;      // Illum.cfx:324, Vegetation.cfx:304
		blendLayer2Tiling = 1.f;    // Illum.cfx:291, Vegetation.cfx:271
		RT_GetMatParam(pRes, "BlendFactor", &blendFactor, 1);
		RT_GetMatParam(pRes, "BlendFalloff", &blendFalloff, 1);
		RT_GetMatParam(pRes, "BlendMaskTiling", &blendMaskTiling, 1);
		RT_GetMatParam(pRes, "BlendLayer2Tiling", &blendLayer2Tiling, 1);
	}

	if (tag == SVO_RT_TAG_VEG_LEAVES)
	{
		// Vegetation.cfx:504-512
		//     translucency = BackDiffuseMultiplier * (LEAVES ? opacityTex : 1)
		//     Transmittance = saturate(translucency) * TransmittanceColor.rgb
		// consumed by ThinTranslucencyBRDF (shadeLib.cfi:952).
		float col[4] = { 1.f, 1.f, 0.6f, 1.f };     // Vegetation.cfx:111
		float mul = 1.f;                            // Vegetation.cfx:133
		RT_GetMatParam(pRes, "TransmittanceColor", col, 4);
		RT_GetMatParam(pRes, "BackDiffuseMultiplier", &mul, 1);

		transmit = Vec3(max(0.f, col[0]), max(0.f, col[1]), max(0.f, col[2]));
		transmitStrength = max(0.f, mul);
	}
	else if (tag == SVO_RT_TAG_HUMAN_SKIN)
	{
		// HumanSkin.cfx:296-298
		//     fTranslucency = opacityTex * TranslucencyMultiplier
		//     Transmittance = exp((1 - saturate(fTranslucency)) * (-8, -40, -64))
		// Stored evaluated at the material's own scalar (no map), so a consumer that does not want to
		// sample the mask can use extras[2].rgb directly; one that does can redo the exp() per texel
		// from extras[3].y and the EFTT_OPACITY slot in extras[1].y.
		float sssIndex = 1.2f;      // HumanSkin.cfx:67
		float translucency = 0.f;   // HumanSkin.cfx:78
		RT_GetMatParam(pRes, "SSSIndex", &sssIndex, 1);
		RT_GetMatParam(pRes, "TranslucencyMultiplier", &translucency, 1);

		const float t = 1.f - SATURATE(translucency);
		transmit = Vec3(exp_tpl(t * -8.f), exp_tpl(t * -40.f), exp_tpl(t * -64.f));
		transmitStrength = max(0.f, translucency);

		typeParam0 = sssIndex;
		typeParam1 = translucency;
	}
	else if (tag == SVO_RT_TAG_GLASS)
	{
		// Glass.cfx:555-575: the glass hit's colour is lerp(cBackbuffer * TintColor,
		// TintColor * diffuseAcc, TintCloudiness), i.e. TintColor is what the ray keeps when it goes
		// through and TintCloudiness is how much of it becomes a lit diffuse instead.
		float col[4] = { 1.f, 1.f, 1.f, 1.f };   // Glass.cfx:118
		float cloudiness = 0.f;                  // Glass.cfx:129
		RT_GetMatParam(pRes, "TintColor", col, 4);
		RT_GetMatParam(pRes, "TintCloudiness", &cloudiness, 1);

		transmit = Vec3(SATURATE(col[0]), SATURATE(col[1]), SATURATE(col[2]));
		transmitStrength = SATURATE(col[3]);

		typeParam0 = SATURATE(col[3]);
		typeParam1 = SATURATE(cloudiness);
	}

	out.bBlend = bHasBlend;
	out.bExtras = bHasDetail || bHasBlend || bAlphaBlend || transmitStrength > 0.f ||
	              tag == SVO_RT_TAG_GLASS || tag == SVO_RT_TAG_HUMAN_SKIN || tag == SVO_RT_TAG_VEG_LEAVES;

	// Known gap 7.2: the record carries ONE atlas UV scale, taken from the albedo copy. A normal,
	// specular, emissive, detail or blend mask copy of a different size then samples with the
	// albedo's scale, which shifts that layer across the hit. Count it and warn once per level so the
	// case is visible instead of silently wrong; the fix needs a second scale field (or the packed
	// atlas of decision 04's alternatives) and is a stage 4 item.
	if (albSlot && texW && texH)
	{
		const bool bMismatch =
		  (norSlot && (norW != texW || norH != texH)) ||
		  (spcSlot && (spcW != texW || spcH != texH)) ||
		  (emiSlot && (emiW != texW || emiH != texH)) ||
		  (detailSlot && (detW != texW || detH != texH)) ||
		  (opacitySlot && (opaW != texW || opaH != texH));

		if (bMismatch)
			gSvoEnv->RTCountUvScaleMismatch(pMat ? pMat->GetName() : "?");
	}

	const float scaleX = albSlot ? (float)texW / (float)maxTexSizeXY : 1.f;
	const float scaleY = albSlot ? (float)texH / (float)maxTexSizeXY : 1.f;

	// The extras record lives at base + 1, so the stored reference is always 1; 0 means "no extras".
	// It is an ExtractUint2 field and caps at 4095, which is why it is a RELATIVE offset and not an
	// absolute record index (a static chunk can start anywhere in a million record pool).
	const int extrasRef = out.bExtras ? 1 : 0;

	out.base[0] = Vec4((float)albSlot, RT_PackUint2(norSlot, spcSlot), RT_PackUint2(emiSlot, extrasRef), RT_PackTC16(scaleX, scaleY));
	out.base[1] = Vec4(normalStrength, specRefl, smoothness, emissive);
	out.base[2] = Vec4(tint.r, tint.g, tint.b, 0.f);
	out.base[3] = Vec4(0.f, 0.f, tag, RT_PackTC16(opacity, alphaRef));

	if (out.bExtras)
	{
		out.extras[0] = Vec4(RT_PackUint2(detailSlot, 0),
		                     detailTiling.x, detailTiling.y,
		                     RT_PackTC16(CLAMP(detailDiffuse, 0.f, RT_TC16_MAX), CLAMP(detailBump, 0.f, RT_TC16_MAX)));

		// .x is patched to the ABSOLUTE blend base record by the caller (the only place that knows
		// where the chunk lands); it stays 0 for a material without a blend layer.
		out.extras[1] = Vec4(0.f, (float)opacitySlot, blendFactor, blendFalloff);

		out.extras[2] = Vec4(transmit.x, transmit.y, transmit.z, transmitStrength);

		out.extras[3] = Vec4(typeParam0, typeParam1, bAlphaBlend ? opacity : 0.f,
		                     RT_PackTC16(CLAMP(blendMaskTiling, 0.f, RT_TC16_MAX), CLAMP(blendLayer2Tiling, 0.f, RT_TC16_MAX)));
	}

	if (out.bBlend)
	{
		// A full second base record for the blend layer, so the consumer decodes it with the same
		// code path and then lerps the two by blendFac. Illum.cfx:507-533:
		//     Albedo      = lerp(Albedo, diffuseMap2, f)          <- Diffuse2Tex, NOT tinted by MatDifColor
		//     Smoothness  = lerp(Smoothness, glossLayer2, f)      <- smoothness2Tex.r, no scalar
		//     Reflectance = lerp(Reflectance, BlendLayer2Specular, f)
		//     vNormalTS   = lerp(vNormalTS, vNormal2, f)          <- Bump2Tex
		uint16 b0W = 0, b0H = 0, b1W = 0, b1H = 0;

		const int albSlot2 = CheckStoreTextureInPool(pShItem, EFTT_CUSTOM, b0W, b0H, arrTexSlicesOut, EFTT_UNKNOWN, EFTT_SMOOTHNESS, pDeferredOut);
		const int norSlot2 = CheckStoreTextureInPool(pShItem, EFTT_CUSTOM_SECONDARY, b1W, b1H, arrTexSlicesOut,
		                                             EFTT_NORMALS, EFTT_DECAL_OVERLAY, pDeferredOut);

		float layer2Spec = 0.04f;   // Illum.cfx:313, Vegetation.cfx:293
		RT_GetMatParam(pRes, "BlendLayer2Specular", &layer2Spec, 1);

		const float bScaleX = albSlot2 ? (float)b0W / (float)maxTexSizeXY : 1.f;
		const float bScaleY = albSlot2 ? (float)b0H / (float)maxTexSizeXY : 1.f;

		out.blend[0] = Vec4((float)albSlot2, RT_PackUint2(norSlot2, 0), RT_PackUint2(0, 0), RT_PackTC16(bScaleX, bScaleY));
		out.blend[1] = Vec4(normalStrength, SATURATE(layer2Spec), 1.f, 0.f);
		out.blend[2] = Vec4(1.f, 1.f, 1.f, 0.f);
		out.blend[3] = Vec4(0.f, 0.f, tag, RT_PackTC16(opacity, alphaRef));
	}
}

//! Appends one material's records to arrRecords: base, then (if any) extras, then (if any) the blend
//! layer's base record. localRecord is the record index of the base within the same numbering the
//! caller uses; absoluteBase is what has to be added to it to make it a pool record index (0 for the
//! static path, where the chunk start is unknown until later and pRelocFloats carries the fix-up).
static void RT_AppendMatRecordSet(const SRTMatRecordSet& set, int localRecord, int absoluteBase, PodArray<Vec4>& arrRecords, PodArray<int>* pRelocFloats)
{
	for (int i = 0; i < SVO_RT_RECORD_TEXELS; i++)
		arrRecords.Add(set.base[i]);

	if (!set.bExtras)
		return;

	const int extrasTexel = arrRecords.Count();

	for (int i = 0; i < SVO_RT_RECORD_TEXELS; i++)
		arrRecords.Add(set.extras[i]);

	if (!set.bBlend)
		return;

	// the blend base record follows the extras record
	arrRecords.GetElements()[extrasTexel + 1].x = (float)(absoluteBase + localRecord + 2);

	if (pRelocFloats)
		pRelocFloats->Add((extrasTexel + 1) * 4 + 0);   // extras texel +1, .x = blend base record

	for (int i = 0; i < SVO_RT_RECORD_TEXELS; i++)
		arrRecords.Add(set.blend[i]);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - deferred material record patching (report 06d fix 3a)
//
// A material emitted while its low resolution texture copies were still being produced carries no
// texture slots. Its record COUNT does not depend on the copies - only the slot ids and the atlas UV
// scale do, and the extras / blend layer records come from the shader and the material flags - so
// the set can be rebuilt once the copies land and written back over exactly the same records.
// Defined here, not in SceneTree.cpp, because it needs RT_AppendMatRecordSet.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CSvoEnv::RTQueueMatPatch(IMaterial* pMat, bool bTerrain, int absRecord, int recordCount, int chunkStart, const PodArray<ITexture*>& waitFor)
{
	if (!recordCount || absRecord <= 0)
		return;

	AUTO_LOCK(m_rtPendingLock);   // called from the voxelization workers

	SRTMatPatch* p = new SRTMatPatch();

	p->pMat = pMat;
	p->bTerrain = bTerrain;
	p->absRecord = absRecord;
	p->recordCount = recordCount;
	p->chunkStart = chunkStart;
	p->waitFor.AddList(const_cast<PodArray<ITexture*>&>(waitFor));

	m_arrRTMatPatches.push_back(p);
	m_rtMatPatchesPending++;
}

//! Caller holds m_arrRTPoolTexs.m_Lock in modify mode (RTProcessPendingFrees).
void CSvoEnv::RTDropMatPatchesForChunk(int chunkStart)
{
	AUTO_LOCK(m_rtPendingLock);

	for (size_t i = 0; i < m_arrRTMatPatches.size(); )
	{
		SRTMatPatch* p = m_arrRTMatPatches[i];

		if (p->chunkStart == chunkStart)
		{
			for (int s = 0; s < p->slices.Count(); s++)
				RTReleaseTexSlice(p->slices[s]);

			if (!p->bApplied)
				m_rtMatPatchesPending--;

			delete p;
			m_arrRTMatPatches.erase(m_arrRTMatPatches.begin() + i);
		}
		else
		{
			i++;
		}
	}
}

void CSvoEnv::RTApplyMatPatches()
{
	if (!m_rtMatPatchesPending)
		return;

	CRY_PROFILE_SECTION(PROFILE_3DENGINE, "CSvoEnv::RTApplyMatPatches");

	// Rebuilding a material record re-copies up to six 256 KB texture slices into the atlas, so it
	// is budgeted like everything else on this path: the cells keep tracing their tint until then.
	int budget = 4;

	// The ready patches are DETACHED under the pending lock and processed without it: the fill below
	// takes the atlas lock, and RTDropMatPatchesForChunk takes the pending lock while already holding
	// the atlas lock, so holding both here in the other order would deadlock. Only the main thread
	// removes from this list, and it runs the two passes one after the other, so a detached patch
	// cannot be freed underneath us.
	std::vector<SRTMatPatch*> arrReady;

	{
		AUTO_LOCK(m_rtPendingLock);

		for (size_t i = 0; i < m_arrRTMatPatches.size() && budget > 0; )
		{
			SRTMatPatch* p = m_arrRTMatPatches[i];

			if (p->bApplied)
			{
				i++;   // done; it stays in the list only to own its atlas references
				continue;
			}

			bool bReady = true;
			for (int w = 0; w < p->waitFor.Count() && bReady; w++)
				bReady = CVoxelSegment::RTIsTexCopyReady(p->waitFor[w]);

			if (!bReady)
			{
				i++;
				continue;
			}

			budget--;
			m_rtMatPatchesPending--;
			arrReady.push_back(p);
			m_arrRTMatPatches.erase(m_arrRTMatPatches.begin() + i);
		}
	}

	for (size_t i = 0; i < arrReady.size(); i++)
	{
		SRTMatPatch* p = arrReady[i];

		SRTMatRecordSet     set;
		PodArray<ITexture*> arrStillDeferred;

		CVoxelSegment::RTFillMaterialRecord(p->pMat, p->bTerrain, p->slices, set, &arrStillDeferred);

		if (arrStillDeferred.Count())
		{
			// a copy this material needs was only discovered now (a slot the first pass never got
			// to): keep waiting for it instead of publishing a half textured record
			p->waitFor.AddList(arrStillDeferred);

			AUTO_LOCK(m_rtPendingLock);
			m_arrRTMatPatches.push_back(p);
			m_rtMatPatchesPending++;
			continue;
		}

		if (set.RecordCount() == p->recordCount)
		{
			PodArray<Vec4> arrRecords;
			RT_AppendMatRecordSet(set, 0, p->absRecord, arrRecords, nullptr);

			AUTO_MODIFYLOCK(m_arrRTPoolTris.m_Lock);

			const int poolRecords = GetRTPoolRecords();

			if (m_arrRTPoolTris.Count() && p->absRecord + p->recordCount <= poolRecords)
			{
				memcpy(m_arrRTPoolTris.GetElements() + (size_t)p->absRecord * SVO_RT_RECORD_TEXELS,
				       arrRecords.GetElements(), arrRecords.GetDataSize());

				RTMarkTrisDirty(p->absRecord, p->recordCount);

				m_rtMatPatchesApplied++;
			}
		}
		else
		{
			// the record layout is supposed to be independent of the texture copies; if it ever is
			// not, drop the patch rather than write a set of another size over its neighbours
			CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
			           "SVO RT: material record set changed size while waiting for its textures (%d -> %d), patch dropped",
			           p->recordCount, set.RecordCount());
		}

		// The patch is KEPT, with its work done: it owns the atlas references the records it just
		// wrote point at, and the only thing entitled to give those back is the chunk being
		// reclaimed. Its waitFor list is cleared so the pass above skips it from now on.
		p->bApplied = true;
		p->waitFor.Reset();

		AUTO_LOCK(m_rtPendingLock);
		m_arrRTMatPatches.push_back(p);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - per cell static BVH build (rt decisions 02, 03, 04)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void RT_ReleaseTexSlices(PodArray<int>& arrSlices)
{
	if (!arrSlices.Count())
		return;

	for (int i = 0; i < arrSlices.Count(); i++)
		gSvoEnv->RTQueueFreeTexSlice(arrSlices[i]);
	arrSlices.Reset();
}

void CVoxelSegment::ReleaseRTChunk()
{
	if (m_rtChunkCount)
	{
		gSvoEnv->RTQueueFreeChunk(m_rtChunkStart, m_rtChunkCount);
		m_rtChunkStart = m_rtChunkCount = m_rtRootRecord = 0;
	}

	RT_ReleaseTexSlices(m_rtTexSlices);
}

void CVoxelSegment::BuildStaticBVH()
{
	if (!GetCVars()->e_svoTI_RT_Active || !GetCVars()->e_svoTI_RT_StaticBVH)
		return;

	// only cells at exactly e_svoMaxNodeSize own a static BVH (consumer gate, rt decision 02, 2.3)
	if (!m_isAreaParent)
		return;

	if (!m_pTrisInArea || !m_pTrisInArea->Count() || !m_pVertInArea || !m_pMatsInArea)
		return;

	CRY_PROFILE_SECTION(PROFILE_3DENGINE, "CVoxelSegment::BuildStaticBVH");

	const float startTime = GetCurAsyncTimeSec();

	SRTBuildStats stats;
	stats.Reset();

	const int oldChunkStart = m_rtChunkStart;
	const int oldChunkCount = m_rtChunkCount;

	PodArray<int> arrOldTexSlices;
	arrOldTexSlices.AddList(m_rtTexSlices);
	m_rtTexSlices.Clear();

	//! One material whose record set went out without its textures (report 06d fix 3a).
	struct SRTDeferredMat
	{
		_smart_ptr<IMaterial> pMat;
		PodArray<ITexture*>   waitFor;
		bool                  bTerrain;
		int                   localRecord;
		int                   recordCount;
	};

	std::vector<SRTDeferredMat> arrDeferredMats;

	PodArray<SRTBuildTri> arrBuild;
	PodArray<Vec4>        arrRecords;
	PodArray<int>         arrReloc;
	std::map<int, int>    matKeyToLocalId;
	int                   matRecords = 0;   //!< records consumed by the material area (base + extras + blend)
	Vec4                  qb(0, 0, 0, 1);

	{
		AUTO_READLOCK(m_superMeshLock);

		const AABB cellBox(m_boxOS.min + m_vSegOrigin, m_boxOS.max + m_vSegOrigin);

		// Quantisation bounds: one per cell, so that a vertex shared between two leaves quantises
		// identically (watertightness, rt decision 02, 2.5). The cell box is grown to the real soup
		// bounds because a triangle that only overlaps the cell may legally have vertices outside it;
		// clamping those would tear the geometry apart.
		AABB qBox = cellBox;
		for (int t = 0; t < m_pTrisInArea->Count(); t++)
		{
			const SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[t];
			if (tr.hitObjectType == HIT_OBJ_TYPE_VISAREA)
				continue;
			for (int v = 0; v < 3; v++)
				qBox.Add((*m_pVertInArea)[tr.arrVertId[v]].v);
		}

		float qSize = max(qBox.max.x - qBox.min.x, qBox.max.y - qBox.min.y);
		qSize = max(qSize, 1.f) * (1.f + 1.f / 1024.f);

		qb = Vec4((qBox.min.x + qBox.max.x) * 0.5f, (qBox.min.y + qBox.max.y) * 0.5f, 0.f, qSize);

		const int terrainSize = max(1, GetTerrain() ? GetTerrain()->GetTerrainSize() : 1);

		arrBuild.PreAllocate(m_pTrisInArea->Count(), 0);

		for (int t = 0; t < m_pTrisInArea->Count(); t++)
		{
			const SRayHitTriangleIndexed& tr = (*m_pTrisInArea)[t];

			// Visarea shells are an opacity seal for voxel cone tracing, not real geometry; tracing
			// them would put a black box into every reflection at an indoor/outdoor border.
			if (tr.hitObjectType == HIT_OBJ_TYPE_VISAREA)
				continue;

			SRTBuildTri bt;
			for (int v = 0; v < 3; v++)
			{
				const SRayHitVertex& vert = (*m_pVertInArea)[tr.arrVertId[v]];
				bt.v[v] = vert.v;
				bt.n[v] = vert.n;
				bt.t[v] = vert.t;
			}
			bt.faceNorm = tr.vFaceNorm;

			// reject degenerates: they give the consumer a zero determinant
			if ((bt.v[1] - bt.v[0]).Cross(bt.v[2] - bt.v[0]).GetLengthSquared() < 1e-16f)
			{
				stats.trisSkipped++;
				continue;
			}

			// vertex normals are flipped into the face hemisphere so the frame the consumer rebuilds
			// from (tangent, bitangent) stays consistent with the winding
			for (int v = 0; v < 3; v++)
				if (bt.n[v].Dot(bt.faceNorm) < 0.f)
					bt.n[v] = -bt.n[v];

			if (tr.hitObjectType == HIT_OBJ_TYPE_TERRAIN)
			{
				// terrain carries no UVs in the soup: regenerate CE's terrain tex-gen (ProcessMaterial)
				for (int v = 0; v < 3; v++)
					bt.t[v] = Vec2(bt.v[v].y / (float)terrainSize, bt.v[v].x / (float)terrainSize);
			}

			// UVs are normalised per triangle into [0, 16), exactly as CE's old producer did
			const Vec2 tcMin(min(bt.t[0].x, min(bt.t[1].x, bt.t[2].x)), min(bt.t[0].y, min(bt.t[1].y, bt.t[2].y)));
			bool       bClamped = false;
			for (int v = 0; v < 3; v++)
			{
				bt.t[v].x -= floor(tcMin.x);
				bt.t[v].y -= floor(tcMin.y);

				if (bt.t[v].x < 0.f || bt.t[v].x >= 16.f || bt.t[v].y < 0.f || bt.t[v].y >= 16.f)
					bClamped = true;

				bt.t[v].x = CLAMP(bt.t[v].x, 0.f, 16.f - 1.f / 4096.f);
				bt.t[v].y = CLAMP(bt.t[v].y, 0.f, 16.f - 1.f / 4096.f);
			}
			if (bClamped)
				stats.uvClamped++;

			// material records are deduplicated per cell by (material id, hit object type) and live
			// at the head of the cell's chunk, so that one free() reclaims everything. A material can
			// occupy 1, 2 or 3 records (base, optional extras, optional blend layer base - rt decision
			// 10), so the local id is a running record counter, not the map's size.
			const int                           matKey = (int)tr.materialID * 4 + (int)tr.hitObjectType;
			std::map<int, int>::const_iterator  it = matKeyToLocalId.find(matKey);
			int                                 localMatId;

			if (it != matKeyToLocalId.end())
			{
				localMatId = it->second;
			}
			else
			{
				SRTMatRecordSet     matSet;
				PodArray<ITexture*> arrDeferred;

				FillRTMaterialRecord(tr, matSet, &arrDeferred);

				localMatId = matRecords;
				matKeyToLocalId[matKey] = localMatId;
				matRecords += matSet.RecordCount();

				// this material was written with no textures because its low resolution copies are
				// still being produced; remember where its records live so they can be filled in
				if (arrDeferred.Count())
				{
					SRTDeferredMat dm;
					dm.pMat = m_pMatsInArea->GetAt(tr.materialID).pMat;
					dm.bTerrain = (tr.hitObjectType == HIT_OBJ_TYPE_TERRAIN);
					dm.localRecord = localMatId;
					dm.recordCount = matSet.RecordCount();
					dm.waitFor.AddList(arrDeferred);
					arrDeferredMats.push_back(dm);
				}

				stats.extras += matSet.bExtras ? 1 : 0;
				stats.blends += matSet.bBlend ? 1 : 0;

				RT_AppendMatRecordSet(matSet, localMatId, 0, arrRecords, &arrReloc);
			}

			bt.matRecord = localMatId;   // made absolute once the chunk start is known

			arrBuild.Add(bt);
		}
	}

	const int matCount = (int)matKeyToLocalId.size();

	if (!arrBuild.Count() || !BuildStaticBVHRecords(arrBuild, qb, matRecords, arrRecords, &arrReloc, stats))
	{
		m_rtRootRecord = 0;
		m_rtChunkStart = m_rtChunkCount = 0;
		if (oldChunkCount)
			gSvoEnv->RTQueueFreeChunk(oldChunkStart, oldChunkCount);
		RT_ReleaseTexSlices(arrOldTexSlices);
		return;
	}

	const int recordCount = arrRecords.Count() / SVO_RT_RECORD_TEXELS;

	int chunkStart = 0;
	int allocRecords = recordCount;
	{
		AUTO_MODIFYLOCK(gSvoEnv->m_arrRTPoolTris.m_Lock);

		chunkStart = gSvoEnv->RTAllocChunk(allocRecords);

		if (chunkStart)
		{
			// make every stored record index absolute
			float* pFloats = (float*)arrRecords.GetElements();
			for (int i = 0; i < arrReloc.Count(); i++)
				pFloats[arrReloc[i]] += (float)chunkStart;

			memcpy(gSvoEnv->m_arrRTPoolTris.GetElements() + (size_t)chunkStart * SVO_RT_RECORD_TEXELS,
			       arrRecords.GetElements(),
			       arrRecords.GetDataSize());

			gSvoEnv->RTMarkTrisDirty(chunkStart, recordCount);

			gSvoEnv->m_rtStats.cells++;
			gSvoEnv->m_rtStats.tris += stats.tris;
			gSvoEnv->m_rtStats.nodes += stats.nodes;
			gSvoEnv->m_rtStats.leaves += stats.leaves;
			gSvoEnv->m_rtStats.records += stats.records;
			gSvoEnv->m_rtStats.mats += matCount;
			gSvoEnv->m_rtStats.extras += stats.extras;
			gSvoEnv->m_rtStats.blends += stats.blends;
			gSvoEnv->m_rtStats.uvClamped += stats.uvClamped;
			gSvoEnv->m_rtStats.trisSkipped += stats.trisSkipped;
			gSvoEnv->m_rtStats.maxDepth = max(gSvoEnv->m_rtStats.maxDepth, stats.maxDepth);
			gSvoEnv->m_rtStats.maxLeafTris = max(gSvoEnv->m_rtStats.maxLeafTris, stats.maxLeafTris);
			gSvoEnv->m_rtStats.buildMs += (GetCurAsyncTimeSec() - startTime) * 1000.f;
		}
	}

	// The old chunk is released only through the deferred queue, i.e. after the tree texel that
	// pointed at it has been rewritten and uploaded (rt STRUCTURE, chain 3).
	if (oldChunkCount)
		gSvoEnv->RTQueueFreeChunk(oldChunkStart, oldChunkCount);
	RT_ReleaseTexSlices(arrOldTexSlices);

	if (!chunkStart)
	{
		// pool overflow: this cell falls back to voxel cone tracing, never to aliased records
		m_rtRootRecord = 0;
		m_rtChunkStart = m_rtChunkCount = 0;
		RT_ReleaseTexSlices(m_rtTexSlices);
		return;
	}

	m_rtChunkStart = chunkStart;
	m_rtChunkCount = allocRecords;
	m_rtRootRecord = chunkStart + matRecords;

	// The chunk start is only known now, so the deferred material records can only be registered
	// here. A patch is dropped again when its chunk is reclaimed (RTDropMatPatchesForChunk), which
	// covers a cell that is re-voxelized before its textures ever arrived.
	for (size_t d = 0; d < arrDeferredMats.size(); d++)
	{
		const SRTDeferredMat& dm = arrDeferredMats[d];

		gSvoEnv->RTQueueMatPatch(dm.pMat, dm.bTerrain, chunkStart + dm.localRecord, dm.recordCount, chunkStart, dm.waitFor);
	}

	if (GetCVars()->e_svoDebug)
	{
		PrintMessage("RT BVH: cell (%.0f %.0f %.0f) size %.0f: %d tris, %d nodes, %d leaves, depth %d, maxLeaf %d, %d records, %d mats, root %d",
		             m_vSegOrigin.x, m_vSegOrigin.y, m_vSegOrigin.z, GetBoxSize(),
		             arrBuild.Count(), stats.nodes, stats.leaves, stats.maxDepth, stats.maxLeafTris, recordCount, matCount, m_rtRootRecord);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - dynamic meshes (rt decision 07, stage 3A)
//
// Shape (Neo, confirmed by the captures in research/04 7): ONE global BVH over the moving objects
// near the camera, rebuilt every frame into DYN_MESH (records 0 .. 30719) with its materials in
// DYN_MATS, root = record 0. The consumer walks that root after the static cell walk, sharing best.
//
// Two levels: a per object sub BVH that is built ONCE with the static SAH builder in OBJECT LOCAL
// space and cached across frames, plus a per frame median split top level over the object bounds.
// A rigid move is therefore a refit: the cached topology is kept, the triangles are transformed and
// the node boxes are recomputed bottom up from them (exact, unlike transforming the cached boxes).
// Both levels are flattened into ONE Neo shaped tree, so the consumer sees a single tree and needs
// nothing new from the renderer.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
//! Topology of one cached per object BVH. Boxes are deliberately NOT cached (see the refit note).
struct SRTDynNode
{
	int first;      //!< first triangle in SRTDynMesh::tris (leaves only)
	int count;      //!< triangle count, 0 on interior nodes
	int child0;     //!< index into SRTDynMesh::nodes, -1 on leaves; holds the LOWER half along splitAxis
	int child1;
	int splitAxis;  //!< 0/1/2, -1 on leaves
};

//! One cached object BVH, in OBJECT LOCAL space, keyed by (render mesh, lod).
struct SRTDynMesh
{
	_smart_ptr<IRenderMesh> pRM;      //!< holds a reference, so the cache key pointer can never be recycled
	PodArray<SRTBuildTri>   tris;     //!< local space, permuted into leaf order; matRecord = sub material id
	PodArray<int>           triVertIds; //!< 3 SOURCE vertex indices per triangle, same permutation as tris; only
	                                    //!< filled for meshes that can be skinned (rt decision 07, stage 3B)
	PodArray<SRTDynNode>    nodes;    //!< nodes[0] is the root, a child always has a higher index than its parent
	AABB                    localBox;
	int                     depth = 0;
	uint                    lastUsedFrame = 0;
};

typedef std::map<std::pair<const void*, int>, SRTDynMesh*> TRTDynCache;

//! One character skin's CPU skinned vertices for this frame, kept across frames so that a character
//! whose skinning data is not ready (it has not been submitted to the renderer yet, or its bone job
//! is still queued) keeps the LAST GOOD pose instead of snapping back to the bind pose.
//! Keyed by (skin attachment, lod) - two characters sharing one skin asset are two entries, because
//! two characters in different poses are two different sets of positions.
struct SRTSkinVerts
{
	PodArray<Vec3> pos;                //!< object local, in the skin's own space (the character matrix still applies)
	PodArray<Vec3> norm;
	uint           lastUsedFrame = 0;
	uint           lastSkinnedFrame = 0;  //!< the frame GetSkinnedVertices() was last ATTEMPTED
	bool           bValid = false;        //!< pos / norm hold a real pose (this frame's or an older one)
};

typedef std::map<std::pair<const void*, int>, SRTSkinVerts*> TRTSkinCache;

//! One object instance collected for this frame.
struct SRTDynInst
{
	SRTDynMesh* pMesh;
	IMaterial*  pMat;        //!< the instance's material (sub materials are resolved per chunk id)
	Matrix34    mat;         //!< object to world
	AABB        worldBox;
	Vec4        qb;          //!< quantisation bounds of this object subtree
	float       importance;  //!< projected size; the budget keeps the biggest
	int         waterMat;    //!< -1 = a normal mesh; else the index of this water surface's ready made material record set (rt decision 09 9.4)
	int         triOfs;      //!< first world space triangle of this instance
	int         boxOfs;      //!< first node box of this instance

	// character skinning (rt decision 07, stage 3B)
	IAttachmentSkin*    pSkinAtt;  //!< the skin attachment this instance came from, else null
	int                 skinLod;   //!< the LOD pSkinAtt was resolved at
	const SRTSkinVerts* pSkin;     //!< the skinned vertices to refit onto, else null (bind pose)
};

//! Everything the emitter needs; frame local, main thread only.
struct SRTDynFrame
{
	PodArray<SRTDynInst>  insts;
	PodArray<SRTBuildTri> worldTris;   //!< per instance blocks, already carrying the ABSOLUTE matRecord
	PodArray<AABB>        nodeBoxes;   //!< per instance blocks, one per cached node
	PodArray<Vec4>        records;     //!< DYN_MESH records, index 0 == record 0
	PodArray<Vec4>        matRecords;  //!< DYN_MATS records
	PodArray<int>         texSlices;   //!< atlas slices referenced this frame
	PodArray<SRTMatRecordSet> waterMats;   //!< one ready made record set per emitted water surface
	int                   overflowed = 0;
	int                   waterObjs = 0;
	int                   waterTris = 0;
};

TRTDynCache   g_rtDynCache;
TRTSkinCache  g_rtSkinCache;
PodArray<int> g_rtDynPrevTexSlices;
int           g_rtDynPrevRecords = 0;
int           g_rtDynPrevMats = 0;

// cached dynamic object query (report 06c F9); see the comment at the collect step
PodArray<IRenderNode*> g_rtDynNodesCached;
Vec3                   g_rtDynCacheCamPos(0.f, 0.f, 0.f);
uint                   g_rtDynCacheFrameId = ~0u;
bool                   g_rtDynCacheValid = false;

//! Quantisation bounds of one world box: XY centre plus the larger XY extent, as for a static cell.
Vec4 RT_DynQuantBounds(const AABB& box)
{
	float size = max(box.max.x - box.min.x, box.max.y - box.min.y);
	size = max(size, 0.05f) * (1.f + 1.f / 1024.f);
	return Vec4((box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, 0.f, size);
}

//! Reads one render mesh through the CPU accessible streams and appends its triangles in LOCAL space.
//! This is the same stream set CE's own SVO triangle collector uses (RenderMeshUtils.cpp
//! ProcessBoxIntersection): GetPosPtr / GetUVPtr return float32 caches even for half float vertex
//! formats, and the vertex normal comes out of the tangent frame stream.
bool RT_ExtractMeshTris(IRenderMesh* pRM, IMaterial* pMat, PodArray<SRTBuildTri>& arrOut, int maxTris, PodArray<int>* pVertIdsOut)
{
	if (!pRM || pRM->GetVerticesCount() <= 0 || pRM->GetIndicesCount() < 3)
		return false;

	pRM->LockForThreadAccess();

	int32 posStride = 0, uvStride = 0, tangStride = 0;

	const int nVerts = pRM->GetVerticesCount();
	const int nInds = pRM->GetIndicesCount();

	const uint8*   pPos = (const uint8*)pRM->GetPosPtr(posStride, FSL_READ);
	const vtx_idx* pInds = pRM->GetIndexPtr(FSL_READ);
	const uint8*   pUV = pPos ? (const uint8*)pRM->GetUVPtr(uvStride, FSL_READ) : nullptr;
	const uint8*   pTangs = pPos ? (const uint8*)pRM->GetTangentPtr(tangStride, FSL_READ) : nullptr;

	// GetTangentPtr falls back to the QTangent stream; the two structures have different sizes
	const bool bQTangents = (pTangs != nullptr) && (tangStride < (int32)sizeof(SPipTangents));

	const int startCount = arrOut.Count();

	if (pPos && pInds)
	{
		TRenderChunkArray& chunks = pRM->GetChunks();

		for (int c = 0; c < (int)chunks.size() && arrOut.Count() < maxTris; c++)
		{
			const CRenderChunk& chunk = chunks[c];

			if ((chunk.m_nMatFlags & MTL_FLAG_NODRAW) || !chunk.pRE)
				continue;

			const int matId = (int)chunk.m_nMatID;

			if (pMat)
			{
				// same visibility filter CE applies when it collects triangles for voxelization
				const SShaderItem& si = pMat->GetShaderItem(matId);
				IShader*           pSh = si.m_pShader;

				if (!pSh || (pSh->GetFlags() & EF_NODRAW) || (pSh->GetFlags() & EF_DECAL) ||
				    (pSh->GetShaderType() != eST_General && pSh->GetShaderType() != eST_Vegetation))
					continue;
			}

			const uint iEnd = chunk.nFirstIndexId + chunk.nNumIndices;

			for (uint ii = chunk.nFirstIndexId; ii + 2 < iEnd && arrOut.Count() < maxTris; ii += 3)
			{
				const int I[3] = { (int)pInds[ii + 0], (int)pInds[ii + 1], (int)pInds[ii + 2] };

				if (I[0] >= nVerts || I[1] >= nVerts || I[2] >= nVerts || I[0] < 0 || I[1] < 0 || I[2] < 0)
					continue;

				SRTBuildTri bt;

				for (int v = 0; v < 3; v++)
				{
					bt.v[v] = *(const Vec3*)(pPos + (size_t)posStride * I[v]);
					bt.t[v] = pUV ? *(const Vec2*)(pUV + (size_t)uvStride * I[v]) : Vec2(0.f, 0.f);

					if (pTangs)
						bt.n[v] = bQTangents ? ((const SPipQTangents*)(pTangs + (size_t)tangStride * I[v]))->GetN()
						                     : ((const SPipTangents*)(pTangs + (size_t)tangStride * I[v]))->GetN();
					else
						bt.n[v] = Vec3(0, 0, 0);
				}

				bt.faceNorm = (bt.v[1] - bt.v[0]).Cross(bt.v[2] - bt.v[0]);

				// degenerates give the consumer a zero determinant
				if (bt.faceNorm.GetLengthSquared() < 1e-16f)
					continue;

				bt.faceNorm.Normalize();

				for (int v = 0; v < 3; v++)
				{
					bt.n[v] = bt.n[v].GetNormalizedSafe(bt.faceNorm);
					if (bt.n[v].Dot(bt.faceNorm) < 0.f)
						bt.n[v] = -bt.n[v];
				}

				// UVs are normalised per triangle into [0, 16), exactly as the static producer does
				const Vec2 tcMin(min(bt.t[0].x, min(bt.t[1].x, bt.t[2].x)), min(bt.t[0].y, min(bt.t[1].y, bt.t[2].y)));
				for (int v = 0; v < 3; v++)
				{
					bt.t[v].x = CLAMP(bt.t[v].x - floor(tcMin.x), 0.f, 16.f - 1.f / 4096.f);
					bt.t[v].y = CLAMP(bt.t[v].y - floor(tcMin.y), 0.f, 16.f - 1.f / 4096.f);
				}

				bt.matRecord = matId;   // sub material id here; made an absolute record index per frame

				// the three source vertex indices, so a deformed copy of this mesh can be looked up
				// per vertex later (rt decision 07, stage 3B); kept in step with arrOut exactly
				if (pVertIdsOut)
				{
					pVertIdsOut->Add(I[0]);
					pVertIdsOut->Add(I[1]);
					pVertIdsOut->Add(I[2]);
				}

				arrOut.Add(bt);
			}
		}
	}

	pRM->UnlockStream(VSF_GENERAL);
	pRM->UnlockStream(VSF_TANGENTS);
	pRM->UnlockStream(VSF_QTANGENTS);
	pRM->UnlockIndexStream();
	pRM->UnLockForThreadAccess();

	return arrOut.Count() > startCount;
}

//! Builds the cached topology of one object: the static SAH builder, then the triangles permuted
//! into leaf order so a node's (first, count) indexes them directly. Local space, boxes discarded.
void RT_BuildDynMeshTopology(PodArray<SRTBuildTri>& arrSrc, const PodArray<int>* pVertIds, SRTDynMesh& out)
{
	// The top level over the objects and this subtree share the 18 levels the consumer's stack is
	// sized for, so the object build is capped at maxDepth - SVO_RT_DYN_TOP_MAX_DEPTH.
	SRTBuilder bld;
	bld.leafTris = max(1, Cry3DEngineBase::GetCVars()->e_svoTI_RT_LeafTris);
	bld.maxDepth = CLAMP(Cry3DEngineBase::GetCVars()->e_svoTI_RT_MaxDepth - SVO_RT_DYN_TOP_MAX_DEPTH, 1, 18 - SVO_RT_DYN_TOP_MAX_DEPTH);
	bld.maxDepthSeen = 0;
	bld.maxLeafTris = 0;
	bld.leafCount = 0;

	bld.refs.PreAllocate(arrSrc.Count(), 0);
	for (int i = 0; i < arrSrc.Count(); i++)
	{
		SRTRef ref;
		ref.box.Reset();
		ref.box.Add(arrSrc[i].v[0]);
		ref.box.Add(arrSrc[i].v[1]);
		ref.box.Add(arrSrc[i].v[2]);
		ref.centroid = (arrSrc[i].v[0] + arrSrc[i].v[1] + arrSrc[i].v[2]) / 3.f;
		ref.triId = i;
		bld.refs.Add(ref);
	}

	bld.nodes.PreAllocate(arrSrc.Count() * 2 + 8, 0);
	bld.Split(bld.AddNode(0, bld.refs.Count(), 0));

	// permute the triangles into leaf order, so a node's (first, count) indexes tris directly
	out.tris.PreAllocate(arrSrc.Count(), 0);
	for (int i = 0; i < bld.refs.Count(); i++)
		out.tris.Add(arrSrc[bld.refs[i].triId]);

	// the source vertex ids follow the very same permutation, so triVertIds[3 * t + v] always belongs
	// to tris[t] whatever the builder did with the order
	if (pVertIds && pVertIds->Count() == arrSrc.Count() * 3)
	{
		out.triVertIds.PreAllocate(arrSrc.Count() * 3, 0);
		for (int i = 0; i < bld.refs.Count(); i++)
		{
			const int t = bld.refs[i].triId;
			out.triVertIds.Add((*pVertIds)[t * 3 + 0]);
			out.triVertIds.Add((*pVertIds)[t * 3 + 1]);
			out.triVertIds.Add((*pVertIds)[t * 3 + 2]);
		}
	}

	out.nodes.PreAllocate(bld.nodes.Count(), 0);
	for (int n = 0; n < bld.nodes.Count(); n++)
	{
		const SRTNode& src = bld.nodes[n];
		SRTDynNode     dst;
		dst.first = src.first;
		dst.count = (src.child0 < 0) ? src.count : 0;
		dst.child0 = src.child0;
		dst.child1 = src.child1;
		dst.splitAxis = src.splitAxis;
		out.nodes.Add(dst);
	}

	out.localBox.Reset();
	for (int i = 0; i < out.tris.Count(); i++)
		for (int v = 0; v < 3; v++)
			out.localBox.Add(out.tris[i].v[v]);

	out.depth = bld.maxDepthSeen;
}

//! Builds (or returns) the cached local space BVH of one render mesh.
SRTDynMesh* RT_GetDynMesh(IRenderMesh* pRM, IMaterial* pMat, int lod, bool bWantVertIds = false)
{
	if (!pRM)
		return nullptr;

	const std::pair<const void*, int> key(pRM, lod);

	TRTDynCache::const_iterator it = g_rtDynCache.find(key);
	if (it != g_rtDynCache.end())
	{
		it->second->lastUsedFrame = GetCurrPassMainFrameID();

		// an entry built before anything asked for a deformable copy of this mesh has no vertex ids;
		// rebuilding it once is cheaper than carrying 12 bytes per triangle for every rigid prop
		const bool bNeedsRebuild = bWantVertIds && it->second->tris.Count() && !it->second->triVertIds.Count();

		if (!bNeedsRebuild)
			return it->second;

		delete it->second;
		g_rtDynCache.erase(key);
	}

	SRTDynMesh* pOut = new SRTDynMesh();
	pOut->pRM = pRM;
	pOut->lastUsedFrame = GetCurrPassMainFrameID();

	PodArray<SRTBuildTri> arrSrc;
	PodArray<int>         arrVertIds;

	// one object may not eat the whole segment on its own
	const int maxTris = SVO_RT_DYN_MESH_COUNT / 2;

	if (!RT_ExtractMeshTris(pRM, pMat, arrSrc, maxTris, bWantVertIds ? &arrVertIds : nullptr))
	{
		// cache the failure too, so a mesh without CPU accessible streams is not retried every frame
		pOut->localBox.Reset();
		g_rtDynCache[key] = pOut;
		return pOut;
	}

	RT_BuildDynMeshTopology(arrSrc, bWantVertIds ? &arrVertIds : nullptr, *pOut);

	g_rtDynCache[key] = pOut;
	return pOut;
}

//! Drops cache entries nothing referenced for a while (level change, object streamed out).
void RT_EvictDynCache()
{
	const uint frameId = GetCurrPassMainFrameID();

	for (TRTDynCache::iterator it = g_rtDynCache.begin(); it != g_rtDynCache.end(); )
	{
		if (frameId > it->second->lastUsedFrame + 600)
		{
			delete it->second;
			it = g_rtDynCache.erase(it);
		}
		else
		{
			++it;
		}
	}
}

//! CPU skins one skin attachment's LOD for this frame, or returns the last pose it managed to skin.
//!
//! The skinning itself is CryAnimation's - IAttachmentSkin::GetSkinnedVertices() drives the very same
//! VertexCommandSkin the engine's software skinning path uses, with the character's CURRENT bone dual
//! quaternions and CryAnimation's own skin -> skeleton remap table. We only own the buffers.
//!
//! Returns null when there is nothing usable at all (no CPU mesh, or the character has never been
//! submitted to the renderer, so no bone transforms exist yet) - the caller then traces the bind pose,
//! which is where stage 3A left it.
SRTSkinVerts* RT_GetSkinnedVerts(IAttachmentSkin* pSkinAtt, int lod, int& skinnedCharsOut, int& skinnedVertsOut, int& reusedOut)
{
	if (!pSkinAtt)
		return nullptr;

	const uint                        frameId = GetCurrPassMainFrameID();
	const std::pair<const void*, int> key(pSkinAtt, lod);

	SRTSkinVerts*          pEntry = nullptr;
	TRTSkinCache::iterator it = g_rtSkinCache.find(key);

	if (it != g_rtSkinCache.end())
	{
		pEntry = it->second;
	}
	else
	{
		pEntry = new SRTSkinVerts();
		g_rtSkinCache[key] = pEntry;
	}

	pEntry->lastUsedFrame = frameId;

	// two instances of one attachment cannot happen, but a second call in the same frame can (the
	// object is collected twice); skin once per frame and hand the same buffer out again
	if (pEntry->lastSkinnedFrame == frameId)
		return pEntry->bValid ? pEntry : nullptr;

	pEntry->lastSkinnedFrame = frameId;

	const uint32 vertexCount = pSkinAtt->GetSkinnedVertexCount((uint32)lod);

	if (!vertexCount)
		return pEntry->bValid ? pEntry : nullptr;

	// PodArray::PreAllocate reallocates and KEEPS what is there, so a failed attempt below leaves the
	// previous frame's pose in place instead of a half written one
	pEntry->pos.PreAllocate((int)vertexCount, (int)vertexCount);
	pEntry->norm.PreAllocate((int)vertexCount, (int)vertexCount);

	// tangents are not requested: the ray tracer rebuilds the tangent frame from the world space UV
	// gradient at the hit, so skinning them would be work nobody reads
	const uint32 written = pSkinAtt->GetSkinnedVertices((uint32)lod, pEntry->pos.GetElements(), pEntry->norm.GetElements(), nullptr, vertexCount);

	if (written == vertexCount)
	{
		pEntry->bValid = true;
		skinnedCharsOut++;
		skinnedVertsOut += (int)written;
		return pEntry;
	}

	// not ready this frame (no skinning data yet, or the pose is still being computed): keep the last
	// good one rather than snap the reflection back to the bind pose in the middle of an animation
	if (pEntry->bValid)
		reusedOut++;

	return pEntry->bValid ? pEntry : nullptr;
}

//! Same eviction rule as the mesh cache: an attachment nothing referenced for a while is gone.
void RT_EvictSkinCache()
{
	const uint frameId = GetCurrPassMainFrameID();

	for (TRTSkinCache::iterator it = g_rtSkinCache.begin(); it != g_rtSkinCache.end(); )
	{
		if (frameId > it->second->lastUsedFrame + 600)
		{
			delete it->second;
			it = g_rtSkinCache.erase(it);
		}
		else
		{
			++it;
		}
	}
}

//! Refit: recompute the cached node boxes of one instance bottom up from its transformed triangles.
//! A child always has a higher index than its parent (SRTBuilder appends children after the parent),
//! so one reverse pass is enough. Recomputing is exact; transforming the cached boxes would not be.
void RT_RefitDynInst(SRTDynFrame& f, const SRTDynInst& inst)
{
	const SRTDynMesh& mesh = *inst.pMesh;
	const int         nodeCount = mesh.nodes.Count();
	const int         boxOfs = inst.boxOfs;

	f.nodeBoxes.PreAllocate(boxOfs + nodeCount, boxOfs + nodeCount);

	for (int n = nodeCount - 1; n >= 0; n--)
	{
		const SRTDynNode& node = mesh.nodes[n];
		AABB&             box = f.nodeBoxes[boxOfs + n];

		box.Reset();

		if (node.child0 < 0)
		{
			for (int t = 0; t < node.count; t++)
				for (int v = 0; v < 3; v++)
					box.Add(f.worldTris[inst.triOfs + node.first + t].v[v]);
		}
		else
		{
			box.Add(f.nodeBoxes[boxOfs + node.child0]);
			box.Add(f.nodeBoxes[boxOfs + node.child1]);
		}
	}
}

//! Writes one node record (rt decision 02, 2.5).
void RT_WriteNodeRecord(Vec4* pRec, const AABB& box, int triCount, int subtreeEnd, const Vec4& qb, int child0, int child1, int splitAxis)
{
	pRec[0] = Vec4(box.min, (float)triCount);
	pRec[1] = Vec4(box.max, (float)subtreeEnd);
	pRec[2] = qb;
	pRec[3] = Vec4((float)child0, (float)child1, 0.f, (float)splitAxis);
}

//! Emits one cached object subtree at record rec, with the instance's transform already applied.
//! Returns the first record after the subtree.
int RT_EmitDynObj(SRTDynFrame& f, const SRTDynInst& inst, int nodeIdx, int rec)
{
	const SRTDynMesh& mesh = *inst.pMesh;
	const SRTDynNode& node = mesh.nodes[nodeIdx];
	const AABB&       box = f.nodeBoxes[inst.boxOfs + nodeIdx];

	Vec4* pRec = f.records.GetElements() + (size_t)rec * SVO_RT_RECORD_TEXELS;

	if (node.child0 < 0)
	{
		const int end = rec + 1 + node.count;

		RT_WriteNodeRecord(pRec, box, node.count, end, inst.qb, 0, 0, -1);

		for (int t = 0; t < node.count; t++)
		{
			const SRTBuildTri& tr = f.worldTris[inst.triOfs + node.first + t];
			RT_WriteTriRecord(tr, inst.qb, tr.matRecord, pRec + (size_t)(1 + t) * SVO_RT_RECORD_TEXELS);
		}

		return end;
	}

	const int c0 = rec + 1;
	const int c1 = RT_EmitDynObj(f, inst, node.child0, c0);
	const int end = RT_EmitDynObj(f, inst, node.child1, c1);

	RT_WriteNodeRecord(pRec, box, 0, end, inst.qb, c0, c1, node.splitAxis);

	return end;
}

//! The top level hit its depth cap with more than one object left: everything below is merged into
//! ONE leaf that owns all their triangles. That costs traversal speed for those rays and nothing
//! else - it can never exceed the consumer's stack, which is the property we must not lose.
int RT_EmitDynOverflowLeaf(SRTDynFrame& f, const int* pIds, int count, int rec)
{
	AABB box;
	box.Reset();
	for (int i = 0; i < count; i++)
		box.Add(f.insts[pIds[i]].worldBox);

	int triCount = 0;
	for (int i = 0; i < count; i++)
		triCount += f.insts[pIds[i]].pMesh->tris.Count();

	const Vec4 qb = RT_DynQuantBounds(box);
	const int  end = rec + 1 + triCount;

	Vec4* pRec = f.records.GetElements() + (size_t)rec * SVO_RT_RECORD_TEXELS;
	RT_WriteNodeRecord(pRec, box, triCount, end, qb, 0, 0, -1);

	int written = 0;
	for (int i = 0; i < count; i++)
	{
		const SRTDynInst& inst = f.insts[pIds[i]];
		for (int t = 0; t < inst.pMesh->tris.Count(); t++, written++)
		{
			const SRTBuildTri& tr = f.worldTris[inst.triOfs + t];
			RT_WriteTriRecord(tr, qb, tr.matRecord, pRec + (size_t)(1 + written) * SVO_RT_RECORD_TEXELS);
		}
	}

	f.overflowed += count;

	return end;
}

//! Per frame top level: median split over the object bounds (rt decision 03's "alternatives kept" -
//! the loose splitter is the right one for a per frame rebuild).
int RT_EmitDynTop(SRTDynFrame& f, int* pIds, int count, int depth, int rec, const Vec4& rootQb)
{
	if (count == 1)
		return RT_EmitDynObj(f, f.insts[pIds[0]], 0, rec);

	if (depth >= SVO_RT_DYN_TOP_MAX_DEPTH)
		return RT_EmitDynOverflowLeaf(f, pIds, count, rec);

	AABB centroidBox;
	centroidBox.Reset();
	for (int i = 0; i < count; i++)
		centroidBox.Add(f.insts[pIds[i]].worldBox.GetCenter());

	const Vec3 ext = centroidBox.max - centroidBox.min;

	int axis = 0;
	if (ext.y > ext[axis]) axis = 1;
	if (ext.z > ext[axis]) axis = 2;

	// child0 must hold the LOWER half along splitAxis (rt decision 02, 2.5)
	SRTDynFrame* pF = &f;
	std::stable_sort(pIds, pIds + count, [pF, axis](int a, int b)
	{
		const float ca = pF->insts[a].worldBox.GetCenter()[axis];
		const float cb = pF->insts[b].worldBox.GetCenter()[axis];
		return (ca != cb) ? (ca < cb) : (a < b);
	});

	const int mid = count / 2;

	const int c0 = rec + 1;
	const int c1 = RT_EmitDynTop(f, pIds, mid, depth + 1, c0, rootQb);
	const int end = RT_EmitDynTop(f, pIds + mid, count - mid, depth + 1, c1, rootQb);

	AABB box;
	box.Reset();
	for (int i = 0; i < count; i++)
		box.Add(f.insts[pIds[i]].worldBox);

	RT_WriteNodeRecord(f.records.GetElements() + (size_t)rec * SVO_RT_RECORD_TEXELS, box, 0, end, rootQb, c0, c1, axis);

	return end;
}

//! Exactly which render nodes are NOT in the static triangle soup.
//!
//! CheckCollectObjectsForVoxelization (VoxelSegment.cpp) only ever queries eERType_Brush,
//! eERType_MovableBrush and eERType_Vegetation, drops the proxy / pending-delete / hidden flags, and
//! then requires GetGIMode() == eGM_StaticVoxelization. So:
//!   - characters are never in the soup at all -> always dynamic;
//!   - a brush with any other GI mode is dropped by that filter -> dynamic;
//!   - a brush flagged ERF_MOVES_EVERY_FRAME has a soup copy, but it is at last voxelization's
//!     position, i.e. stale by construction -> dynamic as well (the stale static copy stays, see
//!     the report's known gaps);
//!   - eGM_HideIfGiIsActive and eGM_IntegrateIntoTerrain are explicit author requests not to appear
//!     as themselves, and are honoured here too;
//!   - vegetation is not collected (decision 07: bending would need a per instance deformation the
//!     cached, rigidly transformed subtree cannot express).
bool RT_IsDynamicForRT(IRenderNode* pRN)
{
	if (!pRN)
		return false;

	const auto flags = pRN->GetRndFlags();

	if (flags & (ERF_COLLISION_PROXY | ERF_RAYCAST_PROXY | ERF_PENDING_DELETE | ERF_HIDDEN))
		return false;

	if (pRN->IsHidden())
		return false;

	const IRenderNode::EGIMode giMode = pRN->GetGIMode();

	if (giMode == IRenderNode::eGM_HideIfGiIsActive || giMode == IRenderNode::eGM_IntegrateIntoTerrain)
		return false;

	if (pRN->GetRenderNodeType() == eERType_Character)
		return true;

	if (giMode == IRenderNode::eGM_StaticVoxelization && !(flags & ERF_MOVES_EVERY_FRAME))
		return false;

	return true;
}

//! One (mesh, material, transform) triple to add to this frame.
struct SRTDynSource
{
	IRenderMesh*     pRM;
	IMaterial*       pMat;
	Matrix34         worldTM;   //!< object (or sub object, or bone) to world; the cache stays local space
	int              lod;
	IAttachmentSkin* pSkinAtt;  //!< set only for a skin attachment, so its vertices can be CPU skinned (stage 3B)
};

//! Resolves one render node into the meshes it contributes.
void RT_CollectNodeSources(IRenderNode* pRN, int lodWanted, PodArray<SRTDynSource>& arrOut)
{
	Matrix34A nodeTM;
	nodeTM.SetIdentity();

	if (ICharacterInstance* pChar = pRN->GetEntityCharacter(&nodeTM, false))
	{
		IMaterial* pNodeMat = pRN->GetMaterial();

		// SKEL / CGA geometry that still lives on the skeleton
		if (IRenderMesh* pRM = pChar->GetIDefaultSkeleton().GetIRenderMesh())
		{
			SRTDynSource src;
			src.pRM = pRM;
			src.pMat = pNodeMat ? pNodeMat : pChar->GetIMaterial();
			src.worldTM = nodeTM;
			src.lod = 0;
			src.pSkinAtt = nullptr;
			arrOut.Add(src);
		}

		if (IAttachmentManager* pAM = pChar->GetIAttachmentManager())
		{
			const int attCount = pAM->GetAttachmentCount();

			for (int a = 0; a < attCount; a++)
			{
				IAttachment* pAtt = pAM->GetInterfaceByIndex(a);
				if (!pAtt || pAtt->IsAttachmentHidden())
					continue;

				IAttachmentObject* pAttObj = pAtt->GetIAttachmentObject();
				if (!pAttObj)
					continue;

				const IAttachmentObject::EType type = pAttObj->GetAttachmentType();

				if (type == IAttachmentObject::eAttachment_SkinMesh)
				{
					IAttachmentSkin* pSkinAtt = pAttObj->GetIAttachmentSkin();
					ISkin*           pSkin = pSkinAtt ? pSkinAtt->GetISkin() : nullptr;
					if (!pSkin)
						continue;

					const int lod = CLAMP(lodWanted, 0, max(0, (int)pSkin->GetNumLODs() - 1));

					IRenderMesh* pRM = pSkin->GetIRenderMesh(lod);
					if (!pRM)
						continue;

					IMaterial* pMat = pAttObj->GetReplacementMaterial(lod);
					if (!pMat)
						pMat = pAttObj->GetBaseMaterial(lod);

					SRTDynSource src;
					src.pRM = pRM;
					src.pMat = pMat;
					// The BIND POSE mesh and its topology, placed by the character's world matrix.
					// Stage 3B then replaces its vertex POSITIONS per frame with the CPU skinned ones
					// (IAttachmentSkin::GetSkinnedVertices, see the skinning budget in
					// RTUpdateDynamic) and refits the cached BVH onto them; the world matrix stays
					// the character's, exactly as it is for a software skinned character.
					// Without a skinning budget, or when the character has no skinning data yet, the
					// bind pose is what gets traced - the stage 3A behaviour.
					src.worldTM = nodeTM;
					src.lod = lod;
					src.pSkinAtt = pSkinAtt;
					arrOut.Add(src);
				}
				else if (type == IAttachmentObject::eAttachment_StatObj)
				{
					IStatObj* pStatObj = pAttObj->GetIStatObj();
					if (!pStatObj)
						continue;

					IStatObj* pLod = (lodWanted > 0) ? pStatObj->GetLodObject(lodWanted, true) : pStatObj;
					if (!pLod)
						pLod = pStatObj;

					IRenderMesh* pRM = pLod->GetRenderMesh();
					if (!pRM)
						continue;

					IMaterial* pMat = pAttObj->GetReplacementMaterial(0);
					if (!pMat)
						pMat = pAttObj->GetBaseMaterial(0);
					if (!pMat)
						pMat = pLod->GetMaterial();

					SRTDynSource src;
					src.pRM = pRM;
					src.pMat = pMat;
					// bone attachments DO follow the animation - the joint transform is world space
					src.worldTM = Matrix34(pAtt->GetAttWorldAbsolute());
					src.lod = lodWanted;
					src.pSkinAtt = nullptr;
					arrOut.Add(src);
				}
			}
		}

		return;
	}

	IStatObj* pStatObj = pRN->GetEntityStatObj(0, &nodeTM, false);
	if (!pStatObj)
		return;

	if (pStatObj->GetFlags() & STATIC_OBJECT_HIDDEN)
		return;

	IStatObj* pLod = (lodWanted > 0) ? pStatObj->GetLodObject(lodWanted, true) : pStatObj;
	if (!pLod)
		pLod = pStatObj;

	IMaterial* pMat = pRN->GetMaterial();
	if (!pMat)
		pMat = pLod->GetMaterial();

	if (IRenderMesh* pRM = pLod->GetRenderMesh())
	{
		SRTDynSource src;
		src.pRM = pRM;
		src.pMat = pMat;
		src.worldTM = nodeTM;
		src.lod = lodWanted;
		src.pSkinAtt = nullptr;
		arrOut.Add(src);
		return;
	}

	// compound CGF: the geometry sits in the sub objects, each with its own local matrix
	for (int s = 0; s < pLod->GetSubObjectCount(); s++)
	{
		IStatObj::SSubObject* pSub = pLod->GetSubObject(s);

		if (!pSub || pSub->bHidden || pSub->nType != STATIC_SUB_OBJECT_MESH || !pSub->pStatObj)
			continue;

		IRenderMesh* pRM = pSub->pStatObj->GetRenderMesh();
		if (!pRM)
			continue;

		SRTDynSource src;
		src.pRM = pRM;
		src.pMat = pMat ? pMat : pSub->pStatObj->GetMaterial();
		// the sub object transform is folded into the INSTANCE matrix, never into the cached mesh,
		// so two sub objects sharing one render mesh still share one cache entry
		src.worldTM = pSub->bIdentityMatrix ? Matrix34(nodeTM) : (Matrix34(nodeTM) * pSub->localTM);
		src.lod = lodWanted;
		src.pSkinAtt = nullptr;
		arrOut.Add(src);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Water surfaces as traceable geometry (rt decision 09 9.4)
//
// Water is in NO static soup: CheckCollectObjectsForVoxelization only ever queries eERType_Brush,
// eERType_MovableBrush and eERType_Vegetation, and the ocean is procedural (COcean is a fake render
// node that is never registered in the object tree at all). So a reflection ray could not see water
// before this stage, at any setting.
//
// Both halves are emitted into the per frame DYN_MESH segment as ordinary objects of the two level
// dynamic tree, so they inherit its budget, its quantisation and its tail zeroing for free:
//   * the ocean  -> ONE camera centred quad grid at the water level, rebuilt only when its size
//                   changes; the per frame cost is the instance translation, nothing else;
//   * a volume   -> the render node's own surface triangles (CWaterVolumeRenderNode's
//                   m_waterSurfaceVertices / m_waterSurfaceIndices - exactly the mesh
//                   Render_JobEntry hands to CREWaterVolume), in the node's space, placed by the
//                   same matrix the render object gets.
// Both carry a material record tagged SVO_RT_TAG_WATER (4) with the fog colour, the fog density and
// a depth hint; see report 05c for the field table.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//! One water surface ready to be added to this frame's instance list.
struct SRTWaterSurface
{
	SRTDynMesh*     pMesh;
	Matrix34        mat;
	AABB            worldBox;
	SRTMatRecordSet matSet;
};

//! Cached local space BVH of one water surface. Keyed by owner pointer, re-validated by a cheap
//! signature every frame so a changed (or recycled) owner can never keep a stale subtree.
struct SRTWaterCacheEntry
{
	SRTDynMesh mesh;
	int64      sig = 0;
	uint       lastUsedFrame = 0;
};

typedef std::map<const void*, SRTWaterCacheEntry*> TRTWaterCache;

TRTWaterCache g_rtWaterCache;
const int     g_rtOceanKey = 0;   //!< stable address used as the ocean's cache key

void RT_ClearWaterCache()
{
	for (TRTWaterCache::iterator it = g_rtWaterCache.begin(); it != g_rtWaterCache.end(); ++it)
		delete it->second;

	g_rtWaterCache.clear();
}

void RT_EvictWaterCache()
{
	const uint frameId = GetCurrPassMainFrameID();

	for (TRTWaterCache::iterator it = g_rtWaterCache.begin(); it != g_rtWaterCache.end(); )
	{
		if (frameId > it->second->lastUsedFrame + 600)
		{
			delete it->second;
			it = g_rtWaterCache.erase(it);
		}
		else
		{
			++it;
		}
	}
}

//! Per triangle UV normalisation into [0, 16), byte for byte the rule the static builder applies
//! before RT_WriteTriRecord packs the three PackTC16 pairs.
void RT_NormalizeTriUV(SRTBuildTri& bt)
{
	const Vec2 tcMin(min(bt.t[0].x, min(bt.t[1].x, bt.t[2].x)), min(bt.t[0].y, min(bt.t[1].y, bt.t[2].y)));

	for (int v = 0; v < 3; v++)
	{
		bt.t[v].x -= floor(tcMin.x);
		bt.t[v].y -= floor(tcMin.y);
		bt.t[v].x = CLAMP(bt.t[v].x, 0.f, RT_TC16_MAX);
		bt.t[v].y = CLAMP(bt.t[v].y, 0.f, RT_TC16_MAX);
	}
}

//! One flat, upward facing quad split into grid x grid cells (2 triangles each). The winding makes
//! the face normal +Z, so the surface is single sided seen from above - a ray arriving from below
//! hits its BACK face and the consumer must reject it (report 05c, back face policy).
void RT_AddWaterQuad(PodArray<SRTBuildTri>& out, const Vec3& org, const Vec3& du, const Vec3& dv, int grid)
{
	const Vec3 n = du.Cross(dv).GetNormalizedSafe(Vec3(0, 0, 1));

	for (int i = 0; i < grid; i++)
	{
		for (int j = 0; j < grid; j++)
		{
			const float u0 = (float)i / grid, u1 = (float)(i + 1) / grid;
			const float v0 = (float)j / grid, v1 = (float)(j + 1) / grid;

			const Vec3 p00 = org + du * u0 + dv * v0;
			const Vec3 p10 = org + du * u1 + dv * v0;
			const Vec3 p11 = org + du * u1 + dv * v1;
			const Vec3 p01 = org + du * u0 + dv * v1;

			SRTBuildTri a;
			a.v[0] = p00; a.v[1] = p10; a.v[2] = p11;
			a.t[0] = Vec2(u0, v0); a.t[1] = Vec2(u1, v0); a.t[2] = Vec2(u1, v1);
			a.n[0] = a.n[1] = a.n[2] = a.faceNorm = n;
			a.matRecord = 0;
			out.Add(a);

			SRTBuildTri b;
			b.v[0] = p00; b.v[1] = p11; b.v[2] = p01;
			b.t[0] = Vec2(u0, v0); b.t[1] = Vec2(u1, v1); b.t[2] = Vec2(u0, v1);
			b.n[0] = b.n[1] = b.n[2] = b.faceNorm = n;
			b.matRecord = 0;
			out.Add(b);
		}
	}
}

//! The material record of a water surface (rt decision 09 9.4, layout in report 05c 3).
//! The record carries no textures: a water hit is shaded from the fog colour and the sky, not from
//! an albedo map, so nothing is pulled into the atlas for it.
void RT_FillWaterMatRecord(IMaterial* pMat, const Vec3& fogColor, float fogDensity, float depthHint, SRTMatRecordSet& out)
{
	ZeroStruct(out);

	float sunSpecMul = 1.f;
	float reflAmount = 1.f;

	if (pMat)
	{
		SShaderItem& shItem = pMat->GetShaderItem();

		if (IRenderShaderResources* pRes = shItem.m_pShaderResources)
		{
			// Water.cfx:632   OUT.cSpecular.xyz = MatSpecColor * CV_SunColor.xyz * CV_SunColor.w
			// i.e. the only CPU visible scalar in front of the sun specular lobe is MatSpecColor,
			// which is exactly GetColorValue(EFTT_SPECULAR) (REG_PM_SPECULAR_COL).
			const ColorF spec = pRes->GetColorValue(EFTT_SPECULAR);
			sunSpecMul = max(0.f, (spec.r + spec.g + spec.b) / 3.f);

			// Water.cfx:103        ReflectionScale  "Reflection scale"   REG_PM_PARAM_1.z, default 1
			// WaterVolume.cfx:355  ssrBGMultiplier  "SSR sky reflection" REG_PM_PARAM_7.z, default 1
			// CE only stores the parameters a .mtl actually overrides, so an absent name keeps the
			// shader default of 1.
			if (!RT_GetMatParam(pRes, "ReflectionScale", &reflAmount, 1))
				RT_GetMatParam(pRes, "ssrBGMultiplier", &reflAmount, 1);

			reflAmount = max(0.f, reflAmount);
		}
	}

	out.bExtras = true;    // a water record ALWAYS has extras - that is where the fog lives
	out.bBlend = false;

	// base record: no atlas slices at all, extras reference 1 (the record at base + 1)
	out.base[0] = Vec4(0.f, RT_PackUint2(0, 0), RT_PackUint2(0, 1), RT_PackTC16(1.f, 1.f));
	out.base[1] = Vec4(1.f, 0.02f, 1.f, 0.f);   // normal strength, F0 = 0.02 (water), smoothness 1, no emissive
	out.base[2] = Vec4(fogColor.x, fogColor.y, fogColor.z, 0.f);
	out.base[3] = Vec4(0.f, 0.f, SVO_RT_TAG_WATER, RT_PackTC16(1.f, 0.f));   // opaque, no alpha test

	out.extras[0] = Vec4(RT_PackUint2(0, 0), 1.f, 1.f, RT_PackTC16(0.f, 0.f));   // no detail map
	out.extras[1] = Vec4(0.f, 0.f, 0.f, 0.f);                                    // no blend layer
	out.extras[2] = Vec4(fogColor.x, fogColor.y, fogColor.z, max(0.f, fogDensity));
	out.extras[3] = Vec4(sunSpecMul, reflAmount, max(0.f, depthHint), 0.f);
}

//! Cache lookup / (re)build of one water surface's local space BVH.
SRTWaterCacheEntry* RT_GetWaterCacheEntry(const void* key, int64 sig)
{
	TRTWaterCache::const_iterator it = g_rtWaterCache.find(key);

	if (it != g_rtWaterCache.end())
	{
		it->second->lastUsedFrame = GetCurrPassMainFrameID();

		if (it->second->sig == sig)
			return nullptr;   // cached and still valid: the caller must not rebuild it

		it->second->sig = sig;
		it->second->mesh.tris.Clear();
		it->second->mesh.nodes.Clear();
		return it->second;
	}

	SRTWaterCacheEntry* pNew = new SRTWaterCacheEntry();
	pNew->sig = sig;
	pNew->lastUsedFrame = GetCurrPassMainFrameID();
	g_rtWaterCache[key] = pNew;

	return pNew;
}

SRTDynMesh* RT_GetCachedWaterMesh(const void* key)
{
	TRTWaterCache::const_iterator it = g_rtWaterCache.find(key);

	return (it != g_rtWaterCache.end()) ? &it->second->mesh : nullptr;
}

//! Collects the ocean plane and every visible water volume near the camera.
void RT_CollectWaterSurfaces(const Vec3& camPos, float distCam, float distRay, PodArray<SRTWaterSurface>& arrOut)
{
	C3DEngine* p3DEngine = Cry3DEngineBase::Get3DEngine();

	if (!p3DEngine)
		return;

	const int  gridDim = CLAMP(Cry3DEngineBase::GetCVars()->e_svoTI_RT_WaterGrid, 1, 4);
	const bool bNoDraw = (p3DEngine->GetOceanRenderFlags() & OCR_NO_DRAW) != 0;

	// ---- the ocean ---------------------------------------------------------------------------
	{
		CTerrain*  pTerrain = Cry3DEngineBase::GetTerrain();
		COcean*    pOcean = pTerrain ? pTerrain->GetOcean() : nullptr;
		IMaterial* pOceanMat = pOcean ? pOcean->GetMaterial() : nullptr;
		const float waterLevel = p3DEngine->GetWaterLevel();

		if (pOcean && pOceanMat && !bNoDraw && Cry3DEngineBase::GetCVars()->e_WaterOcean &&
		    waterLevel > WATER_LEVEL_UNKNOWN && waterLevel < -WATER_LEVEL_UNKNOWN)
		{
			// the ring has to cover every point a ray can reach: the collection radius plus one ray
			const float radius = max(1.f, distCam + distRay);

			// the mesh is rebuilt only when its size or subdivision changes; the camera moves the
			// INSTANCE, so a walking camera costs one Matrix34 and nothing else
			const int64 sig = (int64)gridDim * 0x40000000ll + (int64)(radius * 16.f);

			if (SRTWaterCacheEntry* pEntry = RT_GetWaterCacheEntry(&g_rtOceanKey, sig))
			{
				PodArray<SRTBuildTri> arrSrc;
				RT_AddWaterQuad(arrSrc, Vec3(-radius, -radius, 0.f), Vec3(2.f * radius, 0.f, 0.f), Vec3(0.f, 2.f * radius, 0.f), gridDim);
				for (int i = 0; i < arrSrc.Count(); i++)
					RT_NormalizeTriUV(arrSrc[i]);
				RT_BuildDynMeshTopology(arrSrc, nullptr, pEntry->mesh);
			}

			if (SRTDynMesh* pMesh = RT_GetCachedWaterMesh(&g_rtOceanKey))
			{
				if (pMesh->tris.Count())
				{
					SRTWaterSurface ws;
					ws.pMesh = pMesh;
					ws.mat.SetIdentity();
					ws.mat.SetTranslation(Vec3(camPos.x, camPos.y, waterLevel));
					ws.worldBox = AABB(Vec3(camPos.x - radius, camPos.y - radius, waterLevel),
					                   Vec3(camPos.x + radius, camPos.y + radius, waterLevel));

					// ocean fog: E3DPARAM_OCEANFOG_COLOR / _DENSITY, the same two globals the time of day
					// writes (TimeOfDay.cpp:1393) and COcean::Render reads as m_oceanFogColor /
					// m_oceanFogDensity (terrain_water_quad.cpp:510)
					Vec3 fogColor(0.f, 0.f, 0.f);
					p3DEngine->GetGlobalParameter(E3DPARAM_OCEANFOG_COLOR, fogColor);
					const float fogDensity = p3DEngine->GetGlobalParameter(E3DPARAM_OCEANFOG_DENSITY);

					RT_FillWaterMatRecord(pOceanMat, fogColor, fogDensity,
					                      max(0.f, Cry3DEngineBase::GetCVars()->e_svoTI_RT_WaterDepth), ws.matSet);

					arrOut.Add(ws);
				}
			}
		}
	}

	// ---- water volumes (rivers, lakes) -----------------------------------------------------
	if (bNoDraw)
		return;

	AABB areaBox(camPos, camPos);
	areaBox.Expand(Vec3(distCam));

	const int objCount = (int)gEnv->p3DEngine->GetObjectsByTypeInBox(eERType_WaterVolume, areaBox, (IRenderNode**)0);

	if (objCount <= 0)
		return;

	PodArray<IRenderNode*> arrNodes;
	arrNodes.PreAllocate(objCount, objCount);
	gEnv->p3DEngine->GetObjectsByTypeInBox(eERType_WaterVolume, areaBox, arrNodes.GetElements());

	for (int n = 0; n < arrNodes.Count(); n++)
	{
		IRenderNode* pRN = arrNodes[n];

		if (!pRN || pRN->GetRenderNodeType() != eERType_WaterVolume)
			continue;

		// COcean reports the same node type and is handled above; it calls itself "Ocean", every
		// CWaterVolumeRenderNode calls itself "WaterVolume" (GetEntityClassName)
		const char* szClass = pRN->GetEntityClassName();

		if (!szClass || strcmp(szClass, "WaterVolume"))
			continue;

		if (pRN->GetRndFlags() & (ERF_PENDING_DELETE | ERF_HIDDEN))
			continue;

		if (pRN->IsHidden())
			continue;

		CWaterVolumeRenderNode* pWV = static_cast<CWaterVolumeRenderNode*>(pRN);

		// CE itself draws nothing for a zero density volume (Render_JobEntry: "if (m_fogDensity == 0) return")
		if (pWV->GetRTFogDensity() == 0.f)
			continue;

		const uint32 triCount = pWV->GetRTSurfaceTriCount();

		if (!triCount)
			continue;

		const AABB nodeBox = pRN->GetBBox();

		if (nodeBox.IsReset() || !nodeBox.IsNonZero())
			continue;

		if (nodeBox.GetDistance(camPos) > distCam)
			continue;

		// same visibility shape as the dynamic mesh collector: within one ray length of the frustum
		AABB visBox = nodeBox;
		visBox.Expand(Vec3(distRay));

		if (!gEnv->pSystem->GetViewCamera().IsAABBVisible_E(visBox))
			continue;

		// one object may not eat the whole segment on its own
		if ((int)triCount > SVO_RT_DYN_MESH_COUNT / 2)
			continue;

		Vec3 v0[3];
		Vec2 t0[3];

		if (!pWV->GetRTSurfaceTri(0, v0, t0))
			continue;

		// signature: triangle count plus the first vertex, so an edited or moved surface (and a
		// recycled node pointer) rebuilds instead of keeping a stale subtree
		int64 sig = (int64)triCount;
		sig = sig * 1000003ll + (int64)(v0[0].x * 64.f);
		sig = sig * 1000003ll + (int64)(v0[0].y * 64.f);
		sig = sig * 1000003ll + (int64)(v0[0].z * 64.f);

		if (SRTWaterCacheEntry* pEntry = RT_GetWaterCacheEntry(pRN, sig))
		{
			PodArray<SRTBuildTri> arrSrc;
			arrSrc.PreAllocate(triCount, 0);

			for (uint32 t = 0; t < triCount; t++)
			{
				Vec3 v[3];
				Vec2 uv[3];

				if (!pWV->GetRTSurfaceTri(t, v, uv))
					continue;

				SRTBuildTri bt;
				bt.faceNorm = (v[1] - v[0]).Cross(v[2] - v[0]).GetNormalizedSafe(Vec3(0, 0, 1));

				if (bt.faceNorm.z < 0.f)
				{
					// the triangulator does not guarantee a winding; water is single sided seen from
					// above, so every triangle is flipped to face +Z
					std::swap(v[1], v[2]);
					std::swap(uv[1], uv[2]);
					bt.faceNorm = -bt.faceNorm;
				}

				for (int k = 0; k < 3; k++)
				{
					bt.v[k] = v[k];
					bt.t[k] = uv[k];
					bt.n[k] = bt.faceNorm;
				}

				bt.matRecord = 0;
				RT_NormalizeTriUV(bt);
				arrSrc.Add(bt);
			}

			if (arrSrc.Count())
				RT_BuildDynMeshTopology(arrSrc, nullptr, pEntry->mesh);
		}

		SRTDynMesh* pMesh = RT_GetCachedWaterMesh(pRN);

		if (!pMesh || !pMesh->tris.Count())
			continue;

		SRTWaterSurface ws;
		ws.pMesh = pMesh;
		ws.mat = pWV->GetRTWorldTM();
		ws.worldBox.Reset();

		for (int c = 0; c < 8; c++)
		{
			const Vec3 pt((c & 1) ? pMesh->localBox.max.x : pMesh->localBox.min.x,
			              (c & 2) ? pMesh->localBox.max.y : pMesh->localBox.min.y,
			              (c & 4) ? pMesh->localBox.max.z : pMesh->localBox.min.z);
			ws.worldBox.Add(ws.mat.TransformPoint(pt));
		}

		// Render_JobEntry: m_fogColor is multiplied by the time of day HDR multiplier unless the
		// volume is flagged "fog colour affected by sun"
		const float hdrMul = pWV->GetRTFogColorAffectedBySun()
		                     ? 1.f
		                     : ((CTimeOfDay*)p3DEngine->GetTimeOfDay())->GetHDRMultiplier();

		// the volume knows its own depth; fall back to the ocean hint when it is not authored
		float depthHint = pWV->GetRTVolumeDepth();

		if (depthHint <= 0.f)
			depthHint = max(0.f, Cry3DEngineBase::GetCVars()->e_svoTI_RT_WaterDepth);

		RT_FillWaterMatRecord(pRN->GetMaterial(), pWV->GetRTFogColor() * hdrMul, pWV->GetRTFogDensity(), depthHint, ws.matSet);

		arrOut.Add(ws);
	}
}
}   // anonymous namespace

void CVoxelSegment::RTClearDynamicCache()
{
	for (TRTDynCache::iterator it = g_rtDynCache.begin(); it != g_rtDynCache.end(); ++it)
		delete it->second;

	g_rtDynCache.clear();

	for (TRTSkinCache::iterator itS = g_rtSkinCache.begin(); itS != g_rtSkinCache.end(); ++itS)
		delete itS->second;

	g_rtSkinCache.clear();
	RT_ClearWaterCache();
	g_rtDynNodesCached.Reset();
	g_rtDynCacheValid = false;
	g_rtDynCacheFrameId = ~0u;

	g_rtDynPrevTexSlices.Reset();
	g_rtDynPrevRecords = 0;
	g_rtDynPrevMats = 0;
}

void CVoxelSegment::RTUpdateDynamic()
{
	if (!GetCVars()->e_svoTI_RT_Active || !gSvoEnv)
		return;

	// ONCE per rendered frame (report 06c S7 / F4). CSvoManager::Render's sync update spin calls
	// CSvoEnv::Render() ~130 times inside a single frame id and nothing in the scene advances between
	// two of those calls, so the whole collection, the per object refit, the CPU skinning and the
	// record emit were rebuilt and thrown away ~130 times - 13 to 50 % of the main thread, on work
	// whose result is identical every time. m_currPassMainFrameID only advances in OnFrameStart, so
	// this gate is exactly "once per frame the engine really renders".
	static uint s_lastDynFrameId = ~0u;

	if (s_lastDynFrameId == GetCurrPassMainFrameID())
		return;

	s_lastDynFrameId = GetCurrPassMainFrameID();

	const bool bMeshes = (GetCVars()->e_svoTI_RT_Dynamic != 0);
	const bool bWater = (GetCVars()->e_svoTI_RT_Water != 0);
	const bool bEnabled = bMeshes || bWater;

	// switched off and nothing left over from a previous frame: touch nothing, allocate nothing
	if (!bEnabled && !g_rtDynPrevRecords && !g_rtDynPrevMats)
	{
		gSvoEnv->m_rtDynStats.Reset();
		return;
	}

	FUNCTION_PROFILER_3DENGINE;

	const float startTime = GetCurAsyncTimeSec();

	// character skinning tallies (rt decision 07, stage 3B)
	int   skinnedChars = 0;
	int   skinnedVerts = 0;
	int   skinReused = 0;
	float skinMs = 0.f;

	SRTDynFrame f;

	if (bEnabled)
	{
		const Vec3  camPos = gEnv->pSystem->GetViewCamera().GetPosition();
		const float distCam = max(1.f, GetCVars()->e_svoTI_RT_MaxDistCam * max(0.f, GetCVars()->e_svoTI_RT_DynObjDistRatio));
		const float distRay = max(0.f, GetCVars()->e_svoTI_RT_MaxDistRay);
		const int   lodWanted = max(0, GetCVars()->e_svoTI_RT_LodRatio);

		AABB areaBox(camPos, camPos);
		areaBox.Expand(Vec3(distCam));

		// ---- collect --------------------------------------------------------------------------
		// The node list is held across frames (report 06c F9). Each GetObjectsByTypeInBox is a full
		// octree walk into a temporary PodArray, and the count+fill pattern runs it twice per type,
		// so this is six walks and six temporary allocations per frame over a 200 m box.
		//
		// e_svoTI_RT_DynRefreshFrames defaults to 1 (walk every frame) on purpose: above 1 these raw
		// IRenderNode pointers outlive the frame they were collected in, and an object deleted in
		// between would be dereferenced. Making >1 safe needs an invalidation hook in the octree,
		// which is outside this change; the cvar is here so the cost can be traded when a scene is
		// known to be static.
		const int   refreshFrames = max(1, GetCVars()->e_svoTI_RT_DynRefreshFrames);
		const float camMoveLimit = distCam * 0.05f;

		const bool bReQuery = !bMeshes || !g_rtDynCacheValid
		                      || (int)(GetCurrPassMainFrameID() - g_rtDynCacheFrameId) >= refreshFrames
		                      || camPos.GetSquaredDistance(g_rtDynCacheCamPos) > camMoveLimit * camMoveLimit;

		if (bReQuery)
		{
			CRY_PROFILE_SECTION(PROFILE_3DENGINE, "RTUpdateDynamic_Collect");

			g_rtDynNodesCached.Clear();
			g_rtDynCacheValid = bMeshes;
			g_rtDynCacheFrameId = GetCurrPassMainFrameID();
			g_rtDynCacheCamPos = camPos;

			const EERType arrTypes[3] = { eERType_MovableBrush, eERType_Brush, eERType_Character };

			for (int t = 0; bMeshes && t < 3; t++)
			{
				const int objCount = (int)gEnv->p3DEngine->GetObjectsByTypeInBox(arrTypes[t], areaBox, (IRenderNode**)0);
				if (objCount <= 0)
					continue;

				const int first = g_rtDynNodesCached.Count();
				g_rtDynNodesCached.PreAllocate(first + objCount, first + objCount);
				gEnv->p3DEngine->GetObjectsByTypeInBox(arrTypes[t], areaBox, g_rtDynNodesCached.GetElements() + first);
			}
		}

		PodArray<IRenderNode*>& arrNodes = g_rtDynNodesCached;

		PodArray<SRTDynSource> arrSources;

		for (int n = 0; n < arrNodes.Count(); n++)
		{
			IRenderNode* pRN = arrNodes[n];

			if (!RT_IsDynamicForRT(pRN))
				continue;

			const AABB nodeBox = pRN->GetBBox();

			if (nodeBox.IsReset() || !nodeBox.IsNonZero())
				continue;

			// a reflected object does not have to be on screen itself, but it does have to be
			// within one ray length of something that is (same shape as CollectAnalyticalOccluders)
			AABB visBox = nodeBox;
			visBox.Expand(Vec3(distRay));
			if (!gEnv->pSystem->GetViewCamera().IsAABBVisible_E(visBox))
				continue;

			// CE's "only important objects" rule, as used by the static collector
			const float maxViewDist = nodeBox.GetRadius() * GetCVars()->e_ViewDistRatio;
			const float minAllowedViewDist = GetCVars()->e_svoTI_ObjectsMaxViewDistance * GetCVars()->e_svoTI_ObjectsMaxViewDistanceScale;

			if (minAllowedViewDist > 0.f && maxViewDist < minAllowedViewDist)
				continue;

			const float dist = max(0.5f, nodeBox.GetDistance(camPos));
			if (dist > distCam)
				continue;

			const int firstSource = arrSources.Count();
			RT_CollectNodeSources(pRN, lodWanted, arrSources);

			// projected size: the budget keeps the objects that matter most in the picture
			const float importance = nodeBox.GetRadius() / dist;

			for (int s = firstSource; s < arrSources.Count(); s++)
			{
				SRTDynInst inst;
				inst.pMesh = nullptr;
				inst.pMat = arrSources[s].pMat;
				inst.mat = arrSources[s].worldTM;
				inst.importance = importance;
				inst.waterMat = -1;
				inst.triOfs = s;   // temporary: index into arrSources
				inst.boxOfs = 0;
				inst.worldBox = nodeBox;
				inst.qb = Vec4(0, 0, 0, 1);
				inst.pSkinAtt = arrSources[s].pSkinAtt;
				inst.skinLod = arrSources[s].lod;
				inst.pSkin = nullptr;
				f.insts.Add(inst);
			}
		}

		f.overflowed = 0;
		gSvoEnv->m_rtDynStats.objsFound = f.insts.Count();

		// ---- sort by importance, then build / fetch the cached subtrees under the budget -------
		PodArray<int> arrOrder;
		arrOrder.PreAllocate(f.insts.Count(), 0);
		for (int i = 0; i < f.insts.Count(); i++)
			arrOrder.Add(i);

		SRTDynFrame* pF = &f;
		std::stable_sort(arrOrder.GetElements(), arrOrder.GetElements() + arrOrder.Count(), [pF](int a, int b)
		{
			return pF->insts[a].importance > pF->insts[b].importance;
		});

		const int maxTrisCvar = max(0, GetCVars()->e_svoTI_RT_DynMaxTris);

		// The DYN_MESH segment is the hard limit: an object costs (nodes + tris) records, plus two
		// records per object for the top level. Leave room for the top level and one node of slack.
		const int maxRecords = SVO_RT_DYN_MESH_COUNT - 64;

		PodArray<SRTDynInst> arrKept;
		int                  budgetRecords = 0;
		int                  budgetTris = 0;

		std::map<IMaterial*, int> matToRecord;

		for (int o = 0; o < arrOrder.Count(); o++)
		{
			SRTDynInst inst = f.insts[arrOrder[o]];
			const SRTDynSource& src = arrSources[inst.triOfs];

			const bool  bCanSkin = (src.pSkinAtt != nullptr) && (GetCVars()->e_svoTI_RT_SkinMaxTris > 0);
			SRTDynMesh* pMesh = RT_GetDynMesh(src.pRM, src.pMat, src.lod, bCanSkin);

			if (!pMesh || !pMesh->tris.Count())
				continue;

			const int cost = pMesh->nodes.Count() + pMesh->tris.Count();

			if (budgetTris + pMesh->tris.Count() > maxTrisCvar)
				continue;
			if (budgetRecords + cost + 2 * (arrKept.Count() + 1) > maxRecords)
				continue;

			budgetTris += pMesh->tris.Count();
			budgetRecords += cost;

			inst.pMesh = pMesh;
			inst.triOfs = -1;

			// object world bounds from the local box corners, for the top level and for qb
			inst.worldBox.Reset();
			for (int c = 0; c < 8; c++)
			{
				const Vec3 p((c & 1) ? pMesh->localBox.max.x : pMesh->localBox.min.x,
				             (c & 2) ? pMesh->localBox.max.y : pMesh->localBox.min.y,
				             (c & 4) ? pMesh->localBox.max.z : pMesh->localBox.min.z);
				inst.worldBox.Add(src.worldTM.TransformPoint(p));
			}

			inst.qb = RT_DynQuantBounds(inst.worldBox);

			arrKept.Add(inst);
		}

		// ---- water surfaces (rt decision 09 9.4) ------------------------------------------------
		// The ocean ring and the water volumes are ordinary objects of the same two level tree, so
		// they share its record budget, its per object quantisation and its tail zeroing. They are
		// added AFTER the meshes: a water surface is cheap (the ocean is <= 32 triangles) but it is
		// also the least interesting thing to keep when the segment is full.
		if (bWater)
		{
			CRY_PROFILE_SECTION(PROFILE_3DENGINE, "RTUpdateDynamic_Water");

			PodArray<SRTWaterSurface> arrWater;
			RT_CollectWaterSurfaces(camPos, distCam, distRay, arrWater);

			gSvoEnv->m_rtDynStats.objsFound += arrWater.Count();

			for (int w = 0; w < arrWater.Count(); w++)
			{
				const SRTWaterSurface& ws = arrWater[w];
				const int              triCount = ws.pMesh->tris.Count();
				const int              cost = ws.pMesh->nodes.Count() + triCount;

				if (budgetTris + triCount > maxTrisCvar)
					continue;
				if (budgetRecords + cost + 2 * (arrKept.Count() + 1) > maxRecords)
					continue;

				budgetTris += triCount;
				budgetRecords += cost;

				SRTDynInst inst;
				inst.pMesh = ws.pMesh;
				inst.pMat = nullptr;
				inst.mat = ws.mat;
				inst.importance = 0.f;
				inst.waterMat = f.waterMats.Count();
				inst.triOfs = -1;
				inst.boxOfs = 0;
				inst.worldBox = ws.worldBox;
				inst.qb = RT_DynQuantBounds(ws.worldBox);
				inst.pSkinAtt = nullptr;
				inst.skinLod = 0;
				inst.pSkin = nullptr;

				f.waterMats.Add(ws.matSet);
				arrKept.Add(inst);

				f.waterObjs++;
				f.waterTris += triCount;
			}
		}

		// ---- CPU skin the kept characters, nearest first, under e_svoTI_RT_SkinMaxTris ----------
		// arrKept is already in descending projected size order, so the budget naturally spends itself
		// on the characters that matter most in the picture; the rest keep their bind pose, which is
		// exactly what stage 3A did for all of them.
		{
			CRY_PROFILE_SECTION(PROFILE_3DENGINE, "RTUpdateDynamic_Skinning");

			const float skinStart = GetCurAsyncTimeSec();
			const int   skinMaxTris = max(0, GetCVars()->e_svoTI_RT_SkinMaxTris);
			int         skinTris = 0;

			for (int i = 0; i < arrKept.Count(); i++)
			{
				SRTDynInst& inst = arrKept[i];

				if (!inst.pSkinAtt || !inst.pMesh)
					continue;

				// the cached topology has to carry the source vertex ids, or there is nothing to map
				// the skinned positions onto
				if (inst.pMesh->triVertIds.Count() != inst.pMesh->tris.Count() * 3)
					continue;

				if (skinTris + inst.pMesh->tris.Count() > skinMaxTris)
					continue;

				inst.pSkin = RT_GetSkinnedVerts(inst.pSkinAtt, inst.skinLod, skinnedChars, skinnedVerts, skinReused);

				if (inst.pSkin)
					skinTris += inst.pMesh->tris.Count();
			}

			skinMs = (GetCurAsyncTimeSec() - skinStart) * 1000.f;
		}

		f.insts.Clear();
		f.insts.AddList(arrKept);

		// ---- transform the triangles and resolve the materials ---------------------------------
		f.worldTris.PreAllocate(budgetTris, 0);
		f.nodeBoxes.PreAllocate(budgetRecords, 0);

		for (int i = 0; i < f.insts.Count(); i++)
		{
			SRTDynInst&       inst = f.insts[i];
			const SRTDynMesh& mesh = *inst.pMesh;

			inst.triOfs = f.worldTris.Count();
			inst.boxOfs = f.nodeBoxes.Count();

			// A water surface brings its own ready made record set (rt decision 09 9.4), appended
			// once per surface and never deduplicated: the ocean and every volume carry their own fog
			// colour and density, so two of them are almost never the same record.
			// this instance's skinned vertices, when it has any (rt decision 07, stage 3B). The cached
			// topology is untouched: only the positions and normals a leaf's triangles are built from
			// change, and the node boxes are recomputed from them by the refit below.
			const Vec3* pSkinPos = inst.pSkin ? inst.pSkin->pos.GetElements() : nullptr;
			const Vec3* pSkinNorm = inst.pSkin ? inst.pSkin->norm.GetElements() : nullptr;
			const int   skinVertCount = inst.pSkin ? inst.pSkin->pos.Count() : 0;
			const int*  pTriVertIds = (pSkinPos && mesh.triVertIds.Count() == mesh.tris.Count() * 3) ? mesh.triVertIds.GetElements() : nullptr;

			int waterRecord = 0;

			if (inst.waterMat >= 0)
			{
				const SRTMatRecordSet& waterSet = f.waterMats[inst.waterMat];
				const int              localRecord = f.matRecords.Count() / SVO_RT_RECORD_TEXELS;

				if (localRecord + waterSet.RecordCount() <= SVO_RT_DYN_MATS_COUNT)
				{
					waterRecord = SVO_RT_SEG_DYN_MATS + localRecord;
					RT_AppendMatRecordSet(waterSet, localRecord, SVO_RT_SEG_DYN_MATS, f.matRecords, nullptr);
				}
			}

			// sub material -> DYN_MATS record, deduplicated per frame by the leaf material pointer
			for (int t = 0; t < mesh.tris.Count(); t++)
			{
				SRTBuildTri bt = mesh.tris[t];

				int matRecord = waterRecord;

				if (inst.waterMat < 0)
				{
					IMaterial* pLeafMat = inst.pMat ? inst.pMat->GetSafeSubMtl(bt.matRecord) : nullptr;

					std::map<IMaterial*, int>::const_iterator itM = matToRecord.find(pLeafMat);

					if (itM != matToRecord.end())
					{
						matRecord = itM->second;
					}
					else
					{
						SRTMatRecordSet matSet;
						RTFillMaterialRecord(pLeafMat, false, f.texSlices, matSet);

						const int localRecord = f.matRecords.Count() / SVO_RT_RECORD_TEXELS;

						// the whole set (base + extras + blend) has to fit, or the material falls back to
						// record 0 - never a half written set the consumer would walk into
						if (localRecord + matSet.RecordCount() <= SVO_RT_DYN_MATS_COUNT)
						{
							matRecord = SVO_RT_SEG_DYN_MATS + localRecord;
							matToRecord[pLeafMat] = matRecord;

							// dynamic records are absolute from the start, so no relocation list
							RT_AppendMatRecordSet(matSet, localRecord, SVO_RT_SEG_DYN_MATS, f.matRecords, nullptr);
						}
					}
				}

				// swap the bind pose vertices for this frame's skinned ones, before the instance matrix
				if (pTriVertIds)
				{
					const int i0 = pTriVertIds[t * 3 + 0];
					const int i1 = pTriVertIds[t * 3 + 1];
					const int i2 = pTriVertIds[t * 3 + 2];

					if (i0 >= 0 && i1 >= 0 && i2 >= 0 && i0 < skinVertCount && i1 < skinVertCount && i2 < skinVertCount)
					{
						bt.v[0] = pSkinPos[i0];
						bt.v[1] = pSkinPos[i1];
						bt.v[2] = pSkinPos[i2];
						bt.n[0] = pSkinNorm[i0];
						bt.n[1] = pSkinNorm[i1];
						bt.n[2] = pSkinNorm[i2];
					}
				}

				for (int v = 0; v < 3; v++)
				{
					bt.v[v] = inst.mat.TransformPoint(bt.v[v]);
					bt.n[v] = inst.mat.TransformVector(bt.n[v]);
				}

				bt.faceNorm = (bt.v[1] - bt.v[0]).Cross(bt.v[2] - bt.v[0]).GetNormalizedSafe(Vec3(0, 0, 1));

				for (int v = 0; v < 3; v++)
				{
					bt.n[v] = bt.n[v].GetNormalizedSafe(bt.faceNorm);
					if (bt.n[v].Dot(bt.faceNorm) < 0.f)
						bt.n[v] = -bt.n[v];
				}

				bt.matRecord = matRecord;

				f.worldTris.Add(bt);
			}

			RT_RefitDynInst(f, inst);

			// A deformed mesh leaves its bind pose box: the world box and the quantisation bounds were
			// derived from the cached LOCAL box in the budget loop, which an animated pose can stick
			// out of - and a triangle outside qb quantises wrong. The refit has just produced the exact
			// root box, so take it. Rigid instances keep the corner-transformed box they always had.
			if (inst.pSkin)
			{
				inst.worldBox = f.nodeBoxes[inst.boxOfs];
				inst.qb = RT_DynQuantBounds(inst.worldBox);
			}
		}
	}

	// ---- emit one flattened tree starting at record 0 ------------------------------------------
	int usedRecords = 0;

	if (f.insts.Count())
	{
		int upperBound = 2 * f.insts.Count() + 8;
		for (int i = 0; i < f.insts.Count(); i++)
			upperBound += f.insts[i].pMesh->nodes.Count() + f.insts[i].pMesh->tris.Count();

		upperBound = min(upperBound, (int)SVO_RT_DYN_MESH_COUNT);

		f.records.PreAllocate(upperBound * SVO_RT_RECORD_TEXELS, upperBound * SVO_RT_RECORD_TEXELS);

		AABB rootBox;
		rootBox.Reset();
		for (int i = 0; i < f.insts.Count(); i++)
			rootBox.Add(f.insts[i].worldBox);

		PodArray<int> arrIds;
		arrIds.PreAllocate(f.insts.Count(), 0);
		for (int i = 0; i < f.insts.Count(); i++)
			arrIds.Add(i);

		usedRecords = RT_EmitDynTop(f, arrIds.GetElements(), arrIds.Count(), 0, SVO_RT_SEG_DYN_MESH, RT_DynQuantBounds(rootBox));
	}

	// ---- upload: whole region rewritten, stale tail zeroed --------------------------------------
	// records, not materials: one material can own up to three of them (rt decision 10)
	const int matCount = f.matRecords.Count() / SVO_RT_RECORD_TEXELS;

	{
		AUTO_MODIFYLOCK(gSvoEnv->m_arrRTPoolTris.m_Lock);

		gSvoEnv->RTCachePoolDims();

		const int poolTexels = gSvoEnv->GetRTPoolXY() * gSvoEnv->GetRTPoolXY() * gSvoEnv->GetRTPoolZ();

		if (poolTexels > 0 && gSvoEnv->GetRTPoolRecords() > SVO_RT_SEG_PARTICLES)
		{
			gSvoEnv->m_arrRTPoolTris.CheckAllocated(poolTexels);

			if (gSvoEnv->m_arrRTDirtyTris.Count() != gSvoEnv->GetRTPoolZ())
				gSvoEnv->m_arrRTDirtyTris.PreAllocate(gSvoEnv->GetRTPoolZ(), gSvoEnv->GetRTPoolZ());

			Vec4* pPool = gSvoEnv->m_arrRTPoolTris.GetElements();

			if (usedRecords)
				memcpy(pPool + (size_t)SVO_RT_SEG_DYN_MESH * SVO_RT_RECORD_TEXELS, f.records.GetElements(),
				       (size_t)usedRecords * SVO_RT_RECORD_TEXELS * sizeof(Vec4));

			// A stale subtree must never be walkable: zero what last frame used and this one does not.
			if (g_rtDynPrevRecords > usedRecords)
				memset(pPool + (size_t)usedRecords * SVO_RT_RECORD_TEXELS, 0,
				       (size_t)(g_rtDynPrevRecords - usedRecords) * SVO_RT_RECORD_TEXELS * sizeof(Vec4));

			if (matCount)
				memcpy(pPool + (size_t)SVO_RT_SEG_DYN_MATS * SVO_RT_RECORD_TEXELS, f.matRecords.GetElements(),
				       (size_t)matCount * SVO_RT_RECORD_TEXELS * sizeof(Vec4));

			if (g_rtDynPrevMats > matCount)
				memset(pPool + (size_t)(SVO_RT_SEG_DYN_MATS + matCount) * SVO_RT_RECORD_TEXELS, 0,
				       (size_t)(g_rtDynPrevMats - matCount) * SVO_RT_RECORD_TEXELS * sizeof(Vec4));

			const int dirtyMesh = max(usedRecords, g_rtDynPrevRecords);
			const int dirtyMats = max(matCount, g_rtDynPrevMats);

			if (dirtyMesh)
				gSvoEnv->RTMarkTrisDirty(SVO_RT_SEG_DYN_MESH, dirtyMesh);
			if (dirtyMats)
				gSvoEnv->RTMarkTrisDirty(SVO_RT_SEG_DYN_MATS, dirtyMats);

			g_rtDynPrevRecords = usedRecords;
			g_rtDynPrevMats = matCount;
		}
	}

	// Atlas references: the new ones are taken before the old ones are queued for release, so a
	// slice that is used in both frames never drops to refcount 0 and never moves.
	for (int i = 0; i < g_rtDynPrevTexSlices.Count(); i++)
		gSvoEnv->RTQueueFreeTexSlice(g_rtDynPrevTexSlices[i]);

	g_rtDynPrevTexSlices.Clear();
	g_rtDynPrevTexSlices.AddList(f.texSlices);

	RT_EvictDynCache();
	RT_EvictSkinCache();
	RT_EvictWaterCache();

	gSvoEnv->m_rtDynStats.objs = f.insts.Count();
	gSvoEnv->m_rtDynStats.tris = f.worldTris.Count();
	gSvoEnv->m_rtDynStats.records = usedRecords;
	gSvoEnv->m_rtDynStats.mats = matCount;
	gSvoEnv->m_rtDynStats.overflowed = f.overflowed;
	gSvoEnv->m_rtDynStats.cached = (int)g_rtDynCache.size();
	gSvoEnv->m_rtDynStats.waterObjs = f.waterObjs;
	gSvoEnv->m_rtDynStats.waterTris = f.waterTris;
	gSvoEnv->m_rtDynStats.skinnedChars = skinnedChars;
	gSvoEnv->m_rtDynStats.skinnedVerts = skinnedVerts;
	gSvoEnv->m_rtDynStats.skinReused = skinReused;
	gSvoEnv->m_rtDynStats.skinMs = skinMs;
	gSvoEnv->m_rtDynStats.ms = (GetCurAsyncTimeSec() - startTime) * 1000.f;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh ray tracing - self test (e_svoTI_RT_SelfTest)
//
// Builds a synthetic soup, writes real records with the real packers, then traces rays through
// THE WRITTEN RECORDS with a CPU traversal that mirrors rt decision 02 exactly, and compares the
// result with brute force. This is the only validation available without an engine window.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
//! Deterministic LCG, so a failing run can always be reproduced.
struct SRTTestRand
{
	uint32 s;
	SRTTestRand(uint32 seed) : s(seed) {}
	float  Next()
	{
		s = s * 1664525u + 1013904223u;
		return (float)((s >> 8) & 0xffffff) / (float)0x1000000;
	}
	float NextRange(float a, float b) { return a + (b - a) * Next(); }
};

//! Mirror of CommonSVO_RT.cfi ExtractUint2 (base 4096, high field first).
void RT_ExtractUint2(float f, int& high, int& low)
{
	uint32 n = (uint32)f;
	low = (int)(n & 4095); n /= 4096;
	high = (int)(n & 4095);
}

//! Mirror of CommonSVO_RT.cfi ExtractTC16 (asuint split, two 16 bit fields over [0, 16)).
void RT_ExtractTC16(float f, float& u, float& v)
{
	const uint32 n = RT_AsUint(f);
	u = (float)(n & 0xffff) / 65535.f * 16.f;
	v = (float)(n >> 16) / 65535.f * 16.f;
}

//! Moller-Trumbore with a relative determinant epsilon (rt decision 03, 3.6).
bool RT_TestTriangle(const Vec3& org, const Vec3& dir, const Vec3& v0, const Vec3& v1, const Vec3& v2, float& tOut)
{
	const Vec3  e1 = v1 - v0;
	const Vec3  e2 = v2 - v0;
	const Vec3  pv = dir.Cross(e2);
	const float det = e1.Dot(pv);

	if (fabs(det) < 1e-7f * e1.GetLength() * e2.GetLength())
		return false;

	const float invDet = 1.f / det;
	const Vec3  tv = org - v0;
	const float u = tv.Dot(pv) * invDet;
	if (u < 0.f || u > 1.f)
		return false;

	const Vec3  qv = tv.Cross(e1);
	const float v = dir.Dot(qv) * invDet;
	if (v < 0.f || u + v > 1.f)
		return false;

	const float t = e2.Dot(qv) * invDet;
	if (t <= 0.f)
		return false;

	tOut = t;
	return true;
}

bool RT_TestAABB(const Vec3& org, const Vec3& invDir, const AABB& box, float tMax, float& tEnter)
{
	float t0 = 0.f, t1 = tMax;

	for (int a = 0; a < 3; a++)
	{
		float n = (box.min[a] - org[a]) * invDir[a];
		float f = (box.max[a] - org[a]) * invDir[a];
		if (n > f)
			std::swap(n, f);
		t0 = max(t0, n);
		t1 = min(t1, f);
		if (t0 > t1)
			return false;
	}

	tEnter = t0;
	return true;
}
//! Reference traversal of written records, mirroring rt decision 02 and the consumer exactly:
//! 20 slot stack, near child first by splitAxis and ray sign, relative determinant Moller-Trumbore.
//! pRecords[0] is record recordBase. Used by both the static and the dynamic self test.
void RT_RefTrace(const Vec4* pRecords, int recordBase, int rootRecord, const Vec3& org, const Vec3& dir,
                 float tMax, float& bestT, int& bestRec, bool& bIncomplete)
{
	const int kMaxStack = 20;

	const Vec3 invDir(1.f / (fabs(dir.x) > 1e-12f ? dir.x : 1e-12f),
	                  1.f / (fabs(dir.y) > 1e-12f ? dir.y : 1e-12f),
	                  1.f / (fabs(dir.z) > 1e-12f ? dir.z : 1e-12f));

	bestT = tMax;
	bestRec = -1;
	bIncomplete = false;

	int stack[kMaxStack];
	int stackPos = 0;
	stack[stackPos++] = rootRecord;

	while (stackPos > 0)
	{
		const int   rec = stack[--stackPos];
		const Vec4* pRec = pRecords + (size_t)(rec - recordBase) * SVO_RT_RECORD_TEXELS;

		AABB box(Vec3(pRec[0].x, pRec[0].y, pRec[0].z), Vec3(pRec[1].x, pRec[1].y, pRec[1].z));

		float tEnter = 0.f;
		if (!RT_TestAABB(org, invDir, box, bestT, tEnter))
			continue;

		const int child0 = (int)pRec[3].x;

		if (child0 > 0)
		{
			const int child1 = (int)pRec[3].y;
			const int axis = (int)pRec[3].w;

			if (stackPos + 2 > kMaxStack)
			{
				bIncomplete = true;
				continue;
			}

			// child0 holds the lower half along the split axis: push the far child first
			if (dir[axis] >= 0.f)
			{
				stack[stackPos++] = child1;
				stack[stackPos++] = child0;
			}
			else
			{
				stack[stackPos++] = child0;
				stack[stackPos++] = child1;
			}
		}
		else
		{
			const int  triCount = (int)pRec[0].w;
			const Vec4 nodeQb = pRec[2];

			for (int t = 0; t < triCount; t++)
			{
				const Vec4* pTri = pRec + (size_t)(1 + t) * SVO_RT_RECORD_TEXELS;

				Vec3 p[3];
				for (int v = 0; v < 3; v++)
				{
					const uint32 packed = RT_AsUint(pTri[v].x);
					p[v] = Vec3(RT_UnquantPos(packed & 0xffff, nodeQb.x, nodeQb.w),
					            RT_UnquantPos((packed >> 16) & 0xffff, nodeQb.y, nodeQb.w),
					            pTri[v].w);
				}

				float tHit;
				if (RT_TestTriangle(org, dir, p[0], p[1], p[2], tHit) && tHit < bestT)
				{
					bestT = tHit;
					bestRec = rec + 1 + t;
				}
			}
		}
	}
}
}

void CVoxelSegment::RunRTSelfTest()
{
	PrintMessage("RT self test: building synthetic soup...");

	// ---- synthetic soup: two quads plus one thin wall -------------------------------------------
	PodArray<SRTBuildTri> arrSoup;

	struct SAddQuad
	{
		static void Add(PodArray<SRTBuildTri>& out, const Vec3& o, const Vec3& du, const Vec3& dv, int subDiv)
		{
			const Vec3 n = du.Cross(dv).GetNormalizedSafe(Vec3(0, 0, 1));
			for (int i = 0; i < subDiv; i++)
			{
				for (int j = 0; j < subDiv; j++)
				{
					const float u0 = (float)i / subDiv, u1 = (float)(i + 1) / subDiv;
					const float v0 = (float)j / subDiv, v1 = (float)(j + 1) / subDiv;

					const Vec3 p00 = o + du * u0 + dv * v0;
					const Vec3 p10 = o + du * u1 + dv * v0;
					const Vec3 p11 = o + du * u1 + dv * v1;
					const Vec3 p01 = o + du * u0 + dv * v1;

					SRTBuildTri a;
					a.v[0] = p00; a.v[1] = p10; a.v[2] = p11;
					a.t[0] = Vec2(u0, v0); a.t[1] = Vec2(u1, v0); a.t[2] = Vec2(u1, v1);
					a.n[0] = a.n[1] = a.n[2] = a.faceNorm = n;
					a.matRecord = 0;
					out.Add(a);

					SRTBuildTri b;
					b.v[0] = p00; b.v[1] = p11; b.v[2] = p01;
					b.t[0] = Vec2(u0, v0); b.t[1] = Vec2(u1, v1); b.t[2] = Vec2(u0, v1);
					b.n[0] = b.n[1] = b.n[2] = b.faceNorm = n;
					b.matRecord = 0;
					out.Add(b);
				}
			}
		}
	};

	// floor quad, 8x8 cells
	SAddQuad::Add(arrSoup, Vec3(0, 0, 0), Vec3(8, 0, 0), Vec3(0, 8, 0), 8);
	// ceiling quad, 4x4 cells, offset and smaller
	SAddQuad::Add(arrSoup, Vec3(1, 1, 5), Vec3(5, 0, 0), Vec3(0, 5, 0), 4);
	// thin wall: a closed box 2 cm thick, spanning the floor
	{
		const float x0 = 3.5f, x1 = 3.52f, y0 = 0.f, y1 = 8.f, z0 = 0.f, z1 = 4.f;
		const Vec3  c[8] =
		{
			Vec3(x0, y0, z0), Vec3(x1, y0, z0), Vec3(x1, y1, z0), Vec3(x0, y1, z0),
			Vec3(x0, y0, z1), Vec3(x1, y0, z1), Vec3(x1, y1, z1), Vec3(x0, y1, z1)
		};
		const int quads[6][4] = { { 3, 2, 1, 0 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };

		for (int f = 0; f < 6; f++)
		{
			const Vec3 p0 = c[quads[f][0]], p1 = c[quads[f][1]], p2 = c[quads[f][2]], p3 = c[quads[f][3]];
			const Vec3 n = (p1 - p0).Cross(p2 - p0).GetNormalizedSafe(Vec3(0, 0, 1));

			SRTBuildTri a;
			a.v[0] = p0; a.v[1] = p1; a.v[2] = p2;
			a.t[0] = Vec2(0, 0); a.t[1] = Vec2(1, 0); a.t[2] = Vec2(1, 1);
			a.n[0] = a.n[1] = a.n[2] = a.faceNorm = n;
			a.matRecord = 0;
			arrSoup.Add(a);

			SRTBuildTri b;
			b.v[0] = p0; b.v[1] = p2; b.v[2] = p3;
			b.t[0] = Vec2(0, 0); b.t[1] = Vec2(1, 1); b.t[2] = Vec2(0, 1);
			b.n[0] = b.n[1] = b.n[2] = b.faceNorm = n;
			b.matRecord = 0;
			arrSoup.Add(b);
		}
	}

	// ---- build real records ---------------------------------------------------------------------
	AABB soupBox;
	soupBox.Reset();
	for (int i = 0; i < arrSoup.Count(); i++)
		for (int v = 0; v < 3; v++)
			soupBox.Add(arrSoup[i].v[v]);

	const float qSize = max(soupBox.max.x - soupBox.min.x, soupBox.max.y - soupBox.min.y) * (1.f + 1.f / 1024.f);
	const Vec4  qb((soupBox.min.x + soupBox.max.x) * 0.5f, (soupBox.min.y + soupBox.max.y) * 0.5f, 0.f, qSize);

	const int      recordBase = SVO_RT_SEG_STATIC;
	PodArray<Vec4> arrRec;
	SRTBuildStats  stats;
	stats.Reset();

	const float buildStart = GetCurAsyncTimeSec();
	if (!BuildStaticBVHRecords(arrSoup, qb, recordBase, arrRec, nullptr, stats))
	{
		PrintMessage("RT self test: FAILED - builder returned nothing");
		return;
	}
	const float buildMs = (GetCurAsyncTimeSec() - buildStart) * 1000.f;

	// ---- decode the written records back, exactly as the consumer will ---------------------------
	PodArray<Vec3> arrDecodedV;   // 3 per triangle, indexed by leaf triangle record order
	PodArray<int>  arrDecodedRec; // record index of each decoded triangle

	float maxPosError = 0.f;

	{
		// Records are only meaningful through the tree - a triangle record read as a node would
		// decode its packed Z as a triangle count - so the decode walks the tree, like the consumer.
		PodArray<int> stack;
		stack.Add(recordBase);

		while (stack.Count())
		{
			const int rec = stack.Last();
			stack.DeleteLast();

			const Vec4* pRec = arrRec.GetElements() + (rec - recordBase) * SVO_RT_RECORD_TEXELS;
			const int   child0 = (int)pRec[3].x;

			if (child0 > 0)
			{
				stack.Add(child0);
				stack.Add((int)pRec[3].y);
				continue;
			}

			const int  triCount = (int)pRec[0].w;
			const Vec4 nodeQb = pRec[2];

			for (int t = 0; t < triCount; t++)
			{
				const Vec4* pTri = pRec + (1 + t) * SVO_RT_RECORD_TEXELS;
				for (int v = 0; v < 3; v++)
				{
					const uint32 packed = RT_AsUint(pTri[v].x);
					const Vec3   p(RT_UnquantPos(packed & 0xffff, nodeQb.x, nodeQb.w),
					               RT_UnquantPos((packed >> 16) & 0xffff, nodeQb.y, nodeQb.w),
					               pTri[v].w);
					arrDecodedV.Add(p);
				}
				arrDecodedRec.Add(rec + 1 + t);
			}
		}
	}

	const int decodedTris = arrDecodedRec.Count();

	if (decodedTris != arrSoup.Count())
	{
		PrintMessage("RT self test: FAILED - %d triangles written but %d decoded back", arrSoup.Count(), decodedTris);
		return;
	}

	// quantisation error: match each decoded triangle against the soup by nearest vertex triple
	for (int i = 0; i < decodedTris; i++)
	{
		float best = 1e30f;
		for (int j = 0; j < arrSoup.Count(); j++)
		{
			float d = 0.f;
			for (int v = 0; v < 3; v++)
				d = max(d, (arrDecodedV[i * 3 + v] - arrSoup[j].v[v]).GetLength());
			best = min(best, d);
		}
		maxPosError = max(maxPosError, best);
	}

	// ---- trace 1000 rays through the written records --------------------------------------------
	SRTTestRand rnd(0x5eed1b);

	int mismatchVsDecoded = 0;
	int mismatchVsSoup = 0;
	int overflowCount = 0;
	int hitCount = 0;
	float maxDistError = 0.f;

	const Vec3 c = soupBox.GetCenter();
	const float radius = soupBox.GetRadius() * 1.5f;

	for (int rayId = 0; rayId < 1000; rayId++)
	{
		Vec3 org(c.x + rnd.NextRange(-radius, radius), c.y + rnd.NextRange(-radius, radius), c.z + rnd.NextRange(-radius, radius));

		Vec3 dir;
		if (rayId & 1)
		{
			// half of the rays are aimed into the soup box so that most of them actually hit
			const Vec3 target(rnd.NextRange(soupBox.min.x, soupBox.max.x),
			                  rnd.NextRange(soupBox.min.y, soupBox.max.y),
			                  rnd.NextRange(soupBox.min.z, soupBox.max.z));
			dir = target - org;
		}
		else
		{
			dir = Vec3(rnd.NextRange(-1.f, 1.f), rnd.NextRange(-1.f, 1.f), rnd.NextRange(-1.f, 1.f));
		}

		if (dir.GetLengthSquared() < 1e-6f)
			dir = Vec3(0, 0, 1);
		dir.Normalize();

		const float tMax = radius * 4.f;

		// (a) traversal of the written records
		float bestT = tMax;
		int   bestRec = -1;
		bool  bIncomplete = false;

		RT_RefTrace(arrRec.GetElements(), recordBase, recordBase, org, dir, tMax, bestT, bestRec, bIncomplete);

		if (bIncomplete)
			overflowCount++;

		// (b) brute force over the decoded triangles - must agree exactly
		float refT = tMax;
		int   refRec = -1;
		for (int i = 0; i < decodedTris; i++)
		{
			float tHit;
			if (RT_TestTriangle(org, dir, arrDecodedV[i * 3 + 0], arrDecodedV[i * 3 + 1], arrDecodedV[i * 3 + 2], tHit) && tHit < refT)
			{
				refT = tHit;
				refRec = arrDecodedRec[i];
			}
		}

		if (bestRec != refRec || fabs(bestT - refT) > 1e-4f)
			mismatchVsDecoded++;

		if (refRec >= 0)
			hitCount++;

		// (c) brute force over the original soup - difference is the quantisation budget
		float soupT = tMax;
		for (int i = 0; i < arrSoup.Count(); i++)
		{
			float tHit;
			if (RT_TestTriangle(org, dir, arrSoup[i].v[0], arrSoup[i].v[1], arrSoup[i].v[2], tHit) && tHit < soupT)
				soupT = tHit;
		}

		const bool bHitA = (bestT < tMax);
		const bool bHitB = (soupT < tMax);

		if (bHitA != bHitB)
			mismatchVsSoup++;
		else if (bHitA)
			maxDistError = max(maxDistError, fabs(bestT - soupT));
	}

	PrintMessage("RT self test: soup %d tris, %d records (%d nodes, %d leaves), depth %d, maxLeaf %d, build %.2f ms",
	             arrSoup.Count(), stats.records, stats.nodes, stats.leaves, stats.maxDepth, stats.maxLeafTris, buildMs);
	PrintMessage("RT self test: 1000 rays, %d hits, traversal vs decoded soup mismatches = %d (must be 0), stack overflows = %d (must be 0)",
	             hitCount, mismatchVsDecoded, overflowCount);
	PrintMessage("RT self test: quantisation - max vertex error %.6f m (qb.w = %.2f m, budget %.6f m), max hit distance error %.6f m, hit/miss disagreements vs original soup = %d",
	             maxPosError, qb.w, qb.w / 65536.f, maxDistError, mismatchVsSoup);

	// ---- material record round trip (stage 2B: specular and emissive slots) ---------------------
	// FillRTMaterialRecord needs a live SShaderItem, so the four texels are assembled here with the
	// same packers and decoded with the same helpers the consumer uses (RT_ProcessBestHit).
	int matMismatches = 0;
	{
		const int kSlots[6][4] =
		{
			//  albedo, normal, specular, emissive   (0 = the material has no such map)
			{ 0, 0, 0, 0 },
			{ 1, 0, 0, 0 },
			{ 1, 2, 3, 4 },
			{ 7, 0, 9, 0 },
			{ 5, 6, 0, 8 },
			{ 256, 4095, 4095, 4095 },
		};

		for (int i = 0; i < 6; i++)
		{
			const int   alb = kSlots[i][0], nor = kSlots[i][1], spc = kSlots[i][2], emi = kSlots[i][3];
			const float specRefl = 0.04f + 0.1f * i;
			const float emissive = 12.5f * i;           // raw kcd/m2, as the record stores it
			const float scaleX = (float)(i + 1) / 8.f;
			const float scaleY = (float)(i + 2) / 8.f;

			Vec4 rec[SVO_RT_RECORD_TEXELS];
			rec[0] = Vec4((float)alb, RT_PackUint2(nor, spc), RT_PackUint2(emi, 0), RT_PackTC16(scaleX, scaleY));
			rec[1] = Vec4(1.f, specRefl, 0.5f, emissive);
			rec[2] = Vec4(0.2f, 0.4f, 0.6f, 0.f);
			rec[3] = Vec4(0.f, 0.f, 0.f, RT_PackTC16(1.f, 0.33f));

			int decNor = 0, decSpc = 0, decEmi = 0, decPad = 0;
			RT_ExtractUint2(rec[0].y, decNor, decSpc);
			RT_ExtractUint2(rec[0].z, decEmi, decPad);

			float decScaleX = 0.f, decScaleY = 0.f;
			RT_ExtractTC16(rec[0].w, decScaleX, decScaleY);

			// the consumer subtracts one and treats a negative result as "no texture"
			const int slotNor = decNor - 1, slotSpc = decSpc - 1, slotEmi = decEmi - 1;

			const bool bOk =
			  ((int)rec[0].x == alb) &&
			  (slotNor == nor - 1) && (slotSpc == spc - 1) && (slotEmi == emi - 1) &&
			  (decPad == 0) &&
			  (fabs(decScaleX - scaleX) < 1e-3f) && (fabs(decScaleY - scaleY) < 1e-3f) &&
			  (fabs(rec[1].y - specRefl) < 1e-6f) && (fabs(rec[1].w - emissive) < 1e-6f);

			if (!bOk)
			{
				matMismatches++;
				PrintMessage("RT self test: material record %d mismatch - alb %d, nor %d/%d, spc %d/%d, emi %d/%d, scale %.4f,%.4f vs %.4f,%.4f",
				             i, (int)rec[0].x, slotNor + 1, nor, slotSpc + 1, spc, slotEmi + 1, emi, decScaleX, decScaleY, scaleX, scaleY);
			}
		}
	}

	PrintMessage("RT self test: material records - 6 cases, specular/emissive slot round trip mismatches = %d (must be 0)", matMismatches);

	// ---- material extras round trip (stage 5A, rt decision 10) ---------------------------------
	// A synthetic material with a detail map, a blend layer and a transmittance colour is assembled
	// with the same packers the producer uses, appended through RT_AppendMatRecordSet into a
	// simulated chunk, and decoded back with the CPU mirrors of the consumer's helpers: the extras
	// reference in matInfo0.z's low field, the blend base pointer after relocation, the detail
	// tiling and scales, the transmittance and the per type parameters.
	int extrasMismatches = 0;
	{
		const int   kChunkStart = 49152 + 64;       // any legal STATIC chunk start
		const float kDetailTilU = 4.f, kDetailTilV = 2.5f;
		const float kDetailDiff = 0.35f, kDetailBump = 1.75f;
		const float kBlendFactor = 8.f, kBlendFalloff = 32.f;
		const float kMaskTiling = 3.f, kLayer2Tiling = 7.25f;
		const float kTransR = 1.f, kTransG = 0.8f, kTransB = 0.6f, kTransMul = 1.5f;
		const float kType0 = 1.2f, kType1 = 0.4f, kOpacity = 0.75f;
		const int   kDetailSlot = 11, kMaskSlot = 12, kBlendAlb = 13, kBlendNor = 14;

		SRTMatRecordSet set;
		ZeroStruct(set);

		set.bExtras = true;
		set.bBlend = true;

		set.base[0] = Vec4(1.f, RT_PackUint2(2, 3), RT_PackUint2(4, 1), RT_PackTC16(1.f, 1.f));
		set.base[1] = Vec4(1.f, 0.04f, 0.5f, 0.f);
		set.base[2] = Vec4(1.f, 1.f, 1.f, 0.f);
		set.base[3] = Vec4(0.f, 0.f, SVO_RT_TAG_VEG_LEAVES, RT_PackTC16(kOpacity, 0.33f));

		set.extras[0] = Vec4(RT_PackUint2(kDetailSlot, 0), kDetailTilU, kDetailTilV, RT_PackTC16(kDetailDiff, kDetailBump));
		set.extras[1] = Vec4(0.f, (float)kMaskSlot, kBlendFactor, kBlendFalloff);
		set.extras[2] = Vec4(kTransR, kTransG, kTransB, kTransMul);
		set.extras[3] = Vec4(kType0, kType1, kOpacity, RT_PackTC16(kMaskTiling, kLayer2Tiling));

		set.blend[0] = Vec4((float)kBlendAlb, RT_PackUint2(kBlendNor, 0), RT_PackUint2(0, 0), RT_PackTC16(1.f, 1.f));
		set.blend[1] = Vec4(1.f, 0.04f, 1.f, 0.f);
		set.blend[2] = Vec4(1.f, 1.f, 1.f, 0.f);
		set.blend[3] = Vec4(0.f, 0.f, SVO_RT_TAG_VEG_LEAVES, RT_PackTC16(kOpacity, 0.33f));

		// a material with no extras at all must still occupy exactly one record and reference none
		SRTMatRecordSet plain;
		ZeroStruct(plain);
		plain.base[0] = Vec4(5.f, RT_PackUint2(6, 7), RT_PackUint2(8, 0), RT_PackTC16(1.f, 1.f));
		plain.base[3] = Vec4(0.f, 0.f, SVO_RT_TAG_ILLUM, RT_PackTC16(1.f, 0.f));

		PodArray<Vec4> arrRec;
		PodArray<int>  arrRel;

		int matRecords = 0;
		const int recPlain = matRecords;
		RT_AppendMatRecordSet(plain, recPlain, 0, arrRec, &arrRel);
		matRecords += plain.RecordCount();

		const int recFull = matRecords;
		RT_AppendMatRecordSet(set, recFull, 0, arrRec, &arrRel);
		matRecords += set.RecordCount();

		if (plain.RecordCount() != 1 || set.RecordCount() != 3 || matRecords != 4 || arrRec.Count() != 4 * SVO_RT_RECORD_TEXELS)
			extrasMismatches++;

		// the static path makes every stored record index absolute exactly like this
		{
			float* pFloats = (float*)arrRec.GetElements();
			for (int i = 0; i < arrRel.Count(); i++)
				pFloats[arrRel[i]] += (float)kChunkStart;
		}

		// ---- decode as the consumer would --------------------------------------------------------
		const Vec4* pBase = arrRec.GetElements() + recFull * SVO_RT_RECORD_TEXELS;

		int decEmi = 0, decExtras = 0;
		RT_ExtractUint2(pBase[0].z, decEmi, decExtras);

		if (decEmi != 4 || decExtras != 1)
			extrasMismatches++;

		// extras sit at (this material's record) + the stored offset
		const int   matRecordAbs = kChunkStart + recFull;
		const Vec4* pExtras = arrRec.GetElements() + (matRecordAbs + decExtras - kChunkStart) * SVO_RT_RECORD_TEXELS;

		int decDetail = 0, decDetailPad = 0;
		RT_ExtractUint2(pExtras[0].x, decDetail, decDetailPad);

		float decDetDiff = 0.f, decDetBump = 0.f;
		RT_ExtractTC16(pExtras[0].w, decDetDiff, decDetBump);

		float decMaskTil = 0.f, decLayerTil = 0.f;
		RT_ExtractTC16(pExtras[3].w, decMaskTil, decLayerTil);

		const int decBlendRecord = (int)pExtras[1].x;

		const bool bOk =
		  (decDetail == kDetailSlot) && (decDetailPad == 0) &&
		  (fabs(pExtras[0].y - kDetailTilU) < 1e-6f) && (fabs(pExtras[0].z - kDetailTilV) < 1e-6f) &&
		  (fabs(decDetDiff - kDetailDiff) < 1e-3f) && (fabs(decDetBump - kDetailBump) < 1e-3f) &&
		  (decBlendRecord == kChunkStart + recFull + 2) &&
		  ((int)pExtras[1].y == kMaskSlot) &&
		  (fabs(pExtras[1].z - kBlendFactor) < 1e-6f) && (fabs(pExtras[1].w - kBlendFalloff) < 1e-6f) &&
		  (fabs(pExtras[2].x - kTransR) < 1e-6f) && (fabs(pExtras[2].y - kTransG) < 1e-6f) &&
		  (fabs(pExtras[2].z - kTransB) < 1e-6f) && (fabs(pExtras[2].w - kTransMul) < 1e-6f) &&
		  (fabs(pExtras[3].x - kType0) < 1e-6f) && (fabs(pExtras[3].y - kType1) < 1e-6f) &&
		  (fabs(pExtras[3].z - kOpacity) < 1e-6f) &&
		  (fabs(decMaskTil - kMaskTiling) < 1e-3f) && (fabs(decLayerTil - kLayer2Tiling) < 1e-3f) &&
		  (fabs(pBase[3].z - SVO_RT_TAG_VEG_LEAVES) < 1e-6f);

		if (!bOk)
		{
			extrasMismatches++;
			PrintMessage("RT self test: extras mismatch - detail %d/%d, tiling %.3f,%.3f, scales %.3f,%.3f, blend rec %d (want %d), mask %d, blendTil %.3f,%.3f",
			             decDetail, kDetailSlot, pExtras[0].y, pExtras[0].z, decDetDiff, decDetBump,
			             decBlendRecord, kChunkStart + recFull + 2, (int)pExtras[1].y, decMaskTil, decLayerTil);
		}

		// the blend base record must be a decodable base record at that index
		const Vec4* pBlend = arrRec.GetElements() + (decBlendRecord - kChunkStart) * SVO_RT_RECORD_TEXELS;

		int decBlendNor = 0, decBlendSpc = 0;
		RT_ExtractUint2(pBlend[0].y, decBlendNor, decBlendSpc);

		if ((int)pBlend[0].x != kBlendAlb || decBlendNor != kBlendNor || decBlendSpc != 0)
			extrasMismatches++;

		// the plain material must reference no extras and must not have grown
		const Vec4* pPlainBase = arrRec.GetElements() + recPlain * SVO_RT_RECORD_TEXELS;
		int         plainEmi = 0, plainExtras = 0;
		RT_ExtractUint2(pPlainBase[0].z, plainEmi, plainExtras);

		if (plainEmi != 8 || plainExtras != 0)
			extrasMismatches++;

		// every tag must survive the float round trip as an exact value
		const float kTags[7] = { SVO_RT_TAG_ILLUM, SVO_RT_TAG_VEG_LEAVES, SVO_RT_TAG_HUMAN_SKIN,
			                       SVO_RT_TAG_GLASS, SVO_RT_TAG_WATER, SVO_RT_TAG_TERRAIN, SVO_RT_TAG_EMISSIVE_ONLY };

		for (int i = 0; i < 7; i++)
		{
			const Vec4 rec3(0.f, 0.f, kTags[i], RT_PackTC16(1.f, 0.f));

			if (rec3.z != kTags[i])
				extrasMismatches++;
		}
	}

	PrintMessage("RT self test: material extras - detail + blend + transmittance round trip, tags 0/0.25/2/3/4/5/6, mismatches = %d (must be 0)", extrasMismatches);


	// ---- gloss resample (report 02d route 1: the gloss map donates the normal slice's alpha) -----
	// Nothing here needs a texture: what can silently go wrong is the index math that writes a
	// smoothness copy of one size into a normal copy of another, so that is what is checked.
	int resampleMismatches = 0;
	{
		const int kCases[4][2] = { { 256, 256 }, { 256, 128 }, { 128, 256 }, { 64, 256 } };

		for (int c = 0; c < 4; c++)
		{
			const int dstSize = kCases[c][0], srcSize = kCases[c][1];

			int prev = -1;
			for (int i = 0; i < dstSize; i++)
			{
				const int v = RT_ResampleIndex(i, dstSize, srcSize);

				if (v < 0 || v >= srcSize || v < prev)   // in range and monotonic
					resampleMismatches++;

				prev = v;
			}

			if (RT_ResampleIndex(0, dstSize, srcSize) != 0)
				resampleMismatches++;

			// when the source is not finer than the destination the last texel must be the last one
			if (srcSize <= dstSize && RT_ResampleIndex(dstSize - 1, dstSize, srcSize) != srcSize - 1)
				resampleMismatches++;
		}
	}

	PrintMessage("RT self test: gloss resample - 4 size pairs, out of range / non monotonic / wrong end texel = %d (must be 0)", resampleMismatches);

	// ---- dynamic two level tree (stage 3A) ------------------------------------------------------
	// Two cached objects with different transforms, flattened under a median split top level and
	// traced through THE WRITTEN RECORDS; then a second frame with one object moved, its records
	// rewritten over the first frame's in a simulated pool with the stale tail zeroed.
	int dynMismatches = 0;
	int dynOverflows = 0;
	int dynStaleTexels = 0;
	int dynRecordsUsed[4] = { 0, 0, 0, 0 };
	int dynOverflowObjs = 0;
	int dynTrisTotal = 0;
	{
		SRTDynMesh meshA, meshB;

		{
			PodArray<SRTBuildTri> arrA, arrB;
			SAddQuad::Add(arrA, Vec3(-1, -1, 0), Vec3(2, 0, 0), Vec3(0, 2, 0), 5);   // 50 tris
			SAddQuad::Add(arrB, Vec3(-2, -2, 0), Vec3(4, 0, 0), Vec3(0, 4, 0), 3);   // 18 tris
			SAddQuad::Add(arrB, Vec3(-2, -2, 0), Vec3(4, 0, 0), Vec3(0, 0, 3), 3);   // + a wall
			RT_BuildDynMeshTopology(arrA, nullptr, meshA);
			RT_BuildDynMeshTopology(arrB, nullptr, meshB);
		}

		dynTrisTotal = meshA.tris.Count() + meshB.tris.Count();

		PodArray<Vec4> pool;
		pool.PreAllocate(SVO_RT_DYN_MESH_COUNT * SVO_RT_RECORD_TEXELS, SVO_RT_DYN_MESH_COUNT * SVO_RT_RECORD_TEXELS);
		memset(pool.GetElements(), 0, pool.GetDataSize());

		int prevUsed = 0;

		for (int frame = 0; frame < 4 && !dynMismatches; frame++)
		{
			SRTDynFrame f;

			Matrix34 matA = Matrix34::CreateRotationZ(DEG2RAD(15.f));
			matA.SetTranslation(Vec3(0.f, 0.f, 0.f));

			Matrix34 matB = Matrix34::CreateRotationX(DEG2RAD(40.f));
			matB.SetTranslation(Vec3(6.f, 2.f, 1.f));

			if (frame == 1)
			{
				// object A moves: the tree is rewritten, the records it used before must not survive
				matA = Matrix34::CreateRotationY(DEG2RAD(25.f));
				matA.SetTranslation(Vec3(-8.f, -4.f, 2.f));
			}

			SRTDynMesh* arrMesh[2] = { &meshA, &meshB };
			Matrix34    arrMat[2] = { matA, matB };

			// frame 2 drops object B: the region shrinks, so the tail it used must be zeroed.
			// frame 3 puts 70 objects on a grid, which is more than the top level depth cap can
			// separate - the overflow leaf policy has to keep the result correct.
			const int instCount = (frame == 2) ? 1 : ((frame == 3) ? 70 : 2);

			for (int i = 0; i < instCount; i++)
			{
				SRTDynMesh* pInstMesh = (frame == 3) ? &meshB : arrMesh[i];
				Matrix34    instMat;

				if (frame == 3)
				{
					instMat = Matrix34::CreateRotationZ(DEG2RAD(i * 7.f));
					instMat.SetTranslation(Vec3((i % 10) * 9.f, (i / 10) * 9.f, (i % 3) * 2.f));
				}
				else
				{
					instMat = arrMat[i];
				}

				SRTDynInst inst;
				inst.pMesh = pInstMesh;
				inst.pMat = nullptr;
				inst.mat = instMat;
				inst.importance = 1.f;
				inst.waterMat = -1;
				inst.triOfs = 0;
				inst.boxOfs = 0;
				inst.pSkinAtt = nullptr;
				inst.skinLod = 0;
				inst.pSkin = nullptr;
				inst.worldBox.Reset();
				for (int c = 0; c < 8; c++)
				{
					const Vec3 p((c & 1) ? pInstMesh->localBox.max.x : pInstMesh->localBox.min.x,
					             (c & 2) ? pInstMesh->localBox.max.y : pInstMesh->localBox.min.y,
					             (c & 4) ? pInstMesh->localBox.max.z : pInstMesh->localBox.min.z);
					inst.worldBox.Add(instMat.TransformPoint(p));
				}
				inst.qb = RT_DynQuantBounds(inst.worldBox);
				f.insts.Add(inst);
			}

			for (int i = 0; i < f.insts.Count(); i++)
			{
				SRTDynInst& inst = f.insts[i];

				inst.triOfs = f.worldTris.Count();
				inst.boxOfs = f.nodeBoxes.Count();

				for (int t = 0; t < inst.pMesh->tris.Count(); t++)
				{
					SRTBuildTri bt = inst.pMesh->tris[t];
					for (int v = 0; v < 3; v++)
					{
						bt.v[v] = inst.mat.TransformPoint(bt.v[v]);
						bt.n[v] = inst.mat.TransformVector(bt.n[v]);
					}
					bt.faceNorm = (bt.v[1] - bt.v[0]).Cross(bt.v[2] - bt.v[0]).GetNormalizedSafe(Vec3(0, 0, 1));
					bt.matRecord = SVO_RT_SEG_DYN_MATS;
					f.worldTris.Add(bt);
				}

				RT_RefitDynInst(f, inst);
			}

			int upperBound = 2 * f.insts.Count() + 8;
			for (int i = 0; i < f.insts.Count(); i++)
				upperBound += f.insts[i].pMesh->nodes.Count() + f.insts[i].pMesh->tris.Count();

			f.records.PreAllocate(upperBound * SVO_RT_RECORD_TEXELS, upperBound * SVO_RT_RECORD_TEXELS);

			AABB rootBox;
			rootBox.Reset();
			for (int i = 0; i < f.insts.Count(); i++)
				rootBox.Add(f.insts[i].worldBox);

			PodArray<int> arrIds;
			for (int i = 0; i < f.insts.Count(); i++)
				arrIds.Add(i);

			const int used = RT_EmitDynTop(f, arrIds.GetElements(), arrIds.Count(), 0, SVO_RT_SEG_DYN_MESH, RT_DynQuantBounds(rootBox));

			dynRecordsUsed[frame] = used;
			dynOverflowObjs = max(dynOverflowObjs, f.overflowed);

			// the upload the engine does: whole region rewritten, stale tail zeroed
			memcpy(pool.GetElements(), f.records.GetElements(), (size_t)used * SVO_RT_RECORD_TEXELS * sizeof(Vec4));
			if (prevUsed > used)
				memset(pool.GetElements() + (size_t)used * SVO_RT_RECORD_TEXELS, 0,
				       (size_t)(prevUsed - used) * SVO_RT_RECORD_TEXELS * sizeof(Vec4));
			prevUsed = used;

			// nothing beyond the used region may be non zero any more
			for (int i = used * SVO_RT_RECORD_TEXELS; i < min(used + 512, (int)SVO_RT_DYN_MESH_COUNT) * SVO_RT_RECORD_TEXELS; i++)
				if (pool[i].x != 0.f || pool[i].y != 0.f || pool[i].z != 0.f || pool[i].w != 0.f)
					dynStaleTexels++;

			// decode the triangles back by walking the tree, exactly as the consumer reads them
			PodArray<Vec3> arrDecV;
			PodArray<int>  arrDecRec;
			{
				PodArray<int> stack;
				stack.Add(SVO_RT_SEG_DYN_MESH);

				while (stack.Count())
				{
					const int rec = stack.Last();
					stack.DeleteLast();

					const Vec4* pRec = pool.GetElements() + (size_t)rec * SVO_RT_RECORD_TEXELS;
					const int   child0 = (int)pRec[3].x;

					if (child0 > 0)
					{
						stack.Add(child0);
						stack.Add((int)pRec[3].y);
						continue;
					}

					const int  triCount = (int)pRec[0].w;
					const Vec4 nodeQb = pRec[2];

					for (int t = 0; t < triCount; t++)
					{
						const Vec4* pTri = pRec + (size_t)(1 + t) * SVO_RT_RECORD_TEXELS;
						for (int v = 0; v < 3; v++)
						{
							const uint32 packed = RT_AsUint(pTri[v].x);
							arrDecV.Add(Vec3(RT_UnquantPos(packed & 0xffff, nodeQb.x, nodeQb.w),
							                 RT_UnquantPos((packed >> 16) & 0xffff, nodeQb.y, nodeQb.w),
							                 pTri[v].w));
						}
						arrDecRec.Add(rec + 1 + t);
					}
				}
			}

			if (arrDecRec.Count() != f.worldTris.Count())
			{
				PrintMessage("RT self test: dynamic frame %d decoded %d triangles, expected %d", frame, arrDecRec.Count(), f.worldTris.Count());
				dynMismatches++;
				continue;
			}

			SRTTestRand dynRnd(0xd1a1u + frame);

			AABB traceBox = rootBox;
			const Vec3  tc = traceBox.GetCenter();
			const float tr = traceBox.GetRadius() * 1.5f;

			for (int rayId = 0; rayId < 1000; rayId++)
			{
				Vec3 org(tc.x + dynRnd.NextRange(-tr, tr), tc.y + dynRnd.NextRange(-tr, tr), tc.z + dynRnd.NextRange(-tr, tr));

				Vec3 dir;
				if (rayId & 1)
					dir = Vec3(dynRnd.NextRange(traceBox.min.x, traceBox.max.x),
					           dynRnd.NextRange(traceBox.min.y, traceBox.max.y),
					           dynRnd.NextRange(traceBox.min.z, traceBox.max.z)) - org;
				else
					dir = Vec3(dynRnd.NextRange(-1.f, 1.f), dynRnd.NextRange(-1.f, 1.f), dynRnd.NextRange(-1.f, 1.f));

				if (dir.GetLengthSquared() < 1e-6f)
					dir = Vec3(0, 0, 1);
				dir.Normalize();

				const float tMaxDyn = tr * 4.f;

				float bestT = tMaxDyn;
				int   bestRec = -1;
				bool  bInc = false;

				RT_RefTrace(pool.GetElements(), SVO_RT_SEG_DYN_MESH, SVO_RT_SEG_DYN_MESH, org, dir, tMaxDyn, bestT, bestRec, bInc);

				if (bInc)
					dynOverflows++;

				float refT = tMaxDyn;
				int   refRec = -1;
				for (int i = 0; i < arrDecRec.Count(); i++)
				{
					float tHit;
					if (RT_TestTriangle(org, dir, arrDecV[i * 3 + 0], arrDecV[i * 3 + 1], arrDecV[i * 3 + 2], tHit) && tHit < refT)
					{
						refT = tHit;
						refRec = arrDecRec[i];
					}
				}

				if (bestRec != refRec || fabs(bestT - refT) > 1e-4f)
					dynMismatches++;

				if (bestRec >= 0 && bestRec >= dynRecordsUsed[frame])
					dynMismatches++;   // a stale record was walked
			}
		}
	}

	PrintMessage("RT self test: dynamic - 2 objects, %d tris, records %d, %d (moved), %d (one dropped), %d (70 objects, %d merged into overflow leaves); 4 x 1000 rays, mismatches = %d (must be 0), stack overflows = %d, stale texels after zeroing = %d",
	             dynTrisTotal, dynRecordsUsed[0], dynRecordsUsed[1], dynRecordsUsed[2], dynRecordsUsed[3], dynOverflowObjs,
	             dynMismatches, dynOverflows, dynStaleTexels);

	// ---------------------------------------------------------------------------------------------
	// Water surfaces (rt decision 09 9.4): a synthetic ocean ring and one water volume quad go
	// through the real producer - RT_AddWaterQuad, RT_BuildDynMeshTopology, RT_FillWaterMatRecord,
	// RT_AppendMatRecordSet, RT_RefitDynInst, RT_EmitDynTop - and are then read back out of the
	// WRITTEN records. Checked: the tag 4 record layout, that every water triangle faces +Z, that a
	// ray from above hits its FRONT face while the same ray from below hits its BACK face (which is
	// what lets the consumer reject it), and that each surface keeps its own material record.
	// ---------------------------------------------------------------------------------------------
	int waterMismatches = 0;
	int waterBackFacing = 0;
	int waterTrisTotal = 0;
	int waterRecordsUsed = 0;
	{
		SRTDynMesh meshOcean, meshVol;

		{
			PodArray<SRTBuildTri> arrO, arrV;
			RT_AddWaterQuad(arrO, Vec3(-40.f, -40.f, 0.f), Vec3(80.f, 0.f, 0.f), Vec3(0.f, 80.f, 0.f), 4);   // 32 tris
			RT_AddWaterQuad(arrV, Vec3(-3.f, -3.f, 0.f), Vec3(6.f, 0.f, 0.f), Vec3(0.f, 6.f, 0.f), 2);       //  8 tris
			for (int i = 0; i < arrO.Count(); i++)
				RT_NormalizeTriUV(arrO[i]);
			for (int i = 0; i < arrV.Count(); i++)
				RT_NormalizeTriUV(arrV[i]);
			RT_BuildDynMeshTopology(arrO, nullptr, meshOcean);
			RT_BuildDynMeshTopology(arrV, nullptr, meshVol);
		}

		waterTrisTotal = meshOcean.tris.Count() + meshVol.tris.Count();

		if (meshOcean.tris.Count() != 32 || meshVol.tris.Count() != 8)
			waterMismatches++;   // the ocean ring has to stay inside its 32 triangle budget

		// ---- the two material records ----------------------------------------------------------
		const Vec3  oceanFog(0.05f, 0.14f, 0.19f);
		const Vec3  volFog(0.10f, 0.30f, 0.22f);
		const float oceanDensity = 0.32f, volDensity = 0.85f;
		const float oceanDepth = 5.f, volDepth = 2.5f;

		SRTMatRecordSet setOcean, setVol;
		RT_FillWaterMatRecord(nullptr, oceanFog, oceanDensity, oceanDepth, setOcean);
		RT_FillWaterMatRecord(nullptr, volFog, volDensity, volDepth, setVol);

		if (setOcean.RecordCount() != 2 || setVol.RecordCount() != 2)
			waterMismatches++;   // base + extras, never a blend record

		PodArray<Vec4> mats;
		RT_AppendMatRecordSet(setOcean, 0, SVO_RT_SEG_DYN_MATS, mats, nullptr);
		RT_AppendMatRecordSet(setVol, setOcean.RecordCount(), SVO_RT_SEG_DYN_MATS, mats, nullptr);

		const int oceanMatRec = SVO_RT_SEG_DYN_MATS + 0;
		const int volMatRec = SVO_RT_SEG_DYN_MATS + setOcean.RecordCount();

		{
			const Vec4* pBase = mats.GetElements();
			const Vec4* pExtra = mats.GetElements() + SVO_RT_RECORD_TEXELS;

			int emissiveSlot = 0, extrasRef = 0;
			RT_ExtractUint2(pBase[0].z, emissiveSlot, extrasRef);

			float opacity = 0.f, alphaRef = 0.f;
			RT_ExtractTC16(pBase[3].w, opacity, alphaRef);

			if (extrasRef != 1 || emissiveSlot != 0)
				waterMismatches++;
			if (pBase[3].z != SVO_RT_TAG_WATER)
				waterMismatches++;
			if (fabs(pBase[1].y - 0.02f) > 1e-6f || fabs(pBase[1].z - 1.f) > 1e-6f)
				waterMismatches++;
			if (fabs(opacity - 1.f) > 1e-3f || fabs(alphaRef) > 1e-3f)
				waterMismatches++;
			if (fabs(pBase[2].x - oceanFog.x) > 1e-6f || fabs(pBase[2].y - oceanFog.y) > 1e-6f || fabs(pBase[2].z - oceanFog.z) > 1e-6f)
				waterMismatches++;
			if (fabs(pExtra[2].x - oceanFog.x) > 1e-6f || fabs(pExtra[2].y - oceanFog.y) > 1e-6f ||
			    fabs(pExtra[2].z - oceanFog.z) > 1e-6f || fabs(pExtra[2].w - oceanDensity) > 1e-6f)
				waterMismatches++;
			if (fabs(pExtra[3].x - 1.f) > 1e-6f || fabs(pExtra[3].y - 1.f) > 1e-6f ||
			    fabs(pExtra[3].z - oceanDepth) > 1e-6f || pExtra[3].w != 0.f)
				waterMismatches++;

			const Vec4* pVolBase = mats.GetElements() + (size_t)setOcean.RecordCount() * SVO_RT_RECORD_TEXELS;
			const Vec4* pVolExtra = pVolBase + SVO_RT_RECORD_TEXELS;

			if (pVolBase[3].z != SVO_RT_TAG_WATER || fabs(pVolExtra[2].w - volDensity) > 1e-6f ||
			    fabs(pVolExtra[3].z - volDepth) > 1e-6f || fabs(pVolBase[2].y - volFog.y) > 1e-6f)
				waterMismatches++;
		}

		// ---- emit both surfaces through the real dynamic path -----------------------------------
		// the volume sits outside the ocean quad's XY footprint, so "from above" is unambiguous
		const Vec3 oceanCentre(5.f, -3.f, 12.f);
		const Vec3 volCentre(60.f, 40.f, 14.f);

		SRTDynFrame f;

		SRTDynMesh* arrMesh[2] = { &meshOcean, &meshVol };
		Matrix34    arrMat[2];
		arrMat[0].SetIdentity();
		arrMat[0].SetTranslation(oceanCentre);
		arrMat[1].SetIdentity();
		arrMat[1].SetTranslation(volCentre);

		const int arrMatRec[2] = { oceanMatRec, volMatRec };

		for (int i = 0; i < 2; i++)
		{
			SRTDynInst inst;
			inst.pMesh = arrMesh[i];
			inst.pMat = nullptr;
			inst.mat = arrMat[i];
			inst.importance = 0.f;
			inst.waterMat = i;
			inst.triOfs = 0;
			inst.boxOfs = 0;
			inst.pSkinAtt = nullptr;
			inst.skinLod = 0;
			inst.pSkin = nullptr;
			inst.worldBox.Reset();
			for (int c = 0; c < 8; c++)
			{
				const Vec3 pt((c & 1) ? arrMesh[i]->localBox.max.x : arrMesh[i]->localBox.min.x,
				              (c & 2) ? arrMesh[i]->localBox.max.y : arrMesh[i]->localBox.min.y,
				              (c & 4) ? arrMesh[i]->localBox.max.z : arrMesh[i]->localBox.min.z);
				inst.worldBox.Add(arrMat[i].TransformPoint(pt));
			}
			inst.qb = RT_DynQuantBounds(inst.worldBox);
			f.insts.Add(inst);
		}

		for (int i = 0; i < f.insts.Count(); i++)
		{
			SRTDynInst& inst = f.insts[i];

			inst.triOfs = f.worldTris.Count();
			inst.boxOfs = f.nodeBoxes.Count();

			for (int t = 0; t < inst.pMesh->tris.Count(); t++)
			{
				SRTBuildTri bt = inst.pMesh->tris[t];
				for (int v = 0; v < 3; v++)
				{
					bt.v[v] = inst.mat.TransformPoint(bt.v[v]);
					bt.n[v] = inst.mat.TransformVector(bt.n[v]);
				}
				bt.faceNorm = (bt.v[1] - bt.v[0]).Cross(bt.v[2] - bt.v[0]).GetNormalizedSafe(Vec3(0, 0, 1));
				bt.matRecord = arrMatRec[inst.waterMat];
				f.worldTris.Add(bt);
			}

			RT_RefitDynInst(f, inst);
		}

		int upperBound = 2 * f.insts.Count() + 8;
		for (int i = 0; i < f.insts.Count(); i++)
			upperBound += f.insts[i].pMesh->nodes.Count() + f.insts[i].pMesh->tris.Count();

		f.records.PreAllocate(upperBound * SVO_RT_RECORD_TEXELS, upperBound * SVO_RT_RECORD_TEXELS);

		AABB rootBox;
		rootBox.Reset();
		for (int i = 0; i < f.insts.Count(); i++)
			rootBox.Add(f.insts[i].worldBox);

		PodArray<int> arrIds;
		for (int i = 0; i < f.insts.Count(); i++)
			arrIds.Add(i);

		waterRecordsUsed = RT_EmitDynTop(f, arrIds.GetElements(), arrIds.Count(), 0, SVO_RT_SEG_DYN_MESH, RT_DynQuantBounds(rootBox));

		PodArray<Vec4> pool;
		pool.PreAllocate(waterRecordsUsed * SVO_RT_RECORD_TEXELS, waterRecordsUsed * SVO_RT_RECORD_TEXELS);
		memcpy(pool.GetElements(), f.records.GetElements(), (size_t)waterRecordsUsed * SVO_RT_RECORD_TEXELS * sizeof(Vec4));

		// ---- read the triangles back out of the written records --------------------------------
		PodArray<Vec3> arrDecV;
		PodArray<int>  arrDecRec;
		PodArray<int>  arrDecMat;
		{
			PodArray<int> stack;
			stack.Add(SVO_RT_SEG_DYN_MESH);

			while (stack.Count())
			{
				const int rec = stack.Last();
				stack.DeleteLast();

				const Vec4* pRec = pool.GetElements() + (size_t)rec * SVO_RT_RECORD_TEXELS;
				const int   child0 = (int)pRec[3].x;

				if (child0 > 0)
				{
					stack.Add(child0);
					stack.Add((int)pRec[3].y);
					continue;
				}

				const int  triCount = (int)pRec[0].w;
				const Vec4 nodeQb = pRec[2];

				for (int t = 0; t < triCount; t++)
				{
					const Vec4* pTri = pRec + (size_t)(1 + t) * SVO_RT_RECORD_TEXELS;
					for (int v = 0; v < 3; v++)
					{
						const uint32 packed = RT_AsUint(pTri[v].x);
						arrDecV.Add(Vec3(RT_UnquantPos(packed & 0xffff, nodeQb.x, nodeQb.w),
						                 RT_UnquantPos((packed >> 16) & 0xffff, nodeQb.y, nodeQb.w),
						                 pTri[v].w));
					}
					arrDecRec.Add(rec + 1 + t);
					arrDecMat.Add((int)pTri[3].x);
				}
			}
		}

		if (arrDecRec.Count() != waterTrisTotal)
			waterMismatches++;

		// every water triangle must face up: that is what makes "seen from above" well defined
		for (int i = 0; i < arrDecRec.Count(); i++)
		{
			const Vec3 n = (arrDecV[i * 3 + 1] - arrDecV[i * 3 + 0]).Cross(arrDecV[i * 3 + 2] - arrDecV[i * 3 + 0]);

			if (n.z <= 0.f)
				waterMismatches++;
			if (arrDecMat[i] != oceanMatRec && arrDecMat[i] != volMatRec)
				waterMismatches++;
		}

		// ---- trace the same points from above and from below ------------------------------------
		SRTTestRand waterRnd(0x7a7eu);

		for (int rayId = 0; rayId < 400; rayId++)
		{
			const bool  bVolume = (rayId & 1) != 0;
			const Vec3  centre = bVolume ? volCentre : oceanCentre;
			const float halfSize = bVolume ? 2.5f : 35.f;
			const int   wantMat = bVolume ? volMatRec : oceanMatRec;

			const Vec3 target(centre.x + waterRnd.NextRange(-halfSize, halfSize),
			                  centre.y + waterRnd.NextRange(-halfSize, halfSize),
			                  centre.z);

			for (int side = 0; side < 2; side++)
			{
				const Vec3 org = target + Vec3(0.f, 0.f, side ? -20.f : 20.f);
				const Vec3 dir = (target - org).GetNormalizedSafe(Vec3(0, 0, -1));

				float bestT = 100.f;
				int   bestRec = -1;
				bool  bInc = false;

				RT_RefTrace(pool.GetElements(), SVO_RT_SEG_DYN_MESH, SVO_RT_SEG_DYN_MESH, org, dir, 100.f, bestT, bestRec, bInc);

				if (bestRec < 0 || bInc)
				{
					// the geometry must be hit from both sides - it is the CONSUMER that rejects one
					waterMismatches++;
					continue;
				}

				int decoded = -1;
				for (int i = 0; i < arrDecRec.Count(); i++)
					if (arrDecRec[i] == bestRec)
						decoded = i;

				if (decoded < 0)
				{
					waterMismatches++;
					continue;
				}

				if (arrDecMat[decoded] != wantMat)
					waterMismatches++;   // each surface keeps its own water material record

				// the geometric normal the consumer rebuilds from the winding, against the ray
				const Vec3  nrm = (arrDecV[decoded * 3 + 1] - arrDecV[decoded * 3 + 0]).Cross(arrDecV[decoded * 3 + 2] - arrDecV[decoded * 3 + 0]);
				const float facing = nrm.GetNormalizedSafe(Vec3(0, 0, 1)).Dot(dir);

				if (side == 0 && facing >= 0.f)
					waterMismatches++;   // from above the water is a FRONT face

				if (side == 1)
				{
					if (facing <= 0.f)
						waterMismatches++;   // from below it must be a BACK face
					else
						waterBackFacing++;
				}
			}
		}
	}

	PrintMessage("RT self test: water - ocean ring %d tris + volume quad, records %d, tag 4 record round trip, 400 x 2 rays, from below back facing = %d (must be 400), mismatches = %d (must be 0)",
	             waterTrisTotal, waterRecordsUsed, waterBackFacing, waterMismatches);





	if (mismatchVsDecoded == 0 && overflowCount == 0 && matMismatches == 0 && extrasMismatches == 0 && resampleMismatches == 0 && dynMismatches == 0 && dynOverflows == 0 && dynStaleTexels == 0 && waterMismatches == 0)
		PrintMessage("RT self test: PASSED");
	else
		PrintMessage("RT self test: FAILED");
}

ColorB* CVoxelSegment::ApplyHighPass(uint16& nTexW, uint16& nTexH, const ColorB* pTexRgbOr)
{
	ColorB* pTexRgbHP = new ColorB[nTexW * nTexH];
	ColorF* pBlurredF = new ColorF[nTexW * nTexH];

	// make blurred copy
	for (int x = 0; x < nTexW; x++)
	{
		for (int y = 0; y < nTexH; y++)
		{
			ColorF colAver(0, 0, 0, 0);

			int samplingRange = 16;

			for (int i = -samplingRange; i <= samplingRange; i += 2)
			{
				for (int j = -samplingRange; j <= samplingRange; j += 2)
				{
					int X = (x + i) & (nTexW - 1);
					int Y = (y + j) & (nTexH - 1);

					colAver.r += pTexRgbOr[X * nTexH + Y].r;
					colAver.g += pTexRgbOr[X * nTexH + Y].g;
					colAver.b += pTexRgbOr[X * nTexH + Y].b;
					colAver.a++;
				}
			}

			pBlurredF[x * nTexH + y] = colAver / colAver.a;
		}
	}

	// get difference between blurred and original
	for (int x = 0; x < nTexW; x++)
	{
		for (int y = 0; y < nTexH; y++)
		{
			ColorF colF;
			colF.r = pTexRgbOr[x * nTexH + y].r;
			colF.g = pTexRgbOr[x * nTexH + y].g;
			colF.b = pTexRgbOr[x * nTexH + y].b;
			colF.a = pTexRgbOr[x * nTexH + y].a;

			colF = (colF - pBlurredF[x * nTexH + y] + 127.5f);

			pTexRgbHP[x * nTexH + y].r = SATURATEB((int)colF.r);
			pTexRgbHP[x * nTexH + y].g = SATURATEB((int)colF.g);
			pTexRgbHP[x * nTexH + y].b = SATURATEB((int)colF.b);
			pTexRgbHP[x * nTexH + y].a = 255;
		}
	}

	delete[] pBlurredF;

	return pTexRgbHP;
}

void CVoxelSegment::DebugDrawVoxels()
{
	if (Cry3DEngineBase::GetCVars()->e_svoDebug == 6 || Cry3DEngineBase::GetCVars()->e_svoDebug == 3)
	{
		if (m_voxData.pData[SVoxBrick::OPA3D] && CVoxelSegment::m_voxCam.IsAABBVisible_F(m_pNode->m_nodeBox) && gSvoEnv->m_debugDrawVoxelsCounter < 100000)
		{
			Vec4 voxNodeData[SVO_NODE_BRICK_SIZE * SVO_NODE_BRICK_SIZE * SVO_NODE_BRICK_SIZE];
			ZeroStruct(voxNodeData);
			voxNodeData[0] = Vec4(m_boxOS.min + m_vSegOrigin, 0);
			voxNodeData[1] = Vec4(m_boxOS.max + m_vSegOrigin, 0);
			voxNodeData[0] = voxNodeData[0] + Vec4(Vec3((float)m_vCropBoxMin.x / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropBoxMin.y / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropBoxMin.z / SVO_VOX_BRICK_MAX_SIZE) * GetBoxSize(), 0);
			voxNodeData[1] = voxNodeData[0] + Vec4(Vec3((float)m_vCropTexSize.x / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropTexSize.y / SVO_VOX_BRICK_MAX_SIZE, (float)m_vCropTexSize.z / SVO_VOX_BRICK_MAX_SIZE) * GetBoxSize(), 0);

			if (Cry3DEngineBase::GetCVars()->e_svoDebug == 3)
			{
				Vec3 vMin = *(Vec3*)&(voxNodeData[0]);
				Vec3 vMax = *(Vec3*)&(voxNodeData[1]);
				Cry3DEngineBase::DrawBBox(AABB(vMin, vMax));
				return;
			}

			for (int x = 0; x < m_vCropTexSize.x; x++)
				for (int y = 0; y < m_vCropTexSize.y; y++)
					for (int z = 0; z < m_vCropTexSize.z; z++)
					{
						int id = z * m_vCropTexSize.x * m_vCropTexSize.y + y * m_vCropTexSize.x + x;
						ColorB& opaOutFin = m_voxData.pData[SVoxBrick::OPA3D][id];

						if (opaOutFin.r || opaOutFin.g || opaOutFin.b)
						{
							Vec4 vMin = voxNodeData[0] + (voxNodeData[1] - voxNodeData[0]) * Vec4((float) x / m_vCropTexSize.x, (float) y / m_vCropTexSize.y, (float) z / m_vCropTexSize.z, 1);
							Vec4 vMax = voxNodeData[0] + (voxNodeData[1] - voxNodeData[0]) * Vec4((float) (x + 1) / m_vCropTexSize.x, (float) (y + 1) / m_vCropTexSize.y, (float) (z + 1) / m_vCropTexSize.z, 1);

							//          // safety border support
							//          if(Cry3DEngineBase::GetCVars()->e_svoDebug == 6)
							//          {
							//            Vec4 vCenter(m_vOrigin.x,m_vOrigin.y,m_vOrigin.z,0);
							//            vMin += (vMin - vCenter)/(kVoxTexMaxDim/2)/2;
							//            vMax += (vMax - vCenter)/(kVoxTexMaxDim/2)/2;
							//          }

							AABB voxBox;
							voxBox.min.Set(vMin.x, vMin.y, vMin.z);
							voxBox.max.Set(vMax.x, vMax.y, vMax.z);

							if (!CVoxelSegment::m_voxCam.IsAABBVisible_F(voxBox))
								continue;

							voxBox.Expand(-voxBox.GetSize() * 0.025f);

							Cry3DEngineBase::DrawBBox(voxBox,
							                          ColorF(
							                            (GetBoxSize() == GetCVars()->e_svoMinNodeSize * 1),
							                            (GetBoxSize() == GetCVars()->e_svoMinNodeSize * 2),
							                            (GetBoxSize() == GetCVars()->e_svoMinNodeSize * 4),
							                            1));

							gSvoEnv->m_debugDrawVoxelsCounter++;
						}
					}
		}
	}
}

void CVoxelSegment::ErrorTerminate(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	char szText[512];
	cry_vsprintf(szText, format, args);
	va_end(args);

	#if CRY_PLATFORM_WINDOWS
	if (!gEnv->pGameFramework)
	{
		// quick terminate if in developer mode
		char szTextFull[512];
		cry_sprintf(szTextFull, "%s\nTerminate process?", szText);
		if (CryMessageBox(szTextFull, "3DEngine fatal error", eMB_YesCancel) == eQR_Yes)
			TerminateProcess(GetCurrentProcess(), 0);
		else
			return;
	}
	#endif

	gEnv->pSystem->FatalError(szText); // it causes 8 message boxes but we need just one
}

const float kSvoSuperMeshHashScale = .1f;

int SSuperMesh::FindVertex(const Vec3& rPos, const Vec2 rTC, const Vec3& rNor, PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim], PodArrayRT<SRayHitVertex>& vertsInArea)
{
	Vec3i vPosI0 = rPos / kSvoSuperMeshHashScale - Vec3(VEC_EPSILON, VEC_EPSILON, VEC_EPSILON);
	Vec3i vPosI1 = rPos / kSvoSuperMeshHashScale + Vec3(VEC_EPSILON, VEC_EPSILON, VEC_EPSILON);

	for (int x = vPosI0.x; x <= vPosI1.x; x++)
		for (int y = vPosI0.y; y <= vPosI1.y; y++)
			for (int z = vPosI0.z; z <= vPosI1.z; z++)
			{
				Vec3i vPosI(x, y, z);

				PodArray<SMINDEX>& rSubIndices = arrVertHash[vPosI.x & (hashDim - 1)][vPosI.y & (hashDim - 1)][vPosI.z & (hashDim - 1)];

				for (int ii = 0; ii < rSubIndices.Count(); ii++)
				{
					if (vertsInArea[rSubIndices[ii]].v.IsEquivalent(rPos))
						if (vertsInArea[rSubIndices[ii]].t.IsEquivalent(rTC))
							if (vertsInArea[rSubIndices[ii]].n.IsEquivalent(rNor))
							  return rSubIndices[ii];
				}
			}

	return -1;
}

int SSuperMesh::AddVertex(const SRayHitVertex& rVert, PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim], PodArrayRT<SRayHitVertex>& vertsInArea)
{
	Vec3 vPosS = rVert.v / kSvoSuperMeshHashScale;

	Vec3i vPosI(int(floor(vPosS.x)), int(floor(vPosS.y)), int(floor(vPosS.z)));

	PodArray<SMINDEX>& rSubIndices = arrVertHash[vPosI.x & (hashDim - 1)][vPosI.y & (hashDim - 1)][vPosI.z & (hashDim - 1)];

	rSubIndices.Add(vertsInArea.Count());

	vertsInArea.Add(rVert);

	return vertsInArea.Count() - 1;
}

void SSuperMesh::AddSuperTriangle(SRayHitTriangle& htIn, PodArray<SMINDEX> arrVertHash[hashDim][hashDim][hashDim], ObjectLayerIdType nObjLayerId)
{
	if (!m_pTrisInArea)
	{
		m_pTrisInArea = new PodArrayRT<SRayHitTriangleIndexed>;
		m_pVertInArea = new PodArrayRT<SRayHitVertex>;
		m_pMatsInArea = new PodArrayRT<SSvoMatInfo>;
		m_pFaceNormals = new PodArrayRT<Vec3>;
	}

	if (m_pVertInArea->Count() + 3 > (SMINDEX) ~0)
		return;

	SRayHitTriangleIndexed htOut;
	htOut.vFaceNorm = htIn.n;

	SSvoMatInfo matInfo;
	matInfo.pMat = htIn.pMat;
	int matId = m_pMatsInArea->Find(matInfo);
	if (matId < 0)
	{
		matId = m_pMatsInArea->Count();

		// stat obj, get access to texture RGB data
		if (htIn.pMat)
		{
			SShaderItem* pShItem = &htIn.pMat->GetShaderItem();
			int* pLowResSystemCopyAtlasId = 0;
			if (pShItem->m_pShaderResources)
			{
				SEfResTexture* pResTexture = pShItem->m_pShaderResources->GetTexture(EFTT_DIFFUSE);
				if (pResTexture)
				{
					ITexture* pITex = pResTexture->m_Sampler.m_pITex;
					if (pITex)
					{
						{
							AUTO_MODIFYLOCK(CVoxelSegment::m_arrLockedTextures.m_Lock);
							CVoxelSegment::m_arrLockedTextures[pITex] = pITex;
						}
						matInfo.pTexRgb = (ColorB*)pITex->GetLowResSystemCopy(matInfo.textureWidth, matInfo.textureHeight, &pLowResSystemCopyAtlasId);
					}
				}
			}
		}

		m_pMatsInArea->Add(matInfo);
	}
	htOut.materialID = matId;
	htOut.objectLayerId = nObjLayerId;
	htOut.triArea = htIn.nTriArea;
	htOut.opacity = htIn.nOpacity;
	htOut.hitObjectType = htIn.nHitObjType;

	for (int v = 0; v < 3; v++)
	{
		int vertId = FindVertex(htIn.v[v], htIn.t[v], htIn.vn[v], arrVertHash, *m_pVertInArea);

		if (vertId < 0)
		{
			SRayHitVertex hv;
			hv.v = htIn.v[v];
			hv.t = htIn.t[v];
			hv.c = htIn.c[v];
			hv.n = htIn.vn[v];
			assert(hv.n.GetLength() > 0.9f && hv.n.GetLength() < 1.1f);

			vertId = AddVertex(hv, arrVertHash, *m_pVertInArea);
		}

		htOut.arrVertId[v] = vertId;
	}

	m_pTrisInArea->Add(htOut);
	m_pFaceNormals->Add(htIn.n);
}

void SSuperMesh::AddSuperMesh(SSuperMesh& smIn, float vertexOffset)
{
	MEMSTAT_CONTEXT(EMemStatContextType::Other, "AddSuperMesh");

	if (!smIn.m_pVertInArea || !smIn.m_pTrisInArea || !smIn.m_pTrisInArea->Count())
		return;

	if ((m_pVertInArea ? m_pVertInArea->Count() : 0) + smIn.m_pVertInArea->Count() > (SMINDEX) ~0)
		return;

	PodArrayRT<Vec3> vertInNormals;
	vertInNormals.PreAllocate(smIn.m_pVertInArea->Count(), smIn.m_pVertInArea->Count());

	for (int t = 0; t < smIn.m_pTrisInArea->Count(); t++)
	{
		SRayHitTriangleIndexed tr = smIn.m_pTrisInArea->GetAt(t);

		for (int v = 0; v < 3; v++)
			vertInNormals[tr.arrVertId[v]] += smIn.m_pFaceNormals->GetAt(t);
	}

	for (int v = 0; v < smIn.m_pVertInArea->Count(); v++)
	{
		smIn.m_pVertInArea->GetAt(v).v += vertInNormals[v].GetNormalized() * vertexOffset;
		m_boxTris.Add(smIn.m_pVertInArea->GetAt(v).v);
	}

	if (!m_pTrisInArea)
	{
		m_pTrisInArea = new PodArrayRT<SRayHitTriangleIndexed>;
		m_pVertInArea = new PodArrayRT<SRayHitVertex>;
		m_pMatsInArea = new PodArrayRT<SSvoMatInfo>;
	}

	int numVertBefore = m_pVertInArea->Count();

	m_pTrisInArea->PreAllocate(m_pTrisInArea->Count() + smIn.m_pTrisInArea->Count());

	for (int t = 0; t < smIn.m_pTrisInArea->Count(); t++)
	{
		SRayHitTriangleIndexed tr = smIn.m_pTrisInArea->GetAt(t);

		for (int v = 0; v < 3; v++)
			tr.arrVertId[v] += numVertBefore;

		SSvoMatInfo matInfo;
		matInfo.pMat = (*smIn.m_pMatsInArea)[tr.materialID].pMat;

		int matId = m_pMatsInArea->FindReverse(matInfo);
		if (matId < 0)
		{
			matId = m_pMatsInArea->Count();

			// stat obj, get access to texture RGB data
			if (matInfo.pMat)
			{
				SShaderItem* pShItem = &matInfo.pMat->GetShaderItem();
				int* pLowResSystemCopyAtlasId = 0;
				if (pShItem->m_pShaderResources)
				{
					SEfResTexture* pResTexture = pShItem->m_pShaderResources->GetTexture(EFTT_DIFFUSE);
					if (pResTexture)
					{
						ITexture* pITex = pResTexture->m_Sampler.m_pITex;
						if (pITex)
						{
							{
								AUTO_MODIFYLOCK(CVoxelSegment::m_arrLockedTextures.m_Lock);
								CVoxelSegment::m_arrLockedTextures[pITex] = pITex;
							}
							matInfo.pTexRgb = (ColorB*)pITex->GetLowResSystemCopy(matInfo.textureWidth, matInfo.textureHeight, &pLowResSystemCopyAtlasId);
						}
					}
				}
			}

			m_pMatsInArea->Add(matInfo);
		}
		tr.materialID = matId;

		m_pTrisInArea->Add(tr);
	}

	m_pVertInArea->AddList(*smIn.m_pVertInArea);

	if (vertexOffset == SVO_OFFSET_TERRAIN)
	{
		AddSuperMesh(smIn, -1.f);
	}

	smIn.Clear(nullptr);
}

SSuperMesh::SSuperMesh()
{
	ZeroStruct(*this);
}

SSuperMesh::~SSuperMesh()
{
	if (!m_bExternalData)
	{
		SAFE_DELETE(m_pTrisInArea);
		SAFE_DELETE(m_pVertInArea);
		SAFE_DELETE(m_pMatsInArea);
		SAFE_DELETE(m_pFaceNormals);
	}

	m_bExternalData = false;
	m_boxTris.Reset();
}

void SSuperMesh::Clear(PodArray<SMINDEX>* parrVertHash)
{
	if (!m_bExternalData && m_pTrisInArea)
	{
		m_pTrisInArea->Clear();
		m_pVertInArea->Clear();
		m_pMatsInArea->Clear();
		m_pFaceNormals->Clear();
	}

	if (parrVertHash)
		for (int i = 0; i < hashDim * hashDim * hashDim; i++)
			parrVertHash[i].Clear();

	m_bExternalData = false;
	m_boxTris.Reset();
}

void CVoxelSegment::SaveVoxels(PodArray<byte>& arrData)
{
	int texDataSize = m_voxData.pData[SVoxBrick::OPA3D] ? (m_vCropTexSize.x * m_vCropTexSize.y * m_vCropTexSize.z * sizeof(ColorB)) : 0;

	if (SVO_PACK_TO_16_BIT && texDataSize)
	{
		texDataSize /= 2;
	}

	if (gSvoEnv->m_voxTexFormat == eTF_BC3)
	{
		Vec3i vDxtDim = GetDxtDim();
		texDataSize = (vDxtDim.x * vDxtDim.y * vDxtDim.z * sizeof(ColorB)) / 4;
	}

	int dataSize = texDataSize * m_objLayerMap.size() + m_objLayerMap.size() * sizeof(uint32) + sizeof(SVoxSegmentFileHeader);

	byte* pDataStart = new byte[dataSize];
	byte* pDataPtr = pDataStart;

	// store header
	SVoxSegmentFileHeader* pHeader = (SVoxSegmentFileHeader*)pDataStart;

	pHeader->cropTexSize.x = m_vCropTexSize.x;
	pHeader->cropTexSize.y = m_vCropTexSize.y;
	pHeader->cropTexSize.z = m_vCropTexSize.z;

	pHeader->cropBoxMin.x = m_vCropBoxMin.x;
	pHeader->cropBoxMin.y = m_vCropBoxMin.y;
	pHeader->cropBoxMin.z = m_vCropBoxMin.z;

	pHeader->dummy.zero();

	assert(m_objLayerMap.size() <= 255);
	pHeader->cropTexSize.w = (byte)m_objLayerMap.size();

	assert(GetSubSetsNum() <= 255);
	pHeader->cropBoxMin.w = GetSubSetsNum();

	pDataPtr += sizeof(SVoxSegmentFileHeader);

	for (auto& it : m_objLayerMap)
	{
		ObjectLayerIdType nLayerId = it.first;

		// store layer id's
		*((uint32*)pDataPtr) = nLayerId;
		pDataPtr += sizeof(uint32);

		// store voxel data
		if (texDataSize)
		{
			for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
			{
				if (byte* pDataIn = (byte*)it.second.pData[s])
				{
					if (gSvoEnv->m_voxTexFormat == eTF_BC3)
					{
						CompressToDxt((ColorB*)it.second.pData[s], pDataIn, 0);
					}

					if (SVO_PACK_TO_16_BIT)
					{
						for (int i = 0; i < texDataSize; i++)
						{
							byte val0 = *(pDataIn + 0);
							byte val1 = *(pDataIn + 1);

							val0 = SATURATEB(int(powf(float(val0) / 255.f, 1.f / 2.f) * 255.f));
							val1 = SATURATEB(int(powf(float(val1) / 255.f, 1.f / 2.f) * 255.f));

							byte b0 = val0 >> 4;
							byte b1 = val1 >> 4;

							(*pDataPtr) = b0 | (b1 << 4);

							pDataIn += 2;
							pDataPtr++;
						}
					}
					else
					{
						memcpy(pDataPtr, pDataIn, texDataSize);

						pDataPtr += texDataSize;
					}
				}
			}
		}
	}

	assert(pDataPtr == pDataStart + dataSize);

	// compress entire data block
	if (m_objLayerMap.size() && texDataSize)
	{
		CMemoryBlock* pZipped = CMemoryBlock::CompressToMemBlock(pDataStart, dataSize, GetSystem());  // TODO: try CCryPak::RawCompress
		int zipSize = pZipped->GetSize();

		arrData.AddList((byte*)&zipSize, sizeof(zipSize));
		arrData.AddList((byte*)pZipped->GetData(), zipSize);

		while (arrData.Count() & 3)
		{
			arrData.Add(103);
		}
	}
	else
	{
		int zipSize = 0;

		arrData.AddList((byte*)&zipSize, sizeof(zipSize));
	}

	SAFE_DELETE_ARRAY(pDataStart);
}

PodArray<byte> m_arrSaveCompTexture_Data[16];

void CVoxelSegment::SaveCompTexture(const void* data, size_t size, void* userData)
{
	int64 id = (int64)userData;
	m_arrSaveCompTexture_Data[id].Clear();
	m_arrSaveCompTexture_Data[id].AddList((byte*)data, size);
}

int CVoxelSegment::CompressToDxt(ColorB* pImgSource, byte*& pDxtOut, int threadId)
{
	Vec3i vSizeFin = GetDxtDim();

	static PodArray<ColorB> arrImgResizedPool[16];
	PodArray<ColorB>& arrImgResized = arrImgResizedPool[threadId];

	arrImgResized.CheckAllocated(SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE);
	memset(arrImgResized.GetElements(), 0, arrImgResized.GetDataSize());

	for (int z = 0; z < vSizeFin.z; z++)
	{
		for (int y = 0; y < vSizeFin.y; y++)
		{
			for (int x = 0; x < vSizeFin.x; x++)
			{
				Vec3i vXyzCropped;
				vXyzCropped.x = min(x, m_vCropTexSize.x - 1);
				vXyzCropped.y = min(y, m_vCropTexSize.y - 1);
				vXyzCropped.z = min(z, m_vCropTexSize.z - 1);

				ColorB& rIn = pImgSource[vXyzCropped.z * m_vCropTexSize.x * m_vCropTexSize.y + vXyzCropped.y * m_vCropTexSize.x + vXyzCropped.x];
				ColorB& rOut = arrImgResized[z * vSizeFin.x * vSizeFin.y + y * vSizeFin.x + x];
				rOut = rIn;
			}
		}
	}

	static PodArray<byte> arrImgDxtPool[16];
	PodArray<byte>& arrImgDxt = arrImgDxtPool[threadId];

	arrImgDxt.CheckAllocated(SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE * SVO_VOX_BRICK_MAX_SIZE);
	memset(arrImgDxt.GetElements(), 0, arrImgDxt.GetDataSize());

	for (int lineId = 0; lineId < vSizeFin.z; lineId++)
	{
		ColorB* pLayerIn = arrImgResized.GetElements() + (vSizeFin.x * vSizeFin.y * lineId);

		byte* pLayerOut = arrImgDxt.GetElements() + (vSizeFin.x * vSizeFin.y * lineId);

		GetRenderer()->DXTCompress((byte*)pLayerIn, vSizeFin.x, vSizeFin.y, gSvoEnv->m_voxTexFormat, false, 0, 4, SaveCompTexture);

		memcpy(pLayerOut, m_arrSaveCompTexture_Data[threadId].GetElements(), m_arrSaveCompTexture_Data[threadId].GetDataSize());
	}

	pDxtOut = (byte*)arrImgDxt.GetElements();

	return 0;
}

void CVoxelSegment::CheckAllocateSubSets(SVoxBrick& voxData, int elemsNum, bool bClean)
{
	for (int s = 0; s < SVoxBrick::MAX_NUM; s++)
	{
		if (s < GetSubSetsNum())
		{
			CheckAllocateBrick(voxData.pData[s], elemsNum, bClean);
		}
	}
}

int CVoxelSegment::GetSubSetsNum()
{
	// Stays 3 with mesh ray tracing on: the sub set count is written into the .crysvo header and
	// asserted on load, so allocating a fourth sub set would invalidate every exported SVO file.
	return GetCVars()->e_svoTI_IntegrationMode ? SVoxBrick::RTRIS : 1;
}

#endif
