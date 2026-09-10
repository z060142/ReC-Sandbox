#include "StdAfx.h"
#include "CineCamEditorLook.h"
#include "CinematicCameraComponent.h"   // EMeteringMode, and the defaults this look mirrors

#include <Cry3DEngine/I3DEngine.h>
#include <CrySystem/IConsole.h>
#include <CrySystem/ConsoleRegistration.h>
#include <CryRenderer/IRenderer.h>

namespace
{
// What a fresh Cinematic Camera runs with, and therefore what a fly camera with no lens has to
// stand in for. Spelled out rather than default-constructing a component: these are the numbers
// the editor look IS, and a reader should not have to chase reflected in-class initialisers to
// find out what the viewport is showing them.
const float kEditorLookSensorClipStops = 8.0f;      // SCineExposureParams::sensorClipStops
const float kEditorLookMeterSkyWeight  = 0.25f;     // SCineExposureParams::meterSkyWeight
const float kEditorLookAutoEVMin       = -2.0f;     // SCineExposureParams::autoEVMin
const float kEditorLookAutoEVMax       = 20.0f;     // SCineExposureParams::autoEVMax

// The triangle the AUTO fallback exposes at before the first measurement lands: f/5.6, 1/50,
// ISO 400 -> EV100 8.615, the shipped body/lens defaults. Same role as the component's
// "expose as if Manual rather than at EV 0, which would be a white frame".
const float kEditorLookFallbackAperture   = 5.6f;
const float kEditorLookFallbackShutterDen = 50.0f;
const float kEditorLookFallbackISO        = 400.0f;

// The renderer's twin of this is CRendererResources::kSceneReferredMidGrey's companion, the 1.2 in
// the saturation-based sensitivity model (spec D6). Same constant as the component uses in
// ComputeSceneReferredExposureScale(), for the same reason: it is an anchor, not a tunable.
const float kSensorSaturationFactor = 1.2f;
}

void CCineCamEditorLook::RegisterCVars()
{
	if (m_pEVCVar != nullptr || gEnv->pConsole == nullptr)
		return;

	// The ONE knob. Stops, positive = brighter, the same sign convention a camera's exposure
	// compensation dial has and the same one SCineExposureParams::evCompensation has.
	m_pEVCVar = REGISTER_CVAR2("cinecam_EditorLookEV", &m_evCompensationCVar, 0.0f, VF_NULL,
		"Exposure compensation, in stops, for the Sandbox viewport's CineCam camera. "
		"Positive is brighter. Metering-driven exposure only - aperture, shutter, ISO and ND "
		"belong to a Cinematic Camera entity, not to the editor fly camera.");
}

void CCineCamEditorLook::UnregisterCVars()
{
	if (m_pEVCVar != nullptr && gEnv->pConsole != nullptr)
		gEnv->pConsole->UnregisterVariable("cinecam_EditorLookEV");

	m_pEVCVar = nullptr;
}

float CCineCamEditorLook::GetEVCompensation() const
{
	return m_pEVCVar ? m_pEVCVar->GetFVal() : m_evCompensationCVar;
}

float CCineCamEditorLook::ComputeEV100()
{
	const float evCompensation = GetEVCompensation();

	// The AUTO meter, with the component's shipped metering settings. AutoCompensation semantics:
	// the metered EV, minus the compensation - the aperture/shutter/ISO triangle plays no part,
	// which is the whole difference between this and a camera entity.
	CineCam::SAutoMeterSettings meter;   // in-class defaults ARE the component's defaults

	float ev = 0.0f;
	if (!CineCam::UpdateAutoMeteredEV(meter, m_autoMeter, ev))
	{
		// Nothing measured yet: the shipped triangle, so the first frames are a plausible daylight
		// exposure rather than a white or black frame.
		const float t = 1.0f / kEditorLookFallbackShutterDen;
		ev = log2f(kEditorLookFallbackAperture * kEditorLookFallbackAperture / t)
		     - log2f(kEditorLookFallbackISO / 100.0f);
	}

	ev -= evCompensation;

	return clamp_tpl(clamp_tpl(ev, kEditorLookAutoEVMin, kEditorLookAutoEVMax), -6.0f, 24.0f);
}

