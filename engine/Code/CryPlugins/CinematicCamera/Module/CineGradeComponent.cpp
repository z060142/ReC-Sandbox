#include "StdAfx.h"
#include "CineGradeComponent.h"
#include "CinematicCameraComponent.h"
#include "CineGradeAsset.h"

#include <CrySchematyc/Env/Elements/EnvComponent.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CrySchematyc/IObject.h>
#include <CrySchematyc/IObjectProperties.h>
#include <CrySchematyc/Utils/ClassProperties.h>
#include <CryCore/StaticInstanceList.h>
#include <CryString/CryPath.h>

// ---------------------------------------------------------------------------
// Schematyc registration - same package as the camera, same auto-registrar. The plugin's
// ESYSTEM_EVENT_REGISTER_SCHEMATYC_ENV handler invokes every static callback in this module, so a
// second component costs nothing but this block.
// ---------------------------------------------------------------------------
namespace
{
	static void RegisterCineGradeComponent(Schematyc::IEnvRegistrar& registrar)
	{
		Schematyc::CEnvRegistrationScope scope = registrar.Scope(IEntity::GetEntityScopeGUID());
		scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CCineGradeComponent));
	}

	CRY_STATIC_AUTO_REGISTER_FUNCTION(&RegisterCineGradeComponent);
}

// ---------------------------------------------------------------------------
// The fold: every control this component offers -> one ASC CDL
// (decisions/s10-grade-component.md section 3a.1 / 3a.2, research/s10-3a-wheels-cdl-trackview.md).
//
// The ASC evaluation order is ONE affine step then ONE power - out = (in * slope + offset) ^ power
// - and lift/gain, the CDL base's own slope/offset, and contrast about a pivot are ALL affine. So
// they compose into a single (slope, offset) pair with no approximation, for any value of power,
// and the gamma wheel and the base power compose into the single exponent. The consequence is the
// point of the whole design: there is no picture this component can produce that an exported .cdl
// cannot reproduce, in Resolve or anywhere else.
//
// The order below is "the base first, the wheels on top": a .cdl pasted into the CDL group is the
// starting grade and the wheels trim it, which is how a CDL is used on a real timeline. With the
// wheels and contrast neutral the fold collapses to exactly (slope, offset, power) as typed - the
// base is then a bit-exact ASC evaluation, which is what makes "type what you were sent" true.
//
// The one deviation from strict per-node ASC order is that the base's power is applied after the
// wheels rather than immediately after the base's own slope/offset. It has to be: two powers with
// an affine step between them is not a CDL, and a control that cannot be exported is a control
// that lies about what the file will look like.
//
// The 1e-4 floors are the renderer's, copied deliberately (ToneMapping.cpp:648-649) so that the
// numbers written into the EXR header and the .cdl sidecar are the numbers the shader used, not
// the numbers it was asked for.
// ---------------------------------------------------------------------------
SCineEffectiveCdl SCineGradeParams::Resolve() const
{
	SCineEffectiveCdl out;

	const float fContrast = max((float)contrast, 0.0f);
	const float fPivot = clamp_tpl((float)pivot, 0.0f, 1.0f);
	const float fLiftMaster = (float)liftMaster;
	const float fGammaMaster = (float)gammaMaster;
	const float fGainMaster = (float)gainMaster;

	for (int c = 0; c < 3; ++c)
	{
		// The masters are the wheels' Y rings: lift adds (its neutral is 0), gain and gamma
		// multiply (theirs is 1). Every neutral is an exact identity, not a near one.
		const float fLift = lift[c] + fLiftMaster;
		const float fGain = gain[c] * fGainMaster;
		// A gamma of zero is a division; the floor keeps it a picture instead of a NaN. It is
		// needed even though the property is a Range<>, because a TrackView key is not clamped to
		// a Range<>'s bounds (research section 6).
		const float fGamma = max(gamma[c] * fGammaMaster, 1e-3f);

		// 1. the ASC CDL base, affine part
		float a = slope[c];
		float b = offset[c];

		// 2. the wheels. slope = gain - lift, offset = lift: lift holds WHITE put (x = 1 maps to
		//    1 - lift + lift = 1) and gain holds BLACK put (x = 0 maps to 0), which is what every
		//    LGG control in every application does.
		const float fWheelSlope = fGain - fLift;
		a = fWheelSlope * a;
		b = fWheelSlope * b + fLift;

		// 3. contrast about the pivot: c * (x - p) + p = c * x + p * (1 - c).
		a = fContrast * a;
		b = fContrast * b + fPivot * (1.0f - fContrast);

		out.slope[c] = max(a, 1e-4f);
		out.offset[c] = b;
		// 4. the one power. The gamma wheel is the CDL power read the colourist's way round.
		out.power[c] = max(power[c] / fGamma, 1e-4f);
	}

	out.saturation = max((float)saturation, 0.0f);
	return out;
}

