// CineCam Grade - the creative half of the display chain, as its own entity component.
//
// The division (decisions/s10-grade-component.md section 2 A) mirrors ACES: the camera owns the
// device-side transforms - exposure, white balance, the output transform that is its monitor -
// and this component owns the look: the ASC CDL and the LMT slot. A look is then a component you
// can add, copy between cameras and delete, which is the same story design decision D10 tells
// about the LUT file itself.
//
// It never writes the renderer. The cinematic camera on the same entity remains the single writer
// of the Global_User_Grade* bus parameters and the only caller of CineCam::ApplyDisplayLuts; it
// reads this component's two structs when it publishes, and uses the neutral defaults below when
// there is no grade component on the entity (decisions/s10-grade-component.md section 3, P1).
// So: no grade component, or a grade component nobody has touched, are the same picture - pure
// ACES 2.0, exactly as before this component existed.
#pragma once

#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
// CSharedString: the reflected string type behind the .cube path property (LMT File).
#include <CrySchematyc/Utils/SharedString.h>
// Schematyc::CineLutFileName (the Asset Browser picker), the asset type names and the folder
// layout (S10 item 4b), and ELutSpace (the input-space fallback, item 4).
#include "CineCamLutTypes.h"
// Serialization::ActionButton - the two buttons in the preset slot below. Shipped precedent for a
// button on an entity component: CryDefaultEntities' CDebugSerializeHelper (TriggerComponent.h:68).
// The buttons take lambdas and not CryEngine functors, so <CryCore/functor.h> is deliberately NOT
// included here: a functor built from (*this, &member) is exactly the pointer capture item 6 removed.
#include <CrySerialization/Decorators/ActionButton.h>
#include <CryEntitySystem/IEntitySystem.h>
// The shared definition of the two 1D curves: knot positions, the interpolator, the bake and the
// neutrality tests. The renderer's EXR exporter includes the SAME header to write the master curve
// out as a .cube beside a capture, so there is exactly one answer to "what is the curve between
// two knots" (decisions/s10-grade-component.md section 3b.5).
#include <CryRenderer/SceneReferredCurves.h>

// The camera this look belongs to. Forward-declared, never included: CinematicCameraComponent.h
// includes THIS header (the camera reads the two structs below), so the dependency has exactly
// one direction and the .cpp is where the two meet.
class CCinematicCameraComponent;

//! The ACEScct code value of 18 % grey - LinearToACEScct(0.18), the one anchor the pre-exposure
//! guarantees (SceneReferredSpec.md D6). The default contrast pivot, so that "more contrast"
//! leaves mid grey where it was. Same constant as CommonMath.cfi's documented landmark.
constexpr float kACEScctMidGrey = 0.4135878f;

//! What the camera actually publishes and what an exported .cdl carries: ONE ASC CDL, per
//! channel, in the ASC's own order. Everything the user turns - the wheels, contrast, the CDL
//! base - folds into these numbers exactly (see SCineGradeParams::Resolve).
struct SCineEffectiveCdl
{
	Vec3  slope = Vec3(1.0f, 1.0f, 1.0f);
	Vec3  offset = Vec3(0.0f, 0.0f, 0.0f);
	Vec3  power = Vec3(1.0f, 1.0f, 1.0f);
	float saturation = 1.0f;
};

