// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
//
// The grade component's files, as CryEngine assets (S10 item 4b).
//
// Three things live here, all of them CPU-side and none of them known to the renderer:
//
//   1. the .cinegrade PRESET - the whole CineCam Grade state in one XML file, written and read
//      through the component's OWN reflected structs, so that adding a control to the component
//      cannot silently drop it from every preset;
//   2. the .cryasset SIDECAR writer - twelve lines of XML that make a file we just wrote a
//      registered asset immediately, with no editor, no resource compiler and no waiting for a
//      file watcher (research/s10-4b-asset-system.md section 1 and section 3);
//   3. `cinecam_ImportGradeAssets`, which copies the capture path's .cdl / .curve_master.cube /
//      .curves.txt out of a take folder and into the asset tree with sidecars.
//
// The asset TYPES themselves are registered by the Sandbox plugin
// Code/Sandbox/Plugins/CinematicCameraEditor; the only contract between the two is the four
// strings in CineCamLutTypes.h.
#pragma once

#include "CineGradeComponent.h"

namespace CineCamAssets
{

//! The whole CineCam Grade state, as it is written to a .cinegrade file. Version 1.
//! The three groups are serialised through Schematyc's own class serialiser, i.e. through exactly
//! the reflection the inspector uses, so the file's field names ARE the component's member names.
struct SGradePreset
{
	//! Bumped only if a field has to change meaning. A file with a higher version than we know is
	//! refused with a message instead of half-applied.
	int              version = 1;
	//! Free text: which entity and which level this came from. Never read back; it is there so a
	//! preset found in six months can be traced.
	string           source;

	SCineGradeParams grade;
	SCineCurveParams curves;
	SCineLookParams  look;

	void Serialize(Serialization::IArchive& archive);
};

//! Highest `version` this build understands.
constexpr int kPresetVersion = 1;

//! Turn an entity or file name into something safe for a file system and for a cryasset id:
//! `[A-Za-z0-9_-]`, everything else collapsed to '_', never empty.
string SanitiseAssetName(const char* szName);

//! Resolve an asset-relative folder (e.g. CINECAM_FOLDER_GRADES) to a real absolute path and make
//! sure it exists. Returns false and warns when the write path cannot be resolved.
bool   EnsureAssetFolder(const char* szRelFolder, string& outAbsFolder);

//! Write `<szAbsDataFile>.cryasset` beside a file we have just written, so that the Asset Browser
//! adopts it at once. `szTypeName` is one of the CINECAM_ASSET_TYPE_* strings and must match a
//! CAssetType registered by the editor plugin; `szDataFileName` is the LEAF name, because a
//! .cryasset's <File path=...> is relative to its own folder.
bool   WriteCryAssetSidecar(const char* szAbsDataFile, const char* szDataFileName, const char* szTypeName);

//! Save a preset. `szRelPath` is asset-relative (e.g. "Assets/cinecam/grades/hero.cinegrade").
//! Writes the XML and its sidecar; returns false and warns on any failure.
bool   SavePreset(const char* szRelPath, const SGradePreset& preset);

//! Load a preset. `szRelPath` goes through ICryPak, so it resolves in the project and under
//! %ENGINE% exactly like a .cube does. `preset` is expected to arrive holding the component's
//! current values, so a field the file does not carry keeps what it had.
bool   LoadPreset(const char* szRelPath, SGradePreset& preset);

//! Registers / removes `cinecam_ImportGradeAssets`.
void   RegisterGradeAssetCommands();
void   RemoveGradeAssetCommands();

} // namespace CineCamAssets
