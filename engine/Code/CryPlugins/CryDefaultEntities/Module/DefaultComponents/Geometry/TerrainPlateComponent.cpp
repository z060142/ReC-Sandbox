// Copyright 2026 Crytek GmbH / Crytek Group. All rights reserved.
#include "StdAfx.h"
#include "TerrainPlateComponent.h"

#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/IIndexedMesh.h>
#include <Cry3DEngine/IMaterial.h>   // MTL_FLAG_NODRAW, the buried subset marker
#include <Cry3DEngine/IStatObj.h>
#include <Cry3DEngine/IRenderNode.h>
#include <Cry3DEngine/ImageExtensionHelper.h>
#include <CryRenderer/IRenderAuxGeom.h>
#include <CryRenderer/IShader.h>    // SShaderItem / SEfResTexture, for the commit albedo projection
#include <CryRenderer/ITexture.h>   // ITexture::GetLowResSystemCopy, ditto
#include <CryRenderer/IImage.h>     // FIM_SRGB_READ - the colour space of that CPU copy
#include <CryRenderer/IRenderer.h>  // IRenderer::EF_LoadImage, to ask the image file for that flag
#include <CrySystem/File/CryFile.h>
#include <CrySystem/IConsole.h>
#include <CrySystem/ConsoleRegistration.h>
#include <CrySystem/ITimer.h>
#include <CryString/CryPath.h>
#include <CryAISystem/IAISystem.h>
#include <CryAISystem/INavigationSystem.h>
#include <CryAISystem/NavigationSystem/INavigationUpdatesManager.h>

#include <algorithm>

namespace Cry
{
namespace DefaultComponents
{

namespace
{
//! Fallback for the deferred update: if ENTITY_EVENT_XFORM_FINISHED_EDITOR never arrives (a transform typed
//! into the panel, or a tool bypassing CObjectMode), flush the held back rebuild after this many quiet frames.
int    g_terrainPlateSettleFrames = 6;
ICVar* g_pTerrainPlateSettleFramesCVar = nullptr;

//! Bake polling: any editor terrain push (sculpt, undo, layer paint) rewrites the sectors it covers and
//! erases the bake there, and the engine has no notification for it.
int    g_terrainPlateBakeProbeFrames = 16;
ICVar* g_pTerrainPlateBakeProbeFramesCVar = nullptr;

//! Bake probing: spacing of the watched cells in terrain units. Nine probes over a large footprint leave tens
//! of metres between them, so a sculpt can sit entirely between two of them and never be noticed.
int    g_terrainPlateBakeProbeSpacing = 4;
ICVar* g_pTerrainPlateBakeProbeSpacingCVar = nullptr;

//! Every live terrain plate component, registered in Initialize and removed in the destructor. Following
//! component add / remove keeps the overlap cascade correct with no level or editor hook.
std::vector<CTerrainPlateComponent*> g_terrainPlates;

//! Greater than zero while an overlap cascade runs: everything it does writes the terrain and would otherwise
//! start another cascade from inside itself, and would trip every other plate's polling.
int  g_bakeCascadeDepth = 0;
//! Frame of the last completed cascade. The polling skips that frame, so N plates that see the terrain move in
//! the same frame produce one cascade between them, not N.
uint32 g_bakeCascadeFrameId = ~0u;
//! Stamped on every bake that lands, so a cascade can restore in the exact inverse of the order the terrain was
//! ACTUALLY laid down in. Bake Priority is not that order; the baselines nest and unwind as a stack.
uint64 g_bakeApplySeqCounter = 0;
//! Cost warning threshold for one cascade, NOT a cap. A plate left out of a cascade - truncated from the
//! closure or pushed into a second chunk - misses both the ordered unwind and the composite refresh, so its
//! probes read the other plates' relief as an external edit and the two halves restore each other for ever.
int    g_terrainPlateBakeMaxBatch = 64;
ICVar* g_pTerrainPlateBakeMaxBatchCVar = nullptr;
//! Latched, so a runaway configuration costs one warning line and not one per cascade. Cleared again as soon
//! as a cascade comes back under the threshold.
bool   g_bakeLargeBatchWarned = false;
//! Frames of silence the polling owes a finished cascade. One is not enough: SetTerrainElevation rebuilds sector
//! meshes, atlases, the physics heightfield and roads, and a probe on our own writes starts a ping pong.
const uint32 kBakeCascadeQuietFrames = 3;

//! The move batch: plates whose move still owes a restore of the OLD footprint and a re-bake at the new one.
//! Each finishing alone would capture baselines out of a half unwound terrain, so ONE ordered cascade wins.
std::vector<CTerrainPlateComponent*> g_bakeMoveBatch;
//! Frame of the last plate that joined; the batch runs on the first frame after that. One gesture lands in one
//! frame: a multi selection sends all its events together, and a group settles all its children at once.
uint32 g_bakeMoveBatchFrameId = 0;

//! Safety valve for the probe repair loop. A repair costs a mesh regeneration and a SetTerrainElevation, so a
//! configuration that repairs itself says so and backs off; the interval grows, it is never silenced.
const int kBakeRunawayWindowFrames = 600;
const int kBakeRunawayLimit = 8;
const int kBakeBackoffMaxFrames = 600;

//! The valve above is per plate and cannot see a CYCLE between plates: in a ring of N plates each one seeds
//! only every Nth repair, and a plate the cascade has just refreshed polls clean and resets its own counter.
//! So the cycle is counted globally instead - automatic repairs since the user last touched any plate - and
//! there is no time window: a cycle is slow (one turn per e_TerrainPlateBakeProbeFrames) but never stops.
const int kBakeAutoRepairRunawayLimit = 64;
int  g_bakeAutoRepairCount = 0;
//! Latched by the valve: no plate repairs itself again until the user does something to a plate. Terrain left
//! exactly as it is, because a half repaired composite is worse than a stale one.
bool g_bakeAutoRepairStalled = false;

//! The terrain is kept this far below the plate mesh so the two surfaces never fight. Nearly coplanar costs
//! shadow map precision and shows as distant acne; e_TerrainPlateBakeEpsilon 0.005 restores the tight bake.
float  g_terrainPlateBakeEpsilon = 0.02f;
ICVar* g_pTerrainPlateBakeEpsilonCVar = nullptr;

//! Slope correction of the bake Min filter, i.e. the curvature guard. The plain Min filter takes the lowest
//! plate height over the half unit halo around a node, sinking it ~0.5 * unitSize * |g| below the plate for no
//! reason: the terrain tilts between its nodes as the plate does. Subtracting the plate's own tangent plane
//! first leaves only the deviation from the plane. Escape hatch: e_TerrainPlateBakeSlopeCorrect 0.
int    g_terrainPlateBakeSlopeCorrect = 1;
ICVar* g_pTerrainPlateBakeSlopeCorrectCVar = nullptr;

//! Antialias the bake's PLAN VIEW footprint with the coverage the bake already computes. A pointwise inside /
//! outside test at the node makes a plate edge that is not grid aligned bake as a staircase of FULL relief
//! amplitude, which the coarser LODs amplify rather than smooth; weighting by the supersample coverage ratio
//! makes the footprint proportional (CRYENGINE's road falloff is the template). The weight can only be SCALED
//! DOWN, so a cell can never be lifted above the plate. e_TerrainPlateBakeEdgeAA 0 restores the pointwise test.
int    g_terrainPlateBakeEdgeAA = 1;
ICVar* g_pTerrainPlateBakeEdgeAACVar = nullptr;

//! Make the bake's rim curve the same curve the mesh edge weld uses. The weld pulls the ring towards the ground
//! with 1 - smoothstep(d / falloff) while the bake ramped in with a cosine; the two disagree by about 1 % of the
//! relief, which is terrain poking through the plate mesh. Matched, meshZ - bakeZ = (plateZ - bakeZ) * (1 - w)
//! - sink, so the terrain is under the mesh except in the sink sub band - which exists on purpose, so the Weld
//! Sink is NOT mirrored into the bake. e_TerrainPlateBakeWeldCurve 0 restores the cosine.
int    g_terrainPlateBakeWeldCurve = 1;
ICVar* g_pTerrainPlateBakeWeldCurveCVar = nullptr;

//! What pokes through the ground at distance is not "the plate" but its BELOW GROUND regions and its SKIRT:
//! terrain integration rejects them by the height band test so they wear the plate's fallback material, and a
//! sector at LOD >= 1 draws the chord between two heightmap nodes, dipping below the real ground and revealing
//! them. So move every triangle whose three vertices are below the terrain the weld samples into a second
//! MTL_FLAG_NODRAW subset - the engine's idiom for geometry that exists but is not drawn: CRenderMesh and
//! CTerrainNode::AppendTrianglesFromObjects skip it, while CStatObj::PhysicalizeGeomType selects subsets by
//! nPhysicalizeType alone, so the collision trimesh keeps every triangle. Only the index ORDER changes, so the
//! weld ring, UVs and tangent frames survive. e_TerrainPlateCullBuried 0 draws every triangle again.
int    g_terrainPlateCullBuried = 1;
ICVar* g_pTerrainPlateCullBuriedCVar = nullptr;

//! Metres a vertex has to be under the terrain before it counts as buried: half the default Weld Sink (0.1 m),
//! so the deliberately sunk rim reads as buried while float noise cannot flip a surface vertex.
const float kBuriedCullMargin = 0.05f;

//! The plate's contacts report the surface type of the TERRAIN LAYER under them; 0 restores the stock path.
int    g_terrainPlatePhysSurfaceFromTerrain = 1;
ICVar* g_pTerrainPlatePhysSurfaceFromTerrainCVar = nullptr;

//! Frames between two checks that the terrain layers under the plate still match its physics mapping. Editor
//! only, and the check costs kPhysSurfaceProbeGrid^2 terrain rays, so it is slower than the bake probe.
int    g_terrainPlatePhysSurfaceProbeFrames = 32;
ICVar* g_pTerrainPlatePhysSurfaceProbeFramesCVar = nullptr;

//! Probes per axis over the footprint: 25 rays, enough to pick the dominant layer and to notice a repaint.
const int kPhysSurfaceProbeGrid = 5;

//! Hard ceiling on the mapping table: CPhysicalEntity's part stores nMats in an unsigned:7, so 128 entries
//! would wrap to 0 - the same kludge the terrain itself works around in terrain_load.cpp.
const int kPhysSurfaceMaxMappingEntries = 127;

//! Metres above the terrain the surface sampling ray starts, and how far down it travels: enough to clear the
//! plate's relief and the weld sink and to reach back down through both. The ray only tests the heightfield.
const float kPhysSurfaceRayUp = 32.f;
const float kPhysSurfaceRayLength = 128.f;

//! Bake trigger / bake apply tracing. The bake chain has a dozen places where it can legitimately do nothing,
//! and every one of them names itself with a reason code (@see CTerrainPlateComponent::LogBakeSkip).
//! 0 - off
//! 1 - log every bake, restore, probe repair and skip, with a reason code, plus the read back check
//! 2 - additionally draw the sector aligned bake rect as a wire box over the baked terrain
int    g_terrainPlateDebug = 0;
ICVar* g_pTerrainPlateDebugCVar = nullptr;

//! Clamped so a typo in the console cannot invert the guarantee that the terrain stays under the mesh.
float GetBakeEpsilonMeters()
{
	return clamp_tpl(g_terrainPlateBakeEpsilon, 0.f, 1.f);
}
//! Heights are stored as 12 bit per sector: fRange = (fMax - fMin) / 0x0FFF.
const float kTerrainHeightQuantSteps = 4095.f;
//! TERRAIN_DEFORMATION_MAX_DEPTH: the head room SetTerrainElevation reserves below a sector's minimum before
//! it quantises.
const float kTerrainDeformationDepth = 3.f;

//! e_TerrainIntegrateObjectsDebug, the cvar the rest of the terrain integration work logs under.
int GetTerrainIntegrateDebugLevel()
{
	if (gEnv == nullptr || gEnv->pConsole == nullptr)
		return 0;

	ICVar* pCVar = gEnv->pConsole->GetCVar("e_TerrainIntegrateObjectsDebug");
	return (pCVar != nullptr) ? pCVar->GetIVal() : 0;
}

//! The verbosity the plate logs at: the higher of its own e_TerrainPlateDebug and the shared
//! e_TerrainIntegrateObjectsDebug.
int GetPlateDebugLevel()
{
	return max(max(0, g_terrainPlateDebug), GetTerrainIntegrateDebugLevel());
}

//! Cost threshold for one cascade. 0 disables the report; it can never make a cascade smaller.
int GetBakeMaxBatch()
{
	return max(0, g_terrainPlateBakeMaxBatch);
}

//! Anything the user does to a plate re-arms the automatic repairs: the configuration has changed, so the
//! cycle the valve saw may well be gone, and the user is there to see what happens next.
void NoteBakeUserAction()
{
	// Not from inside a cascade: the transforms and teardowns a cascade causes are our own writes, and taking
	// them for user input is exactly how a valve gets reset by the loop it is supposed to stop.
	if (g_bakeCascadeDepth > 0)
		return;

	g_bakeAutoRepairCount = 0;
	g_bakeAutoRepairStalled = false;
}

//! One automatic (probe driven) repair. @see kBakeAutoRepairRunawayLimit for why this is not per plate.
void NoteBakeAutoRepair()
{
	if (g_bakeAutoRepairStalled || ++g_bakeAutoRepairCount <= kBakeAutoRepairRunawayLimit)
		return;

	g_bakeAutoRepairStalled = true;

	CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
	           "Terrain Plate: %d bake repairs have run with nothing being edited, which means the baking plates are "
	           "rewriting the ground under each other in a cycle. All automatic repairs are now stopped and the terrain "
	           "is left exactly as it is; move, edit or delete any baking plate to start them again. Most often this is "
	           "too many overlapping baking plates - moving or deleting more than 16 in one operation is not recommended "
	           "(cost grows with overlap; prefer smaller selections).", g_bakeAutoRepairCount);
}
}

void RegisterTerrainPlateCVars()
{
	if (gEnv == nullptr || gEnv->pConsole == nullptr || g_pTerrainPlateSettleFramesCVar != nullptr)
		return;

	g_pTerrainPlateSettleFramesCVar = REGISTER_CVAR2("e_TerrainPlateSettleFrames", &g_terrainPlateSettleFrames, 6, VF_NULL,
	                                                 "Terrain Plate component with Update During Drag = false: number of frames without a\n"
	                                                 "transform event after which the held back rebuild (weld, physics mesh, shadow cache)\n"
	                                                 "runs anyway. This is only a fallback - the normal trigger is the editor's mouse up\n"
	                                                 "event ENTITY_EVENT_XFORM_FINISHED_EDITOR.\n"
	                                                 "0 - no fallback, only the mouse up event flushes");

	g_pTerrainPlateBakeProbeFramesCVar = REGISTER_CVAR2("e_TerrainPlateBakeProbeFrames", &g_terrainPlateBakeProbeFrames, 16, VF_NULL,
	                                                    "Terrain Plate component with Bake Into Terrain = true: number of frames between two\n"
	                                                    "checks that the bake is still in the terrain. Any editor terrain operation (sculpt,\n"
	                                                    "undo, layer paint, leaving game mode) re-pushes the editor heightmap over the sectors\n"
	                                                    "it touches and erases the bake there, and the engine sends no notification for it, so\n"
	                                                    "the plate watches nine cells of its own footprint and re-applies itself when they move.\n"
	                                                    "0 - no polling, the bake stays erased until the plate is edited");

	g_pTerrainPlateBakeProbeSpacingCVar = REGISTER_CVAR2("e_TerrainPlateBakeProbeSpacing", &g_terrainPlateBakeProbeSpacing, 4, VF_NULL,
	                                                     "Terrain Plate component with Bake Into Terrain = true: distance in terrain units between\n"
	                                                     "two watched cells of the bake. The probes are also capped to one per touched terrain\n"
	                                                     "sector and to 33 per axis. Smaller catches a smaller sculpt in the middle of a large\n"
	                                                     "plate; the cost is one array lookup per probe per poll.");

	g_pTerrainPlateBakeEpsilonCVar = REGISTER_CVAR2("e_TerrainPlateBakeEpsilon", &g_terrainPlateBakeEpsilon, 0.02f, VF_NULL,
	                                                "Terrain Plate component with Bake Into Terrain = true: how far in metres the baked\n"
	                                                "terrain is kept below the plate mesh. A road no longer needs this to be tight: since\n"
	                                                "e_RoadsFollowIntegratedObjects it drapes on the plate mesh instead of on the baked\n"
	                                                "terrain. A tight value leaves the two surfaces nearly coplanar, which costs shadow map\n"
	                                                "precision and shows as shadow acne at the plate / terrain junction seen from far away.\n"
	                                                "0.02 - default, the two surfaces are clearly apart\n"
	                                                "0.005 - tight bake, for a road forced onto the terrain surface\n"
	                                                "        (e_RoadsFollowIntegratedObjects 0); expect distant junction shadow acne");

	g_pTerrainPlateBakeSlopeCorrectCVar = REGISTER_CVAR2("e_TerrainPlateBakeSlopeCorrect", &g_terrainPlateBakeSlopeCorrect, 1, VF_NULL,
	                                                     "Terrain Plate component with Bake Into Terrain = true: subtract the plate local tangent\n"
	                                                     "plane before the bake Min filter takes the lowest plate height over a terrain cell, so the\n"
	                                                     "filter only ever gives ground back for real curvature. Without it the baked terrain sits\n"
	                                                     "half a terrain cell of slope below the plate - visibly lower, and lower still the larger\n"
	                                                     "the plate is scaled.\n"
	                                                     "1 - default: the node lands on the plate mesh minus e_TerrainPlateBakeEpsilon, whatever\n"
	                                                     "    the slope and the scale, and a dip between two terrain nodes still pulls it down\n"
	                                                     "0 - plain Min filter: conservative (it can never leave terrain above the plate) but it\n"
	                                                     "    buries anything drawn on the baked terrain surface on a slope");

	g_pTerrainPlateBakeEdgeAACVar = REGISTER_CVAR2("e_TerrainPlateBakeEdgeAA", &g_terrainPlateBakeEdgeAA, 1, VF_NULL,
	                                               "Terrain Plate component with Bake Into Terrain = true: weight the baked relief of every\n"
	                                               "terrain cell by how much of that cell the plate actually covers in plan, instead of\n"
	                                               "deciding inside / outside at the cell node only. Without it a straight plate edge that is\n"
	                                               "not aligned to the heightmap grid bakes as a one cell staircase carrying the FULL relief,\n"
	                                               "which the coarser terrain LODs amplify rather than smooth.\n"
	                                               "1 - default: the footprint is coverage, so the edge bakes as a one cell ramp that stays\n"
	                                               "    straight in plan. Forces at least 4 x 4 bake samples per cell for the estimate\n"
	                                               "0 - the pointwise test, bit for bit as before. Both settings need a rebake, which the\n"
	                                               "    plate performs by itself when this cvar changes");

	g_pTerrainPlateBakeWeldCurveCVar = REGISTER_CVAR2("e_TerrainPlateBakeWeldCurve", &g_terrainPlateBakeWeldCurve, 1, VF_NULL,
	                                                  "Terrain Plate component with Bake Into Terrain = true: ramp the baked relief in over the\n"
	                                                  "rim band with the same smoothstep the mesh edge weld uses, instead of a cosine. Two\n"
	                                                  "different S curves over one band disagree by up to about 1 % of the relief, which is the\n"
	                                                  "terrain poking through the plate mesh in patches inside the rim band once the LOD-0\n"
	                                                  "appended copy stops covering it.\n"
	                                                  "1 - default: bake and weld follow the same profile; the terrain stays under the mesh\n"
	                                                  "    across the band, except inside the Weld Sink sub band where the border is buried\n"
	                                                  "    on purpose\n"
	                                                  "0 - the cosine falloff, as before");

	g_pTerrainPlateCullBuriedCVar = REGISTER_CVAR2("e_TerrainPlateCullBuried", &g_terrainPlateCullBuried, 1, VF_NULL,
	                                              "Terrain Plate component: leave the plate's buried triangles out of the DRAWN geometry. A\n"
	                                              "triangle is buried when all three of its vertices are more than 5 cm below the terrain\n"
	                                              "surface under them - the surface regions of a partially sunken plate and, with the default\n"
	                                              "Weld Sink, the whole skirt. Those parts are rejected by the terrain integration and so\n"
	                                              "wear the plate's own fallback material; they are meant to be hidden by the ground, and near\n"
	                                              "the camera they are, but a terrain sector at LOD 1 or coarser draws the chord between two\n"
	                                              "heightmap nodes and dips locally below the real ground, which is what reveals them as a\n"
	                                              "scalloped band at distance.\n"
	                                              "Collision is NOT affected: the buried triangles stay in the physics trimesh, they are only\n"
	                                              "moved into a MTL_FLAG_NODRAW subset. Re-evaluated on every mesh rebuild.\n"
	                                              "1 - default\n"
	                                              "0 - stock behaviour: every triangle is drawn");

	// The classification is baked into the index buffer, so the cvar can only take effect by rebuilding every
	// plate's mesh; it is not a render flag that could simply be re-written.
	if (g_pTerrainPlateCullBuriedCVar != nullptr)
	{
		g_pTerrainPlateCullBuriedCVar->AddOnChange(&CTerrainPlateComponent::OnCullBuriedCVarChanged);
	}

	g_pTerrainPlatePhysSurfaceFromTerrainCVar = REGISTER_CVAR2("e_TerrainPlatePhysSurfaceFromTerrain", &g_terrainPlatePhysSurfaceFromTerrain, 1, VF_NULL,
	                                                          "Terrain Plate component: take the surface types the plate's COLLISION reports - footstep sounds,\n"
	                                                          "bullet impact effects and decals, particles, friction - from the terrain layers painted under the\n"
	                                                          "plate instead of from the plate's single material. The ids are sampled with downward terrain rays\n"
	                                                          "when the plate is rebuilt, and re-sampled a moment after a layer under it is repainted.\n"
	                                                          "This is the global kill switch; each plate also has a Physics Surface From Terrain tick box.\n"
	                                                          "1 - default\n"
	                                                          "0 - stock behaviour, bit for bit: the whole plate reports its own material's surface type");

	// The per face ids are baked into the physics trimesh, so this cvar too only takes effect on a rebuild.
	if (g_pTerrainPlatePhysSurfaceFromTerrainCVar != nullptr)
	{
		g_pTerrainPlatePhysSurfaceFromTerrainCVar->AddOnChange(&CTerrainPlateComponent::OnPhysSurfaceCVarChanged);
	}

	g_pTerrainPlatePhysSurfaceProbeFramesCVar = REGISTER_CVAR2("e_TerrainPlatePhysSurfaceProbeFrames", &g_terrainPlatePhysSurfaceProbeFrames, 32, VF_NULL,
	                                                           "Terrain Plate component with Physics Surface From Terrain on: number of frames between two checks\n"
	                                                           "that the terrain layers under the plate are still the ones its collision was built from. Nothing in\n"
	                                                           "the engine announces a layer repaint, so the plate samples 5 x 5 cells of its own footprint and\n"
	                                                           "re-applies itself when they change. Editor only - terrain cannot be repainted at run time.\n"
	                                                           "0 - no polling, a repaint only takes effect when the plate is next edited or the level reloaded");

	g_pTerrainPlateBakeMaxBatchCVar = REGISTER_CVAR2("e_TerrainPlateBakeMaxBatch", &g_terrainPlateBakeMaxBatch, 64, VF_NULL,
	                                                 "Terrain Plate component with Bake Into Terrain = true: number of plates in one restore and\n"
	                                                 "re-bake pass above which the cost of that pass is reported. This is a WARNING THRESHOLD,\n"
	                                                 "NOT a cap: the pass always covers the complete set of plates that share ground, because a\n"
	                                                 "plate left out of it keeps a stale bake and then fights the plates inside the pass over the\n"
	                                                 "shared ground for ever. Moving or deleting more than 16 baking plates in one operation is\n"
	                                                 "not recommended (cost grows with overlap; prefer smaller selections).\n"
	                                                 "0 - never report, whatever the pass costs");

	g_pTerrainPlateDebugCVar = REGISTER_CVAR2("e_TerrainPlateDebug", &g_terrainPlateDebug, 0, VF_NULL,
	                                          "Terrain Plate component: trace the bake trigger and the bake itself.\n"
	                                          "0 - off\n"
	                                          "1 - one line per bake, restore, teardown, probe repair and skip. A skip always names\n"
	                                          "    a reason code: bake-off, torn-down, waiting-for-terrain, no-3dengine, no-terrain,\n"
	                                          "    rect-empty, footprint-too-large, write-no-terrain, write-empty-rect,\n"
	                                          "    write-size-mismatch, write-off-terrain. A bake also logs the read back of the\n"
	                                          "    cells it changed.\n"
	                                          "2 - additionally draw the sector aligned bake rect as a wire box over the terrain");
}

void UnregisterTerrainPlateCVars()
{
	if (gEnv != nullptr && gEnv->pConsole != nullptr)
	{
		if (g_pTerrainPlateSettleFramesCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateSettleFramesCVar->GetName());

		if (g_pTerrainPlateBakeProbeFramesCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeProbeFramesCVar->GetName());

		if (g_pTerrainPlateBakeProbeSpacingCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeProbeSpacingCVar->GetName());

		if (g_pTerrainPlateBakeEpsilonCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeEpsilonCVar->GetName());

		if (g_pTerrainPlateBakeSlopeCorrectCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeSlopeCorrectCVar->GetName());

		if (g_pTerrainPlateBakeEdgeAACVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeEdgeAACVar->GetName());

		if (g_pTerrainPlateBakeWeldCurveCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeWeldCurveCVar->GetName());

		if (g_pTerrainPlateCullBuriedCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateCullBuriedCVar->GetName());

		if (g_pTerrainPlatePhysSurfaceFromTerrainCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlatePhysSurfaceFromTerrainCVar->GetName());

		if (g_pTerrainPlatePhysSurfaceProbeFramesCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlatePhysSurfaceProbeFramesCVar->GetName());

		if (g_pTerrainPlateBakeMaxBatchCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateBakeMaxBatchCVar->GetName());

		if (g_pTerrainPlateDebugCVar != nullptr)
			gEnv->pConsole->UnregisterVariable(g_pTerrainPlateDebugCVar->GetName());
	}

	g_pTerrainPlateSettleFramesCVar = nullptr;
	g_pTerrainPlateBakeProbeFramesCVar = nullptr;
	g_pTerrainPlateBakeProbeSpacingCVar = nullptr;
	g_pTerrainPlateBakeEpsilonCVar = nullptr;
	g_pTerrainPlateBakeSlopeCorrectCVar = nullptr;
	g_pTerrainPlateBakeEdgeAACVar = nullptr;
	g_pTerrainPlateBakeWeldCurveCVar = nullptr;
	g_pTerrainPlateCullBuriedCVar = nullptr;
	g_pTerrainPlatePhysSurfaceFromTerrainCVar = nullptr;
	g_pTerrainPlatePhysSurfaceProbeFramesCVar = nullptr;
	g_pTerrainPlateBakeMaxBatchCVar = nullptr;
	g_pTerrainPlateDebugCVar = nullptr;
}

int CTerrainPlateComponent::GetSettleFrames()
{
	return max(0, g_terrainPlateSettleFrames);
}

bool CTerrainPlateComponent::WantsBakeMoveBatch() const
{
	// A plate that does not write the terrain has no footprint to unwind and no ordering to respect; a torn
	// down plate must not bake again at all.
	return m_bBakeIntoTerrain && !m_bBakeTornDown && m_pEntity != nullptr;
}

int CTerrainPlateComponent::GetEffectiveSettleFrames() const
{
	if (WantsBakeMoveBatch())
	{
		// A baking plate can never take the immediate path: its old footprint is still in the ground and must be
		// restored with every other plate of the same gesture, so the deferral is mandatory whatever the cvars say.
		return m_bUpdateDuringDrag ? 1 : max(1, GetSettleFrames());
	}

	return GetSettleFrames();
}

int CTerrainPlateComponent::GetBakeProbeFrames()
{
	return max(0, g_terrainPlateBakeProbeFrames);
}

const char* CTerrainPlateComponent::GetPlateName() const
{
	return (m_pEntity != nullptr) ? m_pEntity->GetName() : "<no entity>";
}

void CTerrainPlateComponent::LogBakeSkip(const char* szReason) const
{
	if (GetPlateDebugLevel() >= 1)
	{
		CryLog("TerrainPlate '%s': bake did nothing [%s]", GetPlateName(), szReason);
	}
}

float STerrainPlateHeightField::SampleBilinear(float u, float v) const
{
	if (!IsValid())
		return 0.f;

	const float fx = clamp_tpl(u, 0.f, 1.f) * (float)(m_width - 1);
	const float fy = clamp_tpl(v, 0.f, 1.f) * (float)(m_height - 1);

	const int x0 = (int)floorf(fx);
	const int y0 = (int)floorf(fy);
	const float tx = fx - (float)x0;
	const float ty = fy - (float)y0;

	const float h00 = At(x0, y0);
	const float h10 = At(x0 + 1, y0);
	const float h01 = At(x0, y0 + 1);
	const float h11 = At(x0 + 1, y0 + 1);

	return (h00 * (1.f - tx) + h10 * tx) * (1.f - ty) + (h01 * (1.f - tx) + h11 * tx) * ty;
}

// CPU loaders

namespace TerrainPlateLoaderDetail
{

//! Reads a whole file through ICryPak (pak aware, release safe). Never CImageEx: it is editor only and 8 bit
//! per channel, which is exactly the precision trap CHeightmap::LoadRAW avoids by hand.
static bool ReadWholeFile(const char* szPath, std::vector<uint8>& out)
{
	CCryFile file;
	if (!file.Open(szPath, "rb"))
		return false;

	const size_t size = file.GetLength();
	if (size == 0)
		return false;

	out.resize(size);
	return file.ReadRaw(out.data(), size) == size;
}

static float HalfToFloat(uint16 h)
{
	const uint32 sign = (uint32)(h >> 15) & 1u;
	const uint32 exponent = (uint32)(h >> 10) & 0x1Fu;
	const uint32 mantissa = (uint32)h & 0x3FFu;

	uint32 bits;
	if (exponent == 0)
	{
		if (mantissa == 0)
		{
			bits = sign << 31;
		}
		else
		{
			// Subnormal half: normalise it into a float.
			uint32 e = 0;
			uint32 m = mantissa;
			while ((m & 0x400u) == 0)
			{
				m <<= 1;
				++e;
			}
			m &= 0x3FFu;
			bits = (sign << 31) | ((127 - 15 - e + 1) << 23) | (m << 13);
		}
	}
	else if (exponent == 31)
	{
		bits = (sign << 31) | (255u << 23) | (mantissa << 13);
	}
	else
	{
		bits = (sign << 31) | ((exponent + 127 - 15) << 23) | (mantissa << 13);
	}

	float result;
	memcpy(&result, &bits, sizeof(result));
	return result;
}

static uint16 ReadU16LE(const uint8* p) { return (uint16)((uint16)p[0] | ((uint16)p[1] << 8)); }
static uint16 ReadU16BE(const uint8* p) { return (uint16)(((uint16)p[0] << 8) | (uint16)p[1]); }
static uint32 ReadU32LE(const uint8* p) { return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24); }

static float LuminanceFromRGB(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

//! Headerless 16 bit little endian, square. Byte for byte what CHeightmap::LoadRAW reads, except that the side
//! length is inferred from the file size instead of taken from the level.
static bool LoadRaw16(const std::vector<uint8>& data, const char* szPath, STerrainPlateHeightField& out)
{
	if ((data.size() & 1) != 0)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' has an odd byte count, it is not a 16 bit raw heightmap", szPath);
		return false;
	}

	const size_t sampleCount = data.size() / 2;
	const int side = (int)(sqrt((double)sampleCount) + 0.5);
	if ((size_t)side * (size_t)side != sampleCount || side < 2)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' holds %d 16 bit samples, which is not a square heightmap", szPath, (int)sampleCount);
		return false;
	}

