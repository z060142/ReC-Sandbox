// Copyright 2018-2021 Crytek GmbH / Crytek Group. All rights reserved.
#include "StdAfx.h"

#include "QT/QToolTabManager.h"
#include "Objects/EntityObject.h"
#include "Objects/SelectionGroup.h"
#include "Terrain/Heightmap.h"
#include "Terrain/Layer.h"
#include "Terrain/RGBLayer.h"
#include "Terrain/TerrainPlateColourOverlay.h"
#include "Terrain/TerrainLayerUndoObject.h"
#include "Terrain/TerrainManager.h"
#include <Util/Image.h>
#include "Vegetation/VegetationMap.h"
#include "TerrainTexturePainter.h"
#include "IEditorImpl.h"

// Header only downcast onto a pure virtual interface, so nothing here needs a symbol out of
// CryDefaultEntities.
#include <DefaultComponents/Geometry/TerrainPlateComponent.h>

#include <BoostPythonMacros.h>
#include <Controls/QuestionDialog.h>
#include <EditorFramework/Events.h>
#include <FileDialogs/SystemFileDialog.h>
#include <LevelEditor/LevelEditorSharedState.h>
#include <QtUtil.h>

namespace Private_TerrainLayerCommands
{

void PyExportTerrainLayers()
{
	if (0 >= GetIEditorImpl()->GetTerrainManager()->GetLayerCount())
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "No layers exist. You have to create some Terrain Layers first.");
		CQuestionDialog::SWarning(QObject::tr("No Layers"), QObject::tr("No layers exist. You have to create some Terrain Layers first."));
		return;
	}

	const QDir dir(QtUtil::GetAppDataFolder());

	CSystemFileDialog::RunParams runParams;
	runParams.initialDir = dir.absolutePath();
	runParams.title = QObject::tr("Export Layers");
	runParams.extensionFilters << CExtensionFilter(QObject::tr("Layer Files (lay)"), "lay");

	const QString filePath = CSystemFileDialog::RunExportFile(runParams, nullptr);
	string path = filePath.toStdString().c_str();

	if (!filePath.isEmpty())
	{
		CryLog("Exporting layer settings to %s", path);

		CXmlArchive ar("LayerSettings");
		GetIEditorImpl()->GetTerrainManager()->SerializeSurfaceTypes(ar);
		GetIEditorImpl()->GetTerrainManager()->SerializeLayerSettings(ar);
		ar.Save(path);
	}
}

static void PyImportTerrainLayers()
{
	const QDir dir(QtUtil::GetAppDataFolder());

	CSystemFileDialog::RunParams runParams;
	runParams.initialDir = dir.absolutePath();
	runParams.title = QObject::tr("Import Layers");
	runParams.extensionFilters << CExtensionFilter(QObject::tr("Layer Files (lay)"), "lay");

	QString fileName = CSystemFileDialog::RunImportFile(runParams, nullptr);
	string path = fileName.toStdString().c_str();

	if (fileName.isEmpty())
	{
		return;
	}

	CryLog("Importing layer settings from %s", path.GetString());

	CUndo undo("Import Texture Layers");
	GetIEditorImpl()->GetIUndoManager()->RecordUndo(new CTerrainLayersPropsUndoObject);

	CXmlArchive ar;
	if (ar.Load(path))
	{
		GetIEditorImpl()->GetTerrainManager()->SerializeSurfaceTypes(ar);
		GetIEditorImpl()->GetTerrainManager()->SerializeLayerSettings(ar);
	}

	GetIEditorImpl()->GetTerrainManager()->ReloadSurfaceTypes();
}

static void PyCreateNewTerrainLayerAt(int index)
{
	CUndo undo("New Terrain Layer");
	GetIEditorImpl()->GetIUndoManager()->RecordUndo(new CTerrainLayersPropsUndoObject);

	CLayer* pNewLayer = new CLayer;
	pNewLayer->SetLayerName("NewLayer");
	pNewLayer->LoadTexture("%ENGINE%/EngineAssets/Textures/white.dds");
	pNewLayer->AssignMaterial("%ENGINE%/EngineAssets/Materials/material_terrain_default");
	pNewLayer->GetOrRequestLayerId();

	GetIEditorImpl()->GetTerrainManager()->AddLayer(pNewLayer, index);
}

static void PyCreateNewTerrainLayer()
{
	PyCreateNewTerrainLayerAt(-1);
}

static void PyDeleteTerrainLayer()
{
	GetIEditorImpl()->GetTerrainManager()->RemoveSelectedLayer();
}

static void PyDuplicateTerrainLayer()
{
	GetIEditorImpl()->GetTerrainManager()->DuplicateSelectedLayer();
}

static void MoveTerrainLayer(int src, int dest)
{
	GetIEditorImpl()->GetTerrainManager()->MoveLayer(src, dest);
}

static void PyMoveTerrainLayerToTop()
{
	const int index = GetIEditorImpl()->GetTerrainManager()->GetSelectedLayerIndex();
	if (index > 0)
	{
		MoveTerrainLayer(index, 0);
	}
}

static void PyMoveTerrainLayerUp()
{
	const int index = GetIEditorImpl()->GetTerrainManager()->GetSelectedLayerIndex();
	if (index > 0)
	{
		MoveTerrainLayer(index, index - 1);
	}
}

static void PyMoveTerrainLayerDown()
{
	const int index = GetIEditorImpl()->GetTerrainManager()->GetSelectedLayerIndex();
	const int count = GetIEditorImpl()->GetTerrainManager()->GetLayerCount();
	if (count > 1 && index < count - 1)
	{
		MoveTerrainLayer(index, index + 2);
	}
}

static void PyMoveTerrainLayerToBottom()
{
	const int index = GetIEditorImpl()->GetTerrainManager()->GetSelectedLayerIndex();
	const int count = GetIEditorImpl()->GetTerrainManager()->GetLayerCount();
	if (count > 1 && index < count - 1)
	{
		MoveTerrainLayer(index, -1);
	}
}