// The camera's grade (SceneReferredSpec.md S5). ASC CDL in ACEScct, the interchange every
// grading tool speaks, applied to scene data before the output transform - not to a picture
// after it. Neutral by default: with these values the chain is exactly ACES 2.0 (D10).
//
// THE UI IS A PARAMETERISATION OF THE CDL, NOT A SECOND OPERATOR (decisions/s10-grade-component.md
// section 3a). Three colour wheels in the convention Resolve, Nuke and every other LGG control
// share - lift holds white put, gain holds black put, gamma is the mids - plus contrast about a
// pivot, all of which fold EXACTLY into a single slope/offset/power triple because the ASC order
// is "one affine step, then one power" and every one of these operators is affine. So there is
// never a picture the engine can show and a .cdl cannot reproduce.
//
// Nothing here is ever written back by the component: the editor snapshots a component's
// properties for saving BEFORE it tells the component they changed
// (EntityObject.cpp:925 vs :930), so a member written from an event handler renders but does not
// reliably save. The published CDL is therefore derived at publish time and shown in the log line
// (`grade published:`), in the EXR header (`rec/cdl`) and in the .cdl sidecar - never stored.
struct SCineGradeParams
{
	//! Field-wise and not memcmp: the struct has padding after the bool, and padding bytes are not
	//! guaranteed to be equal between two structs that hold the same values.
	inline bool operator==(const SCineGradeParams& rhs) const
	{
		return bBypassGrade == rhs.bBypassGrade
		       && lift == rhs.lift && (float)liftMaster == (float)rhs.liftMaster
		       && gamma == rhs.gamma && (float)gammaMaster == (float)rhs.gammaMaster
		       && gain == rhs.gain && (float)gainMaster == (float)rhs.gainMaster
		       && (float)contrast == (float)rhs.contrast && (float)pivot == (float)rhs.pivot
		       && (float)saturation == (float)rhs.saturation
		       && slope == rhs.slope && offset == rhs.offset && power == rhs.power;
	}

	//! Fold everything the user turned into the one ASC CDL the renderer and every exported file
	//! see. Exact - no approximation anywhere in it. Defined in CineGradeComponent.cpp.
	SCineEffectiveCdl Resolve() const;

	static void ReflectType(Schematyc::CTypeDesc<SCineGradeParams>& desc)
	{
		desc.SetGUID("{4C81D3E6-7A29-4F05-B6D4-92E37C0158BA}"_cry_guid);
		desc.SetLabel("Grade");
		desc.AddMember(&SCineGradeParams::bBypassGrade,
			'gbyp', "BypassGrade", "Bypass Grade",
			"Switch the whole camera grade out of the chain - white balance, CDL and look - and "
			"leave only the output transform. The A/B: what the shot looks like ungraded, one "
			"click away, with the exposure and the optics unchanged.", false);

		// --- the wheels -------------------------------------------------------------------
		desc.AddMember(&SCineGradeParams::lift,
			'wlft', "Lift", "Lift (RGB)",
			"The shadow end, per channel, in the convention every colour wheel shares: lifting "
			"the blacks leaves WHITE exactly where it was (slope = gain - lift, offset = lift). "
			"0 = neutral. Negative numbers crush. This is the place a colour cast in the shadows "
			"is corrected.", Vec3(0.0f, 0.0f, 0.0f));
		desc.AddMember(&SCineGradeParams::liftMaster,
			'wlfm', "LiftMaster", "Lift Master",
			"The wheel's Y ring: added to all three Lift channels at once. 0 = neutral.", 0.0f);
		desc.AddMember(&SCineGradeParams::gamma,
			'wgam', "Gamma", "Gamma (RGB)",
			"The midtones, per channel - the CDL power written the way a colourist reads it "
			"(power = 1 / gamma), so ABOVE 1 opens the mids and below 1 closes them, the "
			"opposite sense to the raw power below. 1 = neutral. Neither black nor white moves.",
			Vec3(1.0f, 1.0f, 1.0f));
		desc.AddMember(&SCineGradeParams::gammaMaster,
			'wgmm', "GammaMaster", "Gamma Master",
			"The wheel's Y ring: multiplies all three Gamma channels at once. 1 = neutral.", 1.0f);
		desc.AddMember(&SCineGradeParams::gain,
			'wgan', "Gain", "Gain (RGB)",
			"The highlight end, per channel - a multiply in log. Raising the gain leaves BLACK "
			"exactly where it was. 1 = neutral. With Lift at 0 this is the ASC slope itself.",
			Vec3(1.0f, 1.0f, 1.0f));
		desc.AddMember(&SCineGradeParams::gainMaster,
			'wgnm', "GainMaster", "Gain Master",
			"The wheel's Y ring: multiplies all three Gain channels at once. 1 = neutral.", 1.0f);

		// --- contrast about a pivot -------------------------------------------------------
		desc.AddMember(&SCineGradeParams::contrast,
			'wcon', "Contrast", "Contrast",
			"Steepen or flatten the whole curve about the pivot below. 1 = neutral, 0 = a flat "
			"grey field. It is an affine operator in the grading space, so it folds into the "
			"published slope and offset EXACTLY - there is no extra step in the shader and "
			"nothing here that a .cdl cannot carry.", 1.0f);
		desc.AddMember(&SCineGradeParams::pivot,
			'wpiv', "ContrastPivot", "Contrast Pivot",
			"The code value Contrast turns about, in ACEScct. The default 0.4136 is ACEScct(0.18) "
			"- 18 % grey - so contrast leaves a grey card exactly where it is and opens or closes "
			"everything around it. Lower it to pivot on the shadows, raise it to pivot on the "
			"highlights.", kACEScctMidGrey);

		desc.AddMember(&SCineGradeParams::saturation,
			'cdlt', "Saturation", "Saturation",
			"The ASC's saturation extension, applied after the slope/offset/power triple about "
			"the luminance of the graded value, with the ASC's own Rec.709 luma weights so that "
			"the number means here exactly what it means in Resolve, OCIO or Flame. 1 = neutral, "
			"0 = monochrome. Because this happens in the working space and before the output "
			"transform, pushing it does not drift the hues the way a saturation slider on a "
			"finished picture does.", 1.0f);

		// --- the ASC CDL the wheels sit on top of -----------------------------------------
		desc.AddMember(&SCineGradeParams::slope,
			'cdls', "Slope", "CDL Slope (base)",
			"ASC CDL slope, per channel - the raw interchange numbers, for pasting a .cdl that "
			"came from Resolve. WITH THE WHEELS NEUTRAL THESE THREE GROUPS ARE EXACTLY WHAT THE "
			"CAMERA PUBLISHES; the wheels and contrast compose on top of them and the result is "
			"still one legal CDL. 1 = neutral. ASC order: out = (in * slope + offset) ^ power.",
			Vec3(1.0f, 1.0f, 1.0f));
		desc.AddMember(&SCineGradeParams::offset,
			'cdlo', "Offset", "CDL Offset (base)",
			"ASC CDL offset, per channel. An add in log. 0 = neutral. Note this is the raw ASC "
			"offset, which moves white as well as black - the Lift wheel above is the one that "
			"holds white put.", Vec3(0.0f, 0.0f, 0.0f));
		desc.AddMember(&SCineGradeParams::power,
			'cdlp', "Power", "CDL Power (base)",
			"ASC CDL power, per channel. 1 = neutral; BELOW 1 opens the mids, above 1 closes "
			"them - the opposite sense to the Gamma wheel above, which is 1 / this.",
			Vec3(1.0f, 1.0f, 1.0f));
	}