	out.m_width = side;
	out.m_height = side;
	out.m_values.resize(sampleCount);

	for (size_t i = 0; i < sampleCount; ++i)
	{
		out.m_values[i] = (float)ReadU16LE(&data[i * 2]) * (1.f / 65535.f);
	}

	return true;
}

//! Binary PGM (P5). Values above 255 are big endian, as the format specifies.
static bool LoadPgm(const std::vector<uint8>& data, const char* szPath, STerrainPlateHeightField& out)
{
	size_t pos = 0;
	const size_t size = data.size();

	auto skipWhitespaceAndComments = [&]()
	{
		while (pos < size)
		{
			const uint8 c = data[pos];
			if (c == '#')
			{
				while (pos < size && data[pos] != '\n')
					++pos;
			}
			else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			{
				++pos;
			}
			else
			{
				break;
			}
		}
	};

	auto readInt = [&](int& value) -> bool
	{
		skipWhitespaceAndComments();
		if (pos >= size || data[pos] < '0' || data[pos] > '9')
			return false;
		value = 0;
		while (pos < size && data[pos] >= '0' && data[pos] <= '9')
		{
			value = value * 10 + (data[pos] - '0');
			++pos;
		}
		return true;
	};

	if (size < 2 || data[0] != 'P' || data[1] != '5')
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is not a binary PGM (magic P5 expected; ASCII P2 is not supported)", szPath);
		return false;
	}
	pos = 2;

	int width = 0, height = 0, maxValue = 0;
	if (!readInt(width) || !readInt(height) || !readInt(maxValue) || width < 2 || height < 2 || maxValue <= 0)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' has a malformed PGM header", szPath);
		return false;
	}

	// Exactly one whitespace character separates the header from the raster.
	if (pos < size)
		++pos;

	const int bytesPerSample = (maxValue > 255) ? 2 : 1;
	const size_t needed = (size_t)width * (size_t)height * (size_t)bytesPerSample;
	if (size - pos < needed)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is a truncated PGM (%d bytes of raster missing)", szPath, (int)(needed - (size - pos)));
		return false;
	}

	out.m_width = width;
	out.m_height = height;
	out.m_values.resize((size_t)width * (size_t)height);

	const float scale = 1.f / (float)maxValue;
	for (size_t i = 0; i < out.m_values.size(); ++i)
	{
		const uint8* p = &data[pos + i * bytesPerSample];
		const int value = (bytesPerSample == 2) ? (int)ReadU16BE(p) : (int)*p;
		out.m_values[i] = clamp_tpl((float)value * scale, 0.f, 1.f);
	}

	return true;
}

//! Uncompressed BMP, 8 bit palettised or 24/32 bit. BMP rows are bottom up unless the height is
//! negative, and the height field wants row 0 on top, so the rows are flipped here.
static bool LoadBmp(const std::vector<uint8>& data, const char* szPath, STerrainPlateHeightField& out)
{
	if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is not a BMP file", szPath);
		return false;
	}

	const uint32 dataOffset = ReadU32LE(&data[10]);
	const uint32 infoSize = ReadU32LE(&data[14]);
	const int width = (int)ReadU32LE(&data[18]);
	const int signedHeight = (int)ReadU32LE(&data[22]);
	const int bitCount = (int)ReadU16LE(&data[28]);
	const uint32 compression = ReadU32LE(&data[30]);

	const bool bTopDown = (signedHeight < 0);
	const int height = bTopDown ? -signedHeight : signedHeight;

	if (infoSize < 40 || compression != 0 || width < 2 || height < 2)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is a compressed or unsupported BMP (only uncompressed BI_RGB is read)", szPath);
		return false;
	}

	if (bitCount != 8 && bitCount != 24 && bitCount != 32)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is a %d bit BMP; only 8, 24 and 32 bit are read", szPath, bitCount);
		return false;
	}

	const size_t rowStride = (((size_t)width * bitCount / 8) + 3) & ~(size_t)3;
	if (data.size() < dataOffset + rowStride * height)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is a truncated BMP", szPath);
		return false;
	}

	// 8 bit BMPs are palettised; the palette sits right behind the info header as BGRA quads.
	float palette[256];
	if (bitCount == 8)
	{
		const size_t paletteOffset = 14 + infoSize;
		if (data.size() < paletteOffset + 256 * 4)
		{
			CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is an 8 bit BMP without a full palette", szPath);
			return false;
		}
		for (int i = 0; i < 256; ++i)
		{
			const uint8* p = &data[paletteOffset + (size_t)i * 4];
			palette[i] = LuminanceFromRGB((float)p[2], (float)p[1], (float)p[0]) * (1.f / 255.f);
		}
	}

	out.m_width = width;
	out.m_height = height;
	out.m_values.resize((size_t)width * (size_t)height);

	for (int y = 0; y < height; ++y)
	{
		const int srcRow = bTopDown ? y : (height - 1 - y);
		const uint8* pRow = &data[dataOffset + (size_t)srcRow * rowStride];
		float* pDst = &out.m_values[(size_t)y * width];

		for (int x = 0; x < width; ++x)
		{
			if (bitCount == 8)
			{
				pDst[x] = palette[pRow[x]];
			}
			else
			{
				const uint8* p = pRow + (size_t)x * (bitCount / 8);
				pDst[x] = LuminanceFromRGB((float)p[2], (float)p[1], (float)p[0]) * (1.f / 255.f);
			}
		}
	}

	return true;
}

//! Bytes per pixel of the uncompressed DDS formats this loader understands, 0 for everything else.
static int GetSupportedDDSPixelSize(ETEX_Format format)
{
	switch (format)
	{
	case eTF_R8:
	case eTF_A8:
	case eTF_L8:
		return 1;
	case eTF_R16:
	case eTF_R16F:
		return 2;
	case eTF_R32F:
	case eTF_R8G8B8A8:
	case eTF_B8G8R8A8:
		return 4;
	default:
		return 0;
	}
}

static float DecodeDDSPixel(const uint8* p, ETEX_Format format)
{
	switch (format)
	{
	case eTF_R8:
	case eTF_A8:
	case eTF_L8:
		return (float)p[0] * (1.f / 255.f);
	case eTF_R16:
		return (float)ReadU16LE(p) * (1.f / 65535.f);
	case eTF_R16F:
		return HalfToFloat(ReadU16LE(p));
	case eTF_R32F:
		{
			float value;
			memcpy(&value, p, sizeof(value));
			return value;
		}
	case eTF_R8G8B8A8:
		return LuminanceFromRGB((float)p[0], (float)p[1], (float)p[2]) * (1.f / 255.f);
	case eTF_B8G8R8A8:
		return LuminanceFromRGB((float)p[2], (float)p[1], (float)p[0]) * (1.f / 255.f);
	default:
		return 0.f;
	}
}

} // namespace TerrainPlateLoaderDetail

namespace TerrainPlateLoader
{

bool LoadHeightmapFile(const char* szPath, STerrainPlateHeightField& out)
{
	using namespace TerrainPlateLoaderDetail;

	out.Clear();

	if (szPath == nullptr || szPath[0] == '\0')
		return false;

	std::vector<uint8> data;
	if (!ReadWholeFile(szPath, data))
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: cannot open heightmap '%s'", szPath);
		return false;
	}

	const char* szExt = PathUtil::GetExt(szPath);
	bool bLoaded = false;

	if (!stricmp(szExt, "r16") || !stricmp(szExt, "raw"))
	{
		bLoaded = LoadRaw16(data, szPath, out);
	}
	else if (!stricmp(szExt, "pgm"))
	{
		bLoaded = LoadPgm(data, szPath, out);
	}
	else if (!stricmp(szExt, "bmp"))
	{
		bLoaded = LoadBmp(data, szPath, out);
	}
	else
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' has an unsupported extension (r16, raw, pgm and bmp are read)", szPath);
	}

	if (!bLoaded)
		out.Clear();

	return bLoaded;
}

bool LoadDisplacementTexture(const char* szPath, STerrainPlateHeightField& out)
{
	using namespace TerrainPlateLoaderDetail;

	out.Clear();

	if (szPath == nullptr || szPath[0] == '\0')
		return false;

	// The texture picker hands out the source asset path; the readable data always sits in the DDS.
	string ddsPath = szPath;
	if (stricmp(PathUtil::GetExt(ddsPath.c_str()), "dds") != 0)
	{
		const string replaced = PathUtil::ReplaceExtension(ddsPath, "dds");
		ddsPath = gEnv->pCryPak->IsFileExist(replaced.c_str()) ? replaced : (ddsPath + ".dds");
	}

	std::vector<uint8> data;
	if (!ReadWholeFile(ddsPath.c_str(), data))
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: cannot open displacement texture '%s'", ddsPath.c_str());
		return false;
	}

	if (data.size() < sizeof(CImageExtensionHelper::DDS_FILE_DESC))
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is too small to be a DDS", ddsPath.c_str());
		return false;
	}

	CImageExtensionHelper::DDS_FILE_DESC desc;
	memcpy(&desc, data.data(), sizeof(desc));

	if (!desc.IsValid())
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is not a DDS file", ddsPath.c_str());
		return false;
	}

	uint32 dxgiFormat = 0;
	if (desc.header.IsDX10Ext())
	{
		if (data.size() < sizeof(desc) + sizeof(CImageExtensionHelper::DDS_HEADER_DXT10))
		{
			CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' has a truncated DX10 header", ddsPath.c_str());
			return false;
		}
		CImageExtensionHelper::DDS_HEADER_DXT10 dx10;
		memcpy(&dx10, data.data() + sizeof(desc), sizeof(dx10));
		dxgiFormat = dx10.dxgiFormat;
	}

	const ETEX_Format format = DDSFormats::GetFormatByDesc(desc.header.ddspf, dxgiFormat);
	const int pixelSize = GetSupportedDDSPixelSize(format);
	if (pixelSize == 0)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
		           "Terrain Plate: '%s' is in a format the CPU loader cannot read (%s). Use an uncompressed R8, R16, R16F, R32F or RGBA8 texture; the plate stays flat.",
		           ddsPath.c_str(), CImageExtensionHelper::NameForTextureFormat(format));
		return false;
	}

	const int width = (int)desc.header.dwWidth;
	const int height = (int)desc.header.dwHeight;
	if (width < 2 || height < 2)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' is only %dx%d", ddsPath.c_str(), width, height);
		return false;
	}

	const size_t mip0Size = (size_t)width * (size_t)height * (size_t)pixelSize;
	const size_t headerSize = desc.GetFullHeaderSize();

	const uint8* pMip0 = nullptr;
	std::vector<uint8> chunkData;

	if (data.size() >= headerSize + mip0Size)
	{
		pMip0 = data.data() + headerSize;
	}
	else
	{
			// The engine splits streamed textures: the .dds only holds the persistent mips and the largest mip lives
			// in the highest numbered .dds.<n> chunk file. Take the first chunk that is large enough for mip 0.
		for (int chunk = 15; chunk >= 1 && pMip0 == nullptr; --chunk)
		{
			string chunkPath;
			chunkPath.Format("%s.%d", ddsPath.c_str(), chunk);
			if (!gEnv->pCryPak->IsFileExist(chunkPath.c_str()))
				continue;
			if (ReadWholeFile(chunkPath.c_str(), chunkData) && chunkData.size() >= mip0Size)
			{
				pMip0 = chunkData.data();
			}
		}
	}

	if (pMip0 == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING, "Terrain Plate: '%s' does not contain its top mip level; the plate stays flat", ddsPath.c_str());
		return false;
	}

	out.m_width = width;
	out.m_height = height;
	out.m_values.resize((size_t)width * (size_t)height);

	for (size_t i = 0; i < out.m_values.size(); ++i)
	{
		out.m_values[i] = clamp_tpl(DecodeDDSPixel(pMip0 + i * pixelSize, format), 0.f, 1.f);
	}

	return true;
}

} // namespace TerrainPlateLoader

// Component

// Panel groups. Each draws members of the component it belongs to inside one collapsible row; the members keep
// their own AddMember and archive key, and the isEdit() early out keeps every non panel archive out of them.

void STerrainPlateShapeGroup::Serialize(Serialization::IArchive& archive)
{
	if (!archive.isEdit() || m_pOwner == nullptr)
		return;

	CTerrainPlateComponent& plate = *m_pOwner;
	archive(plate.m_heightmapPath, "HeightmapFile", "Heightmap File");
	archive(plate.m_bNormalize, "Normalize", "Normalize");
	archive(plate.m_gridMode, "GridMode", "Grid Mode");

	// Only the row the current Grid Mode actually reads. The other keeps its value: nothing writes a member
	// whose row was not emitted, and both are persisted by their own AddMember anyway.
	if (plate.m_gridMode == ETerrainPlateGridMode::Fixed)
	{
		archive(plate.m_grid, "Grid", "Grid");
	}
	else
	{
		archive(plate.m_vertexSpacing, "VertexSpacing", "Vertex Spacing");
	}

	archive(plate.m_effectiveGridInfo, "EffectiveGrid", "Effective Grid");
	archive(plate.m_bMirrorX, "MirrorX", "Mirror X");
	archive(plate.m_bMirrorY, "MirrorY", "Mirror Y");
	archive(plate.m_bMirrorZ, "MirrorZ", "Mirror Z");
	archive(plate.m_groundLevel, "GroundLevel", "Ground Level");
}

// The Bake drawer. Its master tick box, Bake Into Terrain, is a top level row of its own and is deliberately NOT
// drawn here: the drawer only exists while it is on, so such a row could never be reached to switch it off.
void STerrainPlateBakeGroup::Serialize(Serialization::IArchive& archive)
{
	if (!archive.isEdit() || m_pOwner == nullptr)
		return;

	CTerrainPlateComponent& plate = *m_pOwner;
	archive(plate.m_bakeMode, "BakeMode", "Bake Mode");
	archive(plate.m_bakePriority, "BakePriority", "Bake Priority");
	archive(plate.m_bakeOffset, "BakeOffset", "Bake Offset");
	// An ordinary row, drawn here with its real label; its hidden top level twin carries the archive key and the
	// default. @see STerrainPlateUIGroup for why the row has to be emitted twice.
	archive(plate.m_bLiveTerrainColour, "LiveTerrainColour", "Terrain Colour From Plate");

	// The two stamps, beside the bake items they make permanent. An action button carries no data, so it needs no
	// AddMember: FunctorActionButton's own Serialize is edit only, which is exactly this archive.
	// ar.doc() documents the row just emitted - the only tooltip channel a member with no AddMember has.
	archive(plate.m_stampHeightButton, "StampHeight", "Stamp Height");
	archive.doc("Writes the baked terrain height you see now into the level; the plate keeps working as before. The "
	            "relief becomes ordinary terrain and stays where it is if the plate is moved or deleted. Nothing on "
	            "this panel changes and the picture does not change, so pressing it twice is harmless.");
	archive(plate.m_stampColourButton, "StampColour", "Stamp Colour");
	archive.doc("Writes the plate's terrain colour you see now into the level; the plate keeps working as before. "
	            "The colour becomes part of the level's own terrain colour and stays where it is if the plate is "
	            "moved or deleted. Nothing on this panel changes and the picture does not change, so pressing it "
	            "twice is harmless.");
}

// The Weld drawer. Edge Weld itself is a top level row, same as Bake Into Terrain above.
void STerrainPlateWeldGroup::Serialize(Serialization::IArchive& archive)
{
	if (!archive.isEdit() || m_pOwner == nullptr)
		return;

	CTerrainPlateComponent& plate = *m_pOwner;
	archive(plate.m_weldSink, "WeldSink", "Weld Sink");
	archive(plate.m_weldInnerFalloff, "WeldInnerFalloff", "Weld Inner Falloff");
	archive(plate.m_skirt, "Skirt", "Skirt");
}

// The two conditional drawers: present when their tick box is on, ABSENT (not empty) when it is off. yasli
// resolves a type through the free four argument overload in preference to the member Serialize(), and that
// overload runs before the property tree creates any row and owns its name and label - so returning false
// without touching the archive creates no row at all, the only way to suppress a whole member row.
bool Serialize(Serialization::IArchive& archive, STerrainPlateBakeGroup& value, const char* szName, const char* szLabel)
{
	if (!archive.isEdit() || value.m_pOwner == nullptr || !value.m_pOwner->m_bBakeIntoTerrain)
		return false;

	Serialization::SStruct ser(value);
	return archive(ser, szName, szLabel);
}

bool Serialize(Serialization::IArchive& archive, STerrainPlateWeldGroup& value, const char* szName, const char* szLabel)
{
	if (!archive.isEdit() || value.m_pOwner == nullptr || !value.m_pOwner->m_bEdgeWeld)
		return false;

	Serialization::SStruct ser(value);
	return archive(ser, szName, szLabel);
}

void STerrainPlateEditorGroup::Serialize(Serialization::IArchive& archive)
{
	if (!archive.isEdit() || m_pOwner == nullptr)
		return;

	CTerrainPlateComponent& plate = *m_pOwner;
	archive(plate.m_bUpdateDuringDrag, "UpdateDuringDrag", "Update During Drag");
}

void CTerrainPlateComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	{
		auto pFunction = SCHEMATYC_MAKE_ENV_FUNCTION(&CTerrainPlateComponent::Rebuild, "{2A9C4F17-8D3E-4A55-B0C6-1E7F4D82A9B0}"_cry_guid, "Rebuild");
		pFunction->SetDescription("Reloads the source and rebuilds the plate mesh");
		pFunction->SetFlags({ Schematyc::EEnvFunctionFlags::Member, Schematyc::EEnvFunctionFlags::Construction });
		componentScope.Register(pFunction);
	}
	{
		auto pFunction = SCHEMATYC_MAKE_ENV_FUNCTION(&CTerrainPlateComponent::RegenerateMesh, "{5B0E2D93-71A4-4C8F-8E12-9D3A6C05F7B2}"_cry_guid, "RegenerateMesh");
		pFunction->SetDescription("Rebuilds the plate mesh from the already loaded source");
		pFunction->SetFlags({ Schematyc::EEnvFunctionFlags::Member, Schematyc::EEnvFunctionFlags::Construction });
		componentScope.Register(pFunction);
	}
}

CTerrainPlateComponent::~CTerrainPlateComponent()
{
	// The backstop, not the normal path: by the time a destructor runs the entity is half gone, so the teardown
	// is done at ENTITY_EVENT_DONE / OnShutDown and this catches a component destroyed without either.
	RestoreBakeOnTeardown();

	ReleaseNativePhysics();

	// Nothing in the engine invalidates the static shadow cache on unregister, so a removed plate would keep
	// casting into the cached cascade and HeightMap AO. The node less form, as the editor's display settings use.
	if (m_pStatObj != nullptr && gEnv != nullptr && gEnv->p3DEngine != nullptr)
	{
		gEnv->p3DEngine->OnObjectModified(nullptr, ERF_CASTSHADOWMAPS);
	}
}

void CTerrainPlateComponent::Initialize()
{
	// A plate is ground: it never moves at runtime, whatever its Type is. Without this a Type = Render plate's
	// node is ERF_MOVES_EVERY_FRAME, becomes FOB_DYNAMIC_OBJECT and loses every deferred decal on it.
	m_bForceStaticGeometry = true;

	// The two stamp buttons. The terrain API they end up in is exported from Sandbox.exe, so they reach it the
	// only way a plugin can: by asking the editor to run its own "py" command over the current selection.
	if (gEnv != nullptr && gEnv->IsEditor())
	{
		m_stampHeightButton = Serialization::ActionButton(std::function<void()>([]()
		{
			if (gEnv != nullptr && gEnv->pConsole != nullptr)
				gEnv->pConsole->ExecuteString("py terrain.stamp_plate_height()");
		}));

		m_stampColourButton = Serialization::ActionButton(std::function<void()>([]()
		{
			if (gEnv != nullptr && gEnv->pConsole != nullptr)
				gEnv->pConsole->ExecuteString("py terrain.stamp_plate_colour()");
		}));
	}

	RegisterPlate();

	// The Bake Mode guard compares against this, so a plate that was SAVED with a Ground Level is not read as
	// somebody raising it now. @see ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED.
	m_lastGroundLevel = (float)m_groundLevel;

	Rebuild();
}

void CTerrainPlateComponent::OnShutDown()
{
	// Removing the component from an entity that stays alive never sends ENTITY_EVENT_DONE - this covers it. It
	// also runs ahead of the destructor on the teardown path, while entity, slot and terrain are still whole.
	RestoreBakeOnTeardown();
}

Cry::Entity::EventFlags CTerrainPlateComponent::GetEventMask() const
{
	Cry::Entity::EventFlags bitFlags = CBaseMeshComponent::GetEventMask();

	// The plate's size and relief height live in the entity scale and the mesh is BUILT at that size, so a pure
	// scale change is a full regeneration. XFORM is always subscribed; the handler decides what a change needs.
	bitFlags |= ENTITY_EVENT_XFORM;

	// End of an editor transform gesture (mouse up), sent by CSelectionGroup::ObjectModified from
	// CObjectMode::OnLButtonUp. @see FlushDeferredTransform.
	bitFlags |= ENTITY_EVENT_XFORM_FINISHED_EDITOR;

	// The bake must not run before the level's terrain is in the engine.
	bitFlags |= ENTITY_EVENT_LEVEL_LOADED;

	// Deleting the plate has to give the ground back, and this is the earliest and only unconditional point on
	// the removal path - sent before the delete / defer / hide decision, while everything is still whole.
	bitFlags |= ENTITY_EVENT_DONE;

	// Leaving game mode resurrects an entity deleted in game (CEntity::OnEditorGameModeChanged) and re-sends
	// only this. Without it the plate is torn down for good and bakes while absent from the registry.
	bitFlags |= ENTITY_EVENT_INIT;

	// IEntity::SetMaterial runs CEntityPhysics::OnGlobalEntityMaterialChanged and then sends this, so it is the
	// hook that lets the plate push its own mapping back over the new material's. @see ApplyPhysicsSurfaceMapping.
	bitFlags |= ENTITY_EVENT_MATERIAL;

	// Only ticked while a rebuild is held back, the first bake is owed, a live bake needs watching, or the
	// editor-only terrain surface probe is wanted - so a plate that is not baking costs nothing per frame.
	const bool bWantsPhysSurfacePoll = m_bPhysSurfaceFromTerrain && g_terrainPlatePhysSurfaceFromTerrain != 0
	                                   && ((uint32)m_type & (uint32)EMeshType::Collider) != 0
	                                   && g_terrainPlatePhysSurfaceProbeFrames > 0
	                                   && gEnv != nullptr && gEnv->IsEditor();

	if (m_bRegenPending || m_bDeferredXformPending || m_bBakeMoveBatchPending || m_bInitialBakePending
	    || bWantsPhysSurfacePoll
	    || (m_bBakeIntoTerrain && (GetBakeProbeFrames() > 0 || GetPlateDebugLevel() >= 1)))
	{
		bitFlags |= ENTITY_EVENT_UPDATE;
	}

	return bitFlags;
}

void CTerrainPlateComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_DONE:
		// The plate is being removed. Give the ground back here rather than in the destructor.
		RestoreBakeOnTeardown();
		break;

	case ENTITY_EVENT_INIT:
		// The resurrect path. Re-arming the bake without the registry would leave the plate invisible to every
		// cascade, so the two always move together. @see ReArmAfterTeardown.
		ReArmAfterTeardown();
		break;

	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		// A property change can only reach a plate the user is working on, so an entity that came back out
		// of the deletion queue is allowed to bake again.
		NoteBakeUserAction();

		// It is also the one edit the frame/material comparison in IsColourStamped cannot see: rim falloff,
		// grid, skirt all change the picture without moving the plate. @see ClearColourStamp.
		ClearColourStamp();
		ReArmAfterTeardown();
			// A property change is a user action in an open level, so the terrain is in and the "never bake from
			// Initialize" rule is satisfied. Releasing here makes ticking Bake Into Terrain apply in the same frame.
		if (m_bInitialBakePending && gEnv->p3DEngine != nullptr && gEnv->p3DEngine->GetITerrain() != nullptr)
		{
			m_bInitialBakePending = false;

			if (GetPlateDebugLevel() >= 1)
			{
				CryLog("TerrainPlate '%s': first bake released by a property change", GetPlateName());
			}
		}

		// Ground Level just left 0 while the bake only ever adds ground: the part of the relief that is now under
		// the plate's ground would stay buried under the untouched terrain and be culled, so the plate simply
		// disappears. Raise And Lower is the mode that makes the setting mean anything, so it is switched here -
		// once, on the edit that causes it, and never behind the user's back at any later time.
		if ((float)m_groundLevel > 0.f && m_lastGroundLevel <= 0.f
		    && m_bBakeIntoTerrain && m_bakeMode == ETerrainPlateBakeMode::Raise)
		{
			m_bakeMode = ETerrainPlateBakeMode::RaiseAndLower;
			CryLog("Terrain Plate '%s': Ground Level is above 0, so Bake Mode was switched to Raise And Lower - "
			       "otherwise the terrain would not follow the plate down and the sunken part would stay buried.",
			       GetPlateName());
		}

		m_lastGroundLevel = (float)m_groundLevel;

		if (GetPlateDebugLevel() >= 1)
		{
			CryLog("TerrainPlate '%s': property changed, Bake Into Terrain = %s, terrain %s",
			       GetPlateName(), m_bBakeIntoTerrain ? "on" : "off",
			       (gEnv->p3DEngine != nullptr && gEnv->p3DEngine->GetITerrain() != nullptr) ? "present" : "MISSING");
		}

		Rebuild();
			// Bake Into Terrain decides whether the component needs the per frame tick that watches the bake, so the
			// event mask has to be re-evaluated after any property change.
		if (m_pEntity != nullptr)
		{
			m_pEntity->UpdateComponentEventMask(this);
		}
		break;

	case ENTITY_EVENT_XFORM:
			// A scale change always invalidates the mesh - the vertices are built at the plate's world size. The test
			// is on the total world scale, so gizmo, scaled group or prefab parent and script all fire it.
		if (NeedsRegenerateOnTransform() || !GetWorldScale().IsEquivalent(m_lastWorldScale, 0.0001f))
		{
			// A transform is the user acting, whoever sent it, so it re-arms the stalled automatic repairs.
			NoteBakeUserAction();

				// With Update During Drag off, an editor transform only records that a rebuild is owed and it runs on
				// mouse up. ENTITY_XFORM_EDITOR is set only for editor driven transforms, so the rest keep the old path.
			const bool bEditorTransform = (event.nParam[0] & ENTITY_XFORM_EDITOR) != 0;

				// For a BAKING plate neither the editor bit nor Update During Drag is the right test: whatever moved the
				// plate, its bake is still in the ground where it used to be, and that restore has to be ordered against
				// every other plate that moved with it. So ANY transform arms the settle countdown and the batch does it.
			if (WantsBakeMoveBatch() || (bEditorTransform && !m_bUpdateDuringDrag))
			{
					// Nothing has to be done to the slot: it still holds the compensation for the size the mesh was built at
					// while the entity matrix is already at the new size, so the plate stretches live until the rebuild.
				m_bDeferredXformPending = true;
				m_lastXformFrameId = gEnv->nMainFrameID;
				m_pEntity->UpdateComponentEventMask(this);
			}
			else
			{
				RequestRegenerate();
			}
		}
		break;

	case ENTITY_EVENT_XFORM_FINISHED_EDITOR:
		if (m_bDeferredXformPending)
		{
			FlushDeferredTransform();
		}
		break;

	case ENTITY_EVENT_UPDATE:
		{
					// Proof of life for the deferred machinery: the first bake, the probe polling and the settle fallback
					// all ride on this event, which IS delivered in Sandbox edit mode, not only in game mode.
			if (!m_bUpdateTickLogged && GetPlateDebugLevel() >= 1)
			{
				m_bUpdateTickLogged = true;
				CryLog("TerrainPlate '%s': first update tick, terrain %s, first bake %s",
				       GetPlateName(),
				       (gEnv->p3DEngine != nullptr && gEnv->p3DEngine->GetITerrain() != nullptr) ? "present" : "MISSING",
				       m_bInitialBakePending ? "still owed" : "already released");
			}

			DrawBakeDebug();

					// The move batch runs one frame after the last plate joined, so a whole gesture lands in one ordered
					// pass. NOT gated on the cascade quiet frames or the backoff: a batch is a real user move, not our own.
			if (m_bBakeMoveBatchPending && gEnv->nMainFrameID != g_bakeMoveBatchFrameId)
			{
				FlushBakeMoveBatch();
			}

					// Settle fallback for the deferred path: flush once no transform has been seen for N frames, in case
					// ENTITY_EVENT_XFORM_FINISHED_EDITOR never arrives. A plate inside a GROUP never gets it either -
					// CSelectionGroup::ObjectModified only sends it to selected OBJTYPE_ENTITY objects.
			const int settleFrames = GetEffectiveSettleFrames();
			const bool bSettled = m_bDeferredXformPending && settleFrames > 0
			                      && (int)(gEnv->nMainFrameID - m_lastXformFrameId) >= settleFrames;

				// One rebuild clears both flags, so the two reasons never cost two rebuilds. A settled baking plate
				// goes through FlushDeferredTransform, which puts it in the batch instead of rebuilding on its own.
			if (bSettled)
			{
				FlushDeferredTransform();
			}
			else if (m_bRegenPending)
			{
				RegenerateMesh();
				m_pEntity->UpdateComponentEventMask(this);
			}

			if (m_bInitialBakePending)
			{
						// Sandbox never sends ENTITY_EVENT_LEVEL_LOADED when a level is opened for editing, so the first update
						// tick that finds a terrain is what releases the bake here. Either way, never Initialize().
				if (gEnv->p3DEngine != nullptr && gEnv->p3DEngine->GetITerrain() != nullptr)
				{
					m_bInitialBakePending = false;
					if (m_bBakeIntoTerrain)
					{
						RegenerateMesh();
					}
					m_pEntity->UpdateComponentEventMask(this);
				}
			}
			else
			{
				PollBake();
			}

				// Independent of the bake polling above on purpose: PollBake early-returns unless a bake is live, and
				// it compares heights, so it cannot see a layer repaint under a plate that does not bake at all.
			PollPhysicsSurface();
		}
		break;

	case ENTITY_EVENT_LEVEL_LOADED:
		// The launcher and game mode path. The editor has pushed the pristine CHeightmap by now, so the baseline
		// the bake captures is genuinely the original ground.
		if (m_bInitialBakePending)
		{
			m_bInitialBakePending = false;
			if (m_bBakeIntoTerrain)
			{
				RegenerateMesh();
			}
			m_pEntity->UpdateComponentEventMask(this);
		}
		break;

	case ENTITY_EVENT_PHYSICAL_TYPE_CHANGED:
		{
				// A physics component (rigid body, character controller) re-physicalizes the whole entity and destroys
				// our static body first; from then on the plate only contributes its slot geometry.
			IPhysicalEntity* pPhysicalEntity = m_pEntity->GetPhysicalEntity();
			if (m_bOwnsPhysicalEntity && (pPhysicalEntity == nullptr || pPhysicalEntity->GetType() != PE_STATIC))
			{
				m_bOwnsPhysicalEntity = false;
			}
		}
		break;
	}

	if (event.event == ENTITY_EVENT_SLOT_CHANGED && m_pEntity->GetStatObj((int)event.nParam[0]) != m_pStatObj)
		return; // someone else owns that slot

	CBaseMeshComponent::ProcessEvent(event);

	// Every one of these events ends in a call that rewrites the part's material mapping from the slot's RENDER
	// material, throwing away the terrain ids the plate put there - the base class handler above is itself one,
	// which is why this sits AFTER it. UpdateSlotGeometry is covered by the ApplyPhysicsSurfaceMapping at the end
	// of the rebuild; anything else is caught by the read-back half of PollPhysicsSurface.
	switch (event.event)
	{
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
	case ENTITY_EVENT_SLOT_CHANGED:
	case ENTITY_EVENT_PHYSICAL_TYPE_CHANGED:
	case ENTITY_EVENT_MATERIAL:
		ApplyPhysicsSurfaceMapping();
		break;
	default:
		break;
	}
}

bool CTerrainPlateComponent::SetMaterial(int slotId, const char* szMaterial)
{
	if (slotId == GetEntitySlotId())
	{
		if (IMaterial* pMaterial = gEnv->p3DEngine->GetMaterialManager()->LoadMaterial(szMaterial, false))
		{
			m_materialPath.value = szMaterial;
			m_pEntity->SetSlotMaterial(GetEntitySlotId(), pMaterial);
		}
		else if (szMaterial[0] == '\0')
		{
			m_materialPath.value.clear();
			m_pEntity->SetSlotMaterial(GetEntitySlotId(), nullptr);
		}

		return true;
	}

	return false;
}

bool CTerrainPlateComponent::NeedsRegenerateOnTransform() const
{
	return m_bEdgeWeld || (float)m_skirt > 0.f || m_gridMode == ETerrainPlateGridMode::Spacing;
}

void CTerrainPlateComponent::Rebuild()
{
	LoadSourceIfNeeded();
	RegenerateMesh();
}

void CTerrainPlateComponent::OnTransformChanged()
{
	// Only the COMPONENT transform reaches this hook; moving or scaling the entity itself comes through
	// ENTITY_EVENT_XFORM. Both end in the same place, throttled to one rebuild per frame by RequestRegenerate.
	if (m_pEntity == nullptr)
		return;

	RequestRegenerate();
}

void CTerrainPlateComponent::RequestRegenerate()
{
	// At most one rebuild per frame: dragging the entity fires ENTITY_EVENT_XFORM continuously.
	if (gEnv->nMainFrameID != m_lastRegenFrameId)
	{
		RegenerateMesh();
		return;
	}

	if (!m_bRegenPending)
	{
		m_bRegenPending = true;
		if (m_pEntity != nullptr)
		{
			m_pEntity->UpdateComponentEventMask(this);
		}
	}
}

void CTerrainPlateComponent::FlushDeferredTransform()
{
	// A baking plate does not rebuild here: its old footprint is still in the terrain and the restore has to be
	// ordered against every other plate that moved in the same gesture. @see FlushBakeMoveBatch.
	if (WantsBakeMoveBatch())
	{
		m_bDeferredXformPending = false;   // consumed: the batch owns the rebuild from here
		JoinBakeMoveBatch();

		if (m_pEntity != nullptr)
		{
			m_pEntity->UpdateComponentEventMask(this);
		}
		return;
	}

	// Runs the whole rebuild the drag held back. Deliberately not RequestRegenerate: the gesture is over, so
	// there is nothing left to coalesce with. RegenerateMesh clears both pending flags.
	RegenerateMesh();

	if (m_pEntity != nullptr)
	{
		m_pEntity->UpdateComponentEventMask(this);
	}
}

void CTerrainPlateComponent::LoadSourceIfNeeded()
{
	// The displacement source is parked: Source Mode and Displacement Texture are off the panel and out of the
	// archive, so m_sourceMode is forced here too, in case an object still carries the old value.
	m_sourceMode = ETerrainPlateSource::HeightmapFile;

	const string& path = m_heightmapPath.value;

	if (m_bSourceLoaded && m_loadedSourceMode == m_sourceMode && m_loadedSource == path)
		return;

	m_heightField.Clear();

	if (!path.empty())
	{
		TerrainPlateLoader::LoadHeightmapFile(path.c_str(), m_heightField);

		// The parked branch, kept verbatim so un-parking is a two line job:
		//   if (m_sourceMode == ETerrainPlateSource::HeightmapFile)
		//     TerrainPlateLoader::LoadHeightmapFile(path.c_str(), m_heightField);
		//   else
		//     TerrainPlateLoader::LoadDisplacementTexture(path.c_str(), m_heightField);
	}

	m_loadedSource = path;
	m_loadedSourceMode = m_sourceMode;
	m_bSourceLoaded = true;
}

Matrix34 CTerrainPlateComponent::GetPlateFrameTM() const
{
	if (m_pEntity == nullptr)
		return Matrix34(IDENTITY);

	// Built from the entity and the component transform rather than read back from the slot: the slot local TM
	// carries the scale-cancelling transform, and this is the only frame that exists before the slot does.
	Matrix34 frameTM = m_pEntity->GetWorldTM() * GetTransformMatrix();

	// Re-impose the authoritative size on the columns. GetWorldScale is the single source of it, so without
	// this the frame and the size would disagree exactly when it matters - and the frame is what the weld, the
	// bake footprint and the node matrix are solved against. Directions (and the sign of a mirrored axis) are
	// preserved: each column is only rescaled to the length the plate is declared to have.
	const Vec3 size = GetWorldScale();
	static const Vec3 kAxis[3] = { Vec3(1.f, 0.f, 0.f), Vec3(0.f, 1.f, 0.f), Vec3(0.f, 0.f, 1.f) };

	for (int i = 0; i < 3; ++i)
	{
		const Vec3  column = frameTM.GetColumn(i);
		const float length = column.GetLength();
		frameTM.SetColumn(i, (length > 1e-6f) ? column * (size[i] / length) : kAxis[i] * size[i]);
	}

	return frameTM;
}

bool CTerrainPlateComponent::InvertPlateFrameTM(const Matrix34& frameTM, Matrix34& outInvTM)
{
	// Matrix34::Invert divides by the determinant unguarded, so a zero scale (entity, component or group parent)
	// would fill every welded vertex with NaN and register a NaN AABB in the octree. Same epsilon as the 2x2
	// solve in SamplePlateSurface, so "edge on" means the same thing everywhere in this component.
	if (fabs_tpl(frameTM.Determinant()) < 1e-8f)
		return false;

	outInvTM = frameTM;
	outInvTM.Invert();
	return true;
}

Vec3 CTerrainPlateComponent::GetMeshBuildScale() const
{
	// The size RegenerateMesh multiplied into the position stream. It equals GetWorldScale() except
	// between a deferred transform and its flush, when the mesh is deliberately one size behind.
	return Vec3(
	  (fabs_tpl(m_lastWorldScale.x) > 0.0001f) ? m_lastWorldScale.x : 1.f,
	  (fabs_tpl(m_lastWorldScale.y) > 0.0001f) ? m_lastWorldScale.y : 1.f,
	  (fabs_tpl(m_lastWorldScale.z) > 0.0001f) ? m_lastWorldScale.z : 1.f);
}

Matrix34 CTerrainPlateComponent::GetPlateNodeTM() const
{
	// The design frame with the MESH's size divided out, column by column. The mesh is in world metres, so
	// node * meshVertex == frame * unitVertex and the two are interchangeable given the right space.
	const Vec3 meshScale = GetMeshBuildScale();
	Matrix34 nodeTM = GetPlateFrameTM();
	nodeTM.SetColumn(0, nodeTM.GetColumn(0) / meshScale.x);
	nodeTM.SetColumn(1, nodeTM.GetColumn(1) / meshScale.y);
	nodeTM.SetColumn(2, nodeTM.GetColumn(2) / meshScale.z);
	return nodeTM;
}

void CTerrainPlateComponent::ApplySlotScaleCompensation()
{
	// The renderer reconstructs the vertex normal as cross(T, B) in OBJECT space and multiplies it by the object
	// matrix, tilting it by S^2 instead of S^-1 under a non-uniform scale - so no tangent stream can make a
	// non-uniformly scaled node shade correctly. The cure is to leave no scale on the node: the mesh is in metres
	// and the slot carries the transform that puts the node back at unit scale. During a deferred drag the
	// columns are NOT unit: the ratio stretches the plate live, and returns to 1 on rebuild.
	const int slotId = GetEntitySlotId();
	if (m_pEntity == nullptr || slotId == IEntityComponent::EmptySlotId)
		return;

	// Solved from the node matrix rather than derived a second time from the component transform and the mesh
	// size: entityWorldTM * slotLocalTM == GetPlateNodeTM() by construction, so the slot cannot drift away from
	// the frame the rest of the component reasons in (it did when the component transform held a scale of
	// its own).
	Matrix34 invEntityTM;
	if (!InvertPlateFrameTM(m_pEntity->GetWorldTM(), invEntityTM))
		return; // a zero-scaled entity draws nothing; leave the slot alone rather than write NaNs into it

	const Vec3     meshScale = GetMeshBuildScale();
	const Matrix34 slotLocalTM = invEntityTM * GetPlateNodeTM();

	m_pEntity->SetSlotLocalTM(slotId, slotLocalTM);

	if (GetPlateDebugLevel() >= 1)
	{
		const Matrix34 slotWorldTM = m_pEntity->GetSlotWorldTM(slotId);
		CryLog("TerrainPlate '%s': plate size %.3f x %.3f x %.3f m, mesh built at %.3f x %.3f x %.3f, "
		       "node column lengths %.4f / %.4f / %.4f (1 = no scale on the node)",
		       GetPlateName(), GetWorldScale().x, GetWorldScale().y, GetWorldScale().z,
		       meshScale.x, meshScale.y, meshScale.z,
		       slotWorldTM.GetColumn0().GetLength(), slotWorldTM.GetColumn1().GetLength(), slotWorldTM.GetColumn2().GetLength());
	}
}

Vec3 CTerrainPlateComponent::GetWorldScale() const
{
	if (m_pEntity == nullptr)
		return Vec3(1.f, 1.f, 1.f);

	// THE authoritative read, and the reason it does not go through GetPlateFrameTM (which would recurse):
	// the entity's world transform and the component's own transform are the only two matrices the plate
	// never writes. The slot local TM is excluded on purpose - it carries the compensation this component
	// puts there itself (@see ApplySlotScaleCompensation), and so is the mesh, which was built from this
	// very number. Reading the size back out of either is what makes a rebuild grow the plate.
	const Matrix34 authoredTM = m_pEntity->GetWorldTM() * GetTransformMatrix();
	const Vec3 scale(authoredTM.GetColumn0().GetLength(), authoredTM.GetColumn1().GetLength(), authoredTM.GetColumn2().GetLength());

	// A zero scale would divide by zero all over the generator and produces no visible geometry, so it reads 1.
	// Nothing else is imposed: the mesh is exactly the size the entity is scaled to, whatever that is, so the
	// gizmo and the plate can never disagree. The guards that matter are elsewhere and are about cost, not size:
	// the Spacing grid clamps to 513 vertices per axis, the physics surface sampler to 65 probes per axis, the
	// bake footprint is clipped to the map and refused past 8M cells.
	return Vec3(
	  (scale.x > 0.0001f) ? scale.x : 1.f,
	  (scale.y > 0.0001f) ? scale.y : 1.f,
	  (scale.z > 0.0001f) ? scale.z : 1.f);
}

void CTerrainPlateComponent::ApplyMaterial()
{
	IMaterialManager* pMaterialManager = gEnv->p3DEngine->GetMaterialManager();

	IMaterial* pMaterial = nullptr;
	if (!m_materialPath.value.empty())
	{
		pMaterial = pMaterialManager->LoadMaterial(m_materialPath.value, false);
	}

	// A procedural statobj has no material of its own, and CTerrainNode::AppendTrianglesFromObjects skips any
	// integrating node whose IRenderNode::GetMaterial() is null. Fall back to the engine default material.
	if (pMaterial == nullptr)
	{
		pMaterial = pMaterialManager->GetDefaultMaterial();
	}

	if (m_pStatObj != nullptr)
	{
		m_pStatObj->SetMaterial(pMaterial);
	}

	m_pEntity->SetSlotMaterial(GetEntitySlotId(), m_materialPath.value.empty() ? nullptr : pMaterial);
}

int CTerrainPlateComponent::GetIntegrationVertexBudget() const
{
	if (gEnv->pConsole == nullptr)
		return 0;

	ICVar* pCVar = gEnv->pConsole->GetCVar("e_TerrainIntegrateObjectsMaxVertices");
	if (pCVar == nullptr)
		return 0;

	// Half of the per sector budget: a sector usually holds more than one integrating object, and the budget is
	// shared by all of them.
	const int budget = pCVar->GetIVal();
	return (budget > 0) ? (budget / 2) : 0;
}

void CTerrainPlateComponent::ComputeEffectiveGrid(int& vertsX, int& vertsY)
{
	const int budgetHalf = GetIntegrationVertexBudget();

	float spacingX = 0.f;
	float spacingY = 0.f;

	// The plate is solved on a normalised 1 x 1 unit square, so the world edge length on an axis is simply the
	// plate's size on that axis.
	const Vec3 worldScale = GetWorldScale();
	const float edgeX = worldScale.x;
	const float edgeY = worldScale.y;

	if (m_gridMode == ETerrainPlateGridMode::Fixed)
	{
		vertsX = vertsY = clamp_tpl((int)m_grid, 2, 513);
	}
	else
	{
		// Spacing is a world space distance and the two axes are resolved independently, which is why the grid may
		// end up non square.
		const float spacing = max(0.0001f, (float)m_vertexSpacing);
		vertsX = clamp_tpl((int)floorf(edgeX / spacing + 0.5f) + 1, 2, 513);
		vertsY = clamp_tpl((int)floorf(edgeY / spacing + 0.5f) + 1, 2, 513);

			// Never ask for more vertices than the source has texels on that axis: past that point the bilinear
			// sampler only interpolates and the extra triangles buy nothing.
		if (m_heightField.IsValid())
		{
			vertsX = max(2, min(vertsX, m_heightField.m_width));
			vertsY = max(2, min(vertsY, m_heightField.m_height));
		}
	}

	// Budget. In Fixed mode the user asked for an exact grid, so it is only reported. In Spacing mode the grid is
	// derived, so it is clamped proportionally - both axes by the same factor, which keeps the aspect ratio.
	if (budgetHalf > 0 && vertsX * vertsY > budgetHalf)
	{
		if (m_gridMode == ETerrainPlateGridMode::Fixed)
		{
			if (!m_bBudgetWarningIssued)
			{
				m_bBudgetWarningIssued = true;
				CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
				           "Terrain Plate: a %d x %d grid is %d vertices, more than half of the per sector integration budget "
				           "(e_TerrainIntegrateObjectsMaxVertices = %d). A second plate or object in the same sector will overflow it; "
				           "lower Grid or raise the cvar.",
				           vertsX, vertsY, vertsX * vertsY, budgetHalf * 2);
			}
		}
		else
		{
			const int requestedX = vertsX;
			const int requestedY = vertsY;
			const float factor = sqrtf((float)budgetHalf / (float)(requestedX * requestedY));

			vertsX = clamp_tpl((int)(requestedX * factor), 2, 513);
			vertsY = clamp_tpl((int)(requestedY * factor), 2, 513);

			if (!m_bBudgetClampWarningIssued)
			{
				m_bBudgetClampWarningIssued = true;
				CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
				           "Terrain Plate: Vertex Spacing %.3f m over %.2f x %.2f m asks for a %d x %d grid (%d vertices), more than half of the "
				           "per sector integration budget (e_TerrainIntegrateObjectsMaxVertices = %d). Clamped to %d x %d; raise Vertex Spacing "
				           "or the cvar to get the density back.",
				           (float)m_vertexSpacing, edgeX, edgeY, requestedX, requestedY, requestedX * requestedY,
				           budgetHalf * 2, vertsX, vertsY);
			}
		}
	}

	spacingX = edgeX / (float)(vertsX - 1);
	spacingY = edgeY / (float)(vertsY - 1);

	char szInfo[128];
	cry_sprintf(szInfo, "%d x %d vertices, %.3f x %.3f m spacing", vertsX, vertsY, spacingX, spacingY);
	m_effectiveGridInfo = szInfo;
}