static void PyFloodTerrainLayer()
{
	CLevelEditorSharedState* pLevelEditor = GetIEditorImpl()->GetLevelEditorSharedState();
	CEditTool* pTool = pLevelEditor->GetEditTool();
	if (!pTool || !pTool->IsKindOf(RUNTIME_CLASS(CTerrainTexturePainter)))
	{
		pTool = new CTerrainTexturePainter();
		pLevelEditor->SetEditTool(pTool);
	}

	CTerrainTexturePainter* pPainterTool = static_cast<CTerrainTexturePainter*>(pTool);
	pPainterTool->Action_StopUndo();
	pPainterTool->Action_Flood();
	pPainterTool->Action_StopUndo();
}

} // namespace Private_TerrainLayerCommands

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyExportTerrainLayers, terrain, export_layers, "Export terrain layers")
REGISTER_EDITOR_COMMAND_TEXT(terrain, export_layers, "Export Layers...")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyImportTerrainLayers, terrain, import_layers, "Import terrain layers")
REGISTER_EDITOR_COMMAND_TEXT(terrain, import_layers, "Import Layers...")

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainLayerCommands::PyCreateNewTerrainLayerAt, terrain, create_layer_at, CCommandDescription("Create a new layer").Param("index", "layer index, -1 = last"))

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyCreateNewTerrainLayer, terrain, create_layer, "Create a new layer")
REGISTER_EDITOR_COMMAND_TEXT(terrain, create_layer, "Create Layer")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyDeleteTerrainLayer, terrain, delete_layer, "Delete selected layer")
REGISTER_EDITOR_COMMAND_TEXT(terrain, delete_layer, "Delete Layer")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyDuplicateTerrainLayer, terrain, duplicate_layer, "Duplicate selected layer")
REGISTER_EDITOR_COMMAND_TEXT(terrain, duplicate_layer, "Duplicate Layer")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyMoveTerrainLayerToTop, terrain, move_layer_to_top, "Move selected layer to Top")
REGISTER_EDITOR_COMMAND_TEXT(terrain, move_layer_to_top, "Move to Top")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyMoveTerrainLayerUp, terrain, move_layer_up, "Move selected layer up")
REGISTER_EDITOR_COMMAND_TEXT(terrain, move_layer_up, "Move Up")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyMoveTerrainLayerDown, terrain, move_layer_down, "Move selected layer down")
REGISTER_EDITOR_COMMAND_TEXT(terrain, move_layer_down, "Move Down")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyMoveTerrainLayerToBottom, terrain, move_layer_to_bottom, "Move selected layer to buttom")
REGISTER_EDITOR_COMMAND_TEXT(terrain, move_layer_to_bottom, "Move to Bottom")

REGISTER_PYTHON_COMMAND(Private_TerrainLayerCommands::PyFloodTerrainLayer, terrain, flood_layer, "Floods the selected layer over the all terrain")
REGISTER_EDITOR_COMMAND_TEXT(terrain, flood_layer, "Flood Layer")

namespace Private_TerrainCommands
{

static void PyRefineTerrainTiles()
{
	auto answer = CQuestionDialog::SQuestion(QObject::tr("Error"),
	                                         QObject::tr("Refine TerrainTexture?\r\n"
	                                                     "(all terrain texture tiles become split in 4 parts so a tile with 2048x2048\r\n"
	                                                     "no longer limits the resolution) You need to save afterwards!"));

	if (QDialogButtonBox::StandardButton::Yes != answer)
	{
		return;
	}

	if (!GetIEditorImpl()->GetTerrainManager()->GetRGBLayer()->RefineTiles())
	{
		CQuestionDialog::SCritical(QObject::tr(""), QObject::tr("TerrainTexture refine failed (make sure current data is saved)"));
	}
	else
	{
		CQuestionDialog::SWarning(QObject::tr(""), QObject::tr("Successfully refined TerrainTexture - Save is now required!"));
	}
}


//! Brings painted vegetation instances (and merged-mesh grass) back onto the current ENGINE terrain.
//! Instance Z is sampled once at paint time and stored absolutely, and the stock re-sampler
//! (CVegetationMap::OnHeightMapChanged) runs at mission load only and skips auto-merged instances, so
//! an engine side change such as a terrain plate bake needs this opt-in repair.
static void PyRepositionVegetation()
{
	CVegetationMap* pVegetationMap = GetIEditorImpl()->GetVegetationMap();
	if (!pVegetationMap)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "terrain.reposition_vegetation: no vegetation map (is a level loaded?)");
		return;
	}

	// Area: the bounding box of the current selection if there is one, the whole map otherwise.
	const char* szArea = "whole map";
	const float mapSize = (float)pVegetationMap->GetSize();
	AABB box(Vec3(0.0f, 0.0f, -FLT_MAX), Vec3(mapSize, mapSize, FLT_MAX));

	const CSelectionGroup* pSelection = GetIEditorImpl()->GetSelection();
	if (pSelection && !pSelection->IsEmpty())
	{
		const AABB selectionBox = pSelection->GetBounds();
		if (!selectionBox.IsReset())
		{
			box = selectionBox;
			szArea = "selection";
		}
	}

	const CTimeValue startTime = gEnv->pTimer->GetAsyncTime();

	int moved = 0;
	int skipped = 0;
	{
		// One undo step for the whole run.
		CUndo undo("Reposition Vegetation on Terrain");
		pVegetationMap->RepositionInstancesOnTerrain(box, moved, skipped);
	}

	const float elapsedMs = (gEnv->pTimer->GetAsyncTime() - startTime).GetMilliSeconds();

	CryLog("terrain.reposition_vegetation: moved %d, skipped %d of %d instances, area %s (%.1f, %.1f)-(%.1f, %.1f), %.1f ms",
	       moved, skipped, pVegetationMap->GetNumInstances(), szArea, box.min.x, box.min.y, box.max.x, box.max.y, elapsedMs);

	if (moved > 0)
	{
		GetIEditorImpl()->SetModifiedFlag();
	}
}

//! The terrain plates in the current selection, entity (for the log) plus component. Empty and already
//! warned about when there is nothing to work on.
static std::vector<std::pair<IEntity*, Cry::DefaultComponents::CTerrainPlateComponent*>>
CollectSelectedTerrainPlates(const char* szCommandName)
{
	using Cry::DefaultComponents::CTerrainPlateComponent;
	using Cry::DefaultComponents::ITerrainPlateCommit;

	std::vector<std::pair<IEntity*, CTerrainPlateComponent*>> plates;

	const CSelectionGroup* pSelection = GetIEditorImpl()->GetSelection();
	if (pSelection == nullptr || pSelection->IsEmpty())
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "%s: nothing selected. Select the terrain plate entities and run it again.", szCommandName);
		return plates;
	}

	for (int i = 0; i < pSelection->GetCount(); ++i)
	{
		CBaseObject* pObject = pSelection->GetObject(i);
		if (pObject == nullptr || !pObject->IsKindOf(RUNTIME_CLASS(CEntityObject)))
			continue;

		IEntity* pEntity = static_cast<CEntityObject*>(pObject)->GetIEntity();
		if (pEntity == nullptr)
			continue;

		IEntityComponent* pComponent = pEntity->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
		if (pComponent == nullptr)
			continue;

		plates.emplace_back(pEntity, static_cast<CTerrainPlateComponent*>(pComponent));
	}

	if (plates.empty())
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "%s: no Terrain Plate component in the selection.", szCommandName);
	}

	return plates;
}

