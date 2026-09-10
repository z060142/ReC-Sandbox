// The reflected types the display chain's asset properties need (S10 items 4 and 4b).
//
// Shared by BOTH components: the camera owns the ODT path (its monitor) and the CineCam Grade
// component owns the LMT path and the preset slot (the look), and neither should invent its own
// answer to "what is a LUT property" or "what space is a cube in".
#pragma once

#include <CrySchematyc/ResourceTypes.h>       // SerializationUtils::SResourceNameSelector
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySerialization/Decorators/Resources.h>       // Serialization::MakeResourceSelector
#include <CrySerialization/Decorators/ResourceSelector.h>

// ---------------------------------------------------------------------------
// The names of our three asset types (S10 item 4b)
// ---------------------------------------------------------------------------
// These four strings are the whole contract between this engine plugin and the Sandbox plugin
// Code/Sandbox/Plugins/CinematicCameraEditor, which registers a CAssetType for each of the three:
//
//   * the TYPE NAME is what goes into every .cryasset's type= attribute AND - because
//     CAssetManager::RegisterAssetResourceSelectors (AssetManager.cpp:728-742) gives every asset
//     type a resource selector named after it - it is also the selector name the property rows
//     below look up. One string, two jobs; renaming it orphans files and breaks pickers at once.
//   * the EXTENSION is what the asset type claims and what the auto-repair watcher registers a
//     file-monitor listener for, so that a .cube copied into the project gets a sidecar by itself.
//
// The editor plugin does not have to be installed: an unregistered selector name falls back to a
// plain text field (PropertyRowResourceSelector.cpp:427-466 - "a default selector implementation
// should always be returned"), so a level saved with these properties still opens in a stock
// editor, and nothing here reaches the launcher at all.
#define CINECAM_ASSET_TYPE_LUT   "CineLut"
#define CINECAM_ASSET_TYPE_CDL   "CineCDL"
#define CINECAM_ASSET_TYPE_GRADE "CineGrade"

//! Where the shipped and the user's own grade files live, under the project's asset directory and
//! (for the shipped defaults) under %ENGINE%. Item 4b replaced the old ad-hoc "Assets/ODT" with
//! this layout; kLegacyLutFolder is kept so that a level saved before the move still loads.
#define CINECAM_FOLDER_LUTS   "Assets/cinecam/luts"
#define CINECAM_FOLDER_SYSTEM "Assets/cinecam/luts/system"
#define CINECAM_FOLDER_CDL    "Assets/cinecam/cdl"
#define CINECAM_FOLDER_GRADES "Assets/cinecam/grades"
#define CINECAM_FOLDER_LEGACY "Assets/ODT"

namespace Serialization
{
//! The generic factory Resources.h itself points at ("Do not add any more methods here ... Use
//! MakeResourceSelector() instead"). The string has to match the CAssetType's GetTypeName().
template<class T> ResourceSelector<T> CineLutPicker(T& s)   { return ResourceSelector<T>(s, CINECAM_ASSET_TYPE_LUT); }
template<class T> ResourceSelector<T> CineGradePicker(T& s) { return ResourceSelector<T>(s, CINECAM_ASSET_TYPE_GRADE); }
}