void CTerrainPlateComponent::RegenerateMesh()
{
	m_bRegenPending = false;
	m_bDeferredXformPending = false;
	m_lastRegenFrameId = gEnv->nMainFrameID;

	// If this plate shares ground with other baking plates the whole set has to be unwound BEFORE anything is
	// restored - and the restore below is part of "anything". Deciding here gives the whole unwind to
	// RunBakeCascade, and this plate still holds its live rect, so both the footprint it leaves and the one it
	// takes pull neighbours in. Depth > 0 means we ARE that regeneration and must not start a second cascade.
	if (g_bakeCascadeDepth == 0 && m_bBakeIntoTerrain && !m_bBakeTornDown && !m_bInitialBakePending
	    && m_pEntity != nullptr && gEnv->p3DEngine != nullptr)
	{
		std::vector<CTerrainPlateComponent*> affected;
		CollectBakeCascadeSet(affected);

		if (affected.size() > 1)
		{
			RunBakeCascade(affected, this, "overlap cascade");
			return;
		}
	}

	// Put the original ground back before anything reads the terrain again: the edge weld below samples
	// GetTerrainElevation and would otherwise weld the plate onto its own bake. Also before the plate moves.
	RestoreBakeBaseline();

	if (m_pEntity == nullptr || gEnv->p3DEngine == nullptr)
		return;

	int nx = 2, ny = 2;
	ComputeEffectiveGrid(nx, ny);

	// The bake evaluates the very same piecewise surface this grid produces (@see GetPlateReliefAt), so the
	// vertex counts have to outlive the generation.
	m_lastGridX = nx;
	m_lastGridY = ny;

	const int gridVertexCount = nx * ny;
	const float fInvLastX = 1.f / (float)(nx - 1);
	const float fInvLastY = 1.f / (float)(ny - 1);

	// The plate is SOLVED on a normalised unit plate (-0.5 .. +0.5 in XY, 0 .. 1 in Z) and EMITTED in world
	// metres: the entity scale is multiplied into the vertices and cancelled out of the slot local transform
	// (@see ApplySlotScaleCompensation), so the render node carries no scale and the shading frame is correct.
	// Everything below the "world size" line is in METRES; above it, still normalised, read with GetPlateFrameTM.
	const Vec3 worldScale = GetWorldScale();
	m_lastWorldScale = worldScale;

	// Skirt is a world distance and the mesh is in world metres, so it is used verbatim.
	const float skirtWorld = max(0.f, (float)m_skirt);
	const float skirt = skirtWorld;
	const bool  bSkirt = (skirtWorld > 0.f);

	// Normalise a world space direction, falling back when the input degenerates (a flat plate's cross product
	// on a zero sized axis, a collapsed skirt corner).
	auto normalizeOr = [](const Vec3& v, const Vec3& fallback)
	{
		return v.IsZero() ? fallback : v.GetNormalized();
	};

	// ---------------------------------------------------------------- heights
	// UVs are the sampling coordinates as well, so mirroring flips the relief and the texture together.
	std::vector<float> heights((size_t)gridVertexCount, 0.f);
	std::vector<Vec2>  uvs((size_t)gridVertexCount);

	for (int r = 0; r < ny; ++r)
	{
		for (int c = 0; c < nx; ++c)
		{
			float u = (float)c * fInvLastX;
			float v = (float)r * fInvLastY;
			if (m_bMirrorX) u = 1.f - u;
			if (m_bMirrorY) v = 1.f - v;

			const size_t index = (size_t)r * nx + c;
			uvs[index] = Vec2(u, v);
			heights[index] = m_heightField.IsValid() ? m_heightField.SampleBilinear(u, v) : 0.f;
		}
	}

	// The rescale Normalize solves is cached, because the bake has to reproduce the very same surface from the
	// same source at arbitrary sample points (@see GetPlateReliefAt).
	m_reliefOffset = 0.f;
	m_reliefScale = 1.f;

	if (m_bNormalize && m_heightField.IsValid())
	{
		float minHeight = heights[0], maxHeight = heights[0];
		for (float h : heights)
		{
			minHeight = min(minHeight, h);
			maxHeight = max(maxHeight, h);
		}

		const float range = maxHeight - minHeight;
		const float invRange = (range > FLT_EPSILON) ? (1.f / range) : 0.f;
		for (float& h : heights)
		{
			h = (h - minHeight) * invRange;
		}

		m_reliefOffset = minHeight;
		m_reliefScale = invRange;
	}

	// Mirror Z (h -> 1 - h) and Ground Level (h -> h - level) are one affine step on the normalised relief, and
	// the bake has to reproduce it at arbitrary points too, so it is cached beside the Normalize rescale. The
	// defaults are the identity, so a plate that uses neither gets the very same numbers as before.
	m_reliefSign = m_bMirrorZ ? -1.f : 1.f;
	m_reliefShift = (m_bMirrorZ ? 1.f : 0.f) - (float)m_groundLevel;

	for (float& h : heights)
	{
		h = h * m_reliefSign + m_reliefShift;
	}

	// ---------------------------------------------------------------- positions
	// Unit plate: -0.5 .. +0.5 in XY centred on the component origin, -Ground Level .. 1 - Ground Level in Z.
	// The transform makes metres. Z 0 is the ground the plate stands on, so a negative Z is a dug pit.
	std::vector<Vec3> positions((size_t)gridVertexCount);
	for (int r = 0; r < ny; ++r)
	{
		for (int c = 0; c < nx; ++c)
		{
			const size_t index = (size_t)r * nx + c;
			positions[index] = Vec3(
			  -0.5f + ((float)c * fInvLastX),
			  0.5f - ((float)r * fInvLastY),
			  heights[index]);
		}
	}

	// ---------------------------------------------------------------- edge weld
	// The outermost ring is moved onto the terrain under it, faded out over Weld Inner Falloff of the half size,
	// as a full inverse transform of the world point (x, y, targetZ) so it lands exactly even when rotated.
	// Welding onto the sampled height still leaves a seam: GetZApr always splits the unit quad along fX + fY = 1
	// while the rendered sector flips that diagonal per quad (IsMeshQuadFlipped, not public), sectors at LOD >= 1
	// use a coarser mesh, and between two ring vertices the plate edge is straight while the terrain bends. So
	// the fix is a guaranteed sign, not a better sample: the ring is pushed Weld Sink metres below the sampled
	// surface over the outer quarter of the band, so the border always ends inside the terrain.
	const Matrix34 weldWorldTM = GetPlateFrameTM();
	Matrix34 invWorldTM;
	const bool bWeldFrameValid = InvertPlateFrameTM(weldWorldTM, invWorldTM);

	// A degenerate frame is not an error the user cannot see: the plate is flat, so it simply keeps the
	// generated surface and picks the weld back up as soon as the scale is a number again.
	if (m_bEdgeWeld && !bWeldFrameValid && GetPlateDebugLevel() >= 1)
	{
		CryLog("TerrainPlate '%s': edge weld skipped, the plate has a zero scale on one axis", GetPlateName());
	}

	if (m_bEdgeWeld && bWeldFrameValid && gEnv->p3DEngine->GetITerrain() != nullptr)
	{
		const Matrix34& worldTM = weldWorldTM;

		const float falloff = clamp_tpl((float)m_weldInnerFalloff, 0.f, 1.f);
		const float weldSink = max(0.f, (float)m_weldSink);
		const float sinkBand = falloff * 0.25f;
		const float halfSpanX = (float)(nx - 1) * 0.5f;
		const float halfSpanY = (float)(ny - 1) * 0.5f;

		for (int r = 0; r < ny; ++r)
		{
			for (int c = 0; c < nx; ++c)
			{
				const float du = (float)min(c, nx - 1 - c) / halfSpanX;
				const float dv = (float)min(r, ny - 1 - r) / halfSpanY;
				const float d = min(du, dv);

				float weight;
				if (falloff <= 0.f)
				{
					weight = (d <= 0.f) ? 1.f : 0.f;
				}
				else if (d >= falloff)
				{
					weight = 0.f;
				}
				else
				{
					const float t = d / falloff;
					weight = 1.f - (t * t * (3.f - 2.f * t)); // 1 - smoothstep(t)
				}

				if (weight <= 0.f)
					continue;

				// Sink profile: the same 1 - smoothstep curve, but over a band four times narrower than
				// the weld band, so the sink is still ~0 where the weld starts and full at the ring.
				float sinkWeight;
				if (sinkBand <= 0.f)
				{
					sinkWeight = (d <= 0.f) ? 1.f : 0.f;
				}
				else if (d >= sinkBand)
				{
					sinkWeight = 0.f;
				}
				else
				{
					const float ts = d / sinkBand;
					sinkWeight = 1.f - (ts * ts * (3.f - 2.f * ts)); // 1 - smoothstep(ts)
				}

				const size_t index = (size_t)r * nx + c;
				const Vec3 world = worldTM * positions[index];
				const float terrainZ = gEnv->p3DEngine->GetTerrainElevation(world.x, world.y);
					// The sink is a world distance, applied to the world position before the inverse transform, so it stays
					// the same number of metres at any scale - unlike Skirt, which is extruded in local space.
				const float targetZ = terrainZ - weldSink * sinkWeight;
				const Vec3 conformed = invWorldTM * Vec3(world.x, world.y, targetZ);
				positions[index] += (conformed - positions[index]) * weight;
			}
		}
	}

	// ---------------------------------------------------------------- world size
	// THE line. Above it every position is normalised and read with GetPlateFrameTM(); below it in METRES and
	// read with GetPlateNodeTM(). The two agree exactly. The weld ran first: it solves in world space.
	for (Vec3& p : positions)
	{
		p = Vec3(p.x * worldScale.x, p.y * worldScale.y, p.z * worldScale.z);
	}

	// ---------------------------------------------------------------- perimeter (skirt)
	// Counter clockwise seen from above, starting at the -X/-Y corner.
	std::vector<int> perimeter;
	if (bSkirt)
	{
		perimeter.reserve((size_t)2 * (nx - 1) + (size_t)2 * (ny - 1));
		for (int c = 0; c < nx - 1; ++c)
			perimeter.push_back((ny - 1) * nx + c);          // -Y edge, +X direction
		for (int r = ny - 1; r > 0; --r)
			perimeter.push_back(r * nx + (nx - 1));          // +X edge, +Y direction
		for (int c = nx - 1; c > 0; --c)
			perimeter.push_back(c);                          // +Y edge, -X direction
		for (int r = 0; r < ny - 1; ++r)
			perimeter.push_back(r * nx);                     // -X edge, -Y direction
	}

	const int perimeterCount = (int)perimeter.size();
	const int totalVertexCount = gridVertexCount + (bSkirt ? perimeterCount * 2 : 0);
	const int totalIndexCount = (nx - 1) * (ny - 1) * 6 + (bSkirt ? perimeterCount * 6 : 0);

	// ---------------------------------------------------------------- mesh
	if (m_pStatObj == nullptr)
	{
		m_pStatObj = gEnv->p3DEngine->CreateStatObj();
		if (m_pStatObj == nullptr)
			return;

		// Keeps the system memory copy of the mesh alive, which the terrain integration pass reads
		// through IRenderMesh::GetPosPtr(FSL_READ).
		m_pStatObj->SetFlags(m_pStatObj->GetFlags() | STATIC_OBJECT_DYNAMIC);
	}

	IIndexedMesh* pIndexedMesh = m_pStatObj->GetIndexedMesh(true);
	if (pIndexedMesh == nullptr)
		return;

	pIndexedMesh->FreeStreams();
	pIndexedMesh->SetVertexCount(totalVertexCount);
	pIndexedMesh->SetFaceCount(0);
	pIndexedMesh->SetTexCoordCount(totalVertexCount);
	pIndexedMesh->SetTangentCount(totalVertexCount);
	pIndexedMesh->SetIndexCount(totalIndexCount);

	CMesh* pMesh = pIndexedMesh->GetMesh();
	Vec3* const pPositions = pMesh->GetStreamPtr<Vec3>(CMesh::POSITIONS);
	SMeshNormal* const pNormals = pMesh->GetStreamPtr<SMeshNormal>(CMesh::NORMALS);
	SMeshTexCoord* const pTexCoords = pMesh->GetStreamPtr<SMeshTexCoord>(CMesh::TEXCOORDS);
	SMeshTangents* const pTangents = pMesh->GetStreamPtr<SMeshTangents>(CMesh::TANGENTS);
	vtx_idx* const pIndices = pMesh->GetStreamPtr<vtx_idx>(CMesh::INDICES);

	if (!pPositions || !pNormals || !pTexCoords || !pTangents || !pIndices)
		return;

		// Grid vertices. The tangent frame is analytic: the engine's own generator (IIndexedMesh::Optimize) re-welds
		// and reorders the vertices, which would destroy the grid. Stored in world space, which the node's is.
	for (int r = 0; r < ny; ++r)
	{
		for (int c = 0; c < nx; ++c)
		{
			const size_t index = (size_t)r * nx + c;

			const Vec3& pXPrev = positions[(size_t)r * nx + max(0, c - 1)];
			const Vec3& pXNext = positions[(size_t)r * nx + min(nx - 1, c + 1)];
			const Vec3& pYPrev = positions[(size_t)max(0, r - 1) * nx + c]; // one row towards +Y
			const Vec3& pYNext = positions[(size_t)min(ny - 1, r + 1) * nx + c];

			Vec3 dX = pXNext - pXPrev;   // along +X, in metres
			Vec3 dY = pYPrev - pYNext;   // along +Y, in metres
			if (dX.IsZero())
				dX = Vec3(1.f, 0.f, 0.f);
			if (dY.IsZero())
				dY = Vec3(0.f, 1.f, 0.f);

			const Vec3 normal = normalizeOr(dX.Cross(dY), Vec3(0.f, 0.f, 1.f));

			// dP/du and dP/dv of the UVs written below; mirroring reverses the direction of the
			// texture axis relative to the geometry, hence the sign flips.
			const Vec3 tangent = normalizeOr(m_bMirrorX ? -dX : dX, Vec3(1.f, 0.f, 0.f));
			const Vec3 bitangent = normalizeOr(m_bMirrorY ? dY : -dY, Vec3(0.f, 1.f, 0.f));

			pPositions[index] = positions[index];
			pNormals[index] = SMeshNormal(normal);
			pTexCoords[index] = SMeshTexCoord(uvs[index].x, uvs[index].y);
			pTangents[index] = SMeshTangents(tangent, bitangent, normal);
		}
	}

	// Skirt: a duplicated border ring plus a second ring pushed straight down, closed with outward facing quads;
	// the duplication lets the side wall have its own horizontal normal. It starts at the welded (and sunk) ring,
	// so with the default Weld Sink it is below the surface and seals what the sink alone cannot. Left in the
	// single subset: terrain integration classifies per triangle and its height band test rejects these faces.
	if (bSkirt)
	{
		const int topBase = gridVertexCount;
		const int bottomBase = gridVertexCount + perimeterCount;

		for (int i = 0; i < perimeterCount; ++i)
		{
			const int gridIndex = perimeter[i];
			const int prevGridIndex = perimeter[(i + perimeterCount - 1) % perimeterCount];
			const int nextGridIndex = perimeter[(i + 1) % perimeterCount];

			// Outward normal of an edge walked counter clockwise is (dir.y, -dir.x, 0). Positions are in
			// metres and the node has no scale, so this is already the world direction.
			Vec3 outward(ZERO);
			const Vec3 dirIn = positions[gridIndex] - positions[prevGridIndex];
			const Vec3 dirOut = positions[nextGridIndex] - positions[gridIndex];
			outward += Vec3(dirIn.y, -dirIn.x, 0.f);
			outward += Vec3(dirOut.y, -dirOut.x, 0.f);
			outward = normalizeOr(outward, Vec3(0.f, -1.f, 0.f));

			const Vec3 along = normalizeOr(dirIn + dirOut, Vec3(1.f, 0.f, 0.f));
			const Vec3 down(0.f, 0.f, -1.f);

			const Vec3 top = positions[gridIndex];
			const Vec3 bottom = top - Vec3(0.f, 0.f, skirt);

			pPositions[topBase + i] = top;
			pPositions[bottomBase + i] = bottom;
			pNormals[topBase + i] = SMeshNormal(outward);
			pNormals[bottomBase + i] = SMeshNormal(outward);
			pTexCoords[topBase + i] = SMeshTexCoord(uvs[gridIndex].x, uvs[gridIndex].y);
			pTexCoords[bottomBase + i] = SMeshTexCoord(uvs[gridIndex].x, uvs[gridIndex].y);
			pTangents[topBase + i] = SMeshTangents(along, down, outward);
			pTangents[bottomBase + i] = SMeshTangents(along, down, outward);
		}
	}

	// ---------------------------------------------------------------- buried triangle classification
	// Every vertex position is final by now (weld included), so one pass over the position stream decides per
	// vertex whether it is under the ground. @see the e_TerrainPlateCullBuried comment at the top of this file.
	// The reference is GetZApr, the same sampler the edge weld uses, which makes the test the exact inverse of
	// the weld's intent. Off the terrain it returns TERRAIN_BOTTOM_LEVEL, so nothing is culled there.
	std::vector<char> buried;
	const bool bCullBuried = (g_terrainPlateCullBuried != 0) && (gEnv->p3DEngine->GetITerrain() != nullptr);

	// The terrain read below is the ORIGINAL ground: RegenerateMesh restored it at the top, and this plate's own
	// bake only runs at the very end. A relief that dips BELOW the plate's ground (Ground Level > 0) is therefore
	// classified against ground the bake is about to dig away, and the pit would be culled and never drawn. So the
	// interior of such a plate is exempt while it bakes with lowering: the terrain there ends up the epsilon under
	// the mesh, i.e. not buried. The WELDED ring and the skirt keep the test - the bake's rim falloff leaves the
	// original ground exactly where it is, so they really are buried, which is what Weld Sink puts them there for.
	const bool bReliefBelowGround = ((float)m_groundLevel > 0.f);
	const bool bBakeExemptsInterior = bReliefBelowGround && m_bBakeIntoTerrain && !m_bBakeTornDown
	                                  && (m_bakeMode == ETerrainPlateBakeMode::RaiseAndLower);

	if (bCullBuried)
	{
		// The node matrix, not the frame: pPositions is the emitted stream and that is in metres.
		const Matrix34 nodeTM = GetPlateNodeTM();
		buried.resize((size_t)totalVertexCount, 0);

		for (int i = 0; i < totalVertexCount; ++i)
		{
			if (bBakeExemptsInterior && i < gridVertexCount)
			{
				const int c = i % nx;
				const int r = i / nx;
				const bool bOuterRing = m_bEdgeWeld && (c == 0 || c == nx - 1 || r == 0 || r == ny - 1);
				if (!bOuterRing)
					continue;   // the ground under this vertex follows the plate down, so it is not buried
			}

			const Vec3 world = nodeTM * pPositions[i];
			const float terrainZ = gEnv->p3DEngine->GetTerrainElevation(world.x, world.y);
			buried[(size_t)i] = (world.z < terrainZ - kBuriedCullMargin) ? 1 : 0;
		}
	}

	// Culled triangles are moved to the END of the index buffer, where a second MTL_FLAG_NODRAW subset picks them
	// up: the render mesh drops them, the physics trimesh keeps them. No vertex is touched.
	int indexCursor = 0;
	std::vector<vtx_idx> culledIndices;

	auto emitTriangle = [&](int a, int b, int c)
	{
		if (bCullBuried && buried[(size_t)a] && buried[(size_t)b] && buried[(size_t)c])
		{
			culledIndices.push_back((vtx_idx)a);
			culledIndices.push_back((vtx_idx)b);
			culledIndices.push_back((vtx_idx)c);
			return;
		}

		pIndices[indexCursor++] = (vtx_idx)a;
		pIndices[indexCursor++] = (vtx_idx)b;
		pIndices[indexCursor++] = (vtx_idx)c;
	};

	// Grid indices, counter clockwise seen from +Z (the engine's front face winding, see
	// CTerrain::MakeAreaRenderMesh).
	for (int r = 0; r < ny - 1; ++r)
	{
		for (int c = 0; c < nx - 1; ++c)
		{
			const int v00 = r * nx + c;
			const int v01 = r * nx + c + 1;
			const int v10 = (r + 1) * nx + c;
			const int v11 = (r + 1) * nx + c + 1;

			emitTriangle(v00, v10, v01);
			emitTriangle(v01, v10, v11);
		}
	}

	// Skirt indices. The same rule applies and it is the rule the skirt was written for: a skirt quad whose TOP
	// edge is already under the ground seals nothing - it is only something to be revealed by a dipping LOD.
	if (bSkirt)
	{
		const int topBase = gridVertexCount;
		const int bottomBase = gridVertexCount + perimeterCount;

		for (int i = 0; i < perimeterCount; ++i)
		{
			const int j = (i + 1) % perimeterCount;

			emitTriangle(topBase + i, bottomBase + i, bottomBase + j);
			emitTriangle(topBase + i, bottomBase + j, topBase + j);
		}
	}

	const int visibleIndexCount = indexCursor;

	for (vtx_idx culled : culledIndices)
	{
		pIndices[indexCursor++] = culled;
	}

	CRY_ASSERT(indexCursor == totalIndexCount);

	// The hash is what tells a real geometry change from a rebuild that produced the same mesh, so that only the
	// former pays for a shadow cache refresh. @see InvalidateShadowCache.
	AABB bbox(AABB::RESET);
	uint32 geometryHash = 2166136261u; // FNV-1a over the position stream
	for (int i = 0; i < totalVertexCount; ++i)
	{
		bbox.Add(pPositions[i]);

		uint32 vertexBits[3];
		memcpy(vertexBits, &pPositions[i], sizeof(vertexBits));
		for (uint32 bits : vertexBits)
		{
			geometryHash = (geometryHash ^ bits) * 16777619u;
		}
	}

	// The burial split is part of what the shadow cache holds and can change without a vertex moving (Edge Weld
	// off while the ground is sculpted), so the visible count is folded in.
	geometryHash = (geometryHash ^ (uint32)visibleIndexCount) * 16777619u;

	// Two subsets over one index buffer: [0, visibleIndexCount) is drawn and physicalised, the rest physicalised
	// only. Both keep nMatID 0 and PHYS_GEOM_TYPE_DEFAULT, so CStatObj::PhysicalizeGeomType - which selects on
	// nPhysicalizeType, never nMatFlags - still builds the trimesh from every triangle. The split is per triangle,
	// so the vertex range is the whole stream for both.
	const int culledIndexCount = totalIndexCount - visibleIndexCount;

	pIndexedMesh->SetSubSetCount(culledIndexCount > 0 ? 2 : 1);
	pIndexedMesh->SetSubsetIndexVertexRanges(0, 0, visibleIndexCount, 0, totalVertexCount);
	pIndexedMesh->SetSubsetMaterialId(0, 0);
	pIndexedMesh->SetSubsetMaterialProperties(0, 0, PHYS_GEOM_TYPE_DEFAULT);
	pIndexedMesh->SetSubsetBounds(0, bbox.GetCenter(), bbox.GetRadius());

	if (culledIndexCount > 0)
	{
		pIndexedMesh->SetSubsetIndexVertexRanges(1, visibleIndexCount, culledIndexCount, 0, totalVertexCount);
		pIndexedMesh->SetSubsetMaterialId(1, 0);
		pIndexedMesh->SetSubsetMaterialProperties(1, MTL_FLAG_NODRAW, PHYS_GEOM_TYPE_DEFAULT);
		pIndexedMesh->SetSubsetBounds(1, bbox.GetCenter(), bbox.GetRadius());

		if (GetPlateDebugLevel() >= 1)
		{
			CryLog("TerrainPlate '%s': %d of %d triangles are buried and are not drawn (e_TerrainPlateCullBuried)",
			       GetPlateName(), culledIndexCount / 3, totalIndexCount / 3);
		}
	}

	// The bounding box is deliberately the WHOLE mesh, culled triangles included: it is what the physics proxy,
	// the entity slot and the terrain sector invalidation are sized from.
	pIndexedMesh->SetBBox(bbox);

	const bool bPhysicalize = ((uint32)m_type & (uint32)EMeshType::Collider) != 0;

	// The previous generation's trimesh has to go first: CStatObj::AssignPhysGeom overwrites the entry in
	// m_arrPhysGeomInfo without unregistering it, so Invalidate(true) twice would leak one phys_geometry per
	// rebuild. SetPhysGeom(nullptr) is the accessor that unregisters, and the slot part must go before it.
	// The per face ids are an INPUT to the trimesh, so the snapshot and the sampling happen here while the indexed
	// mesh is alive; it is the whole mesh, both subsets, because a buried triangle stays in the physics trimesh.
	bool bPhysMeshCached = false;
	if (bPhysicalize && IsPhysSurfaceFromTerrainEnabled())
	{
		bPhysMeshCached = CachePhysicsMesh(pPositions, totalVertexCount, pIndices, totalIndexCount);
	}
	else
	{
		// Type changed to Render, or the property or the cvar turned off: drop the snapshot, so nothing downstream
		// can build a per face trimesh out of a mesh that is no longer the current one.
		ReleasePhysicsMeshCache();
	}

	BuildPhysicsSurfaceMapping();

	// Built BEFORE the old trimesh is dropped, so that a failure here cannot leave the plate without
	// collision: on null the stock Invalidate(true) path below runs exactly as it always has.
	phys_geometry* pPerFacePhysGeom = bPhysMeshCached ? CreatePhysicsGeometryFromCache() : nullptr;

	ReleasePhysicsGeometry();

	if (pPerFacePhysGeom != nullptr)
	{
		m_pStatObj->SetPhysGeom(pPerFacePhysGeom, PHYS_GEOM_TYPE_DEFAULT);
	}

	m_pStatObj->Invalidate(bPhysicalize && pPerFacePhysGeom == nullptr);

	// The cache only exists for the editor's repaint probe, which rebuilds the trimesh without regenerating the
	// mesh. Nothing can repaint a layer at run time, so a launcher build gives the memory straight back.
	if (gEnv == nullptr || !gEnv->IsEditor())
	{
		ReleasePhysicsMeshCache();
	}

	// CStatObj::PhysicalizeGeomType builds the trimesh with 16 bit indices and bails out on the first index
	// >= 0xffff, so a plate denser than 65535 vertices silently ends up without collision. Say so once.
	if (bPhysicalize && m_pStatObj->GetPhysGeom() == nullptr && !m_bPhysMeshWarningIssued)
	{
		m_bPhysMeshWarningIssued = true;
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
		           "Terrain Plate: '%s' could not be turned into a physics trimesh (%d vertices, the 16 bit index limit is 65535); "
		           "the plate is rendered but does not collide. Lower Grid or raise Vertex Spacing.",
		           m_pEntity->GetName(), totalVertexCount);
	}

	// Assigning the slot re-creates the render node, which makes C3DEngine drop the terrain sector meshes over
	// the union of the old and the new bounding box. No explicit ResetTerrainVertBuffers is needed here.
	m_pEntity->SetStatObj(m_pStatObj, GetOrMakeEntitySlotId() | ENTITY_SLOT_ACTUAL, false);

	// The mesh is in world metres, so the slot must contribute S^-1 and leave the node at unit scale. It goes
	// after GetOrMakeEntitySlotId and BEFORE the two physics calls: CEntityPhysics::AddSlotGeometry reads
	// GetSlotLocalTM to place the part, so a stale slot transform would put the collision somewhere else.
	ApplySlotScaleCompensation();

	ApplyMaterial();

	// Before ApplyBaseMeshProperties: the base class only feeds the slot geometry to a physical entity
	// that already exists, so the static body has to be there by then.
	UpdateNativePhysics();

	ApplyBaseMeshProperties();

	// Last of the physics chain, and it has to be: UpdateNativePhysics and ApplyBaseMeshProperties both end in
	// code that writes the part's mapping from the render material, discarding anything pushed before them.
	ApplyPhysicsSurfaceMapping();

	if (geometryHash != m_geometryHash)
	{
		m_geometryHash = geometryHash;
		InvalidateShadowCache();
	}

	// Last, so the baseline is captured from the restored ground and the block from the relief just solved.
	// Cascaded, because a baseline is only the original ground if everything above it is out of the terrain.
	ApplyBakeCascaded();

	// Idempotency. Regenerating must be a pure function of the plate's authored transform: if the size read
	// AFTER the rebuild differs from the size it was built at, something the rebuild wrote (the slot, the mesh,
	// the render node's bounds) has fed back into the size, and the next rebuild will multiply again. That is
	// the defect that grew a plate to the size of the map; say so once, loudly, rather than let it compound.
	const Vec3 sizeAfter = GetWorldScale();
	if (!sizeAfter.IsEquivalent(worldScale, 0.001f) && !m_bSizeFeedbackWarned)
	{
		m_bSizeFeedbackWarned = true;
		CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_ERROR,
		           "Terrain Plate '%s': rebuilding changed the plate's own size, %.3f x %.3f x %.3f m -> %.3f x %.3f x %.3f m. "
		           "The size must depend only on the entity and component transforms; this is a feedback loop and the plate "
		           "will keep growing. Please report it.",
		           GetPlateName(), worldScale.x, worldScale.y, worldScale.z, sizeAfter.x, sizeAfter.y, sizeAfter.z);
	}

	if (GetPlateDebugLevel() >= 1)
	{
		CryLog("TerrainPlate '%s': rebuild done, size in %.3f x %.3f x %.3f m, size out %.3f x %.3f x %.3f m, grid %d x %d",
		       GetPlateName(), worldScale.x, worldScale.y, worldScale.z, sizeAfter.x, sizeAfter.y, sizeAfter.z, nx, ny);
	}
}

// Virtual bake: the plate's low frequency relief written into the live engine terrain.
// CGameExporter::ExportHeightMap serialises ITerrain::GetCompiledData, the live quantised sector arrays
// SetTerrainElevation writes - never the editor's CHeightmap - so a bake that exists at export time IS the
// shipped terrain, and CHeightmap stays pristine and is therefore also the baseline. SetTerrainElevation's body
// is #ifndef _RELEASE, so in a release build every call below is empty, as a shipped level wants.

namespace
{
//! Terrain metrics of the current level. Everything in the bake is in terrain units.
SBakeTerrainMetrics GetBakeTerrainMetrics()
{
	SBakeTerrainMetrics metrics;

	if (gEnv == nullptr || gEnv->p3DEngine == nullptr || gEnv->p3DEngine->GetITerrain() == nullptr)
		return metrics;

	metrics.unitSize = gEnv->p3DEngine->GetHeightMapUnitSize();
	if (metrics.unitSize <= 0.f)
		return SBakeTerrainMetrics();

	metrics.hmapSize = int(gEnv->p3DEngine->GetTerrainSize() / metrics.unitSize);
	metrics.sectorUnits = max(1, int(gEnv->p3DEngine->GetTerrainSectorSize() / metrics.unitSize));
	return metrics;
}

//! Height of one terrain node exactly as stored: GetTerrainZ (CTerrain::GetZfromUnits) truncates to units and
//! does not interpolate. GetZApr does, and returns TERRAIN_BOTTOM_LEVEL on the x = 0 / y = 0 border.
float SampleTerrainUnit(const SBakeTerrainMetrics& metrics, int ux, int uy)
{
	const int cx = clamp_tpl(ux, 0, metrics.hmapSize - 1);
	const int cy = clamp_tpl(uy, 0, metrics.hmapSize - 1);
	return gEnv->p3DEngine->GetTerrainZ(((float)cx + 0.5f) * metrics.unitSize, ((float)cy + 0.5f) * metrics.unitSize);
}

//! The quantisation step SetTerrainElevation will give a sector holding this height span.
float TerrainQuantStep(float fMin, float fMax)
{
	return (max(0.f, fMin - kTerrainDeformationDepth) < fMax) ? ((fMax - max(0.f, fMin - kTerrainDeformationDepth)) / kTerrainHeightQuantSteps) : 0.f;
}
}

float CTerrainPlateComponent::GetPlateReliefAt(float lx, float ly) const
{
	if (!m_heightField.IsValid())
		return 0.f;

	// The mesh is NOT the source: RegenerateMesh samples the source once per grid vertex and the surface is the
	// interpolation between them, so sampling the source directly returned valleys the grid had smoothed away and
	// the Min filter followed them. Snap to the grid, sample the four surrounding grid VERTICES, interpolate.
	const int nx = (m_lastGridX > 1) ? m_lastGridX : m_heightField.m_width;
	const int ny = (m_lastGridY > 1) ? m_lastGridY : m_heightField.m_height;

	const float cf = clamp_tpl(lx + 0.5f, 0.f, 1.f) * (float)(nx - 1);
	const float rf = clamp_tpl(0.5f - ly, 0.f, 1.f) * (float)(ny - 1);

	const int   c0 = clamp_tpl((int)floorf(cf), 0, nx - 2);
	const int   r0 = clamp_tpl((int)floorf(rf), 0, ny - 2);
	const float tc = clamp_tpl(cf - (float)c0, 0.f, 1.f);
	const float tr = clamp_tpl(rf - (float)r0, 0.f, 1.f);

	// The quad is evaluated bilinearly while the mesh triangulates it, so the two differ by at most half the
	// quad's twist. The half cell guard in ComputeBakeBlock catches that, on top of the epsilon.
	const auto vertexHeight = [this, nx, ny](int c, int r)
	{
		float u = (float)c / (float)(nx - 1);
		float v = (float)r / (float)(ny - 1);
		if (m_bMirrorX) u = 1.f - u;
		if (m_bMirrorY) v = 1.f - v;

		// Normalize rescale, then the Mirror Z / Ground Level step - the same two the mesh grid went through.
		return ((m_heightField.SampleBilinear(u, v) - m_reliefOffset) * m_reliefScale) * m_reliefSign + m_reliefShift;
	};

	const float h00 = vertexHeight(c0, r0);
	const float h10 = vertexHeight(c0 + 1, r0);
	const float h01 = vertexHeight(c0, r0 + 1);
	const float h11 = vertexHeight(c0 + 1, r0 + 1);

	return (h00 * (1.f - tc) + h10 * tc) * (1.f - tr) + (h01 * (1.f - tc) + h11 * tc) * tr;
}

bool CTerrainPlateComponent::SamplePlateSurface(const Matrix34& worldTM, float worldX, float worldY,
                                                float& outWorldZ, float& outLocalX, float& outLocalY) const
{
	// The plate is a height field over its own local XY, so a terrain cell takes the plate height straight above
	// or below it: world = t + c0*lx + c1*ly + c2*lz, a 2x2 solve once c2*lz is known. With local Z == world Z
	// the first solve is exact; a tilted plate needs one refinement pass and only a vertical projection anyway.
	const Vec3 c0 = worldTM.GetColumn0();
	const Vec3 c1 = worldTM.GetColumn1();
	const Vec3 c2 = worldTM.GetColumn2();
	const Vec3 t = worldTM.GetTranslation();

	const float det = c0.x * c1.y - c1.x * c0.y;
	if (fabs_tpl(det) < 1e-8f)
		return false; // the plate is edge on: it has no footprint

	const float invDet = 1.f / det;
	const bool bTilted = (fabs_tpl(c2.x) > 1e-6f || fabs_tpl(c2.y) > 1e-6f);

	float lx = 0.f, ly = 0.f, lz = 0.f;
	for (int iteration = 0; iteration < 2; ++iteration)
	{
		const float rx = worldX - t.x - c2.x * lz;
		const float ry = worldY - t.y - c2.y * lz;

		lx = (c1.y * rx - c1.x * ry) * invDet;
		ly = (-c0.y * rx + c0.x * ry) * invDet;

		// GetPlateReliefAt clamps its own lookup, so an intermediate lx / ly outside the plate is safe here.
		lz = GetPlateReliefAt(lx, ly);

		if (!bTilted)
			break;
	}

	// Bounds AFTER the refinement, never inside it: on a tilted plate the lz = 0 solve lands a cell that really
	// is under the plate outside the square, and rejecting it there left a strip of original ground along the
	// high edge and a commit footprint smaller than the mesh. The tolerance covers the vertex on the rim.
	const float kLocalBoundsEpsilon = 1e-4f;
	if (lx < -0.5f - kLocalBoundsEpsilon || lx > 0.5f + kLocalBoundsEpsilon
	    || ly < -0.5f - kLocalBoundsEpsilon || ly > 0.5f + kLocalBoundsEpsilon)
		return false;

	lx = clamp_tpl(lx, -0.5f, 0.5f);
	ly = clamp_tpl(ly, -0.5f, 0.5f);

	outWorldZ = (worldTM * Vec3(lx, ly, lz)).z;
	outLocalX = lx;
	outLocalY = ly;
	return true;
}