//! Every terrain plate entity in the level, selected or not. The selection walk above cannot see the
//! neighbours a height stamp flattens, and those are exactly the ones that have to be told.
static std::vector<std::pair<IEntity*, Cry::DefaultComponents::CTerrainPlateComponent*>>
CollectAllTerrainPlates()
{
	using Cry::DefaultComponents::CTerrainPlateComponent;
	using Cry::DefaultComponents::ITerrainPlateCommit;

	std::vector<std::pair<IEntity*, CTerrainPlateComponent*>> plates;

	if (gEnv == nullptr || gEnv->pEntitySystem == nullptr)
		return plates;

	IEntityItPtr it = gEnv->pEntitySystem->GetEntityIterator();
	if (it == nullptr)
		return plates;

	it->MoveFirst();
	while (!it->IsEnd())
	{
		IEntity* pEntity = it->Next();
		if (pEntity == nullptr)
			continue;

		IEntityComponent* pComponent = pEntity->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
		if (pComponent != nullptr)
			plates.emplace_back(pEntity, static_cast<CTerrainPlateComponent*>(pComponent));
	}

	return plates;
}

//! One plate's height block, read out of the live engine terrain BEFORE anything is written.
struct SHeightStampBlock
{
	IEntity*                                 pEntity = nullptr;
	Cry::DefaultComponents::ITerrainPlateCommit* pCommit = nullptr;

	// Footprint in terrain units (world axes) and in heightmap units (transposed). @see the axis note below.
	int                rectX1 = 0, rectY1 = 0, rectSize = 0;
	int                hx0 = 0, hy0 = 0, cols = 0, rows = 0;

	std::vector<float> block;
	float              blockMin = 0.f;
	float              blockMax = 0.f;
};