// ---------------------------------------------------------------------------
// The two 1D curves -> the ten numbers everything downstream sees
// (decisions/s10-grade-component.md section 3b, research/s10-3b-curves.md).
//
// Both of these are plain gathers, deliberately: the knot POSITIONS, the interpolator, the bake
// and the neutrality tests all live in CryRenderer/SceneReferredCurves.h, which the renderer's EXR
// exporter includes as well. A second copy of any of that would be the CPU/GPU drift risk item 3a
// went out of its way to avoid.
//
// The clamps are defensive for the same reason SCineGradeParams::Resolve()'s are: a TrackView key
// is not clamped to a Range<>'s bounds, so an animated curve can arrive outside the slider.
// ---------------------------------------------------------------------------
void SCineCurveParams::GetMasterStops(float(&out)[SceneReferredCurves::kKnotCount]) const
{
	out[0] = clamp_tpl((float)masterBlack, -4.0f, 4.0f);
	out[1] = clamp_tpl((float)masterShadow, -4.0f, 4.0f);
	out[2] = clamp_tpl((float)masterMid, -4.0f, 4.0f);
	out[3] = clamp_tpl((float)masterHighlight, -4.0f, 4.0f);
	out[4] = clamp_tpl((float)masterWhite, -4.0f, 4.0f);
}

void SCineCurveParams::GetSatMultipliers(float(&out)[SceneReferredCurves::kKnotCount]) const
{
	// Floored at 0: a negative multiplier does not desaturate, it rotates the colour 180 degrees
	// about the grey axis. Nobody has ever wanted that from a saturation control.
	out[0] = clamp_tpl((float)satAtGrey, 0.0f, 4.0f);
	out[1] = clamp_tpl((float)satAtLow, 0.0f, 4.0f);
	out[2] = clamp_tpl((float)satAtMid, 0.0f, 4.0f);
	out[3] = clamp_tpl((float)satAtHigh, 0.0f, 4.0f);
	out[4] = clamp_tpl((float)satAtFull, 0.0f, 4.0f);
}