bool CTerrainPlateComponent::ComputeBakeRect(const SBakeTerrainMetrics& metrics, const Matrix34& worldTM,
                                             int& outX1, int& outY1, int& outSize) const
{
	// The footprint is the world AABB of the rotated unit plate box: cells inside the AABB but outside the
	// rotated plate simply keep the baseline.
	// Ground Level slides the relief band down, and on a ROTATED plate the local Z extent is part of the world
	// footprint, so it is taken from the band the plate actually occupies rather than assumed to start at 0.
	const float groundLevel = (float)m_groundLevel;
	const AABB localBox(Vec3(-0.5f, -0.5f, -groundLevel), Vec3(0.5f, 0.5f, 1.f - groundLevel));
	const AABB worldBox = AABB::CreateTransformedAABB(worldTM, localBox);

	const float invUnit = 1.f / metrics.unitSize;
	const int sector = metrics.sectorUnits;

	// One unit of halo, so the falloff band and the writer's inclusive upper row / column both land on
	// cells this plate owns.
	int x1 = (int)floorf(worldBox.min.x * invUnit) - 1;
	int y1 = (int)floorf(worldBox.min.y * invUnit) - 1;
	int x2 = (int)ceilf(worldBox.max.x * invUnit) + 1;
	int y2 = (int)ceilf(worldBox.max.y * invUnit) + 1;

	x1 = clamp_tpl(x1, 0, metrics.hmapSize);
	y1 = clamp_tpl(y1, 0, metrics.hmapSize);
	x2 = clamp_tpl(x2, 0, metrics.hmapSize);
	y2 = clamp_tpl(y2, 0, metrics.hmapSize);

	// SetTerrainElevation asserts whole sectors for X1/Y1 and nSizeX/nSizeY, and rewrites every cell of every
	// touched sector, so the block must carry the baseline of the whole sector-aligned area.
	x1 = (x1 / sector) * sector;
	y1 = (y1 / sector) * sector;
	x2 = ((x2 + sector - 1) / sector) * sector;
	y2 = ((y2 + sector - 1) / sector) * sector;

	// ... and that nSizeX == nSizeY. The extra sectors of a non-square footprint are written with their own
	// baseline, so they end up bit identical; they only cost a sector mesh rebuild.
	int size = max(x2 - x1, y2 - y1);
	if (size <= 0 || size > metrics.hmapSize)
		return false;

	if (x1 + size > metrics.hmapSize) x1 = metrics.hmapSize - size;
	if (y1 + size > metrics.hmapSize) y1 = metrics.hmapSize - size;
	x1 = max(0, x1);
	y1 = max(0, y1);

	outX1 = x1;
	outY1 = y1;
	outSize = size;
	return true;
}

void CTerrainPlateComponent::RefreshBakeBaseline(const SBakeTerrainMetrics& metrics)
{
	const int dim = m_bakeRectSize + 1;
	const int priorDim = m_bakeBaselineSize + 1;
	const bool bHavePrior = (m_bakeBaselineSize > 0) && ((int)m_bakeBaseline.size() == priorDim * priorDim);

	std::vector<float> current((size_t)dim * dim);
	for (int ux = 0; ux < dim; ++ux)
	{
		for (int uy = 0; uy < dim; ++uy)
		{
			current[(size_t)ux * dim + uy] = SampleTerrainUnit(metrics, m_bakeRectX1 + ux, m_bakeRectY1 + uy);
		}
	}

	int adoptedCells = 0;
	int keptCells = 0;

	if (!bHavePrior)
	{
		// The one and only capture from ground nobody baked: pristine terrain, or the ground an external
		// editor left behind after this plate had given its own footprint back.
		m_bakeBaseline.swap(current);
	}
	else
	{
		// Cells with no stored float value - the plate moved and took in new ground - start from the terrain.
		std::vector<float> next(current);

		const int sectorUnits = max(1, metrics.sectorUnits);
		const int sectorsPerSide = max(1, m_bakeRectSize / sectorUnits);

		for (int sx = 0; sx < sectorsPerSide; ++sx)
		{
			for (int sy = 0; sy < sectorsPerSide; ++sy)
			{
				const int ux0 = sx * sectorUnits;
				const int uy0 = sy * sectorUnits;
				const int ux1 = min(ux0 + sectorUnits, dim - 1);
				const int uy1 = min(uy0 + sectorUnits, dim - 1);

				float fMin = FLT_MAX, fMax = -FLT_MAX;
				for (int ux = ux0; ux <= ux1; ++ux)
				{
					for (int uy = uy0; uy <= uy1; ++uy)
					{
						const float z = current[(size_t)ux * dim + uy];
						fMin = min(fMin, z);
						fMax = max(fMax, z);
					}
				}

				// Same shape as m_bakeProbeTolerance, but per sector: a height that only came back changed by
				// the sector's own 12 bit round trip is the ground WE wrote, a real sculpt is not.
				const float tolerance = max(0.002f, TerrainQuantStep(fMin, fMax) * 4.f);

				for (int ux = ux0; ux <= ux1; ++ux)
				{
					for (int uy = uy0; uy <= uy1; ++uy)
					{
						const int px = m_bakeRectX1 + ux - m_bakeBaselineX1;
						const int py = m_bakeRectY1 + uy - m_bakeBaselineY1;
						if (px < 0 || px >= priorDim || py < 0 || py >= priorDim)
							continue;   // outside the stored rect: the terrain reading already stands

						const size_t index = (size_t)ux * dim + uy;
						const float prior = m_bakeBaseline[(size_t)px * priorDim + py];

						if (!m_bBakeWriteLanded || fabs_tpl(current[index] - prior) <= tolerance)
						{
							// Keep the float; re-reading it is what ratchets the ground down. And when our OWN last
							// write never landed, what is under the plate now is whatever that refused write left
							// behind, not somebody's edit - adopting it would make the damage the new baseline and
							// let it survive Bake Into Terrain being switched off. @see VerifyBakeWrite.
							next[index] = prior;
							++keptCells;
						}
						else
						{
							++adoptedCells;   // a genuine edit under the plate: what is there now IS the ground
						}
					}
				}
			}
		}

		m_bakeBaseline.swap(next);
	}

	m_bakeBaselineX1 = m_bakeRectX1;
	m_bakeBaselineY1 = m_bakeRectY1;
	m_bakeBaselineSize = m_bakeRectSize;

	if (GetPlateDebugLevel() >= 2)
	{
		LogBakeBaselineDrift(adoptedCells, keptCells);
	}
}

void CTerrainPlateComponent::LogBakeBaselineDrift(int adoptedCells, int keptCells)
{
	const int refDim = m_bakeBaselineFirstSize + 1;

	if (m_bakeBaselineFirstSize <= 0 || (int)m_bakeBaselineFirst.size() != refDim * refDim)
	{
		m_bakeBaselineFirst = m_bakeBaseline;
		m_bakeBaselineFirstX1 = m_bakeBaselineX1;
		m_bakeBaselineFirstY1 = m_bakeBaselineY1;
		m_bakeBaselineFirstSize = m_bakeBaselineSize;

		CryLog("TerrainPlate '%s': baseline drift reference taken over units (%d,%d)+%d",
		       GetPlateName(), m_bakeBaselineX1, m_bakeBaselineY1, m_bakeBaselineSize);
		return;
	}

	const int dim = m_bakeRectSize + 1;
	float maxDrift = 0.f;
	int comparedCells = 0;

	for (int ux = 0; ux < dim; ++ux)
	{
		for (int uy = 0; uy < dim; ++uy)
		{
			const int px = m_bakeRectX1 + ux - m_bakeBaselineFirstX1;
			const int py = m_bakeRectY1 + uy - m_bakeBaselineFirstY1;
			if (px < 0 || px >= refDim || py < 0 || py >= refDim)
				continue;

			maxDrift = max(maxDrift, fabs_tpl(m_bakeBaseline[(size_t)ux * dim + uy]
			                                  - m_bakeBaselineFirst[(size_t)px * refDim + py]));
			++comparedCells;
		}
	}

	CryLog("TerrainPlate '%s': baseline drift %.4f mm over %d shared cells (%d kept, %d adopted from the terrain)",
	       GetPlateName(), maxDrift * 1000.f, comparedCells, keptCells, adoptedCells);
}

void CTerrainPlateComponent::ComputeBakeBlock(const SBakeTerrainMetrics& metrics, const Matrix34& worldTM,
                                              int& outCellsWritten, float& outMinDelta, float& outMaxDelta)
{
	const int dim = m_bakeRectSize + 1;
	m_bakeResult = m_bakeBaseline;

	outCellsWritten = 0;
	outMinDelta = 0.f;
	outMaxDelta = 0.f;

	// The crest of the relief this bake produces, filled in by the cell loop and consumed by ApplyBakeOffset.
	float maxRelief = 0.f;

	const Vec3 worldScale = GetWorldScale();

	// Falloff band, in the same measure the edge weld uses: distance to the nearest border as a fraction of the
	// half size. Never narrower than one terrain cell, or a falloff of 0 would step by the full relief there.
	const float minBandLocal = clamp_tpl(metrics.unitSize / max(0.0001f, 0.5f * min(worldScale.x, worldScale.y)), 0.f, 1.f);
	const float band = clamp_tpl(max((float)m_weldInnerFalloff, minBandLocal), 0.0001f, 1.f);

	// The terrain node takes the plate height AT THE NODE: GetPlateReliefAt is the mesh surface, and the terrain
	// is linear between its nodes as a plate quad spanning a cell is. The half cell neighbourhood is the guard
	// against the plate curving BELOW that line. Density follows the MESH grid, not the source resolution.
	const float meshSpacing = (m_lastGridX > 1 && m_lastGridY > 1)
	                          ? min(worldScale.x / (float)(m_lastGridX - 1), worldScale.y / (float)(m_lastGridY - 1))
	                          : metrics.unitSize;
	//
	// The floor rises from 2 to 4 while the edge antialiasing is on: the same grid is the coverage estimator and
	// 2 x 2 gives it only four levels. 4 x 4 is 16, finer than the 12 bit height quantisation can carry.
	const int   samplesPerAxisMin = (g_terrainPlateBakeEdgeAA != 0) ? 4 : 2;
	const int   samplesPerAxis = clamp_tpl((int)(metrics.unitSize / max(0.0001f, meshSpacing)) + 1, samplesPerAxisMin, 6);
	const float sampleStep = metrics.unitSize / (float)(samplesPerAxis - 1);

	for (int ux = 0; ux < dim; ++ux)
	{
		const float worldX = (float)(m_bakeRectX1 + ux) * metrics.unitSize;

		for (int uy = 0; uy < dim; ++uy)
		{
			const float worldY = (float)(m_bakeRectY1 + uy) * metrics.unitSize;

			float centreZ, localX, localY;
			if (!SamplePlateSurface(worldTM, worldX, worldY, centreZ, localX, localY))
				continue; // outside the rotated plate: this cell keeps the original ground

			float plateMin = centreZ;
			int   samplesTaken = 0;

			// The halo samples are kept so the slope correction below can run a second pass over them.
			// samplesPerAxis is clamped to 6, so 36 covers the whole grid.
			float sampleZ[36];
			float sampleDX[36];
			float sampleDY[36];
			int   sampleCount = 0;

			for (int sx = 0; sx < samplesPerAxis; ++sx)
			{
				for (int sy = 0; sy < samplesPerAxis; ++sy)
				{
					const float px = worldX - metrics.unitSize * 0.5f + (float)sx * sampleStep;
					const float py = worldY - metrics.unitSize * 0.5f + (float)sy * sampleStep;

					float z, ignoreX, ignoreY;
					if (SamplePlateSurface(worldTM, px, py, z, ignoreX, ignoreY))
					{
						plateMin = min(plateMin, z);

						if (sampleCount < (int)CRY_ARRAY_COUNT(sampleZ))
						{
							sampleZ[sampleCount] = z;
							sampleDX[sampleCount] = px - worldX;
							sampleDY[sampleCount] = py - worldY;
							++sampleCount;
						}

						++samplesTaken;
					}
				}
			}

				// The curvature guard, and the reason the gap no longer grows with the plate's scale. The plain minimum is
				// the plate height at the node minus the drop over half a terrain cell, a term that has nothing to do with
				// poking through, since the terrain tilts between its nodes exactly as the plate does. The gradient is a
				// least squares fit over the same neighbourhood, taken only when the cell is fully covered - then the grid
				// is symmetric, sum(dx) is zero and the fit reduces to the closed forms below.
			if (g_terrainPlateBakeSlopeCorrect != 0 && sampleCount > 1 && samplesTaken == samplesPerAxis * samplesPerAxis)
			{
				float sumXX = 0.f, sumYY = 0.f, sumXZ = 0.f, sumYZ = 0.f;
				for (int i = 0; i < sampleCount; ++i)
				{
					sumXX += sampleDX[i] * sampleDX[i];
					sumYY += sampleDY[i] * sampleDY[i];
					sumXZ += sampleDX[i] * sampleZ[i];
					sumYZ += sampleDY[i] * sampleZ[i];
				}

				const float gradX = (sumXX > 1e-6f) ? (sumXZ / sumXX) : 0.f;
				const float gradY = (sumYY > 1e-6f) ? (sumYZ / sumYY) : 0.f;

				float corrected = centreZ;
				for (int i = 0; i < sampleCount; ++i)
					corrected = min(corrected, sampleZ[i] - gradX * sampleDX[i] - gradY * sampleDY[i]);

				// Never above the plate height at the node, and never below what the plain Min filter would have
				// written: the correction can only give ground back, never take more.
				plateMin = clamp_tpl(corrected, plateMin, centreZ);
			}

			const size_t index = (size_t)ux * dim + uy;
			const float baseline = m_bakeBaseline[index];

				// The plate surface the terrain is asked to meet, kept the epsilon below the mesh so the two can never
				// fight.
			const float plateSurface = plateMin - GetBakeEpsilonMeters();

				// Bake Mode.
				//  * Raise:         max(baseline, plate). Only ever adds ground, so it is safe on someone else's level;
				//                   where the plate is below, the old ground pokes through it.
				//  * RaiseAndLower: the plate surface, above or below, so a modelled valley is cut into the terrain.
				// Lowering is gated on the cell being FULLY covered, or a cell at the edge of a rotated plate would dig
				// outside it. coverage is how much of THIS CELL the plate covers in plan, from the supersample grid.
			const int   totalSamples = samplesPerAxis * samplesPerAxis;
			const float coverage = (totalSamples > 0) ? clamp_tpl((float)samplesTaken / (float)totalSamples, 0.f, 1.f) : 0.f;

			const float raised = max(baseline, plateSurface);
			float combined = raised;

			if (m_bakeMode == ETerrainPlateBakeMode::RaiseAndLower)
				combined = raised + (plateSurface - raised) * coverage;

				// Falloff to the baseline over the rim band, so the border of the footprint is the baseline exactly. The
				// curve is the weld's smoothstep, not a cosine, or the two surfaces cross inside the band.
			const float distanceToBorder = min(0.5f - fabs_tpl(localX), 0.5f - fabs_tpl(localY)) * 2.f;
			const float t = clamp_tpl(distanceToBorder / band, 0.f, 1.f);
			float weight = (g_terrainPlateBakeWeldCurve != 0)
			               ? (t * t * (3.f - 2.f * t))            // smoothstep(t) == 1 - the weld's 1 - smoothstep(t)
			               : (1.f - cosf(gf_PI * t)) * 0.5f;      // the road falloff (RoadObject.cpp)

				// The plan view boundary is a coverage, not a boolean. Scaling the rim weight by it is the same blend
				// the falloff is, so it can never leave terrain above the plate mesh.
			if (g_terrainPlateBakeEdgeAA != 0)
				weight *= coverage;

			const float value = baseline + (combined - baseline) * weight;

			maxRelief = max(maxRelief, value - baseline);

			m_bakeResult[index] = value;
		}
	}

	ApplyBakeOffset(maxRelief);

	ClampBakeResultToTerrainRange();

	// Deliberately after the offset pass: what it reports is what WriteTerrainBlock is about to put in the ground.
	for (size_t index = 0; index < m_bakeResult.size(); ++index)
	{
		const float delta = m_bakeResult[index] - m_bakeBaseline[index];

		if (fabs_tpl(delta) > 0.0001f)
		{
			++outCellsWritten;
			outMinDelta = min(outMinDelta, delta);
			outMaxDelta = max(outMaxDelta, delta);
		}
	}
}

//! Bake Offset. NOT a flat additive offset: it leaves the welded seam alone and lifts the MAXIMUM, so the shape
//! is scaled rather than translated.
//!   height(cell) += BakeOffset * saturate((bakedHeight(cell) - baseline(cell)) / maxRelief)
//! At the rim the relief is 0, so the term is 0 and the border of the footprint IS still the baseline with no
//! rim weighting of its own; at the crest it is the full Bake Offset, hence "metres at the highest point".
//! saturate() drops cells the bake LOWERED, and in Raise mode the result is floored at the baseline so a
//! negative offset cannot turn a mode that only adds ground into one that digs.
void CTerrainPlateComponent::ApplyBakeOffset(float maxRelief)
{
	const float offset = (float)m_bakeOffset;

	// A footprint the bake did not raise anywhere has no crest to scale against, and relief / maxRelief would be
	// a division by zero. The offset has nothing to act on there - which is also the 0 default's path.
	if (fabs_tpl(offset) < 0.0001f || maxRelief < 0.0001f)
		return;

	for (size_t index = 0; index < m_bakeResult.size(); ++index)
	{
		const float baseline = m_bakeBaseline[index];
		const float relief = m_bakeResult[index] - baseline;
		const float factor = clamp_tpl(relief / maxRelief, 0.f, 1.f);

		if (factor <= 0.f)
			continue;

		float value = m_bakeResult[index] + offset * factor;

		if (m_bakeMode == ETerrainPlateBakeMode::Raise)
			value = max(value, baseline);

		m_bakeResult[index] = value;
	}
}

//! CRYENGINE terrain cannot store a height below 0. CTerrain::SetTerrainElevation derives the sector quantiser
//! as fOffset = max(0, blockMin - TERRAIN_DEFORMATION_MAX_DEPTH) and then packs every cell as
//! (height - fOffset) / fRange TRUNCATED INTO A 12 BIT FIELD (terran_edit.cpp), so a negative height does not
//! clamp - it wraps to a huge positive one. That is a spike of terrain thousands of metres tall, the probe read
//! back never matches what we asked for, the plate re-applies itself forever, and RefreshBakeBaseline eventually
//! adopts the wreckage as "the ground", which is why it survives switching Bake Into Terrain off again.
//! So the request is floored HERE, before the write and before the probes are built from m_bakeResult, and the
//! plate says once, in plain words, that its floor was cut off. There is no matching ceiling: fRange is derived
//! from the block's own maximum, so any positive height is representable (resolution is WarnOnBakeQuantisation's
//! job).
void CTerrainPlateComponent::ClampBakeResultToTerrainRange()
{
	float lowest = 0.f;

	for (float& value : m_bakeResult)
	{
		if (value < 0.f)
		{
			lowest = min(lowest, value);
			value = 0.f;
		}
	}

	if (lowest >= 0.f)
	{
		// A bake that stays above 0 is allowed to warn again if it is ever moved back under it.
		m_bBakeFloorWarningIssued = false;
		return;
	}

	if (!m_bBakeFloorWarningIssued)
	{
		m_bBakeFloorWarningIssued = true;
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Terrain Plate '%s': the plate digs down to %.2f m, but terrain cannot go below 0 m, so the baked "
		           "ground was flattened at 0 m where the plate goes deeper. Move the plate up, reduce Ground Level "
		           "or reduce the Z scale so its lowest point stays above 0 m.",
		           GetPlateName(), lowest);
	}
}

bool CTerrainPlateComponent::WriteTerrainBlock(const SBakeTerrainMetrics& metrics, const std::vector<float>& heights)
{
	ITerrain* pTerrain = (gEnv != nullptr && gEnv->p3DEngine != nullptr) ? gEnv->p3DEngine->GetITerrain() : nullptr;
	if (pTerrain == nullptr)
	{
		LogBakeSkip("write-no-terrain");
		return false;
	}

	if (m_bakeRectSize <= 0)
	{
		LogBakeSkip("write-empty-rect");
		return false;
	}

	const int dim = m_bakeRectSize + 1;
	if ((int)heights.size() != dim * dim)
	{
		LogBakeSkip("write-size-mismatch");
		return false;
	}

	// Argument mapping, traced in terran_edit.cpp rather than taken from the doc comment:
	//  - X1 / Y1 / nSizeX / nSizeY are terrain UNITS and must be whole sectors;
	//  - the block is indexed pTerrainBlock[ux * nHmapSize + uy], ux the world X unit and uy the world Y unit.
	//    The editor's caller passes (y1, x1) because CHeightmap stores its array transposed, NOT because the
	//    engine's axes are swapped;
	//  - the sector loop reads x from x1 to x2 INCLUSIVE, hence dim = size + 1;
	//  - only rows X1 .. X1 + nSize are read, so only those are allocated and the pointer is biased back by X1
	//    rows, the stride still the full nHmapSize - what keeps a 3-sector plate at 3 MB, not 256 MB;
	//  - pSurfaceData = nullptr is the heights-only contract, so every cell keeps its surface ids, weights and
	//    hole flag and the nSurf* arguments are ignored; pResolMap's only product is never read, so NULL.
	std::vector<float> block((size_t)dim * metrics.hmapSize, 0.f);

	// Reads past the terrain are clamped to nHmapSize - 1, so a rect that ends exactly on the terrain border
	// simply does not need its last column.
	const int copyCols = min(dim, metrics.hmapSize - m_bakeRectY1);
	if (copyCols <= 0)
	{
		LogBakeSkip("write-off-terrain");
		return false;
	}

	for (int ux = 0; ux < dim; ++ux)
	{
		memcpy(&block[(size_t)ux * metrics.hmapSize + m_bakeRectY1], &heights[(size_t)ux * dim], sizeof(float) * copyCols);
	}

	// The bias is applied to the integer value, not to the pointer: block.data() - X1 * hmapSize would form a
	// pointer before the allocation, which is undefined behaviour even though nothing ever dereferences it (the
	// callee only touches rows X1 .. X1 + nSize, all inside the buffer). Same address, no UB.
	const uintptr_t blockAddr = reinterpret_cast<uintptr_t>(block.data());
	float* const    pBlock = reinterpret_cast<float*>(blockAddr - (uintptr_t)((size_t)m_bakeRectX1 * metrics.hmapSize * sizeof(float)));

	pTerrain->SetTerrainElevation(m_bakeRectX1, m_bakeRectY1, m_bakeRectSize, m_bakeRectSize,
	                              pBlock, nullptr, 0, 0, 0, 0, nullptr, 0, 0);
	return true;
}

AABB CTerrainPlateComponent::GetBakeWorldBox(const SBakeTerrainMetrics& metrics) const
{
	AABB box;
	box.min.Set((float)m_bakeRectX1 * metrics.unitSize, (float)m_bakeRectY1 * metrics.unitSize, FLT_MAX);
	box.max.Set((float)(m_bakeRectX1 + m_bakeRectSize) * metrics.unitSize, (float)(m_bakeRectY1 + m_bakeRectSize) * metrics.unitSize, -FLT_MAX);

	for (float z : m_bakeBaseline)
	{
		box.min.z = min(box.min.z, z);
		box.max.z = max(box.max.z, z);
	}
	for (float z : m_bakeResult)
	{
		box.min.z = min(box.min.z, z);
		box.max.z = max(box.max.z, z);
	}

	if (box.min.z > box.max.z)
	{
		box.min.z = 0.f;
		box.max.z = 0.f;
	}

	return box;
}

void CTerrainPlateComponent::AfterBakeInvalidation(const AABB& box)
{
	if (gEnv == nullptr || gEnv->p3DEngine == nullptr)
		return;

	// SetTerrainElevation rebuilds the sector meshes, atlases, physics heightfield and roads in the box, but it
	// does NOT touch decals, and a terrain decal keeps the mesh it was built with.
	AABB decalBox = box;
	gEnv->p3DEngine->DeleteDecalsInRange(&decalBox, nullptr);

	// The terrain silhouette moved, so the cached cascades and the HeightMap AO have to be rebuilt even
	// when the plate mesh itself did not change (toggling Bake Into Terrain does exactly that).
	InvalidateShadowCache();

	// At export GenerateAiAll runs after the heightmap export, so the shipped navmesh is correct for free. In the
	// editor the only path is CHeightmap::UpdateEngineTerrain -> OnTerrainModified, which we never go through.
	if (gEnv->pAISystem != nullptr)
	{
		if (INavigationSystem* pNavigationSystem = gEnv->pAISystem->GetNavigationSystem())
		{
			if (INavigationUpdatesManager* pUpdateManager = pNavigationSystem->GetUpdateManager())
			{
				AABB navBox = box;
				navBox.Expand(Vec3(2.f, 2.f, 2.f));
				pUpdateManager->WorldChanged(navBox);
			}
		}
	}
}

void CTerrainPlateComponent::RestoreBakeBaseline()
{
	if (!m_bBakeApplied)
		return;

	m_bBakeApplied = false;

	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (metrics.IsValid() && !m_bakeBaseline.empty())
	{
		const AABB box = GetBakeWorldBox(metrics);
		if (WriteTerrainBlock(metrics, m_bakeBaseline))
		{
			AfterBakeInvalidation(box);

			if (GetPlateDebugLevel() >= 1)
			{
				CryLog("TerrainPlate '%s': original ground restored over units (%d,%d)+%d",
				       GetPlateName(), m_bakeRectX1, m_bakeRectY1, m_bakeRectSize);
			}
		}
	}

	m_bakeResult.clear();
	m_bakeProbes.clear();
}

void CTerrainPlateComponent::RestoreBakeOnTeardown()
{
	if (m_bBakeTornDown)
		return;

	m_bBakeTornDown = true;

	// A delete changes the configuration the runaway valve latched on, so it counts as the user acting.
	NoteBakeUserAction();

	// Out of the registry FIRST, so nothing can start an overlap cascade on a plate on its way out. Out of the
	// move batch with it, or the batch would hold a pointer to a component being destructed.
	UnregisterPlate();
	LeaveBakeMoveBatch();

	// The one diagnostic the delete path never had. "no live bake" here says the plate believed it had nothing in
	// the terrain, which is the only way a delete can leave the bake behind.
	if (GetPlateDebugLevel() >= 1)
	{
		CryLog("TerrainPlate '%s': teardown, %s", GetPlateName(),
		       m_bBakeApplied ? "restoring the original ground over the footprint" : "no live bake to restore");
	}

	const int oldX1 = m_bakeRectX1;
	const int oldY1 = m_bakeRectY1;
	const int oldSize = m_bakeRectSize;
	const bool bHadBake = m_bBakeApplied;

	// A lone RestoreBakeBaseline() is only correct when nothing is stacked ON TOP of this footprint. Deleting a
	// GROUP tears every member down in ONE call, in m_children order and in one frame, with no tick in between.
	if (!RestoreBakeOnTeardownCascaded())
	{
		RestoreBakeBaseline();
	}

	if (!bHadBake || oldSize <= 0)
		return;

	// Any plate that was baking on top of this one now sits on ground that just moved. This is a nudge that
	// brings their next poll forward, not a second cascade.
	for (CTerrainPlateComponent* pOther : g_terrainPlates)
	{
		if (pOther == this || !pOther->m_bBakeApplied || pOther->m_bakeRectSize <= 0)
			continue;

		const bool bOverlaps = (oldX1 < pOther->m_bakeRectX1 + pOther->m_bakeRectSize)
		                       && (pOther->m_bakeRectX1 < oldX1 + oldSize)
		                       && (oldY1 < pOther->m_bakeRectY1 + pOther->m_bakeRectSize)
		                       && (pOther->m_bakeRectY1 < oldY1 + oldSize);
		if (bOverlaps)
		{
			pOther->m_lastBakeProbeFrameId = gEnv->nMainFrameID - (uint32)max(1, GetBakeProbeFrames());
		}
	}
}

bool CTerrainPlateComponent::RestoreBakeOnTeardownCascaded()
{
	// Nothing in the ground, or we are already inside a cascade that owns the ordering. Plain restore.
	if (!m_bBakeApplied || m_bakeRectSize <= 0 || g_bakeCascadeDepth > 0)
		return false;

	if (gEnv == nullptr || gEnv->p3DEngine == nullptr || gEnv->p3DEngine->GetITerrain() == nullptr)
		return false;

	// Who else is standing on the footprint we are about to hand back, transitively. This plate is already out of
	// the registry and m_bBakeTornDown is set, so it cannot collect itself: the pass is unwind-only.
	std::vector<int> rects;
	rects.push_back(m_bakeRectX1);
	rects.push_back(m_bakeRectY1);
	rects.push_back(m_bakeRectSize);

	std::vector<CTerrainPlateComponent*> affected;
	CollectBakeCascadeSetFromRects(rects, affected);

	// The stack, not the overlap, is what decides. Our stored baseline is the ground as it was when WE baked, so
	// it is still the truth for every plate that baked BEFORE us, and a lie only about those that baked AFTER:
	// restoring it would resurrect their relief as ground nobody owns. So the unwind is owed exactly then.
	bool bStacked = false;
	for (const CTerrainPlateComponent* pPlate : affected)
	{
		bStacked = bStacked || (pPlate->m_bBakeApplied && pPlate->m_bakeApplySeq > m_bakeApplySeq);
	}

	if (!bStacked)
		return false;   // B2, the single plate delete: the plain restore already is the whole answer

	// One ordered pass: every member and this plate restore in DESCENDING m_bakeApplySeq - the exact inverse of
	// the write order - then the survivors regenerate and re-bake in ascending Bake Priority.
	RunBakeCascade(affected, nullptr, "teardown unwind", this);
	return true;
}