//! Writes the LIVE bake of every selected terrain plate into the level's own heightmap, so the relief
//! becomes ordinary terrain and survives the plate being moved or deleted.
//!
//! A PURE ACTION: it changes nothing on the plate that the user can see - no tick box, no property, no
//! visible effect - so pressing it twice is harmless. The plate adopts the stamped ground as its own
//! baseline and keeps baking; the second press finds the heightmap already holding what it would write
//! and does nothing.
//! The block is read straight back out of the live engine terrain rather than recomputed, so it is the
//! SAME block the bake produced - every batch, cascade, priority and rim shape is already in the ground.
//! The loss is the engine's 12 bit per sector quantisation, a few mm, against the heightmap's own 1.6 cm.
//!
//! THREE PHASES, and the order is the whole point. CHeightmap::UpdateEngineTerrain pushes the EDITOR's
//! heights over a sector snapped square, which flattens every live bake inside that square - including the
//! ones belonging to plates that have not been read yet. So: read every block first, then write them all
//! into CHeightmap, then push ONCE over the union, then tell the neighbours the ground moved.
static void PyStampPlateHeight()
{
	using Cry::DefaultComponents::CTerrainPlateComponent;
	using Cry::DefaultComponents::ITerrainPlateCommit;

	CHeightmap* pHeightmap = GetIEditorImpl()->GetHeightmap();
	I3DEngine* p3DEngine = GetIEditorImpl()->Get3DEngine();
	if (pHeightmap == nullptr || p3DEngine == nullptr || p3DEngine->GetITerrain() == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "terrain.stamp_plate_height: no terrain (is a level loaded?)");
		return;
	}

	const std::vector<std::pair<IEntity*, CTerrainPlateComponent*>> plates = CollectSelectedTerrainPlates("terrain.stamp_plate_height");
	if (plates.empty())
		return;

	const float unitSize = pHeightmap->GetUnitSize();
	// The heightmap is stored transposed against the world: heightmap X is the world Y unit and heightmap
	// Y is the world X unit, linear index x + y * width (CHeightmap::WorldToHmap, and the same swap in
	// every consumer). The plate's rect is in ENGINE units, which are not swapped, so the mapping is
	// hmapX = worldYunit, hmapY = worldXunit.
	const int hmapUnitsY = (int)pHeightmap->GetWidth();    // heightmap X axis: world Y
	const int hmapUnitsX = (int)pHeightmap->GetHeight();   // heightmap Y axis: world X
	if (unitSize <= 0.f || hmapUnitsX <= 0 || hmapUnitsY <= 0)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "terrain.stamp_plate_height: the level has no usable heightmap.");
		return;
	}

	const CTimeValue startTime = gEnv->pTimer->GetAsyncTime();
	const float maxHeight = pHeightmap->GetMaxHeight();

	// PHASE 1 - read. Nothing is written until every selected plate has its block, because the write of the
	// first would flatten the live bake of the second.
	std::vector<SHeightStampBlock> jobs;
	jobs.reserve(plates.size());

	for (const std::pair<IEntity*, CTerrainPlateComponent*>& entry : plates)
	{
		IEntity* pEntity = entry.first;
		ITerrainPlateCommit* pCommit = entry.second;

		if (!pCommit->IsBakeIntoTerrainEnabled())
		{
			// Not a failure: there is no baked relief on screen, so there is nothing to keep.
			CryLog("terrain.stamp_plate_height: '%s' - nothing to stamp: Bake Into Terrain is off.", pEntity->GetName());
			continue;
		}

		SHeightStampBlock job;
		job.pEntity = pEntity;
		job.pCommit = pCommit;

		if (!pCommit->GetCommitFootprint(job.rectX1, job.rectY1, job.rectSize))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			           "terrain.stamp_plate_height: '%s' holds no bake in the terrain right now (its height may be stamped "
			           "already). Nothing written.", pEntity->GetName());
			continue;
		}

		// The engine block covers units rectX1 .. rectX1 + rectSize INCLUSIVE, so the square is
		// rectSize + 1 cells on a side.
		const int dim = job.rectSize + 1;
		job.hx0 = job.rectY1;   // heightmap X, world Y
		job.hy0 = job.rectX1;   // heightmap Y, world X
		job.cols = min(dim, hmapUnitsY - job.hx0);   // along heightmap X
		job.rows = min(dim, hmapUnitsX - job.hy0);   // along heightmap Y
		if (job.cols <= 0 || job.rows <= 0)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			           "terrain.stamp_plate_height: REFUSED for '%s' - its footprint is off the heightmap, so the block reads "
			           "back as 0 cells. The plate keeps its live bake.", pEntity->GetName());
			continue;
		}

		job.block.assign((size_t)job.rows * job.cols, 0.f);
		job.blockMin = FLT_MAX;
		job.blockMax = -FLT_MAX;
		for (int ux = 0; ux < job.rows; ++ux)      // along world X
		{
			for (int uy = 0; uy < job.cols; ++uy)  // along world Y
			{
				// GetTerrainZ truncates to units and does not interpolate; the half unit offset only defends
				// against a float multiply landing just under the integer. Same sampler the baseline uses.
				const float z = p3DEngine->GetTerrainZ(((float)(job.rectX1 + ux) + 0.5f) * unitSize,
				                                       ((float)(job.rectY1 + uy) + 0.5f) * unitSize);
				job.block[(size_t)ux * job.cols + uy] = z;
				job.blockMin = min(job.blockMin, z);
				job.blockMax = max(job.blockMax, z);
			}
		}

		// CHeightmap::Serialize saves heights as uint16 scaled by 65535 / m_fMaxHeight and clamps, and a
		// negative height wraps through the same clamp to the ceiling. Either way the level looks right
		// until it is reloaded, so refuse loudly and write nothing.
		if (job.blockMax > maxHeight || job.blockMin < 0.f)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR,
			           "terrain.stamp_plate_height: REFUSED for '%s'. The ground spans %.2f .. %.2f m and the level's "
			           "terrain range is 0 .. %.2f m (Max Height). Writing it would be silently flattened to %.2f m at the "
			           "next save and reload. Raise Max Height in Terrain Editor > Modify, or lower the plate, and try again.",
			           pEntity->GetName(), job.blockMin, job.blockMax, maxHeight, maxHeight);
			continue;
		}

		jobs.push_back(std::move(job));
	}

	if (jobs.empty())
	{
		CryLog("terrain.stamp_plate_height: nothing to stamp in the selection.");
		return;
	}

	// ONE undo group for the whole button press. It covers the heightmap and nothing else: the plates' own
	// state is runtime state, not a terrain edit, and an undo leaves it alone. That is harmless now - a plate
	// whose baseline is one stamp ahead of the terrain re-bakes over the restored ground on its next poll.
	CUndo undo("Stamp Plate Height");

	// PHASE 2 - write, all of them, into CHeightmap only. The engine terrain is not touched yet.
	int stamped = 0;
	int unionHx0 = INT_MAX, unionHy0 = INT_MAX, unionHx1 = INT_MIN, unionHy1 = INT_MIN;
	float unionZMin = FLT_MAX, unionZMax = -FLT_MAX;
	std::vector<SHeightStampBlock*> written;
	written.reserve(jobs.size());

	for (SHeightStampBlock& job : jobs)
	{
		// The sector aligned square reaches well outside the plate, and an untouched cell still reads back
		// one engine quantisation step off. Only write a cell that moved by more than that readback noise.
		// The step is the coarsest any touched sector can have (12 bits over the block's span).
		const float epsilon = max(0.0001f, (job.blockMax - job.blockMin) / 4095.f);

		int cellsToWrite = 0;
		for (int ux = 0; ux < job.rows && cellsToWrite == 0; ++ux)
		{
			for (int uy = 0; uy < job.cols; ++uy)
			{
				if (fabsf(job.block[(size_t)ux * job.cols + uy] - pHeightmap->GetXY(job.hx0 + uy, job.hy0 + ux)) > epsilon)
				{
					++cellsToWrite;
					break;
				}
			}
		}

		if (cellsToWrite == 0)
		{
			// Silent success, not a refusal: the level heightmap already holds this block, so the stamp has nothing
			// left to do. This is what a second press on an already stamped plate looks like.
			CryLog("terrain.stamp_plate_height: '%s' - the level heightmap already holds this ground, nothing to write.",
			       job.pEntity->GetName());
			written.push_back(&job);
			++stamped;
			continue;
		}

		pHeightmap->RecordUndo(job.hx0, job.hy0, job.cols, job.rows, /*bInfo*/ false);

		int   cellsWritten = 0;
		float minDelta = 0.f;
		float maxDelta = 0.f;
		for (int ux = 0; ux < job.rows; ++ux)
		{
			for (int uy = 0; uy < job.cols; ++uy)
			{
				const float z = job.block[(size_t)ux * job.cols + uy];
				const float delta = z - pHeightmap->GetXY(job.hx0 + uy, job.hy0 + ux);
				if (fabsf(delta) <= epsilon)
					continue;

				pHeightmap->SetXY(job.hx0 + uy, job.hy0 + ux, z);
				++cellsWritten;
				minDelta = min(minDelta, delta);
				maxDelta = max(maxDelta, delta);
			}
		}

		// Only a plate that actually moved a cell joins the pushed square.
		unionHx0 = min(unionHx0, job.hx0);
		unionHy0 = min(unionHy0, job.hy0);
		unionHx1 = max(unionHx1, job.hx0 + job.cols);
		unionHy1 = max(unionHy1, job.hy0 + job.rows);
		unionZMin = min(unionZMin, job.blockMin);
		unionZMax = max(unionZMax, job.blockMax);
		written.push_back(&job);
		++stamped;

		CryLog("terrain.stamp_plate_height: '%s' stamped, %d of %d cells written, delta %.3f .. %.3f m, "
		       "units (%d,%d)+%d, ground %.2f .. %.2f m of a %.2f m range",
		       job.pEntity->GetName(), cellsWritten, job.rows * job.cols, minDelta, maxDelta,
		       job.rectX1, job.rectY1, job.rectSize, job.blockMin, job.blockMax, maxHeight);
	}

	if (stamped == 0)
	{
		CryLog("terrain.stamp_plate_height: nothing was written.");
		return;
	}

	// PHASE 3 - one push, over the union of everything written. UpdateEngineTerrain ignores its 4th argument
	// and always pushes a SQUARE of side areaSize snapped out to whole sectors, so it gets the larger side.
	// Skipped entirely when every plate was already in the heightmap: there is nothing to push, and pushing
	// anyway would flatten the live bakes inside the square for no reason.
	const bool bWroteHeights = (unionHx0 != INT_MAX);
	const int  side = bWroteHeights ? max(unionHx1 - unionHx0, unionHy1 - unionHy0) : 0;

	if (bWroteHeights)
	{
		pHeightmap->UpdateEngineTerrain(unionHx0, unionHy0, side, side, CHeightmap::ETerrainUpdateType::Elevation);
		GetIEditorImpl()->GetTerrainManager()->SetModified(unionHx0, unionHy0, unionHx1, unionHy1);

		AABB worldBox(Vec3((float)unionHy0 * unitSize, (float)unionHx0 * unitSize, unionZMin - 1.f),
		              Vec3((float)unionHy1 * unitSize, (float)unionHx1 * unitSize, unionZMax + 1.f));
		GetIEditorImpl()->UpdateViews(eUpdateHeightmap, &worldBox);
	}

	// The adoption, INSIDE the undo group and only now that the heights are in: each plate takes the stamped
	// ground as its own baseline, so its next bake writes the same heights again (a no-op) and a move or a
	// delete hands the STAMPED ground back rather than the old one. Nothing else about the plate changes.
	for (SHeightStampBlock* pJob : written)
	{
		pJob->pCommit->OnHeightStamped();
	}

	if (bWroteHeights)
	{
		p3DEngine->GetITerrain()->OnTerrainPaintActionComplete();
		GetIEditorImpl()->SetModifiedFlag();

		// The push above wrote the EDITOR's heights over a sector snapped square, so any OTHER plate still baking
		// inside it has just been flattened. Bring their self repair forward instead of leaving it to the poll
		// interval. For a stamped plate the ground and its adopted baseline agree, so its own repair is a no-op.
		const int nudgeRectX1 = unionHy0;   // terrain units: world X
		const int nudgeRectY1 = unionHx0;   // terrain units: world Y
		for (const std::pair<IEntity*, CTerrainPlateComponent*>& entry : CollectAllTerrainPlates())
		{
			// Through the INTERFACE, always: the overrides are final, so a call on the concrete type devirtualises
			// into a direct call and Sandbox has no symbol for it (CryDefaultEntities is never linked).
			ITerrainPlateCommit* pOther = entry.second;
			pOther->NudgeBakeAfterTerrainWrite(nudgeRectX1, nudgeRectY1, side);
		}
	}

	const float elapsedMs = (gEnv->pTimer->GetAsyncTime() - startTime).GetMilliSeconds();
	CryLog("terrain.stamp_plate_height: %d of %d selected plate(s) stamped, %.1f ms. Save the level to keep it; "
	       "the plates go on working exactly as before, and undo restores the terrain.",
	       stamped, (int)plates.size(), elapsedMs);
}