	bool bBypassGrade = false;

	Vec3 lift = Vec3(0.0f, 0.0f, 0.0f);
	Schematyc::Range<-1, 1, -1, 1> liftMaster = 0.0f;
	Vec3 gamma = Vec3(1.0f, 1.0f, 1.0f);
	Schematyc::Range<0, 4, 0, 2> gammaMaster = 1.0f;
	Vec3 gain = Vec3(1.0f, 1.0f, 1.0f);
	Schematyc::Range<0, 4, 0, 2> gainMaster = 1.0f;

	Schematyc::Range<0, 4, 0, 2> contrast = 1.0f;
	Schematyc::Range<0, 1, 0, 1> pivot = kACEScctMidGrey;

	Schematyc::Range<0, 4, 0, 2> saturation = 1.0f;

	Vec3 slope = Vec3(1.0f, 1.0f, 1.0f);
	Vec3 offset = Vec3(0.0f, 0.0f, 0.0f);
	Vec3 power = Vec3(1.0f, 1.0f, 1.0f);
};

// The two 1D curves (S10 item 3b, decisions/s10-grade-component.md section 3b) - the shape a CDL
// cannot make, and the last thing a live desk needs that the wheels do not give.
//
// WHERE THEY SIT: between the ASC CDL and the LMT. That is Pomfort Livegrade's fixed order for
// every one of its grading modes (input transform -> CDL and saturation -> curves -> output
// transform), and it is what keeps the .cdl a capture writes an exact description of the FRONT of
// the chain - so it still reproduces in Resolve as node 1, with the curves as node 2.
//
// WHY FIVE NUMBERS AND NOT A CURVE WIDGET: 5.7.1 does have a curve property row, and a reflected
// spline WOULD draw in this inspector - but the only CCurveEditorPanel in the whole Sandbox is
// created by the Particle Editor, so in the level editor the row's edit broadcast has no receiver
// and the curve cannot be dragged. Five plain floats, on the other hand, are typeable, readable,
// TrackView-animatable, they cross the post-effect bus as they are, and their neutral is the
// literal zero - so "untouched" is an exact identity by construction rather than by measurement.
// Research: scene-notes/research/s10-3b-curves.md section 3.
struct SCineCurveParams
{
	inline bool operator==(const SCineCurveParams& rhs) const
	{
		return (float)masterBlack == (float)rhs.masterBlack
		       && (float)masterShadow == (float)rhs.masterShadow
		       && (float)masterMid == (float)rhs.masterMid
		       && (float)masterHighlight == (float)rhs.masterHighlight
		       && (float)masterWhite == (float)rhs.masterWhite
		       && (float)satAtGrey == (float)rhs.satAtGrey
		       && (float)satAtLow == (float)rhs.satAtLow
		       && (float)satAtMid == (float)rhs.satAtMid
		       && (float)satAtHigh == (float)rhs.satAtHigh
		       && (float)satAtFull == (float)rhs.satAtFull;
	}