namespace Schematyc
{

// ---------------------------------------------------------------------------
// A .cube property that picks from the Asset Browser (S10 item 4b.4)
// ---------------------------------------------------------------------------
// CRYENGINE 5.7.1 has two reflected path families (CrySchematyc/ResourceTypes.h):
//
//   * SResourceNameSerializer<&fn> where fn returns Serialization::ResourceFilePath - a plain FILE
//     picker: a filter string, a "..." button, an engine file dialog. This is what item 4 used,
//     because a .cube was not a registered asset type yet. Two stock limitations came with it: the
//     decorator's startFolder is dropped by the row (PropertyRowResourceFilePath.cpp:53), and the
//     dialog only offers files under the PROJECT, so a LUT installed only into the engine tree had
//     to be typed from memory.
//
//   * SResourceNameSelector<&Serialization::XxxPicker<string>> - an ASSET picker, backed by a
//     registered resource selector and therefore by a registered asset type. This is the family
//     Schematyc::TextureFileName belongs to (this plugin already uses it for the bokeh shape and
//     the filter PSF). Item 4b registers the .cube asset type, so this is now available - and it
//     fixes both limitations, because the Asset Browser has its own folder tree and scans %engine%
//     as a second root (CAssetManager::AsyncScanForAssets / m_knownAliases, AssetManager.cpp:115).
//
// THE STORED VALUE DOES NOT CHANGE. Serialization::ResourceSelector's yasli Serialize wraps the
// value as SStruct::forEdit only when archive.isEdit(); otherwise it writes `ar(value.value, ...)`,
// which is byte-for-byte what the file-path family wrote. So saved levels, defaults and the
// launcher are all unaffected by the swap, and the loader still receives the same kind of
// asset-relative string it always did (what the picker returns is CAsset::GetFile(0)).
typedef SerializationUtils::SResourceNameSelector<&Serialization::CineLutPicker<string>> CineLutFileName;

inline void ReflectType(CTypeDesc<CineLutFileName>& desc)
{
	desc.SetGUID("{9C21A4F7-3E58-4B06-8D19-6A5F0C7E24B3}"_cry_guid);
	desc.SetLabel("Cube LUT File");
	desc.SetDescription("A .cube LUT asset (Resolve dialect)");
}

} // namespace Schematyc

// ---------------------------------------------------------------------------
// What a .cube expects at its input (S10 item 4.1)
// ---------------------------------------------------------------------------
// A Rec.709 display LUT and an ACEScct LMT are both three floats in [0,1] on a unit domain, so
// the wrong one loads without complaint and produces a wrong picture SILENTLY. Nothing in the
// .cube format distinguishes them - the Adobe/Resolve dialect has TITLE, the two sizes,
// DOMAIN_MIN/MAX and comments, and no metadata field at all.
//
// So there are two answers, in this order (decisions/s10-grade-component.md 4.1):
//   1. a `# ReC-LUT-Space: <value>` line in the file's comment block, which our bake tool writes
//      and the plugin parses. Authoritative when present: it was written by whoever made the LUT.
//   2. this property, for a foreign LUT that has no tag and never will. Its default is ACEScct,
//      so an untagged file with an untouched property is exactly the behaviour that existed
//      before this enum did - parse, convert, upload, bit-identical.
// Both present and disagreeing: the tag wins, and one warning names the file.
enum class ELutSpace : uint32
{
	//! The grading space this chain works in. No conversion: the file goes straight to the GPU.
	ACEScct = 0,
	//! Linear ACEScg. Wrapped with the analytic shaper. NOTE that a unit-domain LINEAR cube covers
	//! scene linear [0,1] and nothing above, while ACEScctToLinear(1.0) = 222.86 - a linear look
	//! LUT really wants a shaper, which this chain's parser refuses.
	ACEScg,
	//! An ordinary Rec.709 / sRGB DISPLAY look LUT - the overwhelmingly common kind. Wrapped with
	//! the ACES 2.0 SDR 100 nit sRGB output transform and its inverse.
	Rec709Display,
};

inline void ReflectType(Schematyc::CTypeDesc<ELutSpace>& desc)
{
	desc.SetGUID("{2F6B8D50-91C3-4A7E-B248-05E7D9A16C3F}"_cry_guid);
	desc.SetLabel("LUT Input Space");
	desc.SetDefaultValue(ELutSpace::ACEScct);
	desc.AddConstant(ELutSpace::ACEScct,       "ACEScct",       "ACEScct (an LMT)");
	desc.AddConstant(ELutSpace::ACEScg,        "ACEScg",        "ACEScg linear");
	desc.AddConstant(ELutSpace::Rec709Display, "Rec709Display", "Rec.709 display (a normal LUT)");
}