bool CTerrainPlateComponent::GetCommitFootprint(int& outX1, int& outY1, int& outSize) const
{
	// Only a plate that currently holds a bake in the engine terrain has something to commit: the editor reads
	// the block straight back out of the terrain, so what it stamps is literally what the live bake wrote.
	if (!m_bBakeApplied || m_bakeRectSize <= 0)
		return false;

	outX1 = m_bakeRectX1;
	outY1 = m_bakeRectY1;
	outSize = m_bakeRectSize;
	return true;
}

bool CTerrainPlateComponent::GetCommitAppearanceFootprint(int& outX1, int& outY1, int& outSize) const
{
	// Deliberately NOT m_bakeRect*: the appearance stamp must also work on a plate that never baked and on one
	// baking now. The rect it wants is what ComputeBakeRect answers from the current transform.
	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (!metrics.IsValid())
		return false;

	return ComputeBakeRect(metrics, GetPlateFrameTM(), outX1, outY1, outSize);
}

float CTerrainPlateComponent::GetCommitWeightAtWorld(float worldX, float worldY, float sampleSizeMeters) const
{
	// THE rim weight, and why colour, layer weights and height fade together: the very expression
	// ComputeBakeBlock evaluates per cell. Only the coverage term is an area measure, over sampleSizeMeters.
	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (!metrics.IsValid())
		return 0.f;

	const Matrix34 worldTM = GetPlateFrameTM();

	float centreZ = 0.f, localX = 0.f, localY = 0.f;
	if (!SamplePlateSurface(worldTM, worldX, worldY, centreZ, localX, localY))
		return 0.f;   // outside the rotated plate, exactly as the bake decides it per cell

	const Vec3 worldScale = GetWorldScale();

	// Same band as ComputeBakeBlock, including the one-cell minimum: a falloff of 0 must still meet the
	// baseline at the border rather than step to it.
	const float minBandLocal = clamp_tpl(metrics.unitSize / max(0.0001f, 0.5f * min(worldScale.x, worldScale.y)), 0.f, 1.f);
	const float band = clamp_tpl(max((float)m_weldInnerFalloff, minBandLocal), 0.0001f, 1.f);

	const float distanceToBorder = min(0.5f - fabs_tpl(localX), 0.5f - fabs_tpl(localY)) * 2.f;
	const float t = clamp_tpl(distanceToBorder / band, 0.f, 1.f);
	float weight = (g_terrainPlateBakeWeldCurve != 0)
	               ? (t * t * (3.f - 2.f * t))            // smoothstep(t), the mesh weld's 1 - smoothstep(t) inverted
	               : (1.f - cosf(gf_PI * t)) * 0.5f;      // the road falloff

	// The plan view boundary is a coverage, not a boolean, reproduced with the bake's own supersample grid - so
	// at sampleSizeMeters == unitSize this is bit for bit what ComputeBakeBlock computed for that cell.
	if (g_terrainPlateBakeEdgeAA != 0 && sampleSizeMeters > 0.f)
	{
		const float meshSpacing = (m_lastGridX > 1 && m_lastGridY > 1)
		                          ? min(worldScale.x / (float)(m_lastGridX - 1), worldScale.y / (float)(m_lastGridY - 1))
		                          : metrics.unitSize;
		const int   samplesPerAxis = clamp_tpl((int)(sampleSizeMeters / max(0.0001f, meshSpacing)) + 1, 4, 6);
		const float sampleStep = sampleSizeMeters / (float)(samplesPerAxis - 1);

		int samplesTaken = 0;
		for (int sx = 0; sx < samplesPerAxis; ++sx)
		{
			for (int sy = 0; sy < samplesPerAxis; ++sy)
			{
				const float px = worldX - sampleSizeMeters * 0.5f + (float)sx * sampleStep;
				const float py = worldY - sampleSizeMeters * 0.5f + (float)sy * sampleStep;

				float z, ignoreX, ignoreY;
				if (SamplePlateSurface(worldTM, px, py, z, ignoreX, ignoreY))
					++samplesTaken;
			}
		}

		const int totalSamples = samplesPerAxis * samplesPerAxis;
		weight *= clamp_tpl((float)samplesTaken / (float)totalSamples, 0.f, 1.f);
	}

	return clamp_tpl(weight, 0.f, 1.f);
}

//! Rounds up to a power of two CTexture::GetLowResSystemCopy can serve: its slots are 2^(slot + 4), 16 .. 2048,
//! and anything between snaps DOWN. 512 is the ceiling - one slot is a permanent copy in system RAM.
static int RoundUpColourCopySize(int desired)
{
	int size = 32;
	while (size < desired && size < 512)
		size *= 2;
	return size;
}

//! Resolves, at most once per frame, everything the per texel sampler needs. @see SColourSource.
bool CTerrainPlateComponent::ResolveColourSource(float texelSizeMeters) const
{
	const int frameId = (gEnv != nullptr) ? (int)gEnv->nMainFrameID : 0;

	// RESOLUTION. The CPU copy is a mip chain pick, so ask for as many texels as the plate covers on the terrain;
	// the stock 32 x 32 is a five times upscale on a 40 m plate. Both consumers pass the same texel size.
	const Vec3  worldScale = GetWorldScale();
	const float footprint = max(fabs_tpl(worldScale.x), fabs_tpl(worldScale.y));
	const float texel = (texelSizeMeters > 0.f) ? texelSizeMeters : 0.5f;
	const int   maxTexSize = RoundUpColourCopySize((int)ceilf(footprint / texel));

	if (m_colourSource.frameId == frameId && m_colourSource.maxTexSize == maxTexSize)
		return m_colourSource.bValid;

	const void* pPreviousTexture = m_colourSource.pTextureKey;
	const bool  bPreviousLinear = m_colourSource.bTexelsLinear;
	const bool  bPreviousWarned = m_colourSource.bWarned;

	m_colourSource = SColourSource();
	m_colourSource.frameId = frameId;
	m_colourSource.maxTexSize = maxTexSize;

	if (gEnv == nullptr || gEnv->p3DEngine == nullptr)
		return false;

	IMaterialManager* pMaterialManager = gEnv->p3DEngine->GetMaterialManager();
	if (pMaterialManager == nullptr)
		return false;

	IMaterial* pMaterial = nullptr;
	if (!m_materialPath.value.empty())
		pMaterial = pMaterialManager->LoadMaterial(m_materialPath.value, false);
	if (pMaterial == nullptr && m_pStatObj != nullptr)
		pMaterial = m_pStatObj->GetMaterial();
	if (pMaterial == nullptr)
		return false;

	// A multi sub material has no shader item of its own worth sampling; take the first sub material, which is
	// what a plate built from one grid mesh actually renders with.
	if (pMaterial->GetSubMtlCount() > 0 && pMaterial->GetSubMtl(0) != nullptr)
		pMaterial = pMaterial->GetSubMtl(0);

	const SShaderItem& shaderItem = pMaterial->GetShaderItem();
	if (shaderItem.m_pShaderResources == nullptr)
		return false;

	// The material's diffuse COLOUR. The shader multiplies it over the linear diffuse texture sample, so this is
	// a linear multiplier and it is applied in linear below.
	m_colourSource.diffuse = shaderItem.m_pShaderResources->GetColorValue(EFTT_DIFFUSE);

	SEfResTexture* pResTexture = shaderItem.m_pShaderResources->GetTexture(EFTT_DIFFUSE);
	ITexture*      pTexture = (pResTexture != nullptr) ? pResTexture->m_Sampler.m_pITex : nullptr;

	// The material's texture modifier, so a material that tiles its diffuse over the mesh tiles it over the
	// terrain in exactly the same places. m_Tiling is 1 by default.
	if (pResTexture != nullptr && pResTexture->m_Ext.m_pTexModifier != nullptr)
	{
		const SEfTexModificator& modificator = *pResTexture->m_Ext.m_pTexModifier;
		m_colourSource.tilingU = (modificator.m_Tiling[0] != 0.f) ? modificator.m_Tiling[0] : 1.f;
		m_colourSource.tilingV = (modificator.m_Tiling[1] != 0.f) ? modificator.m_Tiling[1] : 1.f;
		m_colourSource.offsetU = modificator.m_Offs[0];
		m_colourSource.offsetV = modificator.m_Offs[1];
	}

	if (pTexture == nullptr)
	{
		// No texture at all: the material IS its diffuse colour. Black means nothing to project.
		m_colourSource.bValid = (m_colourSource.diffuse.r > 0.f || m_colourSource.diffuse.g > 0.f
		                         || m_colourSource.diffuse.b > 0.f);
		return m_colourSource.bValid;
	}

	m_colourSource.pTextureKey = pTexture;
	m_colourSource.bWarned = (pTexture == pPreviousTexture) ? bPreviousWarned : false;

	uint16 textureWidth = 0, textureHeight = 0;
	int*   pLowResSystemCopyAtlasId = nullptr;
	const ColorB* pTexRgb = pTexture->GetLowResSystemCopy(textureWidth, textureHeight,
	                                                      &pLowResSystemCopyAtlasId, maxTexSize);

	if (pTexRgb == nullptr || textureWidth == 0 || textureHeight == 0)
	{
			// GetLowResSystemCopy only serves mipped 2D BC1..BC7 textures. Anything else has no CPU readable copy at
			// all, and there is no flat colour to fall back on, so say so once per texture and leave the terrain alone.
		if (!m_colourSource.bWarned)
		{
			m_colourSource.bWarned = true;
			CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
			           "Terrain Plate: the diffuse texture '%s' has no CPU readable copy (only mipped 2D BC1..BC7 textures have "
			           "one), so no terrain colour can be taken from this plate.", pTexture->GetName());
		}
		return false;
	}

	m_colourSource.pTexels = pTexRgb;
	m_colourSource.width = (int)textureWidth;
	m_colourSource.height = (int)textureHeight;

	// COLOUR SPACE. PrepareLowResSystemCopy converts the decompressed texels to linear when the source DDS is
	// flagged sRGB, which every RC diffuse map is - so the CPU copy of an sRGB texture is already linear and must
	// not be srgb2rgb'd again. ITexture exposes no sRGB accessor, so ask the image file as the renderer does.
	if (pTexture == pPreviousTexture)
	{
		m_colourSource.bTexelsLinear = bPreviousLinear;
	}
	else if (gEnv->pRenderer != nullptr)
	{
		_smart_ptr<IImageFile> pImage = gEnv->pRenderer->EF_LoadImage(pTexture->GetName(), FIM_STREAM_PREPARE);
		m_colourSource.bTexelsLinear = (pImage != nullptr) && ((pImage->mfGet_Flags() & FIM_SRGB_READ) != 0);
	}

	m_colourSource.bValid = true;
	return true;
}

bool CTerrainPlateComponent::GetCommitAlbedoAtWorld(float worldX, float worldY, float texelSizeMeters,
                                                    ColorF& outLinearColor) const
{
	// THE colour source: the plate material's DIFFUSE texture through ITexture::GetLowResSystemCopy, the same
	// source CRYENGINE's own terrain colour synthesis uses (CImagePainter). GetData32 would stall on a staging
	// download per call and reading the DDS by hand needs a BC1..BC7 decompressor, so only the cap differs.
	if (!ResolveColourSource(texelSizeMeters))
		return false;

	// Where on the plate: the same inverse transform the bake uses, then the mesh's own UV mapping, with the
	// mirror flags applied to the SAMPLING coordinate exactly as RegenerateMesh does.
	float centreZ = 0.f, localX = 0.f, localY = 0.f;
	if (!SamplePlateSurface(GetPlateFrameTM(), worldX, worldY, centreZ, localX, localY))
		return false;

	float u = clamp_tpl(localX + 0.5f, 0.f, 1.f);
	float v = clamp_tpl(0.5f - localY, 0.f, 1.f);
	if (m_bMirrorX) u = 1.f - u;
	if (m_bMirrorY) v = 1.f - v;

	const SColourSource& source = m_colourSource;

	// The material's own tiling and offset, applied after the mesh UV - the same order the shader applies them
	// in, so the pattern lands where the plate mesh shows it.
	u = u * source.tilingU + source.offsetU;
	v = v * source.tilingV + source.offsetV;

	ColorF albedo(1.f, 1.f, 1.f, 1.f);

	if (source.pTexels != nullptr)
	{
		// Bilinear, wrapping.
		const float fx = u * (float)source.width - 0.5f;
		const float fy = v * (float)source.height - 0.5f;
		const int   x0 = (int)floorf(fx);
		const int   y0 = (int)floorf(fy);
		const float rx = fx - (float)x0;
		const float ry = fy - (float)y0;

		const auto texel = [&source](int x, int y)
		{
			const int wx = ((x % source.width) + source.width) % source.width;
			const int wy = ((y % source.height) + source.height) % source.height;
			const ColorB& c = source.pTexels[wx + wy * source.width];
			return ColorF((float)c.r / 255.f, (float)c.g / 255.f, (float)c.b / 255.f, 1.f);
		};

		const ColorF top = texel(x0, y0) * (1.f - rx) + texel(x0 + 1, y0) * rx;
		const ColorF bottom = texel(x0, y0 + 1) * (1.f - rx) + texel(x0 + 1, y0 + 1) * rx;
		albedo = top * (1.f - ry) + bottom * ry;

		// Into linear, once. An sRGB source is already linear in the CPU copy; anything else is raw.
		if (!source.bTexelsLinear)
			albedo.srgb2rgb();
	}

	albedo.r *= source.diffuse.r;
	albedo.g *= source.diffuse.g;
	albedo.b *= source.diffuse.b;
	albedo.a = 1.f;

	outLinearColor = albedo;
	return true;
}

bool CTerrainPlateComponent::IsLiveColourEnabled() const
{
	// The Bake drawer holds the tick box, so a plate whose drawer is gone could not switch the colour off again.
	// @see the free Serialize for STerrainPlateBakeGroup, which opens the drawer on the same condition.
	return m_bLiveTerrainColour && m_bBakeIntoTerrain;
}

bool CTerrainPlateComponent::IsColourStamped() const
{
	if (!m_bColourStamped)
		return false;

	// The record only stands in for the preview while the plate would draw the SAME picture, so it is checked
	// against what the composite actually depends on: the plate frame and the material. Compared rather than
	// cleared on an event, because no event fires for a plate that is merely loaded and the record has to
	// survive a level load - the ground keeps the colour, so the preview has to keep standing down.
	const Matrix34   frameTM = GetPlateFrameTM();
	const IMaterial* pMaterial = GetPlateMaterial();

	if (!m_bColourStampRefValid)
	{
		// First query since the stamp or since the level loaded: take the reference, trust the record.
		m_colourStampFrameTM = frameTM;
		m_pColourStampMaterial = pMaterial;
		m_bColourStampRefValid = true;
		return true;
	}

	// The pointer is only ever compared, never dereferenced, so a released material cannot be followed here.
	if (!frameTM.IsEquivalent(m_colourStampFrameTM, 0.0001f) || pMaterial != m_pColourStampMaterial)
	{
		ClearColourStamp();
		return false;
	}

	return true;
}

void CTerrainPlateComponent::ClearColourStamp() const
{
	// The property tree can report a property change right after an action button was pressed in it, and that
	// is the button, not the user editing the plate. Anything else is at least a frame later.
	if (m_bColourStamped && gEnv != nullptr && (gEnv->nMainFrameID - m_colourStampFrameId) <= 1)
		return;

	// The stamped ground colour is no longer what this plate would draw, so the preview takes over again -
	// over ground that still carries the old stamp, which is visible and is the honest answer: the picture
	// always shows what the plate would draw NOW. Deliberately not called from RegenerateMesh: the bake's own
	// self repair and the overlap cascade go through it without the plate having changed at all.
	m_bColourStamped = false;
	m_bColourStampRefValid = false;
}

//! The live terrain colour overlay, READ ONLY throughout: what it paints is a cache (the engine's terrain colour
//! atlas) whose truth is the level's own CRGBLayer, so there is nothing to capture or restore.
bool CTerrainPlateComponent::GetLiveColourState(int& outX1, int& outY1, int& outSize, ColorB& outTint,
                                                bool& outSettling) const
{
	if (!IsLiveColourEnabled())
		return false;

	// The ground already carries exactly this colour (Stamp Colour wrote it). Blending it over itself would
	// tighten the rim - lerp(dest, C, w) only equals dest where dest is already C - so the preview and the
	// export both stand down here, and the picture comes out of CRGBLayer unchanged. @see OnColourStamped.
	if (IsColourStamped())
		return false;

	if (!GetCommitAppearanceFootprint(outX1, outY1, outSize) || outSize <= 0)
		return false;

	// The plate's diffuse, unfiltered. The tint knob is gone: what is stamped has to be what is shown, and a
	// second colour the stamp would have to reproduce is one more thing that can drift.
	outTint = ColorB(255, 255, 255, 255);

	// The debounce input. A dirty terrain texture sector is a whole tile DXT recompress plus an upload, so a drag
	// must not repaint per frame; these are the flags the plate's own deferred rebuild rides on.
	outSettling = m_bDeferredXformPending || m_bBakeMoveBatchPending || m_bRegenPending;
	return true;
}

void CTerrainPlateComponent::OnHeightStamped()
{
	// The ONLY state a height stamp changes, and it changes nothing the user can see. The stamped ground becomes
	// this plate's baseline, so: re-applying the bake on top of it writes the same heights again (max(baseline,
	// relief) == baseline here), the restore on a move or a delete hands back the stamped ground instead of the
	// old one, and the probes - which expect m_bakeResult, still what the terrain holds - keep passing. Every
	// toggle, the live footprint and m_bBakeApplied are deliberately left exactly as they were.
	if (m_bakeResult.empty())
		return;

	m_bakeBaseline = m_bakeResult;
	// m_bakeResult is the live rect's block, so the baseline's own rect follows it. @see RefreshBakeBaseline.
	m_bakeBaselineX1 = m_bakeRectX1;
	m_bakeBaselineY1 = m_bakeRectY1;
	m_bakeBaselineSize = m_bakeRectSize;

	// Debug only drift reference. Re-take it against the adopted baseline, or every later line would report the
	// whole relief as drift and read like a fault. @see LogBakeBaselineDrift.
	m_bakeBaselineFirst.clear();
	m_bakeBaselineFirstSize = 0;

	CryLog("TerrainPlate '%s': the baked relief is now part of the level heightmap, and stays there if the plate "
	       "moves or goes. The plate keeps working exactly as before. Undo puts the terrain back.", GetPlateName());
}

void CTerrainPlateComponent::OnColourStamped()
{
	// The only state a colour stamp changes, and it changes nothing the user can see: the ground now holds the
	// very texels the preview drew, so the preview and the export stop drawing them a second time over
	// themselves. No tick box moves. Any edit of the plate clears it. @see ClearColourStamp, GetLiveColourState.
	m_bColourStamped = true;
	m_bColourStampRefValid = false;   // re-taken by the next IsColourStamped, against the plate as it is now
	m_colourStampFrameId = (gEnv != nullptr) ? gEnv->nMainFrameID : 0;

	CryLog("TerrainPlate '%s': the plate colour is now part of the level's terrain colour, and stays there if the "
	       "plate moves or goes. Terrain Colour From Plate is untouched; the picture is unchanged.", GetPlateName());
}

void CTerrainPlateComponent::SetColourStampRecord(bool bStamped)
{
	if (bStamped)
	{
		OnColourStamped();
		return;
	}

	// Forced, unlike ClearColourStamp: an undo of the stamp restores the ground colour the tiles held before
	// it, so the record has to go with it whatever frame it is. Leaving it would keep the preview stood down
	// and the plate colour would simply be missing until the plate was nudged.
	m_bColourStamped = false;
	m_bColourStampRefValid = false;
	m_colourStampFrameId = 0;
}

void CTerrainPlateComponent::NudgeBakeAfterTerrainWrite(int rectX1, int rectY1, int rectSize)
{
	if (!m_bBakeApplied || m_bakeRectSize <= 0 || rectSize <= 0 || gEnv == nullptr)
		return;

	const bool bOverlaps = (rectX1 < m_bakeRectX1 + m_bakeRectSize) && (m_bakeRectX1 < rectX1 + rectSize)
	                       && (rectY1 < m_bakeRectY1 + m_bakeRectSize) && (m_bakeRectY1 < rectY1 + rectSize);
	if (!bOverlaps)
		return;

	// Same nudge RestoreBakeOnTeardown gives its neighbours: bring the next self repair poll forward instead of
	// re-baking from here, so the ordering stays PollBake's.
	m_lastBakeProbeFrameId = gEnv->nMainFrameID - (uint32)max(1, GetBakeProbeFrames());
}

bool CTerrainPlateComponent::VerifyBakeWrite(const SBakeTerrainMetrics& metrics)
{
	// ITerrain::SetTerrainElevation reports nothing back: an out of range rect only warns inside the engine, a
	// misaligned rect trips asserts a Profile build compiles out, and a Release Cry3DEngine has no function body
	// at all. So the only way to know the bake landed is to read the terrain back.
	const int dim = m_bakeRectSize + 1;
	if ((int)m_bakeResult.size() != dim * dim || (int)m_bakeBaseline.size() != dim * dim)
		return true;

	// The three cells this bake moved the most: if the write reached the terrain at all it reached those. A change
	// smaller than the sector quantisation step cannot be told apart from rounding, so it is not checked.
	int   cells[3] = { -1, -1, -1 };
	float deltas[3] = { 0.f, 0.f, 0.f };

	for (int i = 0; i < dim * dim; ++i)
	{
		const float delta = fabs_tpl(m_bakeResult[i] - m_bakeBaseline[i]);

		for (int slot = 0; slot < 3; ++slot)
		{
			if (delta > deltas[slot])
			{
				for (int k = 2; k > slot; --k)
				{
					deltas[k] = deltas[k - 1];
					cells[k] = cells[k - 1];
				}
				deltas[slot] = delta;
				cells[slot] = i;
				break;
			}
		}
	}

	int checked = 0;
	int landed = 0;

	for (int slot = 0; slot < 3; ++slot)
	{
		if (cells[slot] < 0 || deltas[slot] <= m_bakeProbeTolerance)
			continue;

		const int   ux = cells[slot] / dim;
		const int   uy = cells[slot] % dim;
		const float expected = m_bakeResult[cells[slot]];
		const float actual = SampleTerrainUnit(metrics, m_bakeRectX1 + ux, m_bakeRectY1 + uy);
		const bool  bLanded = fabs_tpl(actual - expected) <= m_bakeProbeTolerance;

		++checked;
		landed += bLanded ? 1 : 0;

		if (GetPlateDebugLevel() >= 1)
		{
			CryLog("TerrainPlate '%s': read back unit (%d,%d) baseline %.3f, wrote %.3f, terrain has %.3f (tolerance %.3f) - %s",
			       GetPlateName(), m_bakeRectX1 + ux, m_bakeRectY1 + uy,
			       m_bakeBaseline[cells[slot]], expected, actual, m_bakeProbeTolerance,
			       bLanded ? "OK" : "NOT WRITTEN");
		}
	}

	// A write that did not land must not be mistaken for ground somebody else edited. @see RefreshBakeBaseline.
	m_bBakeWriteLanded = !(checked > 0 && landed == 0);

	if (checked > 0 && landed == 0)
	{
		if (!m_bBakeWriteWarningIssued)
		{
			m_bBakeWriteWarningIssued = true;
			CryWarning(VALIDATOR_MODULE_3DENGINE, VALIDATOR_WARNING,
			           "Terrain Plate '%s': Bake Into Terrain wrote %d x %d terrain units at (%d,%d) but the terrain did not move. "
			           "SetTerrainElevation reports nothing back, so the cause is one of: Cry3DEngine built as Release (its body is "
			           "#ifndef _RELEASE), the rect not sector aligned (sector = %d units), or the rect outside the terrain "
			           "(%d units). Run e_TerrainPlateDebug 1 for the per cell read back.",
			           GetPlateName(), m_bakeRectSize, m_bakeRectSize, m_bakeRectX1, m_bakeRectY1,
			           metrics.sectorUnits, metrics.hmapSize);
		}
		return false;
	}

	// A later bake that lands again is allowed to warn again if it ever stops landing.
	m_bBakeWriteWarningIssued = false;
	return true;
}

void CTerrainPlateComponent::DrawBakeDebug() const
{
	if (GetPlateDebugLevel() < 2 || !m_bBakeApplied || m_bakeRectSize <= 0)
		return;

	IRenderAuxGeom* pAux = (gEnv->pRenderer != nullptr) ? gEnv->pRenderer->GetIRenderAuxGeom() : nullptr;
	if (pAux == nullptr)
		return;

	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (!metrics.IsValid())
		return;

	const SAuxGeomRenderFlags previousFlags = pAux->GetRenderFlags();
	SAuxGeomRenderFlags flags = e_Def3DPublicRenderflags;
	flags.SetDepthTestFlag(e_DepthTestOff);
	pAux->SetRenderFlags(flags);
	pAux->DrawAABB(GetBakeWorldBox(metrics), false, ColorB(0, 255, 0, 255), eBBD_Faceted);
	pAux->SetRenderFlags(previousFlags);
}

void CTerrainPlateComponent::ApplyBake()
{
	if (!m_bBakeIntoTerrain)
	{
		LogBakeSkip("bake-off");
		return;
	}

	if (m_bBakeTornDown)
	{
		// The ground has already been given back. Anything that still reaches a mesh regeneration after that - a
		// slot event during the entity teardown, a cascade started by another plate - must not put the relief back.
		LogBakeSkip("torn-down");
		return;
	}

	if (m_bInitialBakePending)
	{
		// Never from Initialize(), only once the level's terrain is in.
		LogBakeSkip("waiting-for-terrain");
		return;
	}

	if (m_pEntity == nullptr || gEnv == nullptr || gEnv->p3DEngine == nullptr)
	{
		LogBakeSkip("no-3dengine");
		return;
	}

	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (!metrics.IsValid())
	{
		LogBakeSkip("no-terrain");
		return;
	}

	const CTimeValue startTime = (gEnv->pTimer != nullptr) ? gEnv->pTimer->GetAsyncTime() : CTimeValue();

	const Matrix34 worldTM = GetPlateFrameTM();

	int x1 = 0, y1 = 0, size = 0;
	if (!ComputeBakeRect(metrics, worldTM, x1, y1, size))
	{
		LogBakeSkip("rect-empty");
		return;
	}

	if (GetPlateDebugLevel() >= 1)
	{
			// Alignment is the one thing SetTerrainElevation asserts about and the one thing a Profile build will not
			// tell us, so it is spelled out here rather than left to be inferred from the numbers.
		CryLog("TerrainPlate '%s': bake entry, plate at (%.2f, %.2f, %.2f), rect units (%d,%d)+%d, "
		       "unit %.2f m, sector %d units, terrain %d units, aligned %s",
		       GetPlateName(), worldTM.GetTranslation().x, worldTM.GetTranslation().y, worldTM.GetTranslation().z,
		       x1, y1, size, metrics.unitSize, metrics.sectorUnits, metrics.hmapSize,
		       ((x1 % metrics.sectorUnits) == 0 && (y1 % metrics.sectorUnits) == 0 && (size % metrics.sectorUnits) == 0)
		       ? "yes" : "NO");
	}

	// The block is (size + 1) rows of the full terrain width. Refuse a footprint that would make that allocation
	// absurd rather than eat a hundred megabytes silently.
	if ((int64)(size + 1) * (int64)metrics.hmapSize > (int64)8 * 1024 * 1024)
	{
		if (!m_bBakeSizeWarningIssued)
		{
			m_bBakeSizeWarningIssued = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Terrain Plate '%s': Bake Into Terrain skipped, the footprint covers %d x %d terrain units, which is too large to bake in one block",
			           m_pEntity->GetName(), size, size);
		}
		LogBakeSkip("footprint-too-large");
		return;
	}

	m_bakeRectX1 = x1;
	m_bakeRectY1 = y1;
	m_bakeRectSize = size;

	// NOT a fresh read of the terrain: RestoreBakeBaseline has just written the stored baseline back, and a
	// height that goes through SetTerrainElevation and comes back out is lower than it went in.
	RefreshBakeBaseline(metrics);

	int cellsWritten = 0;
	float minDelta = 0.f, maxDelta = 0.f;
	ComputeBakeBlock(metrics, worldTM, cellsWritten, minDelta, maxDelta);

	if (!WriteTerrainBlock(metrics, m_bakeResult))
		return;   // WriteTerrainBlock has already named the reason

	m_bBakeApplied = true;
	// The block is in the ground now, so this plate is on top of everything that was written before it. That is
	// the only order the stack of baselines can be unwound in. @see RunBakeCascade.
	m_bakeApplySeq = ++g_bakeApplySeqCounter;
	m_lastBakeProbeFrameId = gEnv->nMainFrameID;
	BuildBakeProbes(metrics);
	WarnOnBakeQuantisation(metrics);
	AfterBakeInvalidation(GetBakeWorldBox(metrics));

	// Last, so the read back sees the terrain exactly as the rest of the engine will see it.
	VerifyBakeWrite(metrics);

	if (GetPlateDebugLevel() >= 1)
	{
		const float milliseconds = (gEnv->pTimer != nullptr) ? (gEnv->pTimer->GetAsyncTime() - startTime).GetMilliSeconds() : 0.f;
		CryLog("TerrainPlate '%s': bake applied, rect units (%d,%d)+%d (%d x %d sectors), %d of %d cells changed, delta %.3f .. %.3f m, %.2f ms",
		       m_pEntity->GetName(), m_bakeRectX1, m_bakeRectY1, m_bakeRectSize,
		       m_bakeRectSize / metrics.sectorUnits, m_bakeRectSize / metrics.sectorUnits,
		       cellsWritten, (m_bakeRectSize + 1) * (m_bakeRectSize + 1), minDelta, maxDelta, milliseconds);
	}
}