	//! The five master-curve knot heights, as OFFSETS IN STOPS from the identity. Defined in
	//! CineGradeComponent.cpp.
	void GetMasterStops(float(&out)[SceneReferredCurves::kKnotCount]) const;
	//! The five Sat vs Sat multipliers, floored at 0 (a negative multiplier would rotate a colour
	//! 180 degrees about grey, which is never what "less saturation" means).
	void GetSatMultipliers(float(&out)[SceneReferredCurves::kKnotCount]) const;

	static void ReflectType(Schematyc::CTypeDesc<SCineCurveParams>& desc)
	{
		desc.SetGUID("{7B4E92A1-0C63-4D18-8F52-1A9D6E7C3B04}"_cry_guid);
		desc.SetLabel("Curves");

		// --- the master (luma) curve ------------------------------------------------------
		// One curve, applied identically to all three channels, so it is a TONE curve and cannot
		// introduce a colour cast - that is what the wheels are for. Each control is how far, in
		// stops, that part of the picture moves; 0 leaves it exactly where it was. The knots sit
		// at fixed places in ACEScct: the bottom of the encoding, four stops under 18 % grey,
		// 18 % grey itself, four stops over, and the top (+10.3 stops).
		desc.AddMember(&SCineCurveParams::masterBlack,
			'cmbk', "MasterBlack", "Master Black (stops)",
			"The very bottom of the tone curve, in stops. Negative crushes the deepest blacks, "
			"positive lifts them. 0 = neutral. Together with Master Shadow this is where a film "
			"toe is made.", 0.0f);
		desc.AddMember(&SCineCurveParams::masterShadow,
			'cmsh', "MasterShadow", "Master Shadow (stops)",
			"Four stops under 18 % grey, in stops. The shadow half of an S-curve: pull this down "
			"and lift Master Highlight and the picture gains contrast without the mid tone "
			"moving at all. 0 = neutral.", 0.0f);
		desc.AddMember(&SCineCurveParams::masterMid,
			'cmmd', "MasterMid", "Master Mid (stops)",
			"18 % grey itself, in stops - the one anchor the exposure guarantees. Moving this is "
			"moving the exposure of the mid tone WITHOUT moving black or white, which is a thing "
			"the aperture cannot do. 0 = neutral.", 0.0f);
		desc.AddMember(&SCineCurveParams::masterHighlight,
			'cmhl', "MasterHighlight", "Master Highlight (stops)",
			"Four stops over 18 % grey, in stops. The highlight half of an S-curve; pull it down "
			"instead for a film shoulder that rolls the brights off softly. 0 = neutral.", 0.0f);
		desc.AddMember(&SCineCurveParams::masterWhite,
			'cmwh', "MasterWhite", "Master White (stops)",
			"The very top of the encoding (+10.3 stops over grey), in stops. Above the output "
			"transform's own domain very little of this is visible on screen - but it is in the "
			"EXR, because the export tap is taken after the curve. 0 = neutral.", 0.0f);

		// --- Sat vs Sat -------------------------------------------------------------------
		// The multiplier, per band of measured saturation. Resolve's control of the same name has
		// exactly this shape: a flat line at 1 = no change, and the y axis is a multiplier.
		desc.AddMember(&SCineCurveParams::satAtGrey,
			'csa0', "SatAtGrey", "Sat @ 0 (neutrals)",
			"How much saturation survives where there was almost none - the near-neutral parts of "
			"the picture. Below 1 cleans a faint cast out of greys; above 1 finds colour in them. "
			"1 = neutral.", 1.0f);
		desc.AddMember(&SCineCurveParams::satAtLow,
			'csa1', "SatAtLow", "Sat @ 25 %",
			"The multiplier for barely-tinted colours - skin, wood, weathered paint. 1 = neutral.",
			1.0f);
		desc.AddMember(&SCineCurveParams::satAtMid,
			'csa2', "SatAtMid", "Sat @ 50 %",
			"The multiplier for ordinary colours. Raising this and lowering Sat @ 100 % is the "
			"classic move: everything gains life except what was already screaming. 1 = neutral.",
			1.0f);
		desc.AddMember(&SCineCurveParams::satAtHigh,
			'csa3', "SatAtHigh", "Sat @ 75 %",
			"The multiplier for strong colours. 1 = neutral.", 1.0f);
		desc.AddMember(&SCineCurveParams::satAtFull,
			'csa4', "SatAtFull", "Sat @ 100 %",
			"The multiplier for the most saturated thing in the frame - a warning light, a red "
			"coat, a clipped neon. Pulling it down tames exactly those and leaves everything else "
			"untouched, which no global saturation slider can do. 1 = neutral.", 1.0f);
	}

