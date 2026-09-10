#include "StdAfx.h"
#include "CineCamShared.h"

#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/ITimeOfDay.h>
#include <CryRenderer/IRenderer.h>
#include <CrySystem/IConsole.h>
#include <CrySystem/ISystem.h>
#include <CryMath/Cry_Math.h>

// The cinecam code that is not the entity component's alone: the metering loop, the light-unit
// convention flip, and (defined next to the LUT slots in CinematicCameraComponent.cpp) the
// display-LUT publication. Lifted out of CCinematicCameraComponent verbatim when the Sandbox
// viewport's built-in "CineCam" camera became a second driver of the same render state
// (decisions/s9-editor-cinecam-preview.md); the entity path is byte-identical to before, it just
// passes its reflected settings in instead of reading them from members.
namespace CineCam
{

// Mid grey, and the ISO 12232 saturation-based sensitivity factor 120/S at S = 100. The renderer's
// twins are CRendererResources::kSceneReferredMidGrey and the 1.2 in
// ComputeSceneReferredExposureScale() below; both are spelled out here rather than shared through
// a header because the plugin cannot see the renderer's internals, and both are anchors of the
// whole pipeline (spec D6) rather than tunables.
static const float kSceneMidGrey = 0.18f;
static const float kSensorSaturationFactor = 1.2f;

// The percentile band, and the EV that puts it on mid grey (decisions/s8-metering.md approach A,
// steps 3-4).
//
// WHY A BAND AND NOT A MEAN. For a geometric mean, a fraction f of the frame at V_hot against a
// background V_bg shifts the answer by exactly f * log2(V_hot / V_bg) - the sensitivity is LINEAR
// in area fraction. That single line is why the old meter was nearly blind to the sun (0.025 % of
// a 50 mm frame, 0.0035 stops) and violently sensitive to the sky (50 % of the frame, 2.65 stops
// at a fabricated albedo). No amount of clamping fixes a bias caused by half the frame being
// ordinary-bright and wrongly weighted; a percentile is robust to it by construction, because it
// asks WHERE the weight sits and not HOW MUCH each texel is worth. This is the shape UE's
// Histogram mode, Frostbite and the Call-of-Duty family all landed on, and a matrix meter is a
// coarser, cleverer version of the same instinct.
//
// WHY 50-90 AND NOT UE'S 10-90. UE's 10/90 exists to make Histogram agree with Basic, i.e. to be
// unsurprising to people migrating from a mean. We do not want to agree with the mean - the mean
// is the bug. Pre-4.25 UE defaulted to 80/98.3, deliberately biased at the bright end, and that
// instinct is the right one for a scene whose subject is sunlit and whose darkest 40 % is shadow
// detail nobody meters for. 50-90 sits between the two: it discards the shadow half (which is what
// dragged the airfield's ground below grey) and the top decile (sky, glints, the sun), and lands
// on the sunlit subject. Both numbers are on the panel precisely so the operator can walk them.
//
// THE STRADDLING BINS ARE SPLIT LINEARLY. 32 stops over 64 bins is half a stop per bin, which as a
// hard quantisation would be visible as steps in the exposure. Interpolating the CDF inside the
// two bins the band's edges fall in takes it under a tenth of a stop, and it is not optional.
//
// Returns false - and the camera then HOLDS its last EV rather than acting - when the band lands
// entirely inside a saturating bucket (bin 0 or bin 63 are bounds, not values) or when the result
// leaves the plausibility window. Holding is always the right answer to a meter that has lost its
// footing; feeding the loop a number it cannot justify is how a runaway starts.
bool ReduceMeterHistogram(const SSceneReferredMeterHistogram& hist, const SAutoMeterSettings& settings,
                          SAutoMeterState& state, float& targetEV)
{
	const int nBins = SSceneReferredMeterHistogram::kBinCount;
	const float step = (hist.logMaxStops - hist.logMinStops) / (float)nBins;

	// Every early return below names its reason on the bus. A meter that holds without saying
	// why is indistinguishable from Manual, which is how a black histogram passed for a working
	// camera for a whole evening (scene@f2d30f98 -> the ClearDeviceOutputState fix).
	state.targetEV = 0.0f;

	if (!(step > 0.0f) || !(hist.totalWeight > 0.0f))
	{
		state.hold = eMeterHold_ZeroTotal;
		return false;
	}

	float lo = clamp_tpl(settings.bandLowPercent, 0.0f, 100.0f);
	float hi = clamp_tpl(settings.bandHighPercent, 0.0f, 100.0f);
	if (hi <= lo)
	{
		// An inverted or empty band is an authoring mistake, not a reason to stop metering: fall
		// back to the whole frame, which is a defensible picture, and say so once.
		if (!state.bLoggedBandInverted)
		{
			state.bLoggedBandInverted = true;
			CryLog("[CinematicCamera] Metering Band Low (%.0f) is not below Metering Band High (%.0f) - "
			       "metering the whole frame until they are.", lo, hi);
		}
		lo = 0.0f;
		hi = 100.0f;
	}

	const float wLo = hist.totalWeight * lo * 0.01f;
	const float wHi = hist.totalWeight * hi * 0.01f;

	// One walk of the CDF: the weight inside the band, the weighted sum of the sub-interval
	// centres over it, and the two edges in stops.
	float acc = 0.0f, bandWeight = 0.0f, bandSum = 0.0f;
	float loStops = hist.logMinStops, hiStops = hist.logMaxStops;
	int firstBin = -1, lastBin = -1;

	for (int i = 0; i < nBins; ++i)
	{
		const float w = hist.bins[i];
		if (w <= 0.0f)
			continue;

		const float a = acc;
		const float b = acc + w;
		acc = b;

		const float lower = max(a, wLo);
		const float upper = min(b, wHi);
		if (upper <= lower)
			continue;

		// Where inside this bin the band's slice sits, as a fraction of the bin's width. The bins
		// are half a stop of log2 luminance each, and the weight is assumed uniform across a bin -
		// which is the only assumption available and the one that makes the interpolation linear.
		const float tMid = ((lower + upper) * 0.5f - a) / w;
		const float centre = hist.logMinStops + ((float)i + tMid) * step;

		const float sliceWeight = upper - lower;
		bandWeight += sliceWeight;
		bandSum += sliceWeight * centre;

		if (firstBin < 0)
		{
			firstBin = i;
			loStops = hist.logMinStops + ((float)i + clamp_tpl((wLo - a) / w, 0.0f, 1.0f)) * step;
		}
		lastBin = i;
		hiStops = hist.logMinStops + ((float)i + clamp_tpl((upper - a) / w, 0.0f, 1.0f)) * step;
	}

	if (bandWeight <= 0.0f || firstBin < 0)
	{
		state.hold = eMeterHold_BandEmpty;
		return false;
	}

	// The band's log-mean, in stops over mid grey, in the EXPOSED units the bins were built in.
	// Computed and published BEFORE the refusals below, so the overlay shows where a refused band
	// sat (-13.75 is bin 0, +17.75 is bin 63) rather than a stale number from the last good frame.
	const float meanStops = bandSum / bandWeight;
	state.bandMeanStops = meanStops;
	state.bandLowLum = (kSceneMidGrey * powf(2.0f, loStops)) / max(hist.exposureAtCapture, 1e-12f);
	state.bandHighLum = (kSceneMidGrey * powf(2.0f, hiStops)) / max(hist.exposureAtCapture, 1e-12f);

	// A band that resolves entirely inside a saturating bucket has a POSITION and no value: bin 0
	// means "at or below 14 stops under grey" and bin 63 means "at or above 18 stops over it", and
	// acting on either would be acting on the edge of the window rather than on the scene.
	if (firstBin == lastBin && (firstBin == 0 || firstBin == nBins - 1))
	{
		state.hold = (firstBin == 0) ? eMeterHold_BandInBottom : eMeterHold_BandInTop;
		return false;
	}

	// Undo the pre-exposure that was in force when these bins were measured. The whole histogram
	// shifts rigidly by that one scalar, which is why this conversion happens once, here, and not
	// per bin: there is no ordering hazard and no possibility of a half-converted result.
	const float exposureAtCapture = max(hist.exposureAtCapture, 1e-12f);
	const float bandAbsolute = (kSceneMidGrey * powf(2.0f, meanStops)) / exposureAtCapture;

	// The plausibility window, in ABSOLUTE engine units of LUMINANCE (1.0 = 10 000 cd/m^2). The
	// illuminance window the log-mean path uses is the same window scaled by 0.18/pi, since
	// L = E * rho / pi at rho = 0.18. Outside it the meter has lost its footing and the answer is
	// to hold, not to drive the camera to a limit.
	if (!NumberValid(bandAbsolute) || bandAbsolute <= 1e-10f || bandAbsolute >= 1e3f)
	{
		state.hold = eMeterHold_OutOfWindow;
		return false;
	}

	// The EV that lands the band on mid grey.
	//
	//   the pre-exposure is  scale = LIGHT_UNIT_SCALE / (1.2 * 2^EV100)   (spec D6)
	//   we want              L_absolute * scale = 0.18
	//   therefore            EV100 = log2( L_absolute * LIGHT_UNIT_SCALE / (1.2 * 0.18) )
	//
	// This is the SAME number the incident form gives, not a second calibration: substituting
	// L = E * 0.18 / pi into it returns log2(E * LIGHT_UNIT_SCALE * 100 / (120*pi)) exactly, which
	// is decisions/s1-exposure-constant.md's C = 120*pi written the other way round. Metering
	// luminance instead of an albedo-divided illuminance estimate changes WHAT is measured - the
	// meter is now reflected rather than incident, and the sky/emissive albedo fallback is simply
	// gone - but not what "correctly exposed" means.
	targetEV = log2f(bandAbsolute * RENDERER_LIGHT_UNIT_SCALE / (kSensorSaturationFactor * kSceneMidGrey));

	// For r_HDRDebug 1 only. Published from ApplySceneExposure(), which is called once a frame;
	// this function can be reached more than once and must not write to the bus itself.
	state.targetEV = targetEV;
	state.hold = eMeterHold_Live;

	return true;
}

// One frame of the AUTO metering loop, lifted whole out of ComputeSceneReferredEV100(): the
// component keeps the mode dial, the bias and the EV limits, this keeps the meter.
bool UpdateAutoMeteredEV(const SAutoMeterSettings& settings, SAutoMeterState& state, float& outEV)
{
	// ----- AUTO -----------------------------------------------------------------------------
	// Two meters, and exactly one of them publishes on any frame (r_SceneReferredMeterHistogram).
	//
	//   HISTOGRAM (default) - 64 bins of log2 LUMINANCE relative to mid grey, accumulated by a
	//   compute pass with the camera's metering mask, and reduced HERE to the log-mean of a
	//   percentile band. This is the one that fixes the outdoor picture: the failure it replaces
	//   was not a wrong constant but a wrong STATISTIC - a whole-frame mean, in which one large
	//   region (the sky, at a fabricated 0.2 albedo and full area weight) owned the answer and put
	//   74 700 lux on mid grey when the subject was lit by 100 000.
	//
	//   LOG-MEAN (fallback) - the shipped whole-frame geometric mean of the albedo-divided
	//   illuminance estimate, kept as the A/B reference.
	//
	// Both arrive with the pre-exposure of the sampled frame already accounted for by the stage,
	// so neither loop has to unwind its own output.
	SSceneReferredMeterHistogram hist;
	ZeroStruct(hist);
	float meteredIlluminance = 0.0f;
	if (gEnv->pRenderer)
	{
		gEnv->pRenderer->EF_Query(EFQ_GetSceneReferredMeterHistogram, hist);
		gEnv->pRenderer->EF_Query(EFQ_GetSceneReferredMeteredIlluminance, meteredIlluminance);
	}

	const uint32 frameId = gEnv->nMainFrameID;
	const bool bNewFrame = (frameId != state.frameId);
	state.frameId = frameId;

	float targetEV = 0.0f;
	bool bHaveTarget = false;

	if (hist.sampleId != 0 && hist.totalWeight > 0.0f)
	{
		bHaveTarget = ReduceMeterHistogram(hist, settings, state, targetEV);
	}
	else if (meteredIlluminance > 0.0f)
	{
		// The incident-meter form, ISO 2720: EV = log2(E * S / C) at S = 100 with C = 120*pi -
		// the constant that puts an 18 % reflector under the metered illuminance on 0.18. The
		// engine's measure divides the G-buffer albedo out, so this really is an incident reading:
		// a white wall and a black wall under the same light expose the same, which is what a
		// light meter on set does and is steadier than reflected metering under a pan.
		const float kIncidentMeterConstant = 120.0f * gf_PI;
		targetEV = log2f(meteredIlluminance * RENDERER_LIGHT_UNIT_SCALE * 100.0f / kIncidentMeterConstant);
		bHaveTarget = true;
		// Not a hold - but the overlay has to say WHICH meter produced the target, because the
		// log-mean has no mask and no band, and its band numbers are meaningless.
		state.hold = eMeterHold_LogMean;
		state.targetEV = targetEV;
		state.bandMeanStops = 0.0f;
	}
	else
	{
		// Neither meter has produced a sample: the readback has not completed yet, the path is
		// off, or the stage retired the value. The camera holds - and says so.
		state.hold = eMeterHold_NoSample;
		state.targetEV = 0.0f;
	}

	if (bHaveTarget && settings.bLock)
		state.hold = eMeterHold_Locked;

	if (bHaveTarget && !settings.bLock && bNewFrame)
	{
		const float dt = max(gEnv->pTimer->GetFrameTime(), 0.0f);

		// ASYMMETRIC time constants, because the problem is asymmetric and a symmetric tau applied
		// to it is what made walking out of a hangar a three-second over-exposure
		// (research/s8-metering.md section 2.5). targetEV ABOVE the current EV means the scene got
		// brighter and the picture has to DARKEN - a camera stops down at once, and photopic light
		// adaptation is about a fifth of a second - so that direction gets Auto Darken Time. The
		// other direction keeps Response Time, because dark adaptation genuinely is slow. 0 on the
		// darken side means "one time constant for both", the old behaviour.
		const float darkenTime = settings.darkenTime;
		const bool bDarkening = (targetEV > state.autoMeteredEV);
		const float tau = (bDarkening && darkenTime > 0.0f) ? darkenTime : settings.tau;

		if (!state.bValid || tau <= 0.0f || dt <= 0.0f)
		{
			// Instant snap: first measurement, or a cut (the component clears the valid flag when
			// it takes over the view). Same behaviour as AutoExposure.cpp:165 - a cut must not ramp.
			state.autoMeteredEV = targetEV;
			state.bValid = true;
		}
		else
		{
			// Frame-rate independent, and scaled by Exposure Response - which under the switch is
			// the METERING RESPONSE: the fraction of a measured change the camera follows per tau.
			// It slows the approach, it does not leave a steady-state error, so the name stays
			// honest and a level authored with 0.6 still behaves like a camera that does not chase
			// every flicker (decisions/s1-auto-mode.md part 2, research option (c)).
			const float response = clamp_tpl(settings.response, 0.0f, 1.0f);
			const float k = response * (1.0f - expf(-dt / tau));
			state.autoMeteredEV += (targetEV - state.autoMeteredEV) * k;
		}
	}
	else if (!state.bValid)
	{
		// Nothing measured yet (first frames, or the meter is stalled): expose as if Manual rather
		// than at EV 0, which would be a white frame.
		return false;
	}

	outEV = state.autoMeteredEV;
	return true;
}

// The 3DEngine-visible half of the switch (SceneReferredSpec.md S2,
// decisions/s2-engine-side-switch.md approach A).
//
// Four unit decisions live inside Cry3DEngine, on the main thread, and cannot read a renderer
// cvar: the sun's divide by PI in ConvertIlluminanceToLightColor(), the volumetric-cloud
// replication of that divide, the legacy .tod migration's multiply by PI, and the Nishita sky's
// sun-intensity scale. E3DPARAM_SCENE_REFERRED carries the answer across the boundary. This is
// the same mechanism the time of day already uses to reach the renderer (E3DPARAM_*), so nothing
// new is invented for it.
//
// Every one of those values is CACHED until the next time-of-day update, and the Nishita sky
// updates its LUT at about 12 % per frame, so flipping the parameter without a forced
// re-evaluation would leave the sun at the old convention until the artist next moved the TOD
// slider, and the sky mid-crossfade for several frames. ITimeOfDay::Update(true, true) is the
// engine's own "recompute everything now" call: bForceUpdate reaches
// C3DEngine::SetSkyLightParameters(..., forceImmediateUpdate) and from there
// CSkyLightManager::FullUpdate(), which recomputes the whole sky dome in one go.
//
// The re-push happens only ON A FLIP. Doing it every frame would mean a full Nishita recompute
// per frame; doing it never would mean a stale sun. Writing to the plain global unconditionally
// is harmless (it is a bool store), so the flip test is on the value we last published.
void PublishSceneReferredConvention(bool bSceneReferred)
{
	if (!gEnv->p3DEngine)
		return;

	// The bisection gate, enforced HERE because the four decision sites inside Cry3DEngine read
	// E3DPARAM_SCENE_REFERRED directly and cannot see a renderer cvar. Withholding the parameter
	// below stage 3 leaves the engine on the stock light-unit convention, which is exactly what
	// "r_SceneReferredStage 2" is supposed to mean. The renderer-side half of the same gate is in
	// CRendererResources::IsSceneReferredLightUnits().
	if (bSceneReferred)
	{
		if (ICVar* pStage = gEnv->pConsole ? gEnv->pConsole->GetCVar("r_SceneReferredStage") : nullptr)
		{
			if (pStage->GetIVal() < 3)
				bSceneReferred = false;
		}
	}

	Vec3 current(0.0f, 0.0f, 0.0f);
	gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_SCENE_REFERRED, current);
	const bool bCurrent = (current.x > 0.5f);

	if (bCurrent == bSceneReferred)
		return;

	gEnv->p3DEngine->SetGlobalParameter(E3DPARAM_SCENE_REFERRED, Vec3(bSceneReferred ? 1.0f : 0.0f, 0.0f, 0.0f));

	if (ITimeOfDay* pTOD = gEnv->p3DEngine->GetTimeOfDay())
	{
		// bInterpolate = true keeps the current time of day; bForceUpdate = true is what makes
		// this a full re-evaluation instead of the usual "only if the time moved" early-out, and
		// it is also what forces the sky dome's full recompute.
		pTOD->Update(true, true);
	}

	CryLog("[SceneReferred] light-unit convention -> %s (time of day re-evaluated, sky dome recomputed)",
	       bSceneReferred ? "radiometric, 1/PI in the BRDF" : "stock (sun pre-divided by PI)");
}

} // namespace CineCam