//! Names any live colour plate that is NOT in the selection, shares ground with the plate about to be
//! stamped, and composites UNDER it.
//!
//! Such a plate was blended BEFORE the stamped one in the preview, but the stamp writes into a CRGBLayer
//! that does not carry it, and it then goes on drawing live over the stamped ground - so in the shared rim
//! the two swap places and the picture no longer matches what the preview showed. A plate that composites
//! OVER the stamped one is not affected: it drew last before and it draws last now.
//! Compositing the unselected plate in as well is not the answer - it would then be blended twice, once in
//! the ground and once live, which tightens its rim and puts it on top of the stamped plate anyway. The
//! honest fix is to select both and stamp them together, which is what the line says.
static void WarnAboutLivePlatesUnder(IEntity* pStampedEntity, int rectX1, int rectY1, int rectSize,
                                     const std::vector<std::pair<IEntity*, Cry::DefaultComponents::CTerrainPlateComponent*>>& selected)
{
	using Cry::DefaultComponents::CTerrainPlateComponent;
	using Cry::DefaultComponents::ITerrainPlateCommit;

	for (const std::pair<IEntity*, CTerrainPlateComponent*>& entry : CollectAllTerrainPlates())
	{
		IEntity* pOtherEntity = entry.first;
		if (pOtherEntity == nullptr || pOtherEntity == pStampedEntity || pOtherEntity->IsHidden())
			continue;

		bool bSelected = false;
		for (const std::pair<IEntity*, CTerrainPlateComponent*>& sel : selected)
			bSelected = bSelected || (sel.first == pOtherEntity);

		if (bSelected)
			continue;

		// Same query the overlay makes: false means this plate is drawing no colour, so it cannot disagree
		// with anything.
		ITerrainPlateCommit* pOther = entry.second;
		int    otherX1 = 0, otherY1 = 0, otherSize = 0;
		ColorB otherTint;
		bool   bSettling = false;
		if (!pOther->GetLiveColourState(otherX1, otherY1, otherSize, otherTint, bSettling) || otherSize <= 0)
			continue;

		// The rects cover x1 .. x1 + size INCLUSIVE.
		const bool bOverlaps = (rectX1 <= otherX1 + otherSize) && (otherX1 <= rectX1 + rectSize)
		                       && (rectY1 <= otherY1 + otherSize) && (otherY1 <= rectY1 + rectSize);
		if (!bOverlaps || !TerrainPlateColourOverlay::CompositesBefore(pOtherEntity, pStampedEntity))
			continue;

		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "terrain.stamp_plate_colour: '%s' shares ground with '%s', which is showing colour and is not in the "
		           "selection. '%s' has the lower Bake Priority, so it drew UNDER '%s' in the preview but will draw over "
		           "the stamped ground where they meet. Select both and stamp them together to keep the picture.",
		           pStampedEntity->GetName(), pOtherEntity->GetName(), pOtherEntity->GetName(), pStampedEntity->GetName());
	}
}

//! The PLATE half of one colour stamp's undo step. RecordTerrainAppearanceUndo restores the RGB tiles;
//! this restores the plate's "the ground already carries my colour" record, which is what makes the live
//! preview stand down. Without it an undo gave back the old ground colour while the preview stayed stood
//! down, so the plate's colour was missing from the picture until the plate was nudged.
//! Recorded AFTER the tile undo object, because a step undoes its objects in reverse: the record is
//! cleared first, then the tiles come back, and the preview takes over the restored ground.
class CUndoPlateColourStamp : public IUndoObject
{
public:
	explicit CUndoPlateColourStamp(EntityId plateId) : m_plateId(plateId) {}

private:
	virtual const char* GetDescription() override { return "Terrain Plate Colour Stamp"; }
	virtual void        Undo(bool bUndo) override { SetRecord(false); }
	virtual void        Redo() override           { SetRecord(true); }