	// Hard range +-4 stops (a curve past that is not a grade any more), slider +-2.
	Schematyc::Range<-4, 4, -2, 2> masterBlack = 0.0f;
	Schematyc::Range<-4, 4, -2, 2> masterShadow = 0.0f;
	Schematyc::Range<-4, 4, -2, 2> masterMid = 0.0f;
	Schematyc::Range<-4, 4, -2, 2> masterHighlight = 0.0f;
	Schematyc::Range<-4, 4, -2, 2> masterWhite = 0.0f;

	Schematyc::Range<0, 4, 0, 2> satAtGrey = 1.0f;
	Schematyc::Range<0, 4, 0, 2> satAtLow = 1.0f;
	Schematyc::Range<0, 4, 0, 2> satAtMid = 1.0f;
	Schematyc::Range<0, 4, 0, 2> satAtHigh = 1.0f;
	Schematyc::Range<0, 4, 0, 2> satAtFull = 1.0f;
};

// The look slot (LMT), one of the two .cube files of the display chain (SceneReferredSpec.md
// S4/S5, D8) - the other, the output transform, stays on the camera because it is the camera's
// monitor. Since S10 item 4b it is an ASSET picker: .cube is a registered asset type, so the
// property is a Schematyc::CineLutFileName and its button opens the Asset Browser filtered to
// LUTs. The stored value is the same plain relative string the old file dialog stored, and the
// path is still resolved through ICryPak - first as given (relative to the project's asset
// directory), then under %ENGINE% - so the same string works in the editor and in the launcher
// whichever tree the file was installed into.
//
// The look slot is also the ONE place a foreign LUT enters this chain, so it is where the input
// space has to be settled (S10 item 4.1): a file tagged '# ReC-LUT-Space:' is believed, otherwise
// LMT Input Space below decides, and a Rec.709 display LUT is wrapped into a legal LMT at load.
struct SCineLookParams
{
	inline bool operator==(const SCineLookParams& rhs) const
	{
		return lmtSpace == rhs.lmtSpace && lmtFile.value == rhs.lmtFile.value;
	}