// ---------------------------------------------------------------------------
// The preset slot (S10 item 4b)
// ---------------------------------------------------------------------------
// Everything difficult about this function is in the header comment on SCineGradePresetSlot; what
// is left here is the order of the four things it does.
//
//   1. the picker row. Serialization::ResourceSelector wraps the string as SStruct::forEdit only
//      in an edit archive; everywhere else it writes the plain string, which is why a .cinegrade
//      path costs a saved level exactly as much as any other string and why the launcher, which
//      never has a selector registered, is untouched.
//   2. the two buttons, edit archives only. ActionButton::Serialize returns false outside edit
//      mode by construction, so this guard is belt and braces - but it also keeps the two lambdas
//      off the hot path (CClassProperties::Read runs this function on every inspector edit).
//   3. an OUTPUT pass syncs the shadow. The property tree always reads before it writes, so this
//      is what makes "the user picked a new file" distinguishable from "some other row changed".
//   4. an INPUT+EDIT pass with a changed path applies the preset. Not isInput() alone: a level
//      load is an input pass too, and re-applying there would turn copy-on-apply into a live link
//      and silently discard whatever the user had trimmed since.
void SCineGradePresetSlot::Serialize(Serialization::IArchive& archive)
{
	archive(Serialization::CineGradePicker<string>(value), "PresetFile", "Grade Preset");

	if (archive.isEdit())
	{
		// BY VALUE, and that is the whole point of item 6: ActionButton's row clones the functor
		// and keeps the clone for the life of the row, which outlives this struct across game
		// mode, undo and a level reload. A lambda that captures an EntityId owns nothing.
		const EntityId ownerEntity = m_ownerEntity;
		archive(Serialization::ActionButton([ownerEntity]() { SCineGradePresetSlot::OnExport(ownerEntity); }),
		        "ExportPreset", "Export Preset");
		archive(Serialization::ActionButton([ownerEntity]() { SCineGradePresetSlot::OnReapply(ownerEntity); }),
		        "ReapplyPreset", "Re-apply Preset");
	}

	if (archive.isOutput())
	{
		m_applied = value;
		return;
	}

	if (archive.isEdit() && value != m_applied)
	{
		m_applied = value;
		// Resolved here and thrown away here. A copy of this struct that the inspector or the
		// component cache is holding carries a stale id at worst, and a stale id resolves to
		// nullptr (EntityId carries a salt, so a recycled index is not mistaken for the old
		// entity) - it can never resolve to somebody else's memory.
		if (CCineGradeComponent* const pOwner = Resolve(m_ownerEntity))
		{
			if (!value.empty() && pOwner->ApplyPreset(value.c_str()))
				pOwner->RefreshAfterApply();
		}
	}
}

CCineGradeComponent* SCineGradePresetSlot::Resolve(EntityId id)
{
	if (id == INVALID_ENTITYID || gEnv == nullptr || gEnv->pEntitySystem == nullptr)
		return nullptr;

	IEntity* const pEntity = gEnv->pEntitySystem->GetEntity(id);
	return pEntity ? pEntity->GetComponent<CCineGradeComponent>() : nullptr;
}

// The button path, unlike the picker path, does NOT run inside an input serialization
// (PropertyRowActionButton::onMouseUp calls the callback and then tree->revert(), which is an
// output pass), so both of these have to re-run the editor's save snapshot by hand.
//
// Static, and reached through the entity: the button that calls this may belong to a property row
// built for a component that no longer exists. The lookup is what turns that from a write into
// freed memory into a no-op.
void SCineGradePresetSlot::OnExport(EntityId id)
{
	CCineGradeComponent* const pOwner = Resolve(id);
	if (pOwner == nullptr)
		return;

	const string written = pOwner->ExportPreset();
	if (written.empty())
		return;

	// SetAppliedPath and not a plain assignment: the next input pass must not read this as a fresh
	// pick and re-apply the file we have just written out of the very values we hold. Written into
	// the LIVE component's slot (pOwner->m_preset), never into whatever copy the row was built
	// from - the two are not the same object once the inspector has cached anything.
	pOwner->SetPresetAppliedPath(written.c_str());
	pOwner->SyncEditorPropertySnapshot();
}

void SCineGradePresetSlot::OnReapply(EntityId id)
{
	CCineGradeComponent* const pOwner = Resolve(id);
	if (pOwner == nullptr)
		return;

	const string path = pOwner->GetPresetPath();
	if (path.empty())
		return;

	if (pOwner->ApplyPreset(path.c_str()))
	{
		pOwner->SetPresetAppliedPath(path.c_str());
		pOwner->SyncEditorPropertySnapshot();
		pOwner->RefreshAfterApply();
	}
}

// ---------------------------------------------------------------------------
// IEntityComponent
// ---------------------------------------------------------------------------
// The one thing initialisation is for: handing the preset slot the entity its three rows need.
// Same place and same reason as CDebugSerializeHelper's SetComponent (TriggerComponent.cpp:132) -
// but an id and not a pointer, see the header comment on SCineGradePresetSlot.
void CCineGradeComponent::Initialize()
{
	m_preset.SetOwnerEntity(m_pEntity ? m_pEntity->GetId() : INVALID_ENTITYID);
}