void CTerrainPlateComponent::BuildBakeProbes(const SBakeTerrainMetrics& metrics)
{
	m_bakeProbes.clear();

	// The knobs this bake was computed with, so PollBake can tell when the console changes them.
	m_bakeEpsilonUsed = GetBakeEpsilonMeters();
	m_bakeSlopeCorrectUsed = g_terrainPlateBakeSlopeCorrect;
	m_bakeEdgeAAUsed = g_terrainPlateBakeEdgeAA;
	m_bakeWeldCurveUsed = g_terrainPlateBakeWeldCurve;
	m_bakeOffsetUsed = (float)m_bakeOffset;

	const int dim = m_bakeRectSize + 1;
	if ((int)m_bakeResult.size() != dim * dim)
		return;

	// Nine probes - corners, edge midpoints, centre - are enough for a small plate and not for a large one: on a
	// 200 m plate they are 50 m apart, so a sculpt can sit entirely between them. No engine notification exists
	// for a terrain edit, so this grid is the whole detection mechanism; it covers the SECTOR ALIGNED rect.
	const int spacing = clamp_tpl(min(max(1, g_terrainPlateBakeProbeSpacing), metrics.sectorUnits), 1, max(1, m_bakeRectSize));
	const int probesPerAxis = clamp_tpl(m_bakeRectSize / spacing + 1, 3, 33);
	const float cellStep = (float)m_bakeRectSize / (float)(probesPerAxis - 1);

	m_bakeProbes.reserve((size_t)probesPerAxis * probesPerAxis);

	for (int i = 0; i < probesPerAxis; ++i)
	{
		for (int j = 0; j < probesPerAxis; ++j)
		{
			const int gx = clamp_tpl((int)(cellStep * (float)i + 0.5f), 0, dim - 1);
			const int gy = clamp_tpl((int)(cellStep * (float)j + 0.5f), 0, dim - 1);

				// Snap the probe to the most changed cell of its block: a cell where the bake equals the baseline cannot
				// detect an editor push that re-pushes CHeightmap, because there is nothing to erase.
			const int half = max(1, (int)(cellStep * 0.5f));
			int bestX = gx, bestY = gy;
			float bestDelta = -1.f;

			for (int ox = max(0, gx - half); ox <= min(dim - 1, gx + half); ++ox)
			{
				for (int oy = max(0, gy - half); oy <= min(dim - 1, gy + half); ++oy)
				{
					const size_t index = (size_t)ox * dim + oy;
					const float delta = fabs_tpl(m_bakeResult[index] - m_bakeBaseline[index]);
					if (delta > bestDelta)
					{
						bestDelta = delta;
						bestX = ox;
						bestY = oy;
					}
				}
			}

			SBakeProbe probe;
			probe.ux = m_bakeRectX1 + bestX;
			probe.uy = m_bakeRectY1 + bestY;
			probe.expectedZ = m_bakeResult[(size_t)bestX * dim + bestY];
			m_bakeProbes.push_back(probe);
		}
	}

	// Tolerance: a height never survives the round trip exactly, because the sector re-quantises to 12 bits over
	// its own span. A few steps of the coarsest touched sector is far below anything an edit would do.
	float quantStep = 0.f;
	const int sectorsPerSide = max(1, m_bakeRectSize / metrics.sectorUnits);
	for (int sx = 0; sx < sectorsPerSide; ++sx)
	{
		for (int sy = 0; sy < sectorsPerSide; ++sy)
		{
			float fMin = FLT_MAX, fMax = -FLT_MAX;
			for (int ux = sx * metrics.sectorUnits; ux <= (sx + 1) * metrics.sectorUnits; ++ux)
			{
				for (int uy = sy * metrics.sectorUnits; uy <= (sy + 1) * metrics.sectorUnits; ++uy)
				{
					const float z = m_bakeResult[(size_t)min(ux, dim - 1) * dim + min(uy, dim - 1)];
					fMin = min(fMin, z);
					fMax = max(fMax, z);
				}
			}
			quantStep = max(quantStep, TerrainQuantStep(fMin, fMax));
		}
	}

	m_bakeProbeTolerance = max(0.002f, quantStep * 4.f);
}

void CTerrainPlateComponent::PollBake()
{
	if (!m_bBakeApplied || m_bakeProbes.empty())
		return;

	// The batch is about to restore this plate's old footprint and re-bake it at the new one, so reacting to the
	// terrain in the meantime would repair a bake that is on its way out.
	if (m_bBakeMoveBatchPending || m_bDeferredXformPending)
		return;

	const int probeFrames = GetBakeProbeFrames();
	if (probeFrames <= 0)
		return;

	// Never poll from inside a cascade, nor for a few frames after one: the terrain moved because we moved it.
	// This is also the coalescing rule - one terrain edit costs one cascade, not one per plate.
	if (g_bakeCascadeDepth > 0)
		return;

	// The cycle valve has fired: the terrain is left exactly as it is until the user touches a plate. Repairing
	// on would only feed the cycle, and a half repaired composite is worse to look at than a stale one.
	if (g_bakeAutoRepairStalled)
		return;

	if (g_bakeCascadeFrameId != ~0u && (uint32)(gEnv->nMainFrameID - g_bakeCascadeFrameId) < kBakeCascadeQuietFrames)
		return;

	// m_bakeBackoffFrames is 0 unless the runaway guard has fired (@see ReapplyBakeAfterExternalEdit).
	if ((int)(gEnv->nMainFrameID - m_lastBakeProbeFrameId) < probeFrames + m_bakeBackoffFrames)
		return;

	m_lastBakeProbeFrameId = gEnv->nMainFrameID;

	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (!metrics.IsValid())
		return;

	// Second line of defence: if the rect this plate holds in the terrain is nowhere near where the plate now is,
	// a transform was missed and the old footprint is an orphan.
	if (HealOrphanedBakeRect(metrics))
	{
		NoteBakeAutoRepair();
		return;
	}

	// Changing a bake cvar moves nothing in the terrain, so the probes cannot see it; compare the values the live
	// bake was computed with instead. The two rim shape knobs MUST be in this list: they change every height in
	// the band, so a probe blind to them would call the new shape an external edit and repair it away every poll.
	if (m_bakeEpsilonUsed != GetBakeEpsilonMeters()
	    || m_bakeSlopeCorrectUsed != g_terrainPlateBakeSlopeCorrect
	    || m_bakeEdgeAAUsed != g_terrainPlateBakeEdgeAA
	    || m_bakeWeldCurveUsed != g_terrainPlateBakeWeldCurve
	    || m_bakeOffsetUsed != (float)m_bakeOffset)
	{
		ReapplyBakeAfterExternalEdit(metrics);
		NoteBakeAutoRepair();
		return;
	}

	// One array lookup per probe, so even the 33 x 33 cap costs nothing at this rate.
	for (const SBakeProbe& probe : m_bakeProbes)
	{
		if (fabs_tpl(SampleTerrainUnit(metrics, probe.ux, probe.uy) - probe.expectedZ) > m_bakeProbeTolerance)
		{
			ReapplyBakeAfterExternalEdit(metrics);
			NoteBakeAutoRepair();
			return;
		}
	}

	// The terrain is exactly where the plate left it, so the plate is at rest and the runaway guard can forget
	// everything it counted.
	m_bakeReapplyCount = 0;
	m_bakeBackoffFrames = 0;
	m_bBakeRunawayWarned = false;
}

bool CTerrainPlateComponent::HealOrphanedBakeRect(const SBakeTerrainMetrics& metrics)
{
	if (!m_bBakeApplied || m_bakeRectSize <= 0 || m_bBakeTornDown || !metrics.IsValid())
		return false;

	int x1 = 0, y1 = 0, size = 0;
	if (!ComputeBakeRect(metrics, GetPlateFrameTM(), x1, y1, size))
		return false;   // no usable footprint right now; ApplyBake's own skip paths report that

	// Only a COMPLETE separation counts. A plate that drifted a few units still overlaps its own rect and the
	// ordinary restore-and-re-bake handles it; this is a plate whose relief stayed behind somewhere else.
	const bool bOverlaps = (x1 < m_bakeRectX1 + m_bakeRectSize) && (m_bakeRectX1 < x1 + size)
	                       && (y1 < m_bakeRectY1 + m_bakeRectSize) && (m_bakeRectY1 < y1 + size);
	if (bOverlaps)
		return false;

	// Loud, always, not only under the debug cvar: reaching this means an event path was missed, and a missed
	// event that heals itself in silence is a bug that never gets reported.
	CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
	           "Terrain Plate '%s': orphan-restored. The bake this plate holds in the terrain at units (%d,%d)+%d does not touch "
	           "the footprint the plate now has at (%d,%d)+%d, so a transform of this plate was never seen by the component. The "
	           "orphaned ground has been put back and the plate re-baked where it actually is. Please report this along with what "
	           "was being done to the plate at the time.",
	           GetPlateName(), m_bakeRectX1, m_bakeRectY1, m_bakeRectSize, x1, y1, size);

	// RegenerateMesh restores the stored rect first (RestoreBakeBaseline still holds the OLD rect and baseline),
	// then re-welds and bakes at the new place, cascading into anything the new footprint shares ground with.
	RegenerateMesh();
	return true;
}

void CTerrainPlateComponent::ReapplyBakeAfterExternalEdit(const SBakeTerrainMetrics& metrics)
{
	const int dim = m_bakeRectSize + 1;
	if ((int)m_bakeBaseline.size() != dim * dim || (int)m_bakeResult.size() != dim * dim)
		return;

	// Safety valve. Every repair costs a mesh regeneration and a SetTerrainElevation, so a self-repairing
	// configuration says so once and slows down. It is never silenced completely, so a genuine edit settles.
	const uint32 frameId = gEnv->nMainFrameID;
	if (m_bakeReapplyCount <= 0 || (int)(frameId - m_bakeReapplyWindowStart) > kBakeRunawayWindowFrames)
	{
		m_bakeReapplyWindowStart = frameId;
		m_bakeReapplyCount = 0;
		m_bakeBackoffFrames = 0;
		m_bBakeRunawayWarned = false;
	}

	++m_bakeReapplyCount;

	if (m_bakeReapplyCount > kBakeRunawayLimit)
	{
		m_bakeBackoffFrames = (m_bakeBackoffFrames > 0)
		                      ? min(m_bakeBackoffFrames * 2, kBakeBackoffMaxFrames)
		                      : max(1, GetBakeProbeFrames()) * 2;

		if (!m_bBakeRunawayWarned)
		{
			m_bBakeRunawayWarned = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Terrain Plate '%s': Bake Into Terrain has re-applied itself %d times in %d frames with no edit of its own. "
			           "The plate is backing off (now one check every %d frames) so it stops costing frame time. This normally means "
			           "the terrain under it keeps being rewritten by something else - most often another baking plate over the same "
			           "ground; give the two plates different Bake Priority values, or stop them overlapping.",
			           GetPlateName(), m_bakeReapplyCount, kBakeRunawayWindowFrames,
			           max(1, GetBakeProbeFrames()) + m_bakeBackoffFrames);
		}
	}

	// Some editor operation pushed CHeightmap over part of this rect and erased the bake there. Restoring the
	// stored baseline would undo the user's edit; recapturing the whole rect would fold the surviving part of our
	// own bake into the baseline. So the baseline is rebuilt cell by cell:
	//   * the cell still holds our baked height -> the editor did not touch it; keep the stored baseline;
	//   * the cell holds something else         -> the editor rewrote it, and what is there now IS the ground.
	int recapturedCells = 0;
	for (int ux = 0; ux < dim; ++ux)
	{
		for (int uy = 0; uy < dim; ++uy)
		{
			const size_t index = (size_t)ux * dim + uy;
			const float current = SampleTerrainUnit(metrics, m_bakeRectX1 + ux, m_bakeRectY1 + uy);

			if (fabs_tpl(current - m_bakeResult[index]) > m_bakeProbeTolerance)
			{
				m_bakeBaseline[index] = current;
				++recapturedCells;
			}
		}
	}

	if (GetPlateDebugLevel() >= 1)
	{
		CryLog("TerrainPlate '%s': terrain changed under the bake, %d of %d cells re-captured as the new ground",
		       m_pEntity->GetName(), recapturedCells, dim * dim);
	}

	// RegenerateMesh restores the corrected baseline first, then re-welds against it and bakes on top.
	// m_bBakeApplied is deliberately left true so that the restore actually runs.
	RegenerateMesh();
}

void CTerrainPlateComponent::RefreshBakeCompositeAfterCascade(const SBakeTerrainMetrics& metrics)
{
	const int dim = m_bakeRectSize + 1;
	if (!m_bBakeApplied || m_bakeRectSize <= 0 || !metrics.IsValid() || (int)m_bakeResult.size() != dim * dim)
		return;

	// Every plate's probes compare the terrain against m_bakeResult, the block THIS plate wrote. In an overlap
	// that is not what the terrain holds - the cascade writes in ascending priority, so a shared cell keeps the
	// height of the plate above - so each participant records what the terrain ACTUALLY holds over its rect.
	for (int ux = 0; ux < dim; ++ux)
	{
		for (int uy = 0; uy < dim; ++uy)
		{
			m_bakeResult[(size_t)ux * dim + uy] = SampleTerrainUnit(metrics, m_bakeRectX1 + ux, m_bakeRectY1 + uy);
		}
	}

	for (SBakeProbe& probe : m_bakeProbes)
	{
		const int ux = probe.ux - m_bakeRectX1;
		const int uy = probe.uy - m_bakeRectY1;
		if (ux >= 0 && ux < dim && uy >= 0 && uy < dim)
		{
			probe.expectedZ = m_bakeResult[(size_t)ux * dim + uy];
		}
	}

	m_lastBakeProbeFrameId = gEnv->nMainFrameID;
	m_bakeReapplyCount = 0;
	m_bakeBackoffFrames = 0;
	m_bBakeRunawayWarned = false;
}

void CTerrainPlateComponent::WarnOnBakeQuantisation(const SBakeTerrainMetrics& metrics)
{
	if (m_bBakeQuantWarningIssued)
		return;

	// A sector encodes its heights as 12 bits over its own span, so raising part of a sector by a lot costs
	// precision everywhere in it - including the untouched terrain around the plate. Say so once.
	const int dim = m_bakeRectSize + 1;
	const int sectorsPerSide = max(1, m_bakeRectSize / metrics.sectorUnits);

	for (int sx = 0; sx < sectorsPerSide; ++sx)
	{
		for (int sy = 0; sy < sectorsPerSide; ++sy)
		{
			float baseMin = FLT_MAX, baseMax = -FLT_MAX, bakedMin = FLT_MAX, bakedMax = -FLT_MAX;

			for (int ux = sx * metrics.sectorUnits; ux <= (sx + 1) * metrics.sectorUnits; ++ux)
			{
				for (int uy = sy * metrics.sectorUnits; uy <= (sy + 1) * metrics.sectorUnits; ++uy)
				{
					const size_t index = (size_t)min(ux, dim - 1) * dim + min(uy, dim - 1);
					baseMin = min(baseMin, m_bakeBaseline[index]);
					baseMax = max(baseMax, m_bakeBaseline[index]);
					bakedMin = min(bakedMin, m_bakeResult[index]);
					bakedMax = max(bakedMax, m_bakeResult[index]);
				}
			}

			const float stepBefore = TerrainQuantStep(baseMin, baseMax);
			const float stepAfter = TerrainQuantStep(bakedMin, bakedMax);

			if (stepBefore > 0.f && stepAfter > stepBefore * 2.f)
			{
				m_bBakeQuantWarningIssued = true;
				CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
				           "Terrain Plate '%s': the bake more than doubled the height quantisation step of the terrain sector at units (%d,%d) "
				           "(%.1f mm -> %.1f mm). A sector stores 12 bit heights over its own span, so the terrain around the plate loses "
				           "precision too. Use a shorter plate, or move it so that its relief does not cross a sector with a small height range.",
				           m_pEntity->GetName(),
				           m_bakeRectX1 + sx * metrics.sectorUnits, m_bakeRectY1 + sy * metrics.sectorUnits,
				           stepBefore * 1000.f, stepAfter * 1000.f);
				return;
			}
		}
	}
}

void CTerrainPlateComponent::OnCullBuriedCVarChanged(ICVar* /*pCVar*/)
{
	// Not a render node flag that can simply be re-written: the split is in the index buffer, so the value can
	// only take effect on a rebuild. Iterated by index because RegenerateMesh can start a cascade and reallocate.
	for (size_t i = 0; i < g_terrainPlates.size(); ++i)
	{
		g_terrainPlates[i]->RequestRegenerate();
	}
}

void CTerrainPlateComponent::ReArmAfterTeardown()
{
	// The torn-down flag and the registry are one state, not two: a plate that may bake again must be visible to
	// the cascades, the move batch and the rect collector, or its neighbours unwind without it.
	const bool bWasTornDown = m_bBakeTornDown;
	m_bBakeTornDown = false;
	RegisterPlate();

	if (bWasTornDown && m_pEntity != nullptr)
	{
		m_pEntity->UpdateComponentEventMask(this);
	}
}

void CTerrainPlateComponent::RegisterPlate()
{
	if (std::find(g_terrainPlates.begin(), g_terrainPlates.end(), this) == g_terrainPlates.end())
	{
		g_terrainPlates.push_back(this);
	}
}

void CTerrainPlateComponent::UnregisterPlate()
{
	auto it = std::find(g_terrainPlates.begin(), g_terrainPlates.end(), this);
	if (it != g_terrainPlates.end())
	{
		g_terrainPlates.erase(it);
	}
}

void CTerrainPlateComponent::GetBakeCascadeRects(std::vector<int>& outRects) const
{
	if (!m_bBakeIntoTerrain || m_bBakeTornDown || m_pEntity == nullptr)
		return;

	const SBakeTerrainMetrics metrics = GetBakeTerrainMetrics();
	if (!metrics.IsValid())
		return;

	// The live footprint, if the terrain is holding one. For a plate that has just been moved this is the OLD
	// one, which is exactly the rect whose neighbours have to be unwound with it.
	if (m_bBakeApplied && m_bakeRectSize > 0)
	{
		outRects.push_back(m_bakeRectX1);
		outRects.push_back(m_bakeRectY1);
		outRects.push_back(m_bakeRectSize);
	}

	// The footprint the plate is about to take. Identical to the live one for a plate that has not moved,
	// in which case it is dropped: that is the ordinary case and it must stay a single rect.
	int x1 = 0, y1 = 0, size = 0;
	if (ComputeBakeRect(metrics, GetPlateFrameTM(), x1, y1, size))
	{
		if (outRects.size() < 3 || outRects[outRects.size() - 3] != x1
		    || outRects[outRects.size() - 2] != y1 || outRects[outRects.size() - 1] != size)
		{
			outRects.push_back(x1);
			outRects.push_back(y1);
			outRects.push_back(size);
		}
	}
}

bool CTerrainPlateComponent::BakesBefore(const CTerrainPlateComponent& other) const
{
	// Higher Bake Priority bakes later and therefore wins in the overlap. The GUID tiebreak is what makes the
	// result reproducible - a GUID is stable across saves and loads, whereas the registry order is spawn order.
	if ((int)m_bakePriority != (int)other.m_bakePriority)
		return (int)m_bakePriority < (int)other.m_bakePriority;

	if (m_pEntity != nullptr && other.m_pEntity != nullptr)
		return m_pEntity->GetGuid() < other.m_pEntity->GetGuid();

	return this < &other;
}

void CTerrainPlateComponent::CollectBakeCascadeSet(std::vector<CTerrainPlateComponent*>& out) const
{
	out.clear();

	// Every rect the members occupy, as (x1, y1, size) triples. A plate contributes two while it is between a
	// move and its rebuild, so both the ground it leaves and the one it takes pull their neighbours in.
	std::vector<int> rects;
	GetBakeCascadeRects(rects);
	if (rects.empty())
		return;

	out.push_back(const_cast<CTerrainPlateComponent*>(this));

	CollectBakeCascadeSetFromRects(rects, out);
}

void CTerrainPlateComponent::CollectBakeCascadeSetFromRects(std::vector<int>& rects,
                                                            std::vector<CTerrainPlateComponent*>& out)
{
	// Transitive closure over rect intersection: a third plate that only overlaps the second still has to be
	// unwound. Quadratic, but it only runs when a bake changes and it terminates at the registry size.
	for (size_t i = 0; i < rects.size(); i += 3)
	{
		// By value: the inner loop appends to rects and would invalidate a reference into it.
		const int ax1 = rects[i], ay1 = rects[i + 1], aSize = rects[i + 2];

		for (CTerrainPlateComponent* pOther : g_terrainPlates)
		{
			if (std::find(out.begin(), out.end(), pOther) != out.end())
				continue;

			std::vector<int> otherRects;
			pOther->GetBakeCascadeRects(otherRects);

			bool bOverlaps = false;
			for (size_t j = 0; j + 2 < otherRects.size() && !bOverlaps; j += 3)
			{
				const int bx1 = otherRects[j], by1 = otherRects[j + 1], bSize = otherRects[j + 2];
				bOverlaps = (ax1 < bx1 + bSize) && (bx1 < ax1 + aSize)
				            && (ay1 < by1 + bSize) && (by1 < ay1 + aSize);
			}

			if (!bOverlaps)
				continue;

			out.push_back(pOther);
			rects.insert(rects.end(), otherRects.begin(), otherRects.end());
		}
	}

	// Cost only. The closure is NEVER cut short and NEVER split: the baselines nest, so a plate left outside the
	// pass keeps a stale bake, its probes read the others' relief as an external edit, and the two halves restore
	// each other for ever (the 16 plate cap did exactly that with 21 overlapping plates).
	const int costLimit = GetBakeMaxBatch();
	if (costLimit > 0 && (int)out.size() > costLimit)
	{
		if (!g_bakeLargeBatchWarned)
		{
			g_bakeLargeBatchWarned = true;
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Terrain Plate: %d baking plates share ground and are restored and re-baked together in one pass "
			           "(e_TerrainPlateBakeMaxBatch = %d). The result is correct - the whole set is always done in one "
			           "pass - but every edit of any of them now costs all %d. Moving or deleting more than 16 baking "
			           "plates in one operation is not recommended (cost grows with overlap; prefer smaller selections).",
			           (int)out.size(), costLimit, (int)out.size());
		}
	}
	else
	{
		g_bakeLargeBatchWarned = false;
	}

	std::stable_sort(out.begin(), out.end(),
	                 [](const CTerrainPlateComponent* pA, const CTerrainPlateComponent* pB) { return pA->BakesBefore(*pB); });
}

void CTerrainPlateComponent::LogBakeOverlapOnce(const CTerrainPlateComponent& other)
{
	if (m_bBakeOverlapWarningIssued || m_pEntity == nullptr || other.m_pEntity == nullptr)
		return;

	m_bBakeOverlapWarningIssued = true;

	// The overlap is handled now, so this is only a log line - but the user still wants to know that two plates
	// share ground and that Bake Priority is what decides which of them is on top.
	CryLog("TerrainPlate '%s' (Bake Priority %d): footprint shared with '%s' (Bake Priority %d). The overlapping plates are "
	       "restored and re-baked together, lowest priority first, so the higher priority plate wins where they meet.",
	       m_pEntity->GetName(), (int)m_bakePriority, other.m_pEntity->GetName(), (int)other.m_bakePriority);
}

void CTerrainPlateComponent::ApplyBakeCascaded()
{
	// For a baking plate the multi plate case is normally taken at the TOP of RegenerateMesh, before the plate
	// restores its own footprint, so by the time control reaches here the set can only be a single plate; this
	// branch is the safety net. Inside a cascade a participant just bakes, which is also the recursion guard.
	if (g_bakeCascadeDepth > 0)
	{
		ApplyBake();
		return;
	}

	std::vector<CTerrainPlateComponent*> affected;
	CollectBakeCascadeSet(affected);

	if (affected.size() <= 1)
	{
		ApplyBake();   // the normal case: this plate owns its footprint alone
		return;
	}

	RunBakeCascade(affected, this, "overlap cascade");
}

void CTerrainPlateComponent::RunBakeCascade(std::vector<CTerrainPlateComponent*>& affected,
                                            CTerrainPlateComponent* pSeed, const char* szReason,
                                            CTerrainPlateComponent* pRemoved)
{
	if ((affected.empty() && pRemoved == nullptr) || g_bakeCascadeDepth > 0)
		return;

	// Sorted here rather than by the callers, so that the invariant "restored in the exact inverse of the order
	// they were applied in" belongs to this one function.
	std::stable_sort(affected.begin(), affected.end(),
	                 [](const CTerrainPlateComponent* pA, const CTerrainPlateComponent* pB) { return pA->BakesBefore(*pB); });

	// The cascade. Every plate captures its baseline from the terrain as it finds it, so a composite is only well
	// defined if the whole overlapping set is unwound and re-applied in one order:
	//   restore in DESCENDING apply order - the exact inverse of the order they were applied in, so each plate
	//                                    writes back a baseline that is still the truth;
	//   bake in ASCENDING priority     - each captures the composite below it, highest priority written last.
	// The restore order is m_bakeApplySeq, NOT Bake Priority: the terrain holds a stack pushed in the OLD order,
	// and unwinding it in a newly edited priority order resurrects a higher plate's relief as unowned ground.
	// The re-bake goes through RegenerateMesh, because the edge weld is solved against the ground that moved.
	++g_bakeCascadeDepth;

	// A separate order, because the set is RE-APPLIED in priority order and UNWOUND in the order it was written.
	// A plate that has never baked carries sequence 0, sorts last and restores nothing. pRemoved is being torn
	// down: it takes part in the UNWIND, its place in the stack as load bearing as any, but not in the re-apply.
	std::vector<CTerrainPlateComponent*> unwind(affected);
	if (pRemoved != nullptr)
	{
		unwind.push_back(pRemoved);
	}
	std::stable_sort(unwind.begin(), unwind.end(),
	                 [](const CTerrainPlateComponent* pA, const CTerrainPlateComponent* pB)
	                 { return pA->m_bakeApplySeq > pB->m_bakeApplySeq; });

	for (CTerrainPlateComponent* pPlate : unwind)
	{
		pPlate->RestoreBakeBaseline();
	}

	for (CTerrainPlateComponent* pPlate : affected)
	{
			// A plate that has not reached its own first-update trigger would refuse to bake. Releasing it here is safe
			// (we only get this far with a terrain in the engine) and is what makes a level load composite correctly.
		if (pPlate->m_bInitialBakePending)
		{
			pPlate->m_bInitialBakePending = false;
			if (pPlate->m_pEntity != nullptr)
			{
				pPlate->m_pEntity->UpdateComponentEventMask(pPlate);
			}
		}

			// For the seed that is one recursion into RegenerateMesh - harmless, the guard above turns the inner call
			// into a plain ApplyBake - and it guarantees every plate is welded against the ground it ends up on.
		pPlate->RegenerateMesh();
		pPlate->m_lastBakeProbeFrameId = gEnv->nMainFrameID;

		if (pSeed != nullptr && pPlate != pSeed)
		{
			pPlate->LogBakeOverlapOnce(*pSeed);
			pSeed->LogBakeOverlapOnce(*pPlate);
		}
	}

	// Every participant now takes the FINAL composite over its own rect as the heights it expects. Without this
	// the plates below the top one mismatch on their next poll and the set re-bakes itself for ever.
	const SBakeTerrainMetrics cascadeMetrics = GetBakeTerrainMetrics();
	for (CTerrainPlateComponent* pPlate : affected)
	{
		pPlate->RefreshBakeCompositeAfterCascade(cascadeMetrics);
	}

	--g_bakeCascadeDepth;

	// Coalescing: the cascade may well have touched a sector a plate outside the set also watches. Remembering the
	// frame lets the polling stand down, so N plates seeing the same terrain change produce one cascade, not N.
	g_bakeCascadeFrameId = gEnv->nMainFrameID;

	if (GetPlateDebugLevel() >= 1)
	{
		const CTerrainPlateComponent* pNamed = (pSeed != nullptr) ? pSeed : pRemoved;
		CryLog("TerrainPlate '%s': %s over %d plates, restored last-applied first and re-baked lowest priority first",
		       (pNamed != nullptr) ? pNamed->GetPlateName() : "<batch>", szReason, (int)unwind.size());
	}
}