	static void ReflectType(Schematyc::CTypeDesc<SCineLookParams>& desc)
	{
		desc.SetGUID("{3A0C6D18-5E47-4B92-9C31-7F6E2D840A15}"_cry_guid);
		desc.SetLabel("Look");

		// WHAT THE FILE EXPECTS AT ITS INPUT. Only consulted when the file itself does not say
		// (see the CineCamLutTypes.h header comment); leaving it at ACEScct with an untagged file
		// is the behaviour that existed before this property did, exactly.
		desc.AddMember(&SCineLookParams::lmtSpace,
			'lmts', "LMTSpace", "LMT Input Space",
			"What the LUT below expects at its input - and therefore whether it has to be wrapped "
			"before it can be a look for this camera. ACEScct = it is already an LMT (the default, "
			"and what our own bake tool and a Resolve ACEScct export produce). Rec.709 display = "
			"an ORDINARY look LUT, made for a finished 709 picture; the plugin wraps it "
			"automatically in the ACES 2.0 output transform and its inverse, so it simply works. "
			"ACEScg = linear in and out. THE FILE WINS: a cube carrying a '# ReC-LUT-Space:' line "
			"is believed over this setting, and the console says so if the two disagree.",
			ELutSpace::ACEScct);

		desc.AddMember(&SCineLookParams::lmtFile,
			'lmtf', "LMTFile", "LMT File",
			"Creative look, as a Resolve .cube - the Look Modification Transform. It is applied "
			"AFTER the CDL and the curves and BEFORE the output transform, so it grades scene "
			"data and not a picture. Empty = identity (pure ACES, no house look: "
			"SceneReferredSpec.md D10). Author it in Resolve on an ACEScct timeline and export a "
			"33-cube - or drop in any ordinary Rec.709 look LUT and set LMT Input Space above. "
			"Shipped, in " CINECAM_FOLDER_LUTS ": lmt_identity_33.cube (neutral, and the plumbing "
			"test), look_film_contrast_33.cube, look_warm_print_33.cube. Editing the file on disk "
			"reloads it live (cinecam_LutHotReload).",
			Schematyc::CineLutFileName());
	}

	ELutSpace lmtSpace = ELutSpace::ACEScct;

	// Keep last: the resource-path types in this plugin all sit at the end of their struct, and
	// operator== compares the string by value.
	Schematyc::CineLutFileName lmtFile;
};

class CCineGradeComponent;

// The preset slot (S10 item 4b) - export, import and reuse of the WHOLE component state.
//
// Three rows: a Grade Preset asset picker, an Export button and a Re-apply button. It carries a
// hand-written Serialize() rather than reflected members because all three of them need something
// a plain reflected field cannot have: a way back to the component that owns it.
//
// WHY THAT WAY BACK IS AN EntityId AND NOT A POINTER (S10 item 6, the heap-corruption fix;
// decisions/s10-grade-component.md "Item 6", research/s10-6-grade-heap-corruption.md).
// This struct is REFLECTED, and a reflected type in Schematyc is copied, cached and outlived:
//   * Schematyc::CClassProperties::Read copy-constructs one into a CScratchpad every time the
//     inspector builds a widget (ClassProperties.h:54/:101) and the editor's component cache keeps
//     one for the whole of a game-mode round trip (EntityComponentsCache.cpp:77);
//   * CClassProperties::Apply then copies that cached value back ONTO a live component, which
//     means whatever the copy holds is written into the live one - a pointer included;
//   * Serialization::ActionButton's row CLONES the functor it is given and keeps the clone for the
//     life of the property row (PropertyRowActionButton), which outlives the component across
//     game mode, undo and level reload.
// A raw CCineGradeComponent* therefore goes stale in three ordinary ways and is then WRITTEN
// THROUGH by the Export / Re-apply path (SetAppliedPath assigns two CryStrings through it, i.e. a
// free and a store into freed memory) - heap corruption with no symptom until some later, innocent
// free. An EntityId is a value: copying it is meaningless, a stale one resolves to nullptr through
// the entity system's salt, and nothing is ever written through it without that lookup succeeding.
// For the same reason the two buttons capture that id BY VALUE in a lambda instead of capturing
// `this`, so the row's cloned functor owns no pointer into this struct at all.
//
// WHY THE APPLY HAPPENS IN HERE, AND WHY THIS MEMBER IS DECLARED LAST
// (decisions/s10-grade-component.md 4b.6, research/s10-4b-asset-system.md section 7).
// The editor takes the snapshot it SAVES from at EntityObject.cpp:928, which is AFTER the input
// serialization at :912 and BEFORE the property-changed event at :931. So a value written from the
// event handler renders but does not reliably save - the reason item 3a refuses to write the
// derived CDL back - while a value written DURING the input pass is inside the snapshot. Schematyc
// serialises members in AddMember order (TypeDesc.inl:72-86), so this member, declared after
// Grade / Curves / Look, can legally overwrite all three. The picker row then triggers apply()
// followed by revert() (PropertyTreeLegacy.cpp:1133-1153), so the inspector redraws with the new
// numbers, and the undo transaction the row opened covers the whole component.
//
// The apply is gated on archive.isEdit(), so loading a level never re-applies: the component owns
// its values from the moment the preset was picked, and the path below is kept only as SOURCE
// INFORMATION - where these numbers came from. Copy on apply, not a live link.
struct SCineGradePresetSlot
{
	//! Only the stored path takes part: the owner pointer and the "what have we applied" shadow are
	//! runtime bookkeeping and must never make two equal components compare unequal.
	inline bool operator==(const SCineGradePresetSlot& rhs) const { return value == rhs.value; }