	void SetRecord(bool bStamped)
	{
		using Cry::DefaultComponents::CTerrainPlateComponent;
		using Cry::DefaultComponents::ITerrainPlateCommit;

		// Re-resolved every time: the plate can be deleted between the stamp and the undo, and the tiles are
		// restored either way.
		if (gEnv == nullptr || gEnv->pEntitySystem == nullptr)
			return;

		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(m_plateId);
		if (pEntity == nullptr)
			return;

		IEntityComponent* pComponent = pEntity->GetComponentByTypeId(ITerrainPlateCommit::GetComponentTypeId());
		if (pComponent == nullptr)
			return;

		// Through the INTERFACE, always: the overrides are final, so a call on the concrete type devirtualises
		// and Sandbox has no symbol for it.
		ITerrainPlateCommit* pCommit = static_cast<CTerrainPlateComponent*>(pComponent);
		pCommit->SetColourStampRecord(bStamped);
	}

	EntityId m_plateId;
};

//! Writes the terrain colour every selected plate is SHOWING into the level's own CRGBLayer, so the colour
//! is permanent, ships with the level and survives the plate being moved or deleted.
//!
//! A PURE ACTION, like Stamp Height: no tick box moves and the picture does not change. The plate records
//! that the ground now carries its colour, which is what stops the preview and the export blending the same
//! colour over itself; a second press finds the ground already carrying it and writes nothing.
//!
//! WYSIWYG by construction: it does not re-derive the picture, it runs the live overlay's own compositing
//! loop (TerrainPlateColourOverlay::ApplyStampToRow -> the file local ApplyPlatesToRow that
//! CHeightmap::GetColorAtPosition and the exporter also go through) over the RGB layer's texels, at the RGB
//! layer's own texel size. Same rim weight, same albedo sampler, same blend, same epsilon.
//!
//! Colour only. The live overlay implies no surface type weights, so stamping any would put something on the
//! ground the user never saw. Refresh is the texture only path - UpdateSectorTexture - so this can never
//! touch a height, which is what let the old version flatten neighbouring bakes.
static void PyStampPlateColour()
{
	using Cry::DefaultComponents::CTerrainPlateComponent;
	using Cry::DefaultComponents::ITerrainPlateCommit;

	CHeightmap* pHeightmap = GetIEditorImpl()->GetHeightmap();
	I3DEngine* p3DEngine = GetIEditorImpl()->Get3DEngine();
	CTerrainManager* pTerrainManager = GetIEditorImpl()->GetTerrainManager();
	if (pHeightmap == nullptr || p3DEngine == nullptr || p3DEngine->GetITerrain() == nullptr || pTerrainManager == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "terrain.stamp_plate_colour: no terrain (is a level loaded?)");
		return;
	}

	std::vector<std::pair<IEntity*, CTerrainPlateComponent*>> plates = CollectSelectedTerrainPlates("terrain.stamp_plate_colour");
	if (plates.empty())
		return;

	// THE composite order, the one the preview and the export sort themselves by. Each plate is written into
	// CRGBLayer over what the previous one already wrote, so walking the selection in this order reproduces
	// the preview's stack exactly; the selection's own order is arbitrary and would not.
	std::sort(plates.begin(), plates.end(),
	          [](const std::pair<IEntity*, CTerrainPlateComponent*>& a, const std::pair<IEntity*, CTerrainPlateComponent*>& b)
	{
		return TerrainPlateColourOverlay::CompositesBefore(a.first, b.first);
	});

	const float unitSize = pHeightmap->GetUnitSize();
	const float terrainSize = (float)p3DEngine->GetTerrainSize();          // metres
	const int   hmapUnitsY = (int)pHeightmap->GetWidth();                  // heightmap X axis: world Y
	const int   hmapUnitsX = (int)pHeightmap->GetHeight();                 // heightmap Y axis: world X
	if (unitSize <= 0.f || terrainSize <= 0.f || hmapUnitsX <= 0 || hmapUnitsY <= 0)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "terrain.stamp_plate_colour: the level has no usable heightmap.");
		return;
	}

	// With this on the terrain colour is synthesised from the detail materials and a written RGB layer is
	// ignored, so the stamp would appear to do nothing.
	ICVar* pAutoBaseTexture = (gEnv->pConsole != nullptr) ? gEnv->pConsole->GetCVar("e_TerrainAutoGenerateBaseTexture") : nullptr;
	if (pAutoBaseTexture != nullptr && pAutoBaseTexture->GetIVal() != 0)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "terrain.stamp_plate_colour: e_TerrainAutoGenerateBaseTexture is on, so the terrain colour is derived from the "
		           "detail materials and what is written here will not be visible. Set it to 0 to see the stamped colour.");
	}

	CRGBLayer* pRGBLayer = pTerrainManager->GetRGBLayer();
	if (pRGBLayer == nullptr || pRGBLayer->GetTileCountX() == 0 || pRGBLayer->GetTileCountY() == 0)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
		           "terrain.stamp_plate_colour: the level has no terrain texture allocated, so there is nowhere to write. "
		           "Generate the terrain texture once (Terrain Editor > File) and run it again.");
		return;
	}

	const uint32 tileCountX = pRGBLayer->GetTileCountX();   // along fpx, i.e. world Y
	const uint32 tileCountY = pRGBLayer->GetTileCountY();   // along fpy, i.e. world X

	const CTimeValue startTime = gEnv->pTimer->GetAsyncTime();
	int stamped = 0;

	// ONE undo group, covering this button press only - the height stamp has its own, so the two never share
	// a step.
	CUndo undo("Stamp Plate Colour");

	for (const std::pair<IEntity*, CTerrainPlateComponent*>& entry : plates)
	{
		IEntity* pEntity = entry.first;
		ITerrainPlateCommit* pCommit = entry.second;

		if (!pCommit->IsLiveColourEnabled())
		{
			// Not a failure: the ground under this plate is showing no plate colour, so there is nothing to keep.
			CryLog("terrain.stamp_plate_colour: '%s' - nothing to stamp: Terrain Colour From Plate is off.", pEntity->GetName());
			continue;
		}

		if (pCommit->IsColourStamped())
		{
			// Silent success: the ground under this plate already holds exactly these texels, so a second press has
			// nothing to write and CRGBLayer stays bit identical. Cleared as soon as the plate itself changes.
			CryLog("terrain.stamp_plate_colour: '%s' - the ground already carries this colour, nothing to write.",
			       pEntity->GetName());
			continue;
		}

		int rectX1 = 0, rectY1 = 0, rectSize = 0;
		if (!pCommit->GetCommitAppearanceFootprint(rectX1, rectY1, rectSize) || rectSize <= 0)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			           "terrain.stamp_plate_colour: '%s' has no footprint on the terrain.", pEntity->GetName());
			continue;
		}

		// The overlay's snapshot of THIS plate, read now. False means the same thing as above; the two tests
		// come from the same query, so they cannot disagree.
		if (!TerrainPlateColourOverlay::BeginStamp(pEntity->GetId()))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			           "terrain.stamp_plate_colour: '%s' is showing no terrain colour right now (hidden?). Nothing written.",
			           pEntity->GetName());
			continue;
		}

		WarnAboutLivePlatesUnder(pEntity, rectX1, rectY1, rectSize, plates);

		// The footprint in world metres and in the RGB layer's normalised measure. The normalised
		// coordinate fpx is the world Y and fpy the world X, which is why every pair below reads (Y, X).
		const float worldXMin = (float)rectX1 * unitSize;
		const float worldYMin = (float)rectY1 * unitSize;
		const float worldXMax = (float)(rectX1 + rectSize) * unitSize;
		const float worldYMax = (float)(rectY1 + rectSize) * unitSize;

		const float normXMin = clamp_tpl(worldXMin / terrainSize, 0.f, 1.f);
		const float normYMin = clamp_tpl(worldYMin / terrainSize, 0.f, 1.f);
		const float normXMax = clamp_tpl(worldXMax / terrainSize, 0.f, 1.f);
		const float normYMax = clamp_tpl(worldYMax / terrainSize, 0.f, 1.f);

		// One undo object per plate, snapshotting the RGB tiles the footprint touches, taken BEFORE anything
		// is written. The same object the texture painter records for a dab.
		RecordTerrainAppearanceUndo((normYMin + normYMax) * 0.5f, (normXMin + normXMax) * 0.5f,
		                            max(normYMax - normYMin, normXMax - normXMin) * 0.5f);

		const int tileX0 = clamp_tpl((int)floorf(normYMin * tileCountX), 0, (int)tileCountX - 1);
		const int tileX1 = clamp_tpl((int)ceilf(normYMax * tileCountX) - 1, 0, (int)tileCountX - 1);
		const int tileY0 = clamp_tpl((int)floorf(normXMin * tileCountY), 0, (int)tileCountY - 1);
		const int tileY1 = clamp_tpl((int)ceilf(normXMax * tileCountY) - 1, 0, (int)tileCountY - 1);

		int colourTexels = 0;

		for (int ty = tileY0; ty <= tileY1; ++ty)
		{
			for (int tx = tileX0; tx <= tileX1; ++tx)
			{
				// GetTileImage is the write handle: it loads the tile if needed and marks it dirty, unlike
				// SetValueAt(float, float, ...), which writes through a private pointer and leaves the
				// tile clean.
				CImageEx* pTile = pRGBLayer->GetTileImage(tx, ty, true);
				if (pTile == nullptr || pTile->GetWidth() <= 0 || pTile->GetHeight() <= 0)
					continue;

				const int resX = pTile->GetWidth();
				const int resY = pTile->GetHeight();

				// One RGB texel's own footprint in metres. The live pull derives the same number from
				// CRGBLayer::CalcMaxLocalResolution, which IS this tile's resolution over the terrain.
				const float texelSize = max(terrainSize / (float)(resX * (int)tileCountX),
				                            terrainSize / (float)(resY * (int)tileCountY));
				const float worldStepY = terrainSize / (float)(resX * (int)tileCountX);

				const int px0 = clamp_tpl((int)floorf((normYMin * tileCountX - (float)tx) * resX), 0, resX - 1);
				const int px1 = clamp_tpl((int)ceilf((normYMax * tileCountX - (float)tx) * resX), 0, resX - 1);
				const int py0 = clamp_tpl((int)floorf((normXMin * tileCountY - (float)ty) * resY), 0, resY - 1);
				const int py1 = clamp_tpl((int)ceilf((normXMax * tileCountY - (float)ty) * resY), 0, resY - 1);

				for (int py = py0; py <= py1; ++py)
				{
					const float fpy = ((float)ty + ((float)py + 0.5f) / (float)resY) / (float)tileCountY;
					const float fpx0 = ((float)tx + ((float)px0 + 0.5f) / (float)resX) / (float)tileCountX;

					// A tile row is contiguous in px (CImageEx::ValueAt is x + y * width), and px walks world Y -
					// exactly the row shape the live overlay is handed by CHeightmap::GetColorAtPosition.
					colourTexels += TerrainPlateColourOverlay::ApplyStampToRow(&pTile->ValueAt(px0, py), px1 - px0 + 1,
					                                                          fpx0 * terrainSize, worldStepY,
					                                                          fpy * terrainSize, texelSize);
				}
			}
		}

		TerrainPlateColourOverlay::EndStamp();

		if (colourTexels == 0)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			           "terrain.stamp_plate_colour: '%s' wrote nothing (no sampleable material on the plate?).",
			           pEntity->GetName());
			continue;
		}

		// Refresh, texture only: the painter's own recipe, (iY, iX) axis order included. Deliberately NOT
		// UpdateEngineTerrain - that pushes editor HEIGHTS over a sector snapped square and would flatten every
		// live bake under it. A colour stamp must not be able to move the ground.
		const int texSectorSize = p3DEngine->GetTerrainTextureNodeSizeMeters();
		if (texSectorSize > 0)
		{
			const int sectorCount = (int)terrainSize / texSectorSize;
			const int minSecX = max((int)floorf(worldXMin / (float)texSectorSize), 0);
			const int minSecY = max((int)floorf(worldYMin / (float)texSectorSize), 0);
			const int maxSecX = min((int)ceilf(worldXMax / (float)texSectorSize), sectorCount);
			const int maxSecY = min((int)ceilf(worldYMax / (float)texSectorSize), sectorCount);

			for (int iY = minSecY; iY < maxSecY; ++iY)
			{
				for (int iX = minSecX; iX < maxSecX; ++iX)
				{
					pHeightmap->UpdateSectorTexture(CPoint(iY, iX), normYMin, normXMin, normYMax, normXMax);
				}
			}
		}

		// The colour is in the level now, so the preview has to stop compositing it a second time - over the
		// viewport AND over the export. No setting changes; the picture does not change either, because what
		// was written IS what the preview drew, and the repaint below reads it straight back out of CRGBLayer.
		// Recorded after the tile snapshot above, so an undo clears the record first and then restores the
		// tiles: the preview is live again over exactly the ground it was drawing on before the stamp.
		if (CUndo::IsRecording())
			CUndo::Record(new CUndoPlateColourStamp(pEntity->GetId()));

		pCommit->OnColourStamped();

		const int hx0 = rectY1;   // heightmap X, world Y
		const int hy0 = rectX1;   // heightmap Y, world X
		pTerrainManager->SetModified(hx0, hy0, hx0 + min(rectSize + 1, hmapUnitsY - hx0),
		                             hy0 + min(rectSize + 1, hmapUnitsX - hy0));
		++stamped;

		CryLog("terrain.stamp_plate_colour: '%s' stamped, %d colour texels, units (%d,%d)+%d, "
		       "world (%.1f, %.1f)-(%.1f, %.1f)",
		       pEntity->GetName(), colourTexels, rectX1, rectY1, rectSize, worldXMin, worldYMin, worldXMax, worldYMax);
	}

	if (stamped > 0)
	{
		GetIEditorImpl()->SetModifiedFlag();
		GetIEditorImpl()->UpdateViews(eUpdateHeightmap);
	}

	const float elapsedMs = (gEnv->pTimer->GetAsyncTime() - startTime).GetMilliSeconds();
	CryLog("terrain.stamp_plate_colour: %d of %d selected plate(s) stamped, %.1f ms. Save the level to keep it; "
	       "the plates go on working exactly as before, one undo puts the terrain colour back, and no height is "
	       "touched.",
	       stamped, (int)plates.size(), elapsedMs);
}

} // namespace Private_TerrainCommands

