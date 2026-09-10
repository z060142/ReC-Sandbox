// The "CineCam" viewport camera: the editor look, published without an entity.
//
// The Sandbox level-editor viewport camera menu offers CineCam under the stock Default camera
// (decisions/s9-editor-cinecam-preview.md). CineCam is Default plus the two things that make the
// editor picture the same picture the pipeline will produce:
//
//   - the scene-referred path on (switch, light-unit convention, default ODT, sun shafts off), and
//   - automatic adaptation: the AUTO meter, and nothing else. There is no aperture, no shutter,
//     no ISO and no ND here - a fly camera has no lens - so the one knob is an EV compensation,
//     the cvar cinecam_EditorLookEV, exactly as EExposureMode::AutoCompensation means it on a
//     component.
//
// It is a second driver of the same shared render state as CCinematicCameraComponent, so it uses
// the same save/restore latches and the same metering code (CineCamShared.h). The plugin's
// resolver guarantees the two never publish in the same frame.
#pragma once

#include "CineCamShared.h"

struct ICVar;

class CCineCamEditorLook
{
public:
	//! Registers cinecam_EditorLookEV. Called once from the plugin's Initialize().
	void RegisterCVars();
	//! Removes it again. Called from the plugin's destructor.
	void UnregisterCVars();

	//! Publish the editor look for this frame. Called from the plugin's per-frame resolver while
	//! the viewport is on the CineCam camera and the level is up.
	void Update();

	//! Give the render state back: every latch this publisher captured is written back and
	//! dropped. Called when the viewport moves to Default or to a cinecam entity, and on level
	//! teardown. Safe to call repeatedly - each restore early-outs when nothing was latched.
	void Restore();

	//! Drop every latch WITHOUT writing, for the case where the bus was reset behind our back
	//! (level load, ResetPostEffects on a game/simulation-mode edge). The next Update() re-latches
	//! from the fresh defaults, exactly as the component's ClearSavedBaselines() does.
	void ClearSavedBaselines();

private:
	//! EV100 the editor look exposes at: the shared AUTO meter, minus cinecam_EditorLookEV. Falls
	//! back to the triangle a fresh Cinematic Camera runs with until the first measurement lands.
	float ComputeEV100();
	float GetEVCompensation() const;

	CineCam::SAutoMeterState m_autoMeter;

	ICVar* m_pEVCVar = nullptr;
	float  m_evCompensationCVar = 0.0f;

	// Scene-referred request (Global_User_SceneReferred).
	float m_savedSceneReferred = 0.f;
	bool  m_bSceneReferredSaved = false;

	// Pre-exposure and the sensor's full well (Global_User_SceneExposure / SensorClipStops).
	float m_savedSceneExposure = 1.f;
	float m_savedSensorClipStops = 0.f;
	bool  m_bSceneExposureSaved = false;

	// Eye adaptation triple (E3DPARAM_HDR_EYEADAPTATION_PARAMS).
	Vec3  m_savedEyeAdaptation = Vec3(4.5f, 17.0f, 1.5f);
	bool  m_bExpParamsSaved = false;

	// Sun-shaft suppression (Global_User_SunShaftsSuppressed).
	bool  m_bSunShaftsSaved = false;

	//! Whether we have published LUT ids that still need taking off the bus.
	bool  m_bLutsPublished = false;
};