void CTerrainPlateComponent::JoinBakeMoveBatch()
{
	if (m_bBakeMoveBatchPending)
	{
		// Already queued. Do NOT re-stamp the batch frame here: a plate that keeps being nudged would otherwise
		// push the flush out for ever and the old footprint would sit in the ground indefinitely.
		return;
	}

	m_bBakeMoveBatchPending = true;
	g_bakeMoveBatch.push_back(this);
	g_bakeMoveBatchFrameId = gEnv->nMainFrameID;

	// Everything else still holding a deferred transform belongs to the same gesture and has to land in THIS
	// batch, not a second one that would restore its old footprints after ours were re-baked over. A plain plate
	// joins on mouse up; a plate inside a GROUP gets no such event, so its settle is pulled forward one tick.
	for (CTerrainPlateComponent* pOther : g_terrainPlates)
	{
		if (pOther == this || !pOther->m_bDeferredXformPending || !pOther->WantsBakeMoveBatch())
			continue;

		pOther->m_lastXformFrameId = gEnv->nMainFrameID - (uint32)max(1, pOther->GetEffectiveSettleFrames());
	}
}

void CTerrainPlateComponent::LeaveBakeMoveBatch()
{
	m_bBakeMoveBatchPending = false;

	auto it = std::find(g_bakeMoveBatch.begin(), g_bakeMoveBatch.end(), this);
	if (it != g_bakeMoveBatch.end())
	{
		g_bakeMoveBatch.erase(it);
	}
}

void CTerrainPlateComponent::FlushBakeMoveBatch()
{
	if (g_bakeMoveBatch.empty() || g_bakeCascadeDepth > 0)
		return;

	// Wait for anything still mid-gesture. Component update order is whatever the entity system registered, so
	// "one frame after the last join" alone would let the first plate run the batch while a straggler joins. It
	// cannot hang: JoinBakeMoveBatch pulls every such plate's settle forward to the next tick.
	for (CTerrainPlateComponent* pOther : g_terrainPlates)
	{
		if (pOther->m_bDeferredXformPending && pOther->WantsBakeMoveBatch())
			return;
	}

	// The whole batch is one user gesture, so it clears whatever the runaway valve latched: the user has acted.
	NoteBakeUserAction();

	// Drain the queue first: everything below regenerates meshes and writes the terrain, and must not be able to
	// re-enter the batch it is draining.
	std::vector<CTerrainPlateComponent*> movers;
	movers.swap(g_bakeMoveBatch);

	for (CTerrainPlateComponent* pMover : movers)
	{
		pMover->m_bBakeMoveBatchPending = false;

		// The flag is one of the reasons a plate subscribes to ENTITY_EVENT_UPDATE, so a plate that only ticks
		// because of it has to be allowed to stop ticking again.
		if (pMover->m_pEntity != nullptr)
		{
			pMover->m_pEntity->UpdateComponentEventMask(pMover);
		}
	}

	// The set is the union of each mover's cascade closure, taken from BOTH the footprint it holds and the one it
	// is taking. A mover that overlaps nothing is still its own closure of one, so its old footprint is restored.
	std::vector<CTerrainPlateComponent*> affected;
	std::vector<CTerrainPlateComponent*> closure;

	for (CTerrainPlateComponent* pMover : movers)
	{
		if (!pMover->WantsBakeMoveBatch())
			continue;

		pMover->CollectBakeCascadeSet(closure);

		for (CTerrainPlateComponent* pPlate : closure)
		{
			if (std::find(affected.begin(), affected.end(), pPlate) == affected.end())
			{
				affected.push_back(pPlate);
			}
		}
	}

	if (affected.empty())
		return;

	// One ordered unwind for the whole gesture, through the same driver the single plate overlap cascade uses -
	// there is deliberately no second implementation.
	RunBakeCascade(affected, nullptr, "move batch");
}

void CTerrainPlateComponent::InvalidateShadowCache()
{
	// The static shadow cache (e_ShadowsCache*) and HeightMap AO are refreshed only when something reports a
	// change through C3DEngine::OnObjectModified. Nothing fires it here: the mesh is replaced in place, so the
	// slot keeps the same statobj pointer and no geometry event is ever seen.
	if (gEnv == nullptr || gEnv->p3DEngine == nullptr)
		return;

	IRenderNode* pRenderNode = nullptr;
	if (m_pEntity != nullptr && GetEntitySlotId() != IEntityComponent::EmptySlotId)
	{
		pRenderNode = m_pEntity->GetSlotRenderNode(GetEntitySlotId());
	}

	// Same call and flags as the editor hook: with ERF_CASTSHADOWMAPS it requests eFullUpdateTimesliced, covering
	// the cached cascades and the HeightMap AO frustum at once. There is no per box invalidation in 5.7.1.
	const IRenderNode::RenderFlagsType renderFlags = (pRenderNode != nullptr) ? pRenderNode->GetRndFlags() : ERF_CASTSHADOWMAPS;
	gEnv->p3DEngine->OnObjectModified(pRenderNode, renderFlags);
}

void CTerrainPlateComponent::ReleasePhysicsGeometry()
{
	if (m_pStatObj == nullptr || m_pStatObj->GetPhysGeom() == nullptr)
		return;

	if (m_pEntity != nullptr && GetEntitySlotId() != IEntityComponent::EmptySlotId && m_pEntity->GetPhysicalEntity() != nullptr)
	{
		m_pEntity->UnphysicalizeSlot(GetEntitySlotId());
	}

	m_pStatObj->SetPhysGeom(nullptr);
}

void CTerrainPlateComponent::UpdateNativePhysics()
{
	const int slotId = GetEntitySlotId();
	const bool bWantsCollider = ((uint32)m_type & (uint32)EMeshType::Collider) != 0
	                            && slotId != IEntityComponent::EmptySlotId
	                            && m_pStatObj != nullptr
	                            && m_pStatObj->GetPhysGeom() != nullptr;

	if (!bWantsCollider)
	{
		ReleaseNativePhysics();
		return;
	}

	if (m_pEntity->GetPhysicalEntity() != nullptr)
	{
			// Either our own static body, or a rigid body / character controller that owns the entity. Never
			// re-physicalize on top of another component: IEntity::Physicalize destroys whatever was there.
		return;
	}

	// Same shape as a brush and as a legacy static entity: a PE_STATIC holding the slot's trimesh. Mass 0 because
	// a static never moves; the weight in SPhysicsParameters only matters once a physics component takes over.
	SEntityPhysicalizeParams physParams;
	physParams.type = PE_STATIC;
	physParams.nSlot = slotId;
	physParams.mass = 0.f;
	physParams.density = -1.f;

	// Set before the call: Physicalize sends ENTITY_EVENT_PHYSICAL_TYPE_CHANGED back into ProcessEvent.
	m_bOwnsPhysicalEntity = true;
	m_pEntity->Physicalize(physParams);
	m_bOwnsPhysicalEntity = m_pEntity->GetPhysicalEntity() != nullptr;
}

void CTerrainPlateComponent::ReleaseNativePhysics()
{
	if (!m_bOwnsPhysicalEntity)
		return;

	m_bOwnsPhysicalEntity = false;

	if (m_pEntity == nullptr)
		return;

	IPhysicalEntity* pPhysicalEntity = m_pEntity->GetPhysicalEntity();
	if (pPhysicalEntity == nullptr || pPhysicalEntity->GetType() != PE_STATIC)
		return;

	SEntityPhysicalizeParams physParams;
	physParams.type = PE_NONE;
	m_pEntity->Physicalize(physParams);
}

// Physics surface types from the terrain.
// The whole feature is one number in one place: the part's pMatMapping. CPhysicalEntity::GetMatId clamps the
// face id to [0, nMats-1] and returns pMatMapping[thatIndex], and every producer of a surface type goes through
// it. Terrain and objects fill that table from the same global ISurfaceTypeManager id space and IMaterialEffects
// keys on that id alone, so a plate part holding a terrain layer's id resolves as the terrain under it does.
// The table is OVERWRITTEN from the slot's render material by CEntityPhysics (end of Physicalize,
// UpdateSlotGeometry, OnGlobalEntityMaterialChanged, AddSlotGeometry), so it has to be pushed AGAIN after each;
// losing it is harmless - nMats falls back to 1 and every face lands on entry 0, the plate's own surface type.

bool CTerrainPlateComponent::IsPhysSurfaceFromTerrainEnabled() const
{
	if (!m_bPhysSurfaceFromTerrain || g_terrainPlatePhysSurfaceFromTerrain == 0)
		return false;

	if (((uint32)m_type & (uint32)EMeshType::Collider) == 0)
		return false;

	if (m_pEntity == nullptr || GetEntitySlotId() == IEntityComponent::EmptySlotId)
		return false;

	return gEnv != nullptr && gEnv->p3DEngine != nullptr && gEnv->p3DEngine->GetITerrain() != nullptr
	       && gEnv->pPhysicalWorld != nullptr;
}

IMaterial* CTerrainPlateComponent::GetPlateMaterial() const
{
	// The same order CEntityPhysics::UpdateParamsFromRenderMaterial uses: the slot override wins, the statobj's
	// own material is the fallback. ApplyMaterial keeps both in step.
	if (m_pEntity != nullptr && GetEntitySlotId() != IEntityComponent::EmptySlotId)
	{
		if (IMaterial* pSlotMaterial = m_pEntity->GetSlotMaterial(GetEntitySlotId()))
			return pSlotMaterial;
	}

	return (m_pStatObj != nullptr) ? m_pStatObj->GetMaterial() : nullptr;
}

int CTerrainPlateComponent::GetPlateMaterialSurfaceTypeId() const
{
	IMaterial* pMaterial = GetPlateMaterial();
	if (pMaterial == nullptr)
		return 0;

	int surfaceTypeIds[MAX_SUB_MATERIALS];
	memset(surfaceTypeIds, 0, sizeof(surfaceTypeIds));
	const int numIds = pMaterial->FillSurfaceTypeIds(surfaceTypeIds);

	// Entry 0 is what the stock path gives every face of the plate today, so it is the right fallback: wherever
	// the terrain has no answer the plate keeps behaving exactly as it does now.
	return (numIds > 0) ? surfaceTypeIds[0] : 0;
}

int CTerrainPlateComponent::SampleTerrainSurfaceTypeId(float worldX, float worldY) const
{
	if (gEnv == nullptr || gEnv->pPhysicalWorld == nullptr || gEnv->p3DEngine == nullptr)
		return -1;

	// I3DEngine / ITerrain expose no per position surface type getter, but physics does: ray_hit::surface_idx is
	// the heightfield cell's GetMatId. ent_terrain means the plate itself cannot be hit.
	const float startZ = gEnv->p3DEngine->GetTerrainElevation(worldX, worldY) + kPhysSurfaceRayUp;

	ray_hit hit;
	memset(&hit, 0, sizeof(hit));

	IPhysicalWorld::SRWIParams rp;
	rp.Init(Vec3(worldX, worldY, startZ), Vec3(0.f, 0.f, -kPhysSurfaceRayLength), ent_terrain,
	        rwi_stop_at_pierceable, SCollisionClass(0, 0), &hit, 1);

	// A terrain hole is not hit (rwi_ignore_terrain_holes is deliberately NOT set), so a hole answers -1 and the
	// caller falls back to the plate's own material - which is what the ground there does too.
	if (gEnv->pPhysicalWorld->RayWorldIntersection(rp) <= 0 || hit.dist < 0.f)
		return -1;

	return (int)hit.surface_idx;
}

int CTerrainPlateComponent::SampleFootprintSurfaceTypes(uint32& outHash) const
{
	outHash = 2166136261u; // FNV-1a, same shape as the geometry hash

	if (m_pEntity == nullptr)
		return -1;

	const Matrix34 worldTM = GetPlateFrameTM();

	// Counts per distinct id over the grid. A footprint sees a handful of layers, so a small linear scan beats
	// any container here.
	int ids[kPhysSurfaceProbeGrid * kPhysSurfaceProbeGrid];
	int counts[kPhysSurfaceProbeGrid * kPhysSurfaceProbeGrid];
	int distinct = 0;

	for (int i = 0; i < kPhysSurfaceProbeGrid; ++i)
	{
		for (int j = 0; j < kPhysSurfaceProbeGrid; ++j)
		{
				// The plate is a unit plate: -0.5 .. +0.5 in local XY. Sampling on the local grid and transforming to
				// the world follows the plate's rotation and scale for free.
			const float u = -0.5f + (float)i / (float)(kPhysSurfaceProbeGrid - 1);
			const float v = -0.5f + (float)j / (float)(kPhysSurfaceProbeGrid - 1);
			const Vec3 world = worldTM * Vec3(u, v, 0.f);

			const int id = SampleTerrainSurfaceTypeId(world.x, world.y);

				// The hash covers the misses too: a layer painted over what used to be a hole, or a plate that grew
				// past the edge of the map, both have to count as a change.
			outHash = (outHash ^ (uint32)(id + 1)) * 16777619u;

			if (id < 0)
				continue;

			int k = 0;
			for (; k < distinct && ids[k] != id; ++k) {}
			if (k == distinct)
			{
				ids[distinct] = id;
				counts[distinct] = 0;
				++distinct;
			}
			++counts[k];
		}
	}

	int best = -1;
	int bestCount = 0;
	for (int k = 0; k < distinct; ++k)
	{
		if (counts[k] > bestCount)
		{
			bestCount = counts[k];
			best = ids[k];
		}
	}

	return best;
}

bool CTerrainPlateComponent::CachePhysicsMesh(const Vec3* pPositions, int vertexCount, const vtx_idx* pIndices, int indexCount)
{
	ReleasePhysicsMeshCache();

	if (pPositions == nullptr || pIndices == nullptr || vertexCount < 3 || indexCount < 3)
		return false;

	// The same bail-out CStatObj::PhysicalizeGeomType makes: the physics trimesh is 16 bit indexed, full stop.
	// Refusing here means the plate takes the stock path and keeps its existing warning.
	if (vertexCount > 0xffff)
		return false;

	m_physVertices.assign(pPositions, pPositions + vertexCount);

	m_physIndices.resize((size_t)indexCount);
	for (int i = 0; i < indexCount; ++i)
	{
		if (pIndices[i] >= 0xffff)
		{
			ReleasePhysicsMeshCache();
			return false;
		}
		m_physIndices[(size_t)i] = (uint16)pIndices[i];
	}

	return true;
}

void CTerrainPlateComponent::ReleasePhysicsMeshCache()
{
	m_physVertices.clear();
	m_physVertices.shrink_to_fit();
	m_physIndices.clear();
	m_physIndices.shrink_to_fit();
}

void CTerrainPlateComponent::BuildPhysicsSurfaceMapping()
{
	m_physMatMapping.clear();
	m_physFaceMats.clear();
	m_physSurfaceHash = 0;

	if (!IsPhysSurfaceFromTerrainEnabled())
		return;

	uint32 hash = 0;
	const int dominantId = SampleFootprintSurfaceTypes(hash);
	m_physSurfaceHash = hash;

	if (dominantId < 0)
	{
		// The terrain answered nowhere under this plate (off the map, all holes, out of reach). An empty mapping
		// pushes nothing and the stock material mapping stands, which is precisely the requested fallback.
		if (!m_bPhysSurfaceFallbackWarned && GetPlateDebugLevel() >= 1)
		{
			m_bPhysSurfaceFallbackWarned = true;
			CryLog("TerrainPlate '%s': no terrain surface type under the footprint, physics keeps the plate's own material", GetPlateName());
		}
		return;
	}

	// ONE dominant id for the whole plate. Every face carries nMatID 0 (both subsets do), so a single entry table
	// is all it takes. It is also what the per face path degrades to, hence computed first and unconditionally.
	m_physMatMapping.assign(1, dominantId);

	// ---------------------------------------------------------------- per face
	if (m_physVertices.empty() || m_physIndices.size() < 3)
		return; // no cached mesh: the stock trimesh is in place and every face id is 0

	// With a genuine multi sub material the plate's faces already index sub-materials, and a mapping built for
	// terrain layers would redefine what those slots mean. Fall back to the single dominant id.
	IMaterial* pMaterial = GetPlateMaterial();
	if (pMaterial != nullptr && (pMaterial->GetFlags() & MTL_FLAG_MULTI_SUBMTL) != 0)
	{
		if (!m_bPhysSurfaceMultiSubWarned && GetPlateDebugLevel() >= 1)
		{
			m_bPhysSurfaceMultiSubWarned = true;
			CryLog("TerrainPlate '%s': multi sub material, physics surface types stay on the single dominant terrain id", GetPlateName());
		}
		return;
	}

	// The node matrix: m_physVertices is a snapshot of the emitted position stream, which is in metres.
	const Matrix34 nodeTM = GetPlateNodeTM();

	// World XY bounds of the plate, from the very vertices the trimesh will be built from.
	AABB worldBox(AABB::RESET);
	for (const Vec3& local : m_physVertices)
	{
		worldBox.Add(nodeTM * local);
	}

	const float unitSize = max(0.125f, gEnv->p3DEngine->GetHeightMapUnitSize());
	const float extentX = max(0.f, worldBox.max.x - worldBox.min.x);
	const float extentY = max(0.f, worldBox.max.y - worldBox.min.y);

	// The terrain-unit grid, capped so a very large plate cannot turn one rebuild into a hundred thousand rays.
	// 65 per axis is 4225; past that the grid coarsens and a smaller layer patch is not resolved.
	const int gridX = clamp_tpl((int)(extentX / unitSize) + 1, 2, 65);
	const int gridY = clamp_tpl((int)(extentY / unitSize) + 1, 2, 65);
	const float stepX = (gridX > 1) ? (extentX / (float)(gridX - 1)) : 0.f;
	const float stepY = (gridY > 1) ? (extentY / (float)(gridY - 1)) : 0.f;

	std::vector<int> grid((size_t)gridX * gridY, -1);
	for (int ix = 0; ix < gridX; ++ix)
	{
		for (int iy = 0; iy < gridY; ++iy)
		{
			grid[(size_t)ix * gridY + iy] = SampleTerrainSurfaceTypeId(worldBox.min.x + stepX * (float)ix,
			                                                           worldBox.min.y + stepY * (float)iy);
		}
	}

	// Entry 0 is the plate's own material surface type: BOTH the fallback for a triangle the terrain has no
	// answer under AND the value GetMatId's clamp produces if the table is ever lost. Deliberately the same.
	std::vector<int> mapping;
	mapping.push_back(GetPlateMaterialSurfaceTypeId());

	const size_t triangleCount = m_physIndices.size() / 3;
	std::vector<char> faceMats((size_t)triangleCount, (char)0);

	const float invStepX = (stepX > 0.f) ? (1.f / stepX) : 0.f;
	const float invStepY = (stepY > 0.f) ? (1.f / stepY) : 0.f;

	for (size_t t = 0; t < triangleCount; ++t)
	{
		const Vec3 centroidLocal = (m_physVertices[m_physIndices[t * 3 + 0]]
		                            + m_physVertices[m_physIndices[t * 3 + 1]]
		                            + m_physVertices[m_physIndices[t * 3 + 2]]) / 3.f;
		const Vec3 centroidWorld = nodeTM * centroidLocal;

		const int ix = clamp_tpl((int)((centroidWorld.x - worldBox.min.x) * invStepX + 0.5f), 0, gridX - 1);
		const int iy = clamp_tpl((int)((centroidWorld.y - worldBox.min.y) * invStepY + 0.5f), 0, gridY - 1);
		const int id = grid[(size_t)ix * gridY + iy];

		if (id < 0)
			continue; // a hole, off the map, or the plate rises out of the terrain's reach -> entry 0

		size_t entry = 1;
		for (; entry < mapping.size() && mapping[entry] != id; ++entry) {}

		if (entry == mapping.size())
		{
			if ((int)mapping.size() >= kPhysSurfaceMaxMappingEntries)
			{
					// The unsigned:7 ceiling. Unreachable with real terrain layers (the terrain itself has at most 127),
					// but a clamp is cheaper than a proof: the surplus triangles keep entry 0.
				continue;
			}
			mapping.push_back(id);
		}

		faceMats[t] = (char)entry;
	}

	m_physMatMapping.swap(mapping);
	m_physFaceMats.swap(faceMats);
}

phys_geometry* CTerrainPlateComponent::CreatePhysicsGeometryFromCache()
{
	if (m_physVertices.size() < 3 || m_physIndices.size() < 3
	    || m_physFaceMats.size() != m_physIndices.size() / 3
	    || m_physMatMapping.empty()
	    || gEnv == nullptr || gEnv->pPhysicalWorld == nullptr || m_pStatObj == nullptr)
	{
		return nullptr;
	}

	IGeomManager* pGeomManager = gEnv->pPhysicalWorld->GetGeomManager();
	if (pGeomManager == nullptr)
		return nullptr;

	// --- Flags and BV heuristics, copied verbatim from CStatObj::PhysicalizeGeomType. The plate must get the
	// same BV tree it gets today; the only difference from the stock call is the pMats array.
	const size_t indexCount = m_physIndices.size();

	int flags = mesh_multicontact1;
	flags |= (indexCount <= 30 /*SMALL_MESH_NUM_INDEX, StatObjPhys.cpp*/) ? mesh_SingleBB : mesh_OBB | mesh_AABB;
	flags |= mesh_approx_box | mesh_approx_sphere | mesh_approx_cylinder | mesh_approx_capsule;
	flags |= mesh_shared_foreign_idx; // with pForeignIdx 0, physics assumes fidx[i] == i
	if (gEnv->IsEditor())
		flags |= mesh_keep_vtxmap_for_saving; // CStatObj::m_bEditor, same condition
	flags |= mesh_full_serialization;

	// Reproduced exactly as the engine has it, INCLUDING the inverted bounds: PhysicalizeGeomType sets
	// ptmin = bbox.max and ptmax = bbox.min, so "size" is negative on every axis and the dense-OBB branch can
	// never be taken. Copying that rather than fixing it keeps the collision identical to the stock one.
	int nMinTrisPerNode = 2;
	int nMaxTrisPerNode = 4;
	{
		AABB box(AABB::RESET);
		for (const Vec3& v : m_physVertices)
		{
			box.Add(v);
		}
		const Vec3 size = box.min - box.max;
		if (indexCount < 600 && max(max(size.x, size.y), size.z) > 6.f)
		{
			nMinTrisPerNode = nMaxTrisPerNode = 1;
		}
	}

	IGeometry* pGeom = pGeomManager->CreateMesh(
	  strided_pointer<const Vec3>(&m_physVertices[0]),
	  strided_pointer<unsigned short>(&m_physIndices[0]),
	  &m_physFaceMats[0],
	  0,
	  (int)(indexCount / 3),
	  flags, 0.05f /*IStatObj::Invalidate's default tolerance*/, nMinTrisPerNode, nMaxTrisPerNode, 2.5f);

	if (pGeom == nullptr)
		return nullptr;

	// RegisterGeometry takes the mapping at the same time. The entity part's own copy is still pushed separately,
	// but a geometry carrying the right table makes a consumer reading the phys_geometry directly correct too.
	phys_geometry* pPhysGeom = pGeomManager->RegisterGeometry(pGeom, m_physFaceMats[0],
	                                                          m_physMatMapping.data(), (int)m_physMatMapping.size());
	pGeom->Release();

	if (pPhysGeom == nullptr)
		return nullptr;

	// CStatObj::AssignPhysGeom does this and IStatObj::SetPhysGeom does not; without it the hit refinement path
	// cannot get from a physics hit back to the stat obj.
	if (pPhysGeom->pGeom != nullptr)
	{
		pPhysGeom->pGeom->SetForeignData((IStatObj*)m_pStatObj.get(), 0);
	}

	return pPhysGeom;
}

void CTerrainPlateComponent::RephysicalizePlateSurface()
{
	// Build first, swap second: a failed build must never leave the plate without collision.
	phys_geometry* pPhysGeom = CreatePhysicsGeometryFromCache();
	if (pPhysGeom == nullptr)
	{
		// Nothing to rebuild - the plate is on the single dominant id path, or the mesh cache is gone. Pushing the
		// mapping is then all there is to do.
		ApplyPhysicsSurfaceMapping();
		return;
	}

	ReleasePhysicsGeometry();
	m_pStatObj->SetPhysGeom(pPhysGeom, PHYS_GEOM_TYPE_DEFAULT);

	// ReleasePhysicsGeometry took the slot off the physical entity; these two put it back on the new trimesh, in
	// the same order RegenerateMesh uses. Nothing here touches a vertex, the weld, the shadow cache or the bake.
	UpdateNativePhysics();
	ApplyBaseMeshProperties();
	ApplyPhysicsSurfaceMapping();
}

void CTerrainPlateComponent::ApplyPhysicsSurfaceMapping()
{
	m_physMatsApplied = 0;

	if (m_physMatMapping.empty() || m_pEntity == nullptr)
		return;

	const int slotId = GetEntitySlotId();
	if (slotId == IEntityComponent::EmptySlotId)
		return;

	IPhysicalEntity* pPhysicalEntity = m_pEntity->GetPhysicalEntity();
	if (pPhysicalEntity == nullptr)
		return;

	// The counter move: CEntityPhysics has just overwritten the part's table from the render material, so we push
	// ours over it. SetParams copies the array, but m_physMatMapping outlives it for the next re-apply.
	pe_params_part partParams;
	partParams.partid = m_pEntity->GetPhysicalEntityPartId0(slotId);
	partParams.nMats = (int)m_physMatMapping.size();
	partParams.pMatMapping = m_physMatMapping.data();

	if (pPhysicalEntity->SetParams(&partParams) != 0)
	{
		m_physMatsApplied = (int)m_physMatMapping.size();

		if (GetPlateDebugLevel() >= 2)
		{
			CryLog("TerrainPlate '%s': physics surface mapping applied, %d entr%s, first id %d",
			       GetPlateName(), m_physMatsApplied, (m_physMatsApplied == 1) ? "y" : "ies", m_physMatMapping[0]);
		}
	}
	else if (!m_bPhysSurfaceFallbackWarned && GetPlateDebugLevel() >= 1)
	{
			// The part is not there (the entity is owned by something that did not take our slot). Without the mapping
			// the plate reports its own material, i.e. today's behaviour.
		m_bPhysSurfaceFallbackWarned = true;
		CryLog("TerrainPlate '%s': physics part not found, physics keeps the plate's own material", GetPlateName());
	}
}

void CTerrainPlateComponent::PollPhysicsSurface()
{
	// Terrain layers cannot be repainted at run time, and the clobber paths all go through events the component
	// already handles there, so the poll costs a launcher build nothing at all.
	if (gEnv == nullptr || !gEnv->IsEditor())
		return;

	if (!IsPhysSurfaceFromTerrainEnabled())
		return;

	// A rebuild or a move batch is about to redo all of this from scratch; and a bake cascade is writing the
	// terrain right now, so nothing read out of it would be stable.
	if (m_bRegenPending || m_bDeferredXformPending || m_bBakeMoveBatchPending || g_bakeCascadeDepth > 0)
		return;

	const int probeFrames = max(0, g_terrainPlatePhysSurfaceProbeFrames);
	if (probeFrames <= 0)
		return;

	if ((int)(gEnv->nMainFrameID - m_lastPhysSurfaceProbeFrameId) < probeFrames)
		return;

	m_lastPhysSurfaceProbeFrameId = gEnv->nMainFrameID;

	// Half of the probe: did a layer under the plate change? No engine notification exists for a terrain layer
	// repaint, so this hash is the whole detection mechanism; PollBake compares heights and is blind to it.
	uint32 hash = 0;
	SampleFootprintSurfaceTypes(hash);

	if (hash != m_physSurfaceHash)
	{
		if (GetPlateDebugLevel() >= 1)
		{
			CryLog("TerrainPlate '%s': terrain layers under the plate changed, re-applying the physics surface types", GetPlateName());
		}

			// The PHYSICS half only. A repaint moves no vertex, so a full RegenerateMesh would re-run the weld, the
			// shadow cache invalidation and the whole bake cascade for nothing, and disturb the bake it re-applies.
		BuildPhysicsSurfaceMapping();
		RephysicalizePlateSurface();
		return;
	}

	// Other half: did something we did not enumerate overwrite the mapping? This is the belt to the braces of
	// re-applying at every known clobber site, and it costs one GetParams.
	if (m_physMatMapping.empty() || m_physMatsApplied <= 0)
		return;

	IPhysicalEntity* pPhysicalEntity = m_pEntity->GetPhysicalEntity();
	if (pPhysicalEntity == nullptr)
		return;

	pe_params_part partParams;
	partParams.partid = m_pEntity->GetPhysicalEntityPartId0(GetEntitySlotId());
	if (pPhysicalEntity->GetParams(&partParams) == 0)
		return;

	// A clobber from the render material usually restores the SAME entry count, so the count alone is not enough -
	// the values have to be compared.
	const bool bLost = partParams.pMatMapping == nullptr
	                   || partParams.nMats != (int)m_physMatMapping.size()
	                   || memcmp(partParams.pMatMapping, m_physMatMapping.data(), m_physMatMapping.size() * sizeof(int)) != 0;

	if (bLost)
	{
		if (GetPlateDebugLevel() >= 1)
		{
			CryLog("TerrainPlate '%s': physics surface mapping was overwritten, re-applying", GetPlateName());
		}

		ApplyPhysicsSurfaceMapping();
	}
}

void CTerrainPlateComponent::OnPhysSurfaceCVarChanged(ICVar* /*pCVar*/)
{
	// Same reasoning and the same index walk as OnCullBuriedCVarChanged: the per face ids live in the physics
	// trimesh, so the cvar only means something once every plate has been built again.
	for (size_t i = 0; i < g_terrainPlates.size(); ++i)
	{
		g_terrainPlates[i]->RequestRegenerate();
	}
}
}
}