REGISTER_PYTHON_COMMAND(Private_TerrainCommands::PyRefineTerrainTiles, terrain, refine_tiles, "Split the tiles into smaller tiles")
REGISTER_EDITOR_COMMAND_TEXT(terrain, refine_tiles, "Refine Terrain Texture Tiles")

REGISTER_PYTHON_COMMAND_WITH_EXAMPLE(Private_TerrainCommands::PyRepositionVegetation, terrain, reposition_vegetation,
                                     "Moves painted vegetation and merged-mesh grass back onto the current engine terrain "
                                     "(selection bounding box if something is selected, whole map otherwise).",
                                     "terrain.reposition_vegetation()")
REGISTER_EDITOR_COMMAND_TEXT(terrain, reposition_vegetation, "Reposition Vegetation on Terrain")

REGISTER_PYTHON_COMMAND_WITH_EXAMPLE(Private_TerrainCommands::PyStampPlateHeight, terrain, stamp_plate_height,
                                     "Writes the baked terrain height of every selected terrain plate into the level's own "
                                     "heightmap, so the relief becomes ordinary terrain and stays after the plate is deleted. "
                                     "Refuses a plate whose ground would not fit the level's Max Height, and one whose bake is "
                                     "not in the terrain.",
                                     "terrain.stamp_plate_height()")