bool CCineGradeComponent::ApplyPreset(const char* szRelPath)
{
	// Seeded with what we already hold, so a preset written by an older build - one that does not
	// carry a control added since - leaves that control alone instead of resetting it.
	CineCamAssets::SGradePreset preset;
	preset.version = CineCamAssets::kPresetVersion;
	preset.grade = m_grade;
	preset.curves = m_curves;
	preset.look = m_look;

	if (!CineCamAssets::LoadPreset(szRelPath, preset))
		return false;

	m_grade = preset.grade;
	m_curves = preset.curves;
	m_look = preset.look;

	CryLogAlways("[cinecam] entity '%s': grade preset '%s' applied - these values are now this "
	             "component's own; editing them does not change the file.",
	             m_pEntity ? m_pEntity->GetName() : "?", szRelPath);
	return true;
}

string CCineGradeComponent::ExportPreset()
{
	CineCamAssets::SGradePreset preset;
	preset.version = CineCamAssets::kPresetVersion;
	preset.grade = m_grade;
	preset.curves = m_curves;
	preset.look = m_look;

	const char* szEntity = m_pEntity ? m_pEntity->GetName() : "grade";
	preset.source.Format("CineCam Grade on entity '%s'", szEntity);

	const string name = CineCamAssets::SanitiseAssetName(szEntity);
	const string relPath = string(CINECAM_FOLDER_GRADES) + "/" + name + ".cinegrade";

	if (!CineCamAssets::SavePreset(relPath.c_str(), preset))
		return string();

	CryLogAlways("[cinecam] entity '%s': grade exported to '%s'. It is an asset - pick it on any "
	             "other CineCam Grade to reuse it.", szEntity, relPath.c_str());
	return relPath;
}

// The pair of calls at EntityObject.cpp:925/928, performed by the component about itself. The
// editor only builds these per-instance properties for a Schematyc component on an entity that has
// a Schematyc object (EntityObject.cpp:884-891), so every step here can legitimately come back
// empty - in which case there is no snapshot to keep in step and nothing to do.
void CCineGradeComponent::SyncEditorPropertySnapshot()
{
	if (!gEnv->IsEditor() || m_pEntity == nullptr)
		return;

	Schematyc::IObject* const pObject = m_pEntity->GetSchematycObject();
	if (pObject == nullptr)
		return;

	Schematyc::IObjectPropertiesPtr pProperties = pObject->GetObjectProperties();
	if (!pProperties)
		return;

	if (Schematyc::CClassProperties* pClassProperties = pProperties->GetComponentProperties(GetGUID()))
	{
		pClassProperties->SetOverridePolicy(Schematyc::EOverridePolicy::Override);
		pClassProperties->Read(GetClassDesc(), this);
	}
}

void CCineGradeComponent::RefreshAfterApply()
{
	if (m_pEntity == nullptr)
		return;

	CCinematicCameraComponent* const pCamera = m_pEntity->GetComponent<CCinematicCameraComponent>();
	if (pCamera != nullptr)
		pCamera->RefreshGrade();

	// Force the next ReportStatus to speak even if the answer has not changed: applying a preset is
	// exactly the moment a user wants to be told whether it reached the picture.
	m_lastStatus = -1;
	ReportStatus(pCamera);
}

// ONE event: the moment a property of this component changes. That is the editor's inspector, and
// - since every one of these properties is animatable - it is also TrackView, which walks the
// component's reflected members, writes the animated one in place and sends exactly this event, on
// the frames the value really moved and not on the others (CryMovie/EntityNode.cpp:2837-2852). So
// an animated grade needs no plumbing of its own: it arrives here and nudges the camera like any
// other edit.
// There is nothing for an update to do (the camera pulls this component's state on the frame it
// publishes) and nothing to do at level load or at game start either - the camera re-reads this
// component every frame regardless, so a component that heard nothing would still be in the
// picture. Staying off the level-load and game-start paths altogether is deliberate: a component
// that holds state and answers questions has no business being woken by a mode change.
Cry::Entity::EventFlags CCineGradeComponent::GetEventMask() const
{
	return ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED;
}

