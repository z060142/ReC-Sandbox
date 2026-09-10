// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
//
// Per-file asset registration for the CineCam grade files (S10 item 4b-fix).
//
// The problem this solves: dropping a `.cube` into `assets/cinecam/luts/` did nothing at all. The
// stock watcher (CAssetGenerator) rejects the file before it does any work, silently, because
// CAssetType::IsValidAssetPath forbids a dot in the asset name (AssetType.cpp:382-388) - and every
// second vendor LUT is called something like "XT3_FLog_FGamut_to_ETERNA_BT.709_33grid_V.1.01.cube".
// The whole-project "Generate/Repair Metadata" would not have helped either: it runs one fixed job
// file, Tools/cryassets/rcjob_cryassets.xml, whose sixteen-extension list contains none of ours.
// Full analysis: scene-notes/research/s10-4b-fix-file-registration.md.
//
// What we do instead, entirely inside this DLL and without ever invoking the Resource Compiler:
//
//   * a file-change listener on `assets/cinecam/`: a new .cube / .cdl / .cinegrade gets its own
//     `.cryasset` written and is handed to the asset manager - one file, one sidecar;
//   * a one-shot sweep of the same folder once the asset manager's initial scan is done, for files
//     that were copied in while the editor was closed;
//   * an asset importer for the three extensions, so dragging a file from Explorer onto the Asset
//     Browser (and File -> Import) works at all - the browser gates drops on importers, never on
//     types (AssetDropHandler.cpp:67-83);
//   * `cinecam_RegisterAssets` on the console, to re-run the sweep by hand.
//
// Two rules the implementation never breaks:
//   1. an existing `.cryasset` is never rewritten. It carries the asset's GUID; re-issuing that
//      would break every reference to it.
//   2. nothing outside `assets/cinecam/` is touched on our own initiative. The importer is the one
//      exception, and only because the user pointed at the destination folder themselves.
#pragma once

#include <CryString/CryString.h>

class CAsset;

namespace CineCamAssets
{

//! The one folder this plugin watches, relative to the project's asset root. Lower case, unix
//! separators, no trailing slash - it is compared with strnicmp against the file monitor's paths.
extern const char* const kWatchedFolder;

//! The registered CAssetType name for a data-file extension we own ("cube" -> "CineLut"), or
//! nullptr for anything else. The strings are the same contract as CineCamAssetTypes.h.
const char* TypeNameForExtension(const char* szExt);

//! Builds the asset for one data file and writes its `.cryasset`, unless one already exists - in
//! which case the existing sidecar is loaded as it stands and nothing is written.
//! \param gameRelDataFile Path to the data file, relative to the assets root, unix separators.
//! \return A new CAsset the caller must hand to CAssetManager::MergeAssets, or nullptr.
CAsset* CreateAssetForFile(const string& gameRelDataFile);

//! CreateAssetForFile + MergeAssets. Returns true if the file is registered afterwards.
bool RegisterFile(const string& gameRelDataFile);

//! Registers every file under kWatchedFolder that the asset manager does not know yet.
//! \return The number of files newly registered.
int SweepWatchedFolder();

//! Called once from the plugin's constructor: file listener, scan-completed hook, console command.
void Install();

//! Called once from the plugin's destructor.
void Uninstall();

}