REGISTER_EDITOR_COMMAND_TEXT(terrain, stamp_plate_height, "Stamp Plate Height")

REGISTER_PYTHON_COMMAND_WITH_EXAMPLE(Private_TerrainCommands::PyStampPlateColour, terrain, stamp_plate_colour,
                                     "Writes the terrain colour every selected terrain plate is showing into the level's own "
                                     "terrain colour, texel for texel, so it is permanent and ships with the level. Independent "
                                     "of the height stamp: run it before, after or instead, with its own undo step.",
                                     "terrain.stamp_plate_colour()")
REGISTER_EDITOR_COMMAND_TEXT(terrain, stamp_plate_colour, "Stamp Plate Colour")

namespace Private_TerrainToolsCommands
{
void EnsureTerrainEditorActive()
{
	CTabPaneManager* pPaneManager = CTabPaneManager::GetInstance();
	if (pPaneManager)
	{
		pPaneManager->OpenOrCreatePane("Terrain Editor");
	}
}

void Flatten()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.flatten_tool").SendToKeyboardFocus();
}

void Smooth()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.smooth_tool").SendToKeyboardFocus();
}

void RaiseLower()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.raise_lower_tool").SendToKeyboardFocus();
}

void Duplicate()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.duplicate_tool").SendToKeyboardFocus();
}

void MakeHoles()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.make_holes_tool").SendToKeyboardFocus();
}

void FillHoles()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.fill_holes_tool").SendToKeyboardFocus();
}

void PaintLayer()
{
	EnsureTerrainEditorActive();
	CommandEvent("terrain.paint_texture_tool").SendToKeyboardFocus();
}
}

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::Flatten, terrain, flatten_tool,
                                   CCommandDescription("Switch to terrain flatten tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, flatten_tool, "Terrain Flatten Tool", "", "", false)

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::Smooth, terrain, smooth_tool,
                                   CCommandDescription("Switch to terrain smooth tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, smooth_tool, "Terrain Smooth Tool", "", "", false)

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::RaiseLower, terrain, raise_lower_tool,
                                   CCommandDescription("Switch to terrain raise/lower tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, raise_lower_tool, "Terrain Raise/Lower Tool", "", "", false)

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::Duplicate, terrain, duplicate_tool,
                                   CCommandDescription("Switch to terrain duplicate tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, duplicate_tool, "Terrain Duplicate Tool", "", "", false)

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::MakeHoles, terrain, make_holes_tool,
                                   CCommandDescription("Switch to terrain make holes tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, make_holes_tool, "Terrain Make Holes Tool", "", "", false)

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::FillHoles, terrain, fill_holes_tool,
                                   CCommandDescription("Switch to terrain fill holes tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, fill_holes_tool, "Terrain Fill Holes Tool", "", "", false)

REGISTER_EDITOR_AND_SCRIPT_COMMAND(Private_TerrainToolsCommands::PaintLayer, terrain, paint_texture_tool,
                                   CCommandDescription("Switch to terrain texture painting tool"))
REGISTER_EDITOR_UI_COMMAND_DESC(terrain, paint_texture_tool, "Terrain Texture Paint Tool", "", "", false)
REGISTER_COMMAND_REMAPPING(edit_mode, terrain_painter, terrain, paint_texture_tool)