	void        Serialize(Serialization::IArchive& archive);
	//! The entity this slot's component sits on. Re-armed in Initialize() and on every
	//! property-changed event, because CClassProperties::Apply overwrites it memberwise from a
	//! cached copy when the editor leaves game mode.
	void        SetOwnerEntity(EntityId id) { m_ownerEntity = id; }
	//! Called by the export path so that the picker shows what was just written without that
	//! looking like a fresh user pick (which would re-apply the file we have just saved).
	void        SetAppliedPath(const char* szPath) { value = szPath; m_applied = szPath; }

	//! The picked .cinegrade, asset-relative. Public because the component reports it.
	string value;

private:
	//! Resolve the owning component from the id, or nullptr. The ONE place a pointer to the
	//! component is produced, and it is produced fresh on every use and never stored.
	static CCineGradeComponent* Resolve(EntityId id);
	//! The two buttons, by value: they take the id rather than a `this`, so the functor the
	//! property row clones and keeps holds nothing that can dangle.
	static void OnExport(EntityId id);
	static void OnReapply(EntityId id);

	//! What value held the last time this slot was read out to the inspector. The inspector always
	//! serialises OUT before it serialises IN, so this is how "the user just picked something new"
	//! is told apart from "the user changed some other property and this row came along".
	string   m_applied;
	//! A handle, never a pointer. See the header comment above this struct.
	EntityId m_ownerEntity = INVALID_ENTITYID;
};

inline void ReflectType(Schematyc::CTypeDesc<SCineGradePresetSlot>& desc)
{
	desc.SetGUID("{5D8A1C64-0B37-4E92-A15F-3C6E80D74B29}"_cry_guid);
	desc.SetLabel("Preset");
	desc.SetDescription("Export, import and reuse of the whole CineCam Grade");
}

//! The look, as an entity component. Holds state and nothing else: see the file header for why
//! it publishes nothing itself.
class CCineGradeComponent final : public IEntityComponent
{
public:
	static void ReflectType(Schematyc::CTypeDesc<CCineGradeComponent>& desc)
	{
		desc.SetGUID("{1E7C4A93-6B25-4D8F-9A03-52D7F1C86B40}"_cry_guid);
		desc.SetEditorCategory("Cameras");
		desc.SetLabel("CineCam Grade");
		desc.SetDescription(
			"The look for the Cinematic Camera on the same entity: ASC CDL (slope, offset, "
			"power, saturation) applied in ACEScct to scene data before the output transform, "
			"plus the LMT slot. Needs a Cinematic Camera on the same entity WITH ITS SCENE "
			"REFERRED SWITCH ON - the grade lives in that camera's output chain and does nothing "
			"without it; the console says so if a value cannot reach the picture. Neutral values "
			"are exact identities, so an untouched component is pure ACES 2.0.");
		desc.SetIcon("icons:General/Camera.ico");
		desc.SetComponentFlags({
			IEntityComponent::EFlags::Singleton,
			IEntityComponent::EFlags::ClientOnly });

		desc.AddMember(&CCineGradeComponent::m_grade,  'ggrd', "Grade",  "Grade",  "", SCineGradeParams());
		// A group of its own and not more members in Grade: everything in Grade folds into one
		// ASC CDL and travels as one, and the curves are precisely the part that does not. Keeping
		// them apart is what stops "the Grade group" from quietly meaning two different things.
		desc.AddMember(&CCineGradeComponent::m_curves, 'gcrv', "Curves", "Curves", "", SCineCurveParams());
		desc.AddMember(&CCineGradeComponent::m_look,   'glok', "Look",   "Look",   "", SCineLookParams());
		// LAST, and that is load-bearing: this member overwrites the three above during the input
		// serialization, which is the only window in which a written value still reaches the
		// editor's save snapshot. See the comment on SCineGradePresetSlot.
		desc.AddMember(&CCineGradeComponent::m_preset, 'gprs', "Preset", "Preset", "", SCineGradePresetSlot());
	}