void CCineGradeComponent::ProcessEvent(const SEntityEvent& event)
{
	if (event.event != ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED || m_pEntity == nullptr)
		return;

	// Re-arm the preset slot's owner id. It is set in Initialize(), and the Schematyc object path
	// applies stored properties BEFORE Initialize (Object.cpp:636-655) so that is normally enough -
	// but CEntityComponentsCache::LoadComponent (:100-108), which is what restores a component's
	// properties when the editor leaves game mode, calls CClassProperties::Apply on a LIVE
	// component and then sends exactly this event. Apply is a memberwise copy, so it overwrites
	// the id with whatever the cached copy held. This assignment closes that window - and since
	// S10 item 6 the thing being overwritten is a HANDLE, so the window being open is a no-op
	// (a stale id resolves to nullptr) instead of a write through a dangling pointer.
	m_preset.SetOwnerEntity(m_pEntity->GetId());

	// The editor sends this event to the EDITED component only (EntityObject.cpp:831/934), so the
	// camera does not hear about a grade change by itself. This is the latency nudge from
	// decisions/s10-grade-component.md section 3 P1 - not the source of truth: the camera re-reads
	// this component on its own ENTITY_EVENT_UPDATE anyway, so if the nudge never fired the
	// picture would still be right one frame later.
	CCinematicCameraComponent* const pCamera = m_pEntity->GetComponent<CCinematicCameraComponent>();
	if (pCamera != nullptr)
		pCamera->RefreshGrade();

	ReportStatus(pCamera);
}

// The three preconditions, named in the console the moment the user edits a value that cannot
// reach the picture. Written as one line listing every reason at once rather than one message per
// reason: a user whose viewport is on another camera AND whose Scene Referred switch is off
// should not have to fix one, re-test, and be told about the other.
void CCineGradeComponent::ReportStatus(const CCinematicCameraComponent* pCamera)
{
	enum : int
	{
		eStatus_NoCamera      = 1 << 0,
		eStatus_NotPreviewing = 1 << 1,
		eStatus_NotSceneRef   = 1 << 2,
	};

	// The cheap half first, and the whole function turns on it. Since S10 item 3a this is a
	// PER-FRAME path during a TrackView playback - CryMovie sends this component
	// ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED on every frame an animated grade property actually
	// moves (EntityNode.cpp:2843-2848) - so building a string on every call would be an
	// allocation per animated key per frame for a message that is almost always the same one.
	int status = 0;
	if (pCamera == nullptr)
	{
		status = eStatus_NoCamera;
	}
	else
	{
		// In the editor the picture belongs to the camera the viewport looks through
		// (decisions/s9-editor-cinecam-preview.md); outside it, to the active camera.
		if (gEnv->IsEditing() && !pCamera->IsEditorPreviewOwner())
			status |= eStatus_NotPreviewing;
		if (!pCamera->IsSceneReferred())
			status |= eStatus_NotSceneRef;
	}

	if (status == m_lastStatus)
		return;

	m_lastStatus = status;

	string reasons;
	if (status & eStatus_NoCamera)
	{
		reasons = "there is no Cinematic Camera on this entity";
	}
	else
	{
		if (status & eStatus_NotPreviewing)
			reasons = "the viewport is not looking through this camera (Camera menu -> this entity)";

		if (status & eStatus_NotSceneRef)
		{
			if (!reasons.empty())
				reasons += "; ";
			reasons += "the camera's Scene Referred switch is off";
		}
	}

	if (reasons.empty())
	{
		CryLog("[cinecam] entity '%s': the grade is live - the camera is publishing this "
		       "component's CDL and look.", m_pEntity->GetName());
		return;
	}

	// A warning and not a log line, for the same reason the missing ODT is one: the user has just
	// dialled a value and the picture did not move, and the explanation has to be findable in a
	// console full of asset spam.
	CryWarning(VALIDATOR_MODULE_SYSTEM, VALIDATOR_WARNING,
	           "[cinecam] entity '%s': this CineCam Grade is NOT in the picture - %s. The ASC CDL "
	           "and the LMT are applied in ACEScct by the scene-referred output chain, so they do "
	           "nothing on the stock path or through another camera.",
	           m_pEntity->GetName(), reasons.c_str());
}