void CCineCamEditorLook::Update()
{
	if (gEnv == nullptr || gEnv->p3DEngine == nullptr)
		return;

	I3DEngine* const p3DEngine = gEnv->p3DEngine;

	// ----- the scene-referred switch ---------------------------------------------------------
	if (!m_bSceneReferredSaved)
	{
		p3DEngine->GetPostEffectParam("Global_User_SceneReferred", m_savedSceneReferred);
		m_bSceneReferredSaved = true;
	}

	p3DEngine->SetPostEffectParam("Global_User_SceneReferred", 1.0f);
	CineCam::PublishSceneReferredConvention(true);

	// ----- the display chain -----------------------------------------------------------------
	// Default ODT, identity look. Without an output transform the picture is a clamped ACEScct
	// encode, which is exactly the "looks like a bug because it is one" case the component's
	// empty-field substitution exists for.
	CineCam::ApplyDisplayLuts(nullptr, "");
	m_bLutsPublished = true;

	// ----- the pre-exposure and the meter's inputs -------------------------------------------
	if (!m_bSceneExposureSaved)
	{
		p3DEngine->GetPostEffectParam("Global_User_SceneExposure", m_savedSceneExposure);
		p3DEngine->GetPostEffectParam("Global_User_SensorClipStops", m_savedSensorClipStops);
		m_bSceneExposureSaved = true;
	}

	const float ev100 = ComputeEV100();

	p3DEngine->SetPostEffectParam("Global_User_SceneExposure",
	                              RENDERER_LIGHT_UNIT_SCALE / (kSensorSaturationFactor * powf(2.0f, ev100)));
	p3DEngine->SetPostEffectParam("Global_User_SensorClipStops", kEditorLookSensorClipStops);

	p3DEngine->SetPostEffectParam("Global_User_MeterMode", (float)(uint32)EMeteringMode::CenterWeighted);
	p3DEngine->SetPostEffectParam("Global_User_MeterSkyWeight", kEditorLookMeterSkyWeight);

	// Diagnostics for r_HDRDebug 1, on the same terms as the component: the driver owns the
	// reduction, so the renderer cannot print the band's edges unless the driver says what they were.
	p3DEngine->SetPostEffectParam("Global_User_MeterBandLow", m_autoMeter.bandLowLum);
	p3DEngine->SetPostEffectParam("Global_User_MeterBandHigh", m_autoMeter.bandHighLum);
	p3DEngine->SetPostEffectParam("Global_User_MeterTargetEV", m_autoMeter.targetEV);
	p3DEngine->SetPostEffectParam("Global_User_MeterBandMean", m_autoMeter.bandMeanStops);
	p3DEngine->SetPostEffectParam("Global_User_MeterHold", (float)m_autoMeter.hold);

	// ----- sun shafts ------------------------------------------------------------------------
	// Screen-space shafts are retired on ReCS; re-asserted per frame so a ResetPostEffects cannot
	// leave the stage on under the editor look.
	m_bSunShaftsSaved = true;
	p3DEngine->SetPostEffectParam("Global_User_SunShaftsSuppressed", 1.0f);

	// ----- the eye-adaptation triple ---------------------------------------------------------
	// Not what produces the picture on the scene-referred path (the tone map pins its own window),
	// but the particle system's ExposureValue domain reads it, so it is kept meaningful and
	// consistent - and .z = 0 because a camera has no scene-key tilt.
	Vec3 v;
	p3DEngine->GetGlobalParameter(E3DPARAM_HDR_EYEADAPTATION_PARAMS, v);

	if (!m_bExpParamsSaved)
	{
		m_savedEyeAdaptation = v;
		m_bExpParamsSaved = true;
	}

	v.x = ev100;
	v.y = ev100;
	v.z = 0.0f;
	p3DEngine->SetGlobalParameter(E3DPARAM_HDR_EYEADAPTATION_PARAMS, v);
}

void CCineCamEditorLook::Restore()
{
	if (gEnv == nullptr || gEnv->p3DEngine == nullptr)
	{
		ClearSavedBaselines();
		return;
	}

	I3DEngine* const p3DEngine = gEnv->p3DEngine;

	if (m_bSceneReferredSaved)
	{
		p3DEngine->SetPostEffectParam("Global_User_SceneReferred", m_savedSceneReferred);
		m_bSceneReferredSaved = false;
		// The engine-side mirror goes back by the same flip path, so the 3D engine is left on the
		// stock light-unit convention with the time of day already re-evaluated for it.
		CineCam::PublishSceneReferredConvention(m_savedSceneReferred > 0.5f);
	}

	if (m_bLutsPublished)
	{
		CineCam::RestoreDisplayLuts();
		m_bLutsPublished = false;
	}

	if (m_bSceneExposureSaved)
	{
		p3DEngine->SetPostEffectParam("Global_User_SceneExposure", m_savedSceneExposure);
		p3DEngine->SetPostEffectParam("Global_User_SensorClipStops", m_savedSensorClipStops);

		// Back to the bus defaults, as RestoreSceneExposure() does: nothing but a cinecam driver
		// ever writes these, and the readouts describe a measurement no longer being made.
		p3DEngine->SetPostEffectParam("Global_User_MeterMode", 1.0f);
		p3DEngine->SetPostEffectParam("Global_User_MeterSkyWeight", 0.25f);
		p3DEngine->SetPostEffectParam("Global_User_MeterBandLow", 0.0f);
		p3DEngine->SetPostEffectParam("Global_User_MeterBandHigh", 0.0f);
		p3DEngine->SetPostEffectParam("Global_User_MeterTargetEV", 0.0f);

		m_bSceneExposureSaved = false;
	}

	if (m_bSunShaftsSaved)
	{
		p3DEngine->SetPostEffectParam("Global_User_SunShaftsSuppressed", 0.0f);
		m_bSunShaftsSaved = false;
	}

	if (m_bExpParamsSaved)
	{
		p3DEngine->SetGlobalParameter(E3DPARAM_HDR_EYEADAPTATION_PARAMS, m_savedEyeAdaptation);
		m_bExpParamsSaved = false;
	}

	// A hand-over is a cut: the next time the editor look drives, the meter snaps.
	m_autoMeter.bValid = false;
}

void CCineCamEditorLook::ClearSavedBaselines()
{
	m_bSceneReferredSaved = false;
	m_bSceneExposureSaved = false;
	m_bExpParamsSaved = false;
	m_bSunShaftsSaved = false;
	m_bLutsPublished = false;
	m_autoMeter.bValid = false;
}