	// IEntityComponent
	virtual void Initialize() override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void ProcessEvent(const SEntityEvent& event) override;
	// ~IEntityComponent

	//! Read by the cinematic camera on the same entity when it publishes the grade. Never a copy:
	//! the camera reads through these on the frame it publishes.
	const SCineGradeParams& GetGrade()  const { return m_grade; }
	const SCineCurveParams& GetCurves() const { return m_curves; }
	const SCineLookParams&  GetLook()   const { return m_look; }

	//! Read a .cinegrade and copy its values over Grade, Curves and Look. Asset-relative path,
	//! resolved by ICryPak like every other file this plugin reads. Returns false and warns
	//! (without touching anything) when the file cannot be read or is from a newer build.
	bool ApplyPreset(const char* szRelPath);
	//! Write the current state to Assets/cinecam/grades/<entity>.cinegrade plus its .cryasset, and
	//! point the preset slot at it. Returns the asset-relative path, or an empty string.
	string ExportPreset();
	//! The path the preset slot currently holds, by value. By value and not by reference: the
	//! button path re-resolves the component and must not hand a reference into a member it is
	//! about to overwrite.
	string GetPresetPath() const { return m_preset.value; }
	//! Point the slot at a file and mark it as already applied, so the next input pass does not
	//! read it as a fresh user pick. Only ever called on a LIVE component.
	void   SetPresetAppliedPath(const char* szPath) { m_preset.SetAppliedPath(szPath); }
	//! Re-run the editor's own "read the component into the thing that gets saved" step - literally
	//! the pair of calls at EntityObject.cpp:925/928. Needed after any write that did NOT happen
	//! inside an input serialization (i.e. after a button), and a harmless no-op outside the editor
	//! or on an entity that has no Schematyc object.
	void   SyncEditorPropertySnapshot();
	//! Nudge the sibling camera so an applied preset reaches the picture on this frame rather than
	//! the next one, and re-run the "is this grade live" report.
	void   RefreshAfterApply();

private:
	//! Say - once per distinct answer - why an edited look is not in the picture, or that it is.
	//! A grade is inert unless three things are true at once, and none of them is visible from
	//! this component's own inspector: there has to be a Cinematic Camera on the same entity, the
	//! viewport (in the editor) has to be looking through it, and that camera's Scene Referred
	//! switch has to be on, because the ASC CDL and the LMT are applied by the tone map's
	//! scene-referred branch only. "Nothing happens and nothing is said" is the one outcome this
	//! component must never produce; that is the whole reason this exists.
	void ReportStatus(const CCinematicCameraComponent* pCamera);

	SCineGradeParams m_grade;
	SCineCurveParams m_curves;
	SCineLookParams  m_look;
	//! Declared last for the same reason it is added last: member order is serialization order.
	SCineGradePresetSlot m_preset;

	//! The last answer ReportStatus() worked out, as a bitmask, so that dragging a slider - or a
	//! TrackView playback, which sends this component one property-changed event per animated key
	//! per frame - says it once and not once per event. 0 means "published"; -1 means nothing has
	//! been said yet. An int and not the message text: the comparison has to be the cheap thing,
	//! because on the animated path it is the only thing that runs.
	int m_lastStatus = -1;
};
