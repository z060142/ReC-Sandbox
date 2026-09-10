#include "StdAfx.h"
#include "CinematicCameraComponent.h"

// Full CameraComponent definition needed for SetFieldOfView / Activate / IsActive calls.
// Include path ${CRYENGINE_DIR}/Code/CryPlugins/CryDefaultEntities/Module is set in CMakeLists.
#include <DefaultComponents/Cameras/CameraComponent.h>

#include <CrySchematyc/Env/Elements/EnvComponent.h>
#include <CrySchematyc/Env/Elements/EnvFunction.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CryCore/StaticInstanceList.h>
#include <CryMath/Angle.h>
#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/ITimeOfDay.h>
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IRenderAuxGeom.h>
#include <CrySystem/File/ICryPak.h>
#include <CrySystem/IConsole.h>
#include <CrySystem/ConsoleRegistration.h>   // cinecam_LutHotReload
#include <vector>

#include <algorithm>

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------
CCinematicCameraComponent* CCinematicCameraComponent::s_pActive = nullptr;
CCinematicCameraComponent* CCinematicCameraComponent::s_pEditorPreviewOwner = nullptr;
std::vector<CCinematicCameraComponent*> CCinematicCameraComponent::s_all;

// ---------------------------------------------------------------------------
// Schematyc registration
// ---------------------------------------------------------------------------
namespace
{
	static void RegisterCinematicCameraComponent(Schematyc::IEnvRegistrar& registrar)
	{
		Schematyc::CEnvRegistrationScope scope = registrar.Scope(IEntity::GetEntityScopeGUID());
		{
			Schematyc::CEnvRegistrationScope componentScope =
				scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CCinematicCameraComponent));
			CCinematicCameraComponent::Register(componentScope);
		}
	}

	CRY_STATIC_AUTO_REGISTER_FUNCTION(&RegisterCinematicCameraComponent);
}

void CCinematicCameraComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	{
		auto pFunc = SCHEMATYC_MAKE_ENV_FUNCTION(&CCinematicCameraComponent::Activate,
			"{A1B2C3D4-E5F6-4A7B-C8D9-0E1F2A3B4C5D}"_cry_guid, "Activate");
		pFunc->SetDescription("Make this the active cinematic camera");
		pFunc->SetFlags(Schematyc::EEnvFunctionFlags::Construction);
		componentScope.Register(pFunc);
	}
	{
		auto pFunc = SCHEMATYC_MAKE_ENV_FUNCTION(&CCinematicCameraComponent::IsActive,
			"{B2C3D4E5-F6A7-4B8C-D9E0-1F2A3B4C5D6E}"_cry_guid, "IsActive");
		pFunc->SetDescription("Is this the currently active camera?");
		pFunc->SetFlags(Schematyc::EEnvFunctionFlags::Construction);
		pFunc->BindOutput(0, 'iact', "IsActive");
		componentScope.Register(pFunc);
	}
	{
		auto pFunc = SCHEMATYC_MAKE_ENV_FUNCTION(&CCinematicCameraComponent::Deactivate,
			"{C3D4E5F6-A7B8-4C9D-E0F1-2A3B4C5D6E7F}"_cry_guid, "Deactivate");
		pFunc->SetDescription("Restore previous DOF and exposure settings");
		pFunc->SetFlags(Schematyc::EEnvFunctionFlags::Construction);
		componentScope.Register(pFunc);
	}
}

// ---------------------------------------------------------------------------
// IEntityComponent lifecycle
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::PurgeDuplicateCameraComponents()
{
	DynArray<Cry::DefaultComponents::CCameraComponent*> cameras;
	m_pEntity->GetAllComponents<Cry::DefaultComponents::CCameraComponent>(cameras);

	int nRemoved = 0;
	for (Cry::DefaultComponents::CCameraComponent* pCamera : cameras)
	{
		if (pCamera == m_pCameraComponent)
			continue;
		// A camera the user added on purpose is not ours to delete.
		if (pCamera->GetComponentFlags().Check(EEntityComponentFlags::UserAdded))
			continue;
		m_pEntity->RemoveComponent(pCamera);
		++nRemoved;
	}

	if (nRemoved > 0)
		CryLog("[CinematicCamera] entity '%s': removed %d duplicate preview camera component(s) "
		       "accumulated by earlier saves", m_pEntity->GetName(), nRemoved);
}

void CCinematicCameraComponent::Initialize()
{
	// Join the registry the resolver walks. Main-thread only, so no locking.
	if (std::find(s_all.begin(), s_all.end(), this) == s_all.end())
		s_all.push_back(this);

	// Create (or get) the sibling CCameraComponent.
	// Having CCameraComponent present means:
	//  - The entity appears in Sandbox's Camera → Camera Entity menu
	//  - The editor viewport reads its FOV for preview rendering
	//  - ICameraManager is handled by CCameraComponent (no extra wiring needed)
	m_pCameraComponent = m_pEntity->GetOrCreateComponent<Cry::DefaultComponents::CCameraComponent>();

	// The preview camera is OURS, created for the editor's benefit - it must never be
	// serialized. Without this flag every save wrote it into the layer, and the next load
	// deserialized it *in addition* to the one this Initialize creates, growing the entity
	// by one camera component per save/load cycle. A camera component the user added
	// deliberately (UserAdded) keeps its persistence - we only adopt it, never claim it.
	if (!m_pCameraComponent->GetComponentFlags().Check(EEntityComponentFlags::UserAdded))
		m_pCameraComponent->GetComponentFlags().Add(EEntityComponentFlags::NoSave);

	// Repair pass for levels saved before the NoSave flag existed: drop every duplicate
	// camera component that piled up. Runs again on LEVEL_LOADED because the serialized
	// duplicates are deserialized AFTER this component's Initialize.
	PurgeDuplicateCameraComponents();

	// Prevent CCameraComponent from auto-re-activating on ENTITY_EVENT_START_GAME;
	// our Activate() is the single control point for that.
	// Note: CCameraComponent::Initialize() already ran and may have auto-activated.
	m_pCameraComponent->EnableAutomaticActivation(false);

	// Push computed FOV / near / far into the sibling so the editor viewport
	// immediately shows the correct frustum.
	UpdateCameraComponentParams();

	// Deliberately NOT claiming ownership here. The sibling auto-activates during its own
	// Initialize(), before we can disable auto activation above, so every cinematic camera
	// would claim the view at construction and the last one initialised would win at random.
	// ResolveActive() picks the camera manager's actual active camera on the first frame.
}

Cry::Entity::EventFlags CCinematicCameraComponent::GetEventMask() const
{
	Cry::Entity::EventFlags flags =
		ENTITY_EVENT_START_GAME |
		ENTITY_EVENT_LEVEL_LOADED |
		ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED |
		ENTITY_EVENT_RESET;

	// Subscribe to per-frame updates while we are the active camera - or, in the editor, while we
	// are the viewport's preview owner: every editor-live effect hangs off this event, so a preview
	// owner that is not the camera manager's active camera would publish nothing at all.
	if (IsActive() || IsEditorPreviewOwner())
		flags |= ENTITY_EVENT_UPDATE;

	return flags;
}

void CCinematicCameraComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_UPDATE:
		UpdateFocalMotor(event.fParam[0]);
		// Keep the sibling CCameraComponent's FOV in sync even in edit mode
		// (the editor viewport reads it directly from CCameraComponent).
		UpdateCameraComponentParams();
		// Vignette/distortion are cheap cvar writes; keeping them live outside the game
		// finalize hook lets the properties be tuned with immediate feedback.
		ApplyLensCharacter();
		ApplyViewfinder();
		// The scene-referred switch changes the whole picture, so it is editor-live too.
		ApplySceneReferred();
		ApplySceneExposure();
		ApplyExportMetadata();
		ApplyDisplayLuts();
		ApplyGrade();
		// DOF belongs to the camera the picture is taken through: game mode, or the editor
		// viewport's preview owner (whose focus settings are the ones being looked through).
		if (CanApplyViewEffects() && m_dof.bEnableDOF)
			ApplyDOF();
		break;

	case ENTITY_EVENT_LEVEL_LOADED:
		// Legacy duplicates from saves made before the NoSave flag deserialize after
		// Initialize, so the repair has to run once more when the level is fully loaded.
		PurgeDuplicateCameraComponents();
		break;

	case ENTITY_EVENT_START_GAME:
		// Optional auto activation. This must stay opt-in: every component receives
		// START_GAME, so with more than one cinematic camera in the level the last one
		// to get the event would silently steal the view from a Schematyc Activate call.
		if (m_body.bActivateOnGameStart && !IsActive() && m_pCameraComponent)
			Activate();
		break;

	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		// Immediately push updated lens params so the editor preview reflects changes.
		UpdateCameraComponentParams();
		if (CanApplyViewEffects())
		{
			if (m_dof.bEnableDOF)
				ApplyDOF();
			else
				RestoreDOF();
		}
		// Event mask may have changed (e.g., EnableDOF toggled).
		m_pEntity->UpdateComponentEventMask(this);
		ApplyLensCharacter();
		ApplyViewfinder();
		ApplySceneReferred();
		ApplySceneExposure();
		ApplyExportMetadata();
		ApplyDisplayLuts();
		ApplyGrade();
		break;

	case ENTITY_EVENT_RESET:
		// RESET is bidirectional: nParam[0] == 1 entering simulation/game, 0 leaving.
		if (event.nParam[0] == 0)
		{
			// Leaving game mode: give the render state back.
			RestoreAllEffects();
		}
		else
		{
			// Entering game/simulation mode. ResetPostEffects() already wiped the renderer
			// to defaults, so our edit-mode baselines are stale. Drop them WITHOUT writing:
			// the next Apply* re-latches from the fresh defaults.
			ClearSavedBaselines();
		}
		m_pEntity->UpdateComponentEventMask(this);
		break;

	default:
		break;
	}
}

void CCinematicCameraComponent::OnShutDown()
{
	// Leave the registry before anything else so the resolver can never see a dying component.
	s_all.erase(std::remove(s_all.begin(), s_all.end(), this), s_all.end());

	// Unconditional restore + clear, as before: the component being destroyed may well be
	// the active one, and there is no later frame in which the resolver could hand it back.
	RestoreAllEffects();

	if (s_pActive == this)
		s_pActive = nullptr;

	if (s_pEditorPreviewOwner == this)
		s_pEditorPreviewOwner = nullptr;

	m_pCameraComponent = nullptr;
}

// ---------------------------------------------------------------------------
// Activate / Deactivate / IsActive
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::Activate()
{
	if (!m_pCameraComponent)
		return;

	m_bCinematicSuspended = false;
	// A camera change is a cut: the next measurement snaps instead of ramping from the previous
	// camera's exposure, which is what AutoExposure.cpp:165 does for the engine's own adaptation.
	m_autoMeter.bValid = false;

	// Activating the sibling CCameraComponent IS the ownership transfer: it moves the camera
	// manager's active camera, and ResolveActive() picks that up next frame - including the
	// restore sweep on whichever component was dethroned.
	m_pCameraComponent->Activate();

	// Subscribe to ENTITY_EVENT_UPDATE now that we are the active camera.
	m_pEntity->UpdateComponentEventMask(this);

	// DOF is view-dependent: it applies where this camera IS the view - game mode, or the editor
	// viewport looking through this entity. On the editor fly camera, pushing this camera's focus
	// onto somebody else's view is meaningless, and that is what the gate keeps out.
	if (CanApplyViewEffects() && m_dof.bEnableDOF)
		ApplyDOF();
}

void CCinematicCameraComponent::Deactivate()
{
	// The engine always needs SOME active view, so this cannot null the camera manager's
	// active camera. Meaning: strip the cinematic effects and stay a plain view until
	// another camera activates. s_pActive is cleared by the resolver, not here.
	RestoreAllEffects();
	m_bCinematicSuspended = true;

	m_pEntity->UpdateComponentEventMask(this);
}

bool CCinematicCameraComponent::IsActive() const
{
	return !m_bCinematicSuspended && m_pCameraComponent && m_pCameraComponent->IsActive();
}

// ---------------------------------------------------------------------------
// Single ownership authority
// ---------------------------------------------------------------------------
CCinematicCameraComponent* CCinematicCameraComponent::ResolveActive()
{
	CCinematicCameraComponent* pManagerActive = nullptr;
	for (CCinematicCameraComponent* p : s_all)
	{
		if (p->IsActive())
		{
			pManagerActive = p;
			break;
		}
	}

	if (pManagerActive != s_pActive)
	{
		if (s_pActive)           // dethroned: give the render state back
			s_pActive->RestoreAllEffects();

		s_pActive = pManagerActive;

		if (s_pActive)
			s_pActive->m_pEntity->UpdateComponentEventMask(s_pActive);
	}

	return s_pActive;
}

// The edit-mode counterpart of ResolveActive(): the Sandbox viewport, not the camera manager,
// names the owner, and "nobody" is a legitimate answer (the stock Default camera, or the
// plugin-owned CineCam publisher, which has no entity at all).
//
// Identical choreography to a resolver dethroning, and deliberately so: the outgoing owner gets
// the render state back BEFORE the incoming one latches its baselines, so a switch never captures
// the previous owner's values as "what the renderer had". Both components' event masks are
// refreshed, because ENTITY_EVENT_UPDATE - which every editor-live effect hangs off - follows
// ownership through GetEventMask().
void CCinematicCameraComponent::SetEditorPreviewOwner(CCinematicCameraComponent* pOwner)
{
	if (pOwner == s_pEditorPreviewOwner)
		return;

	CCinematicCameraComponent* const pPrevious = s_pEditorPreviewOwner;
	s_pEditorPreviewOwner = pOwner;

	if (pPrevious)
	{
		// The flag is already cleared, so CanDriveRenderState() refuses any Apply* the restore
		// path might otherwise re-trigger.
		pPrevious->RestoreAllEffects();
		if (pPrevious->m_pEntity)
			pPrevious->m_pEntity->UpdateComponentEventMask(pPrevious);
	}

	if (pOwner && pOwner->m_pEntity)
	{
		// A hand-over is a cut: the metered EV snaps to the new camera's first measurement
		// instead of ramping from the previous one, exactly as Activate() does.
		pOwner->m_autoMeter.bValid = false;
		pOwner->m_pEntity->UpdateComponentEventMask(pOwner);
	}
}

CCinematicCameraComponent* CCinematicCameraComponent::FindBySlot(int slot)
{
	if (slot <= 0)
		return nullptr;
	for (CCinematicCameraComponent* p : s_all)
	{
		if (p->m_body.slot.value == slot && p->m_pCameraComponent)
			return p;
	}
	return nullptr;
}

void CCinematicCameraComponent::RestoreAllEffects()
{
	RestoreDOF();
	RestoreFilmGrain();
	RestoreMotionBlur();
	RestoreSpriteBokeh();
	RestoreWhiteBalance();
	RestoreLensCharacter();
	RestoreSceneReferred();
	RestoreSceneExposure();
	RestoreExportMetadata();
	RestoreDisplayLuts();
	RestoreGrade();
	RestoreViewfinder();
	RestoreStreaks();
	RestoreHalation();
	RestoreSunShafts();
	if (m_bExpParamsSaved)
	{
		RestoreExposure();
	}
}

void CCinematicCameraComponent::ClearSavedBaselines()
{
	m_bDofParamsSaved      = false;
	m_bExpParamsSaved      = false;
	m_bGrainParamSaved     = false;
	m_bWBParamsSaved       = false;
	m_bLensCharSaved       = false;
	m_bSceneReferredSaved  = false;
	m_bSceneExposureSaved  = false;
	m_autoMeter.bValid  = false;
	m_bSpriteBokehSaved    = false;
	m_bBokehShapeTexPushed = false;
	m_bMBParamsSaved       = false;
	m_bStreakParamsSaved   = false;
	m_bFilterPSFTexPushed  = false;
	m_bHalationSaved       = false;
	m_bSunShaftsSaved      = false;
	m_bViewfinderSaved     = false;
}

// The engine's own answer to "is the level up yet", and the reason it is asked here rather than
// tracked with a listener: ESYSTEM_GLOBAL_STATE goes back to RUNNING at exactly one place per
// host - CCryEditDoc::LoadLevel() right after it fires ESYSTEM_EVENT_LEVEL_LOAD_END, and
// CCryAction after ESYSTEM_EVENT_LEVEL_GAMEPLAY_START - so a poll is the same answer as the
// event with no bookkeeping to get out of step and nothing to leave the camera stuck if a host
// fires a begin without an end.
bool CCinematicCameraComponent::IsLevelReady()
{
	ISystem* const pSystem = gEnv ? gEnv->pSystem : nullptr;
	return pSystem != nullptr && pSystem->GetSystemGlobalState() == ESYSTEM_GLOBAL_STATE_RUNNING;
}

bool CCinematicCameraComponent::CanDriveRenderState() const
{
	// NOT WHILE A LEVEL IS LOADING (2026-09-07). Everything below this gate reaches into render
	// state that the load is in the middle of tearing down and rebuilding: the post-effect bus
	// (which EF_ResetPostEffects clears on the render thread at the end of the load, so anything
	// published during it is thrown away anyway), the renderer's texture set (ApplyDisplayLuts
	// uploads two 3D textures whose ids the tone map then holds), and - through
	// PublishSceneReferredConvention - a forced ITimeOfDay::Update(true, true), which recomputes
	// the whole Nishita sky dome. Pushing the request from a component that initialises DURING
	// the load means the scene-referred path is asked to run on a pipeline whose targets are
	// still being created, and it is then reset back off at load end by the engine, so the switch
	// is flipped twice for nothing before the first frame is drawn.
	//
	// The trodden path is the one every other level-sensitive engine system takes: do nothing
	// until the level is loaded, then apply. ENTITY_EVENT_UPDATE and the plugin's per-frame
	// resolve both run again on the first frame after the load, so the camera applies itself
	// there - the switch comes on by itself, one frame late, with the level fully built.
	if (!IsLevelReady())
		return false;

	// WHO OWNS THE BUS. Outside the editor this is unchanged: the resolver's cache, i.e. the
	// camera manager's answer. Inside the editor it is the Sandbox viewport's answer instead - the
	// camera the user is actually looking through - and "nobody" is a legitimate answer there (the
	// stock Default camera, and the CineCam publisher, which is not a component). That is also
	// what retires the old edit-mode leak, where the last-initialised camera published its look
	// over whatever the viewport happened to show.
	const bool bOwner = gEnv->IsEditing() ? (s_pEditorPreviewOwner == this) : (s_pActive == this);

	return bOwner
	       && m_pEntity != nullptr
	       && m_pEntity->GetSimulationMode() != EEntitySimulationMode::Preview;
}

// ---------------------------------------------------------------------------
// Sync lens params to the sibling CCameraComponent
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::UpdateCameraComponentParams()
{
	if (!m_pCameraComponent)
		return;

	m_pCameraComponent->SetFieldOfView(
		CryTransform::CAngle::FromRadians(ComputeVerticalFOV()));
	m_pCameraComponent->SetNearPlane((float)m_body.nearPlane);
	m_pCameraComponent->SetFarPlane((float)m_body.farPlane);
}

// ---------------------------------------------------------------------------
// Optics helpers
// ---------------------------------------------------------------------------
float CCinematicCameraComponent::GetSensorWidth() const
{
	switch (m_body.sensorPreset)
	{
	case ESensorPreset::FullFrame35mm:   return 36.0f;
	case ESensorPreset::APSC:            return 23.6f;
	case ESensorPreset::MicroFourThirds: return 17.3f;
	default:                             return max((float)m_body.customSensorWidth, 1.0f);
	}
}

float CCinematicCameraComponent::GetSensorHeight() const
{
	switch (m_body.sensorPreset)
	{
	case ESensorPreset::FullFrame35mm:   return 24.0f;
	case ESensorPreset::APSC:            return 15.7f;
	case ESensorPreset::MicroFourThirds: return 13.0f;
	default:                             return GetSensorWidth() * (2.0f / 3.0f);
	}
}

// Classic prime lens series; the nearest entry (in log space, so the spacing feels even)
// is what the optics actually use when Focal Step Mode is PrimeSet.
static const float s_primeFocalLengths[] = {
	8.f, 10.f, 12.f, 14.f, 16.f, 18.f, 20.f, 24.f, 28.f, 35.f, 40.f, 50.f, 65.f, 75.f,
	85.f, 100.f, 135.f, 150.f, 180.f, 200.f, 250.f, 300.f, 400.f, 500.f, 600.f
};

float CCinematicCameraComponent::ComputeTargetFocalLength() const
{
	const float f_mm = max((float)m_lens.focalLength, 0.1f);

	switch (m_lens.focalStepMode)
	{
	case EFocalStepMode::PrimeSet:
		{
			float best = s_primeFocalLengths[0];
			float bestDistance = fabs_tpl(logf(f_mm / best));
			for (const float candidate : s_primeFocalLengths)
			{
				const float distance = fabs_tpl(logf(f_mm / candidate));
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = candidate;
				}
			}
			return best;
		}

	case EFocalStepMode::Geometric:
		{
			// Equal ratio steps: every m_lens.focalStepsPerDoubling detents doubles the focal length.
			const float stepsPerDoubling = (float)max((int)m_lens.focalStepsPerDoubling, 1);
			const float index = floorf(log2f(f_mm) * stepsPerDoubling + 0.5f);
			return powf(2.0f, index / stepsPerDoubling);
		}

	default:
		return f_mm;
	}
}

// Servo zoom: the quantised target is approached in log space with a framerate independent
// exponential ease that settles within Focal Step Time - the behaviour of a broadcast servo
// zoom rather than a hard cut. (A real prime swap IS a hard cut; the fader is a zoom rocker,
// so the servo metaphor is the right one.) Continuous mode and the editor snap directly.
void CCinematicCameraComponent::UpdateFocalMotor(float deltaSeconds)
{
	const uint32 frameId = (uint32)gEnv->nMainFrameID;
	if (frameId == m_focalMotorFrameId)
		return;
	m_focalMotorFrameId = frameId;

	const float target = ComputeTargetFocalLength();

	if (m_focalMotorMm <= 0.0f || m_lens.focalStepMode == EFocalStepMode::Continuous ||
	    (float)m_lens.focalStepTime <= 0.0f || gEnv->IsEditing())
	{
		m_focalMotorMm = target;
		return;
	}

	const float alpha = 1.0f - expf(-max(deltaSeconds, 0.0f) * 3.0f / max((float)m_lens.focalStepTime, 0.01f));
	const float current = log2f(m_focalMotorMm);
	m_focalMotorMm = powf(2.0f, current + (log2f(target) - current) * alpha);

	if (fabs_tpl(m_focalMotorMm - target) < 0.01f)
		m_focalMotorMm = target;
}

float CCinematicCameraComponent::GetEffectiveFocalLength() const
{
	return m_focalMotorMm > 0.0f ? m_focalMotorMm : ComputeTargetFocalLength();
}

// vFOV = 2 * atan(sensorHeight / (2 * focalLength))
float CCinematicCameraComponent::ComputeVerticalFOV() const
{
	float f_mm = GetEffectiveFocalLength();

	// Focus breathing: a unit-focus lens extends to focus close, so the effective focal
	// length grows by the magnification m = f/(d-f) and the view tightens. Intensity scales
	// the physical amount (0 = internally corrected cine glass).
	if ((float)m_lens.breathingIntensity > 0.0f)
	{
		const float f_m = f_mm * 0.001f;
		const float d = max((float)m_lens.focusDistance, f_m * 2.0f);
		const float magnification = f_m / (d - f_m);
		f_mm *= 1.0f + (float)m_lens.breathingIntensity * magnification;
	}

	// Anamorphic squeeze: the cylindrical element compresses a wider horizontal capture onto
	// the same sensor, so the projected (desqueezed) format is sensorWidth * S by sensorHeight.
	// Whenever that format is wider than the viewport, the format's WIDTH is what has to fit -
	// the horizontal field comes from the widened capture and the vertical field follows the
	// viewport aspect, the bars taking the rest. Otherwise (spherical, or a squeeze small
	// enough that the format still fits) the height is what fits, as before.
	const float S = clamp_tpl((float)m_anamorphic.squeeze, 1.0f, 2.0f);
	const float w_mm = GetSensorWidth() * S;
	const float h_mm = GetSensorHeight();

	if (S > 1.001f)
	{
		const float formatAspect = w_mm / max(h_mm, 0.001f);
		const float viewportAspect = gEnv->pRenderer
			? (float)gEnv->pRenderer->GetOverlayWidth() / (float)max(gEnv->pRenderer->GetOverlayHeight(), 1)
			: formatAspect;
		if (formatAspect > viewportAspect)
		{
			const float hFOV = 2.0f * atanf(w_mm * 0.5f / f_mm);
			return 2.0f * atanf(tanf(hFOV * 0.5f) / max(viewportAspect, 0.001f));
		}
	}

	return 2.0f * atanf(h_mm * 0.5f / f_mm);
}

// The mask the desqueezed format needs, or 0 when the picture already fits the viewport.
// Kept in one place so the frustum (ComputeVerticalFOV) and the bars (r_LensLetterbox) can
// never disagree about whether the format is the wider one.
float CCinematicCameraComponent::ComputeLetterboxAspect() const
{
	const float S = clamp_tpl((float)m_anamorphic.squeeze, 1.0f, 2.0f);
	if (S <= 1.001f)
		return 0.0f;

	const float formatAspect = (GetSensorWidth() * S) / max(GetSensorHeight(), 0.001f);
	const float viewportAspect = gEnv->pRenderer
		? (float)gEnv->pRenderer->GetOverlayWidth() / (float)max(gEnv->pRenderer->GetOverlayHeight(), 1)
		: formatAspect;

	if (formatAspect <= viewportAspect)
		return 0.0f;

	// Negative = squeezed presentation: the renderer stretches the band to fill the viewport
	// instead of masking it. Same frustum either way (ComputeVerticalFOV does not look at this).
	return m_anamorphic.bSqueezedFrame ? -formatAspect : formatAspect;
}

// Circle-of-confusion limit in metres (sensorDiagonal_mm / 1500 / 1000).
float CCinematicCameraComponent::ComputeCoCLimit() const
{
	const float w = GetSensorWidth();
	const float h = GetSensorHeight();
	return (sqrtf(w * w + h * h) / 1500.0f) * 0.001f;
}

// ---------------------------------------------------------------------------
// Mask as an optical element. Past the crossover the mask is the smaller hole, so it - not
// the iris - decides how large a defocused point spreads (CoC follows Mask Aperture) and how
// much light gets through (the smaller hole, times the fraction of it that is actually open).
// The renderer still receives the raw iris f-number: that is what its own domain decision
// (N < N_mask) is keyed on, and the iris polygon is not drawn in the mask domain anyway.
// ---------------------------------------------------------------------------
// CPU copy of the shape mask, so the component can integrate the pupil (mask x iris disc) for
// any f-number. Same DDS contract as the renderer's gather kernel (uncompressed 32-bit, R
// channel, power-of-two 4..512); a mask the renderer cannot read falls back to the geometric
// hole ratio. One table - the shape texture is one per active camera in practice, and a
// reload on a name change is a 64x64 read.
namespace
{
struct SMaskPupilTable
{
	static const int kBins = 64;
	static constexpr float kMaxRadius = 1.5f;   // past the mask's corners (sqrt 2): everything is inside
	string name;
	bool   valid = false;
	float  openArea[kBins + 1] = {};            // open area (mask units^2, radius 1 = bounding circle) within radius i / kBins * kMaxRadius
};
SMaskPupilTable s_maskPupil;

bool LoadMaskPupilTable(const char* szName)
{
	if (s_maskPupil.name == szName)
		return s_maskPupil.valid;
	s_maskPupil.name = szName;
	s_maskPupil.valid = false;

	std::vector<uint8> data;
	if (FILE* pFile = gEnv->pCryPak->FOpen(szName, "rb"))
	{
		gEnv->pCryPak->FSeek(pFile, 0, SEEK_END);
		const size_t nSize = (size_t)gEnv->pCryPak->FTell(pFile);
		gEnv->pCryPak->FSeek(pFile, 0, SEEK_SET);
		data.resize(nSize);
		if (nSize < 128 + 16 || gEnv->pCryPak->FReadRaw(data.data(), 1, nSize, pFile) != nSize)
			data.clear();
		gEnv->pCryPak->FClose(pFile);
	}
	if (data.empty() || memcmp(data.data(), "DDS ", 4) != 0)
		return false;

	const uint32* pHdr = (const uint32*)(data.data() + 4);
	const int h = (int)pHdr[2];
	const int w = (int)pHdr[3];
	if ((pHdr[19] & 0x40) == 0 || pHdr[20] != 0 || pHdr[21] != 32)
		return false;
	if (w < 4 || h < 4 || w > 512 || h > 512 || (w & (w - 1)) != 0 || (h & (h - 1)) != 0)
		return false;
	if (data.size() < 128 + (size_t)w * h * 4)
		return false;

	const uint8* pPix = data.data() + 128;
	const float texelArea = (2.0f / (float)w) * (2.0f / (float)h);
	float bins[SMaskPupilTable::kBins + 1] = {};
	for (int y = 0; y < h; y++)
	{
		const float fy = ((float)y + 0.5f) / (float)h * 2.0f - 1.0f;
		for (int x = 0; x < w; x++)
		{
			const float fx = ((float)x + 0.5f) / (float)w * 2.0f - 1.0f;
			const float r = sqrtf(fx * fx + fy * fy);
			const int bin = min((int)(r / SMaskPupilTable::kMaxRadius * SMaskPupilTable::kBins) + 1, SMaskPupilTable::kBins);
			bins[bin] += (float)pPix[((size_t)y * w + x) * 4 + 2] / 255.0f * texelArea;
		}
	}
	float acc = 0.0f;
	for (int i = 0; i <= SMaskPupilTable::kBins; i++)
	{
		acc += bins[i];
		s_maskPupil.openArea[i] = acc;
	}
	s_maskPupil.valid = true;
	CryLog("[CinematicCamera] mask pupil table from '%s' (%dx%d, open %.0f%% of the bounding circle)",
	       szName, w, h, acc / gf_PI * 100.0f);
	return true;
}
} // namespace

bool CCinematicCameraComponent::IsMaskDomain() const
{
	return m_lens.bUseSpriteBokeh
	       && !m_lens.bokehShapeTex.value.empty()
	       && (float)m_lens.aperture < (float)m_lens.maskAperture;
}

float CCinematicCameraComponent::GetGeometricAperture() const
{
	const bool bMaskInUse = m_lens.bUseSpriteBokeh && !m_lens.bokehShapeTex.value.empty();
	const float N = bMaskInUse ? max((float)m_lens.aperture, (float)m_lens.maskAperture) : (float)m_lens.aperture;
	return max(N, 0.7f);
}

float CCinematicCameraComponent::ComputeMaskTransmission() const
{
	if (!m_lens.bUseSpriteBokeh || m_lens.bokehShapeTex.value.empty())
		return 1.0f;

	const float N  = max((float)m_lens.aperture, 0.7f);
	const float Nm = max((float)m_lens.maskAperture, 0.7f);
	const float k  = Nm / N;                                   // iris radius in mask units
	const float tint = clamp_tpl((float)m_lens.maskOpenArea, 0.01f, 1.0f);

	if (LoadMaskPupilTable(m_lens.bokehShapeTex.value.c_str()))
	{
		// area(iris INTERSECT mask) / area(iris): 1 while the iris sits inside the mask's open
		// centre, then falling continuously as it opens out through the cut-out's edges.
		const float kc = min(k, SMaskPupilTable::kMaxRadius);
		const float fBin = kc / SMaskPupilTable::kMaxRadius * SMaskPupilTable::kBins;
		const int i = min((int)fBin, SMaskPupilTable::kBins - 1);
		const float t = fBin - (float)i;
		const float open = s_maskPupil.openArea[i] + (s_maskPupil.openArea[i + 1] - s_maskPupil.openArea[i]) * t;
		return clamp_tpl(open / (gf_PI * k * k) * tint, 0.01f, 1.0f);
	}

	// Fallback without the texture: the smaller hole's area ratio past the crossover.
	if (k <= 1.0f)
		return tint;
	return clamp_tpl(tint / (k * k), 0.01f, 1.0f);
}

// Thin-lens DOF bounds.
//   near = d * f² / (f² + N * c * (d - f))
//   far  = d * f² / (f² - N * c * (d - f))
void CCinematicCameraComponent::ComputeDOFBounds(float& outNear, float& outFar) const
{
	const float f_m = GetEffectiveFocalLength() * 0.001f;
	const float N   = GetGeometricAperture();
	const float d   = max((float)m_lens.focusDistance, f_m * 1.01f);
	const float c   = ComputeCoCLimit();
	const float f2  = f_m * f_m;
	const float nd  = f2 + N * c * (d - f_m);
	const float fd  = f2 - N * c * (d - f_m);

	outNear = (nd > 1e-6f) ? (d * f2 / nd) : 0.01f;
	outFar  = (fd > 1e-6f) ? (d * f2 / fd) : 1.0e6f;
	outNear = clamp_tpl(outNear, 0.01f, d - 0.001f);
	outFar  = max(outFar, d + 0.001f);
}

// Normalised blur: maxBokeh_mm = focalLength / aperture; reference = 85mm / f1.4 ≈ 60mm.
float CCinematicCameraComponent::ComputeMaxBlurAmount() const
{
	// Experimentally unclamped past 1.0 (engine side has no hard cap; the value scales the
	// CoC). Long lenses wide open now produce genuinely larger discs - watch for gather
	// undersampling (sparse/ringy discs) at the top end, the kernel is still 49 taps.
	return clamp_tpl(GetEffectiveFocalLength() / GetGeometricAperture() / 60.0f, 0.0f, 4.0f);
}

// What to feed Dof_User_FocusRange.
//
// The DOF shader has no sharp band: it computes
//     coc = saturate(|depth - focusDistance| / (focusRange * 0.5))^2 * blurAmount
// so blur starts rising the moment you leave the focus plane and is maximal at
// focusDistance +/- focusRange/2. Handing it the physical depth of field width therefore
// puts *full* blur exactly where the image should still be acceptably sharp, which is why
// the in-focus zone looked far too thin.
//
// Feeding it dofWidth * focusFalloff instead puts the physical DOF limits at
// 1/focusFalloff^2 of maximum blur (11% at the default 3), so the depth of field reads as
// sharp and the falloff beyond it stays smooth.
float CCinematicCameraComponent::ComputeFocusRange() const
{
	float dofNear, dofFar;
	ComputeDOFBounds(dofNear, dofFar);

	const float dofWidth = max(dofFar - dofNear, 0.001f);
	return clamp_tpl(dofWidth * (float)m_dof.focusFalloff, 0.02f, 20000.0f);
}

// ---------------------------------------------------------------------------
// DOF post-effect helpers
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::ApplyDOF()
{
	// Single writer: only the active cinematic camera may drive the shared render state.
	if (!CanDriveRenderState())
		return;

	// Ensure DOF is enabled.
	ICVar* pDofCVar = gEnv->pConsole->GetCVar("r_dof");
	if (pDofCVar && !m_bDofParamsSaved)
		m_savedDofCVar = pDofCVar->GetIVal();
	if (pDofCVar && pDofCVar->GetIVal() == 0)
		pDofCVar->Set(1);

	if (!m_bDofParamsSaved)
	{
		gEnv->p3DEngine->GetPostEffectParam("Dof_Active",             m_savedDofActive);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_Active",        m_savedDofUserActive);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_FocusDistance", m_savedDofFocusDist);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_FocusRange",    m_savedDofFocusRange);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_BlurAmount",    m_savedDofBlurAmount);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_BladeRotation", m_savedDofBladeRotation);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_BladeCurvature", m_savedDofBladeCurvature);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_MaskFNumber", m_savedDofMaskFNumber);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_MaskRotation", m_savedDofMaskRotation);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_HighlightThreshold", m_savedDofHighlightThreshold);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_HighlightGain", m_savedDofHighlightGain);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_ApertureBlades", m_savedDofApertureBlades);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_FNumber", m_savedDofFNumber);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_AnamorphicSqueeze", m_savedDofAnamorphicSqueeze);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_CatEye", m_savedDofCatEye);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_PupilScale", m_savedDofPupilScale);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_Coma", m_savedDofComa);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_Astigmatism", m_savedDofAstigmatism);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_FieldCurvature", m_savedDofFieldCurvature);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_EdgeSoftness", m_savedDofEdgeSoftness);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_AxialChromatic", m_savedDofAxialChromatic);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_LensModel", m_savedDofLensModel);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_LensFocal", m_savedDofLensFocal);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_SensorWidth", m_savedDofSensorWidth);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_SphericalAberration", m_savedDofSphericalAberration);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_GhostAmount", m_savedDofGhostAmount);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_GhostCount", m_savedDofGhostCount);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_GhostSpread", m_savedDofGhostSpread);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_GhostSize", m_savedDofGhostSize);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_GhostThreshold", m_savedDofGhostThreshold);
		gEnv->p3DEngine->GetPostEffectParam("Dof_User_GhostHueSpread", m_savedDofGhostHueSpread);
		gEnv->p3DEngine->GetPostEffectParamVec4("Dof_User_GhostTint", m_savedDofGhostTint);
		m_bDofParamsSaved = true;
	}

	gEnv->p3DEngine->SetPostEffectParam("Dof_Active",             0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_Active",        1.0f);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FocusDistance", (float)m_lens.focusDistance);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FocusRange",    ComputeFocusRange());
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_BlurAmount",    ComputeMaxBlurAmount());
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_ApertureBlades", (float)(int)m_lens.apertureBlades);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FNumber",        (float)m_lens.aperture);
	// The iris phase is one physical quantity: the same rotation that turns the diffraction
	// streaks has to turn the bokeh polygon, or the two visibly de-sync. The property still
	// lives in the Streaks group - moving it between groups would reset serialized values,
	// so that migration waits for the lens-preset pass.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_BladeRotation",  DEG2RAD((float)m_streaks.bladeRotation));
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_BladeCurvature", (float)m_lens.bladeCurvature);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_MaskFNumber",    (float)m_lens.maskAperture);
	// The mask is its own element: its rotation is independent of the iris phase above.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_MaskRotation",   DEG2RAD((float)m_lens.maskRotation));
	// Oval bokeh: the pupil image is squeezed horizontally by the same factor, so every
	// out-of-focus disc becomes a vertical oval of height : width = S : 1.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_AnamorphicSqueeze", clamp_tpl((float)m_anamorphic.squeeze, 1.0f, 2.0f));

	// Field-dependent pupil (cat-eye, coma, astigmatism). Everything field-related is authored
	// wide open and divided by this pupil scale on the renderer side, so stopping the iris down
	// from Maximum Aperture shrinks the pupil against the fixed barrel and the effects fade out
	// by themselves - no separate aperture link.
	const float pupilScale = clamp_tpl((float)m_lens.maxAperture / GetGeometricAperture(), 0.05f, 1.0f);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_CatEye",      (float)m_lens.catEye);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_PupilScale",  pupilScale);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_Coma",        (float)m_lens.coma);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_Astigmatism", (float)m_lens.astigmatism);

	// Lens flaws that are focus errors rather than pupil shapes. They ride the same field
	// vector, so they are pushed here and resolved by the same stage; the renderer folds the
	// pupil scale into edge softness, which is why neither needs an aperture link of its own.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FieldCurvature", (float)m_lens.fieldCurvature);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_EdgeSoftness",   (float)m_lens.edgeSoftness);

	// Axial chromatic aberration. A depth of field parameter rather than a lens character cvar,
	// unlike its lateral namesake: it changes the SIZE of the out-of-focus disc per channel, so
	// it has to reach the gather and the splat, not the final composite.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_AxialChromatic", (float)m_lens.axialChromatic);

	// Lens model (DofLensModelSpec.md). The renderer builds the circle of confusion from the
	// thin lens itself, so it needs the two numbers the stock focus-range path never carried -
	// the effective focal length (breathing and the servo zoom already folded in) and the
	// sensor width that maps millimetres on the sensor onto pixels in the frame. The f-number
	// and the focus distance it already has. Focus Falloff, Focus Range and Blur Amount keep
	// being sent unchanged so that switching this off lands back on exactly the stock path.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_LensModel",  m_dof.bLensModelDof ? 1.0f : 0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_LensFocal",  GetEffectiveFocalLength());
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_SensorWidth", GetSensorWidth());
	// Spherical aberration is a property of the glass and reaches the sprite splats and the
	// ghosts in both modes; only the gather's own halo needs the lens-model permutation for it.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_SphericalAberration", (float)m_lens.sphericalAberration);

	// Hybrid highlight splat tuning. Harmless in pure gather mode - nothing reads them there.
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_HighlightThreshold", (float)m_dof.highlightThreshold);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_HighlightGain",      (float)m_dof.highlightSplatGain);

	// Internal reflection ghosts. Depth of field parameters because the renderer draws them
	// inside that stage - a ghost is an image of the aperture, so it re-uses the bokeh splat and
	// everything shaping it - even though nothing about them follows focus. Amount 0 costs
	// nothing: the stage skips the whole pass. Unlike the splat they are NOT gated on the hybrid
	// mode, so ghosts still draw with r_DepthOfFieldMode 1.
	const ColorF ghostTint = m_ghosts.tint;
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostAmount",    (float)m_ghosts.amount);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostCount",     (float)(int)m_ghosts.count);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostSpread",    (float)m_ghosts.spread);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostSize",      (float)m_ghosts.size);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostThreshold", (float)m_ghosts.threshold);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostHueSpread", (float)m_ghosts.hueSpread);
	gEnv->p3DEngine->SetPostEffectParamVec4("Dof_User_GhostTint", Vec4(ghostTint.r, ghostTint.g, ghostTint.b, 0.0f));
}

void CCinematicCameraComponent::RestoreDOF()
{
	if (!m_bDofParamsSaved)
		return;

	gEnv->p3DEngine->SetPostEffectParam("Dof_User_Active",        m_savedDofUserActive);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FocusDistance", m_savedDofFocusDist);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FocusRange",    m_savedDofFocusRange);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_BlurAmount",    m_savedDofBlurAmount);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_BladeRotation", m_savedDofBladeRotation);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_BladeCurvature", m_savedDofBladeCurvature);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_MaskFNumber", m_savedDofMaskFNumber);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_MaskRotation", m_savedDofMaskRotation);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_HighlightThreshold", m_savedDofHighlightThreshold);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_HighlightGain", m_savedDofHighlightGain);
	// Also put back the three params ApplyDOF used to push without ever restoring - notably
	// Dof_Active, which was forced to 0 every frame and killed the scene's own
	// (TrackView / flowgraph) depth of field for the rest of the session.
	gEnv->p3DEngine->SetPostEffectParam("Dof_Active",              m_savedDofActive);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_ApertureBlades", m_savedDofApertureBlades);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FNumber",        m_savedDofFNumber);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_AnamorphicSqueeze", m_savedDofAnamorphicSqueeze);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_CatEye", m_savedDofCatEye);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_PupilScale", m_savedDofPupilScale);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_Coma", m_savedDofComa);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_Astigmatism", m_savedDofAstigmatism);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_FieldCurvature", m_savedDofFieldCurvature);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_EdgeSoftness", m_savedDofEdgeSoftness);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_AxialChromatic", m_savedDofAxialChromatic);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_LensModel", m_savedDofLensModel);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_LensFocal", m_savedDofLensFocal);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_SensorWidth", m_savedDofSensorWidth);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_SphericalAberration", m_savedDofSphericalAberration);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostAmount", m_savedDofGhostAmount);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostCount", m_savedDofGhostCount);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostSpread", m_savedDofGhostSpread);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostSize", m_savedDofGhostSize);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostThreshold", m_savedDofGhostThreshold);
	gEnv->p3DEngine->SetPostEffectParam("Dof_User_GhostHueSpread", m_savedDofGhostHueSpread);
	gEnv->p3DEngine->SetPostEffectParamVec4("Dof_User_GhostTint", m_savedDofGhostTint);

	ICVar* pDofCVar = gEnv->pConsole->GetCVar("r_dof");
	if (pDofCVar)
		pDofCVar->Set(m_savedDofCVar);

	m_bDofParamsSaved = false;
}

void CCinematicCameraComponent::RestoreExposure()
{
	if (!m_bExpParamsSaved)
		return;

	gEnv->p3DEngine->SetGlobalParameter(E3DPARAM_HDR_EYEADAPTATION_PARAMS, m_savedEyeAdaptation);
	m_bExpParamsSaved = false;
}

// ---------------------------------------------------------------------------
// Film grain — sensor noise model, perceptual rather than radiometric.
//
// gain:  stops above ISO 100, normalised over the 8 stop range to ISO 25600 and raised
//        to 1.7, so base ISO stays clean and the top end climbs steeply the way small
//        sensor SNR curves do.
// light: photons per exposure ~ t / N² (shutter open time times aperture area).
//        Reference is f/5.6 at 1/50s. Starving the sensor raises grain at equal gain,
//        flooding it lowers grain; ±0.35 per stop, clamped to [0.6, 2.2].
// ---------------------------------------------------------------------------
float CCinematicCameraComponent::ComputeFilmGrain01() const
{
	const float iso = (float)max((int)m_body.iso, 25);

	// Early onset plus an explosive top end: a gentle sub-linear term makes grain readable
	// from ISO 400 up (small sensor / pushed film stock behaviour rather than a clean modern
	// full frame), and the cubic term models the SNR collapse once amplified read noise
	// dominates - the top two stops get dramatically dirty.
	// Sensor size sets the baseline: SNR scales with the sensor's linear size, so a smaller
	// sensor behaves like a bigger one pushed by log2(36/width) stops. Full frame is the
	// reference (+0), APS-C ≈ +0.6, Micro Four Thirds ≈ +1.1, and Custom follows its width.
	const float sensorStops = log2f(36.0f / max(GetSensorWidth(), 4.0f));
	const float gainStops = log2f(iso / 100.0f) + sensorStops;
	const float g = clamp_tpl(gainStops / 8.0f, 0.0f, 1.0f);
	const float base = 0.25f * powf(g, 1.2f) + powf(g, 3.5f);

	// The mask past its crossover eats light exactly like glass losses do - through
	// ComputeApertureShutterStops(), the same term the exposure uses, so grain and exposure
	// cannot drift apart. The reference is f/5.6 at 1/50 s, i.e. log2(5.6^2 * 50) stops.
	const float referenceStops = log2f(5.6f * 5.6f * 50.0f);
	const float lightStops = referenceStops - ComputeApertureShutterStops() - (float)(int)m_lens.ndFilterStops;
	const float lowLightBoost = clamp_tpl(powf(2.0f, -lightStops * 0.35f), 0.6f, 2.2f);

	return clamp_tpl(base * lowLightBoost, 0.0f, 1.5f);
}

// FilterGrain_Amount is combined with the environment (TOD) grain by max() in PostAA,
// so writing it every frame from the finalize hook cannot be stomped and needs no
// ordering tricks. Save once / restore on release, mirroring the DOF params.
void CCinematicCameraComponent::ApplyFilmGrain()
{
	if (!CanDriveRenderState())
		return;

	if (!m_grain.bEnableGrain)
	{
		RestoreFilmGrain();
		return;
	}

	if (!m_bGrainParamSaved)
	{
		gEnv->p3DEngine->GetPostEffectParam("FilterGrain_Amount", m_savedGrainAmount);
		m_bGrainParamSaved = true;
	}

	gEnv->p3DEngine->SetPostEffectParam("FilterGrain_Amount", ComputeFilmGrain01() * (float)m_grain.grainStrength);
}

// The renderer's motion blur exposure window is 1 / r_MotionBlurShutterSpeed, so pointing
// that cvar at the camera shutter closes the physics gap where shutter affected brightness
// but not blur. 1/50 at 25 fps is the classic 180 degree look.
void CCinematicCameraComponent::ApplyMotionBlur()
{
	if (!CanDriveRenderState())
		return;

	if (!m_motionBlur.bEnableMotionBlur)
	{
		RestoreMotionBlur();
		return;
	}

	ICVar* pShutter = gEnv->pConsole->GetCVar("r_MotionBlurShutterSpeed");
	ICVar* pMode = gEnv->pConsole->GetCVar("r_MotionBlur");
	if (pShutter == nullptr || pMode == nullptr)
		return;

	if (!m_bMBParamsSaved)
	{
		m_savedMBShutter = pShutter->GetFVal();
		m_savedMBMode = pMode->GetIVal();
		m_bMBParamsSaved = true;
	}

	if (pMode->GetIVal() == 0)
		pMode->Set(2);

	const float shutter = (float)max((int)m_body.shutterDenominator, 1);
	if (fabs_tpl(pShutter->GetFVal() - shutter) > 0.01f)
		pShutter->Set(shutter);
}

// Blackbody colour via the Tanner-Helland fit (input Kelvin, output linear-ish 0..1 RGB).
static Vec3 KelvinToRGB(float kelvin)
{
	const float t = clamp_tpl(kelvin, 1000.0f, 15000.0f) / 100.0f;
	float r, g, b;
	if (t <= 66.0f)
	{
		r = 255.0f;
		g = 99.4708025861f * logf(t) - 161.1195681661f;
	}
	else
	{
		r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
		g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
	}
	if (t >= 66.0f)
		b = 255.0f;
	else if (t <= 19.0f)
		b = 0.0f;
	else
		b = 138.5177312231f * logf(t - 10.0f) - 305.0447927307f;

	return Vec3(clamp_tpl(r, 0.0f, 255.0f), clamp_tpl(g, 0.0f, 255.0f), clamp_tpl(b, 0.0f, 255.0f)) / 255.0f;
}

// Camera-referred white balance: gains that make light of the SET temperature render white,
// so dialing 3200K under (engine-default) daylight reads blue, 8000K reads warm - exactly a
// stills camera's behaviour. Luminance-normalised so exposure is unaffected; multiplied on
// top of the scene's grading balance rather than replacing it.
void CCinematicCameraComponent::ApplyWhiteBalance()
{
	if (!CanDriveRenderState())
		return;

	// On the scene-referred path white balance is a Bradford adaptation in the working space,
	// applied by the display chain from the Kelvin and tint this component publishes
	// (ApplyGrade). The channel-gain version below writes E3DPARAM_HDR_COLORGRADING_COLOR_BALANCE,
	// which FilmMapping's scene-referred branch does not read - so it would be inert anyway, and
	// leaving it writing a global the stock time of day also owns is noise nobody can trace.
	// Hand the value back and let the real one do the work.
	if (m_sceneReferred.bSceneReferred)
	{
		RestoreWhiteBalance();
		return;
	}

	if (!m_whiteBalance.bEnableWhiteBalance || (int)m_whiteBalance.colorTemperature == 6500)
	{
		RestoreWhiteBalance();
		return;
	}

	Vec3 current;
	gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_HDR_COLORGRADING_COLOR_BALANCE, current);

	if (!m_bWBParamsSaved)
	{
		m_savedColorBalance = current;
		m_baseColorBalance = current;
		m_bWBParamsSaved = true;
	}
	else if (!current.IsEquivalent(m_lastWrittenBalance, 1e-5f))
	{
		// Someone else (TOD curve) refreshed the grading - adopt it as the new base.
		m_baseColorBalance = current;
	}

	const Vec3 reference = KelvinToRGB(6500.0f);
	const Vec3 target = KelvinToRGB((float)(int)m_whiteBalance.colorTemperature);
	Vec3 gains(reference.x / max(target.x, 0.01f), reference.y / max(target.y, 0.01f), reference.z / max(target.z, 0.01f));
	const float luminance = gains.x * 0.2126f + gains.y * 0.7152f + gains.z * 0.0722f;
	gains /= max(luminance, 0.01f);

	const Vec3 out(m_baseColorBalance.x * gains.x, m_baseColorBalance.y * gains.y, m_baseColorBalance.z * gains.z);
	gEnv->p3DEngine->SetGlobalParameter(E3DPARAM_HDR_COLORGRADING_COLOR_BALANCE, out);
	m_lastWrittenBalance = out;

	// Diagnostic: one log line per temperature change so an inverted-feel report can be
	// checked against the actually written gains.
	static int s_lastLoggedKelvin = -1;
	if ((int)m_whiteBalance.colorTemperature != s_lastLoggedKelvin)
	{
		s_lastLoggedKelvin = (int)m_whiteBalance.colorTemperature;
		CryLog("[CinematicCamera] WB %dK gains R%.2f G%.2f B%.2f base R%.2f G%.2f B%.2f out R%.2f G%.2f B%.2f",
		       s_lastLoggedKelvin, gains.x, gains.y, gains.z,
		       m_baseColorBalance.x, m_baseColorBalance.y, m_baseColorBalance.z, out.x, out.y, out.z);
	}
}

void CCinematicCameraComponent::RestoreWhiteBalance()
{
	if (!m_bWBParamsSaved)
		return;

	gEnv->p3DEngine->SetGlobalParameter(E3DPARAM_HDR_COLORGRADING_COLOR_BALANCE, m_savedColorBalance);
	m_bWBParamsSaved = false;
}

// Vignette and distortion, the per-lens "character" pair. Both live behind cvars so the
// console can audition values live; the couplings are explicit tunable properties so a
// lens preset can dial in anything from corrected modern glass to a swirly vintage zoom.
void CCinematicCameraComponent::ApplyLensCharacter()
{
	// Editor-live (view-independent character), but still only from the active component -
	// otherwise any selected cinematic camera, preview windows included, would write these
	// global cvars and latch a baseline.
	if (!CanDriveRenderState())
		return;

	ICVar* pVignette = gEnv->pConsole->GetCVar("r_HDRVignetteAmount");
	ICVar* pDistortion = gEnv->pConsole->GetCVar("r_LensDistortion");
	ICVar* pChromatic = gEnv->pConsole->GetCVar("r_LensChromaticAberration");
	ICVar* pLetterbox = gEnv->pConsole->GetCVar("r_LensLetterbox");
	if (pVignette == nullptr || pDistortion == nullptr)
		return;

	if (!m_bLensCharSaved)
	{
		m_savedVignetteAmount = pVignette->GetFVal();
		m_savedLensDistortion = pDistortion->GetFVal();
		m_savedLensChromatic = pChromatic ? pChromatic->GetFVal() : 0.0f;
		m_savedLetterbox = pLetterbox ? pLetterbox->GetFVal() : 0.0f;
		m_bLensCharSaved = true;
	}

	// Mechanical vignetting: strongest wide open, faded out by f/5.6.
	const float N = max((float)m_lens.aperture, 0.7f);
	const float wideOpenness = clamp_tpl((5.6f - N) / (5.6f - 1.4f), 0.0f, 1.0f);
	const float vignette = (float)m_lens.vignetteAmount *
	                       (1.0f + (float)m_lens.vignetteApertureLink * (wideOpenness - 1.0f) * 0.85f);

	// Zoom characteristic: barrel at short focals, pincushion at long ones, pivoting at 35mm.
	const float focalTerm = log2f(35.0f / max(GetEffectiveFocalLength(), 4.0f));
	const float distortion = clamp_tpl((float)m_lens.distortion + (float)m_lens.distortionFocalLink * focalTerm * 0.5f, -1.0f, 1.0f) * 0.35f;

	if (fabs_tpl(pVignette->GetFVal() - vignette) > 0.001f)
		pVignette->Set(vignette);
	if (fabs_tpl(pDistortion->GetFVal() - distortion) > 0.001f)
		pDistortion->Set(distortion);

	// Lateral chromatic aberration. It belongs here rather than with the depth of field values
	// because it is distortion done per wavelength: same coordinate chain as the radial
	// distortion above, so it composes with it and with the anamorphic squeeze for free.
	if (pChromatic)
	{
		const float chromatic = clamp_tpl((float)m_lens.chromaticAberration, 0.0f, 1.0f);
		if (fabs_tpl(pChromatic->GetFVal() - chromatic) > 0.001f)
			pChromatic->Set(chromatic);
	}

	// Anamorphic gate mask: bars only when the desqueezed format is wider than the viewport.
	if (pLetterbox)
	{
		const float letterbox = ComputeLetterboxAspect();
		if (fabs_tpl(pLetterbox->GetFVal() - letterbox) > 0.001f)
			pLetterbox->Set(letterbox);
	}
}

void CCinematicCameraComponent::RestoreLensCharacter()
{
	if (!m_bLensCharSaved)
		return;

	if (ICVar* pVignette = gEnv->pConsole->GetCVar("r_HDRVignetteAmount"))
		pVignette->Set(m_savedVignetteAmount);
	if (ICVar* pDistortion = gEnv->pConsole->GetCVar("r_LensDistortion"))
		pDistortion->Set(m_savedLensDistortion);
	if (ICVar* pChromatic = gEnv->pConsole->GetCVar("r_LensChromaticAberration"))
		pChromatic->Set(m_savedLensChromatic);
	if (ICVar* pLetterbox = gEnv->pConsole->GetCVar("r_LensLetterbox"))
		pLetterbox->Set(m_savedLetterbox);
	m_bLensCharSaved = false;
}

// ---------------------------------------------------------------------------
// The display chain's two .cube files (SceneReferredSpec.md D8,
// decisions/s4-display-transform.md section 2A).
//
// The plugin owns the LUTs and the renderer only samples them. That split is forced by the
// public surface: IRenderer can create and refresh a 3D texture from CPU data
// (UploadToVideoMemory3D / UpdateTextureInVideoMemory, the same pair Cry3DEngine's SVO brick
// pools use from outside the renderer), but nothing in IRenderer can dispatch compute or bind a
// texture to a pass. So ccam parses, converts and uploads; it publishes the resulting texture
// IDs on the post-effect bus; the tone map resolves them with CTexture::GetByID, exactly as the
// colour grading stage already does for the identity chart.
//
// The file dialect is Resolve's: an ASCII comment block, TITLE, LUT_3D_SIZE, optional
// DOMAIN_MIN / DOMAIN_MAX, then N^3 triplets with the RED index varying fastest. No embedded 1D
// shaper - the linear -> ACEScct encode is analytic in the shader - so the parser stays a
// tokenizer and nothing here has to know what ACES is.
// ---------------------------------------------------------------------------
namespace
{

struct SCubeLut
{
	int                size = 0;
	Vec3               domainMin = Vec3(0.0f, 0.0f, 0.0f);
	Vec3               domainMax = Vec3(1.0f, 1.0f, 1.0f);
	std::vector<float> rgb;      // size^3 * 3, red fastest

	//! `# ReC-LUT-Space: <value>` out of the comment block, if the file carried one and it named
	//! a space we know (S10 item 4.1). The file's own answer beats the property's.
	bool               bTagged = false;
	ELutSpace          tagSpace = ELutSpace::ACEScct;
	//! The tag text exactly as written, so a warning about an unrecognised one can quote it.
	string             tagText;
};

//! The tag's accepted spellings, case-insensitive so a hand-edited file is not a trap.
//!
//! Deliberately NOT accepted: ACES2065-1 (needs the AP0<->AP1 pair and a shaper decision of its
//! own, and nobody authors a look LUT in AP0), and the camera log spaces (LogC, SLog3, Log3G10...)
//! - naming them would promise a conversion this plugin does not have. An unrecognised tag is a
//! warning and a fall-through to the property, not a refusal: a file that loads with a diagnosis
//! beats a file that does not load.
bool ParseLutSpaceName(const char* szValue, ELutSpace& out)
{
	struct SAlias { const char* szName; ELutSpace space; };
	static const SAlias kAliases[] =
	{
		{ "acescct",       ELutSpace::ACEScct       },
		{ "acescg",        ELutSpace::ACEScg        },
		{ "linear",        ELutSpace::ACEScg        },
		{ "rec709",        ELutSpace::Rec709Display },
		{ "rec.709",       ELutSpace::Rec709Display },
		{ "rec709display", ELutSpace::Rec709Display },
		{ "srgb",          ELutSpace::Rec709Display },
		{ "srgbdisplay",   ELutSpace::Rec709Display },
		{ "display",       ELutSpace::Rec709Display },
	};

	for (const SAlias& alias : kAliases)
	{
		if (stricmp(szValue, alias.szName) == 0)
		{
			out = alias.space;
			return true;
		}
	}
	return false;
}

const char* LutSpaceName(ELutSpace space)
{
	switch (space)
	{
	case ELutSpace::ACEScg:        return "ACEScg linear";
	case ELutSpace::Rec709Display: return "Rec.709 display";
	default:                       return "ACEScct";
	}
}

// ICryPak, first as given (relative to the project's asset directory), then under %ENGINE%.
// The plugin's Assets folder can be installed into either tree - a project keeps its looks with
// its levels, an engine-wide install makes the shipped ODTs available to every project - and a
// path written in the editor has to keep working in the launcher whichever one was used.
string ResolveLutPath(const char* szPath)
{
	CryPathString adjusted;
	// No flags: the default behaviour is exactly what a diagnostic wants - alias expansion
	// (%ENGINE%), mapping into the asset root, and a relative path made absolute.
	gEnv->pCryPak->AdjustFileName(szPath, adjusted, 0);
	return string(adjusted.c_str());
}

// The one compatibility shim item 4b owes to every level saved before the folder move: a path
// under the old Assets/ODT that no longer resolves is retried under the two folders its files went
// to. It only ever runs AFTER both real lookups have failed, so a project that kept its own copy
// of the old folder is untouched, and it costs nothing on the normal path.
bool RemapLegacyLutPath(const char* szPath, string& out)
{
	const size_t nLegacy = strlen(CINECAM_FOLDER_LEGACY "/");
	if (strnicmp(szPath, CINECAM_FOLDER_LEGACY "/", nLegacy) != 0)
		return false;

	const char* szLeaf = szPath + nLegacy;
	// The inverse output transform is the only file that went to the system subfolder; everything
	// else in the old folder is now a plain LUT asset.
	string leafLower = szLeaf;
	leafLower.MakeLower();
	const bool bSystem = (leafLower.find("_inv_") != string::npos);
	out = string(bSystem ? CINECAM_FOLDER_SYSTEM : CINECAM_FOLDER_LUTS) + "/" + szLeaf;
	return true;
}

FILE* OpenLutFile(const char* szPath, string& usedPath, string& triedPaths)
{
	if (!szPath || !szPath[0] || !gEnv->pCryPak)
		return nullptr;

	usedPath = szPath;
	triedPaths = ResolveLutPath(usedPath.c_str());
	if (FILE* pFile = gEnv->pCryPak->FOpen(usedPath.c_str(), "rb"))
		return pFile;

	usedPath = string("%ENGINE%/") + szPath;
	triedPaths += string(" | ") + ResolveLutPath(usedPath.c_str());
	if (FILE* pFile = gEnv->pCryPak->FOpen(usedPath.c_str(), "rb"))
		return pFile;

	string remapped;
	if (RemapLegacyLutPath(szPath, remapped))
	{
		usedPath = remapped;
		triedPaths += string(" | ") + ResolveLutPath(usedPath.c_str());
		if (FILE* pFile = gEnv->pCryPak->FOpen(usedPath.c_str(), "rb"))
		{
			CryLog("[cinecam] LUT '%s' was found at '%s' - S10 item 4b moved the shipped LUTs out "
			       "of " CINECAM_FOLDER_LEGACY ". Re-pick the file to store the new path.",
			       szPath, usedPath.c_str());
			return pFile;
		}

		usedPath = string("%ENGINE%/") + remapped;
		triedPaths += string(" | ") + ResolveLutPath(usedPath.c_str());
		if (FILE* pFile = gEnv->pCryPak->FOpen(usedPath.c_str(), "rb"))
		{
			CryLog("[cinecam] LUT '%s' was found at '%s' - S10 item 4b moved the shipped LUTs out "
			       "of " CINECAM_FOLDER_LEGACY ". Re-pick the file to store the new path.",
			       szPath, usedPath.c_str());
			return pFile;
		}
	}

	usedPath.clear();
	return nullptr;
}

bool ParseCubeFile(const char* szPath, SCubeLut& out, string& error, string& resolvedPath)
{
	string usedPath, triedPaths;
	FILE* pFile = OpenLutFile(szPath, usedPath, triedPaths);
	if (!pFile)
	{
		error.Format("file not found - tried %s", triedPaths.c_str());
		return false;
	}
	resolvedPath = ResolveLutPath(usedPath.c_str());

	gEnv->pCryPak->FSeek(pFile, 0, SEEK_END);
	const size_t nSize = (size_t)gEnv->pCryPak->FTell(pFile);
	gEnv->pCryPak->FSeek(pFile, 0, SEEK_SET);

	std::vector<char> text;
	text.resize(nSize + 1);
	const size_t nRead = (nSize > 0) ? gEnv->pCryPak->FReadRaw(text.data(), 1, nSize, pFile) : 0;
	gEnv->pCryPak->FClose(pFile);

	if (nSize == 0 || nRead != nSize)
	{
		error = "unreadable or empty";
		return false;
	}
	text[nSize] = '\0';

	int nExpectedEntries = 0;

	// Line oriented: the format has no continuations, so a stray token on its own line is a
	// malformed file rather than something to recover from.
	char* pCursor = text.data();
	char* const pEnd = text.data() + nSize;
	while (pCursor < pEnd)
	{
		char* pLine = pCursor;
		while (pCursor < pEnd && *pCursor != '\n' && *pCursor != '\r')
			++pCursor;
		while (pCursor < pEnd && (*pCursor == '\n' || *pCursor == '\r'))
			*pCursor++ = '\0';
		*pEnd = '\0';

		while (*pLine == ' ' || *pLine == '\t')
			++pLine;
		if (*pLine == '\0')
			continue;

		// The ONE comment this parser reads. `.cube` has no metadata field at all, so the input
		// space has to travel as a comment or not at all (decisions/s10-grade-component.md 4.1);
		// every other tool ignores it, because it is a comment.
		if (*pLine == '#')
		{
			const char* pBody = pLine + 1;
			while (*pBody == ' ' || *pBody == '\t')
				++pBody;
			static const char kTagKey[] = "ReC-LUT-Space:";
			if (strnicmp(pBody, kTagKey, sizeof(kTagKey) - 1) == 0)
			{
				const char* pValue = pBody + sizeof(kTagKey) - 1;
				while (*pValue == ' ' || *pValue == '\t')
					++pValue;
				out.tagText = pValue;
				out.tagText.Trim();
				out.bTagged = ParseLutSpaceName(out.tagText.c_str(), out.tagSpace);
			}
			continue;
		}

		if (!strnicmp(pLine, "TITLE", 5))
			continue;

		if (!strnicmp(pLine, "LUT_1D_SIZE", 11))
		{
			error = "LUT_1D_SIZE - a 1D curve is not a display transform, this slot wants a 3D cube";
			return false;
		}

		if (!strnicmp(pLine, "LUT_3D_SIZE", 11))
		{
			out.size = atoi(pLine + 11);
			// 2 is the smallest grid that interpolates at all; 129^3 fp16 RGBA is already 34 MiB,
			// far past anything a display transform needs, and a good place to refuse.
			if (out.size < 2 || out.size > 129)
			{
				error.Format("LUT_3D_SIZE %d out of range (2..129)", out.size);
				return false;
			}
			nExpectedEntries = out.size * out.size * out.size;
			out.rgb.reserve((size_t)nExpectedEntries * 3);
			continue;
		}

		if (!strnicmp(pLine, "DOMAIN_MIN", 10))
		{
			sscanf(pLine + 10, "%f %f %f", &out.domainMin.x, &out.domainMin.y, &out.domainMin.z);
			continue;
		}

		if (!strnicmp(pLine, "DOMAIN_MAX", 10))
		{
			sscanf(pLine + 10, "%f %f %f", &out.domainMax.x, &out.domainMax.y, &out.domainMax.z);
			continue;
		}

		float r = 0.0f, g = 0.0f, b = 0.0f;
		if (sscanf(pLine, "%f %f %f", &r, &g, &b) != 3)
		{
			error.Format("unparsable line '%.32s'", pLine);
			return false;
		}
		if (out.size == 0)
		{
			error = "sample data before LUT_3D_SIZE";
			return false;
		}
		out.rgb.push_back(r);
		out.rgb.push_back(g);
		out.rgb.push_back(b);
	}

	if (out.size == 0)
	{
		error = "no LUT_3D_SIZE";
		return false;
	}
	if ((int)(out.rgb.size() / 3) != nExpectedEntries)
	{
		error.Format("%d samples for a %d^3 grid (expected %d)", (int)(out.rgb.size() / 3), out.size, nExpectedEntries);
		return false;
	}

	// DOMAIN_MIN/MAX describe the INPUT range the grid covers, and the shader's input is ACEScct
	// clamped to [0,1] by construction (the shaper is analytic - decisions/s4-display-transform.md
	// section 1A). Rather than carry a per-file input rescale into a per-pixel path, a cube that
	// declares another domain is refused with a message that says why: it is not a display
	// transform for this chain.
	const bool bUnitDomain =
		fabsf(out.domainMin.x) < 1e-6f && fabsf(out.domainMin.y) < 1e-6f && fabsf(out.domainMin.z) < 1e-6f &&
		fabsf(out.domainMax.x - 1.0f) < 1e-6f && fabsf(out.domainMax.y - 1.0f) < 1e-6f && fabsf(out.domainMax.z - 1.0f) < 1e-6f;
	if (!bUnitDomain)
	{
		error.Format("DOMAIN [%g %g %g]..[%g %g %g] - this chain feeds the LUT ACEScct in [0,1]; re-bake the cube on a unit domain",
		             out.domainMin.x, out.domainMin.y, out.domainMin.z, out.domainMax.x, out.domainMax.y, out.domainMax.z);
		return false;
	}

	return true;
}

// One slot per role. The texture survives a path change when the grid size is unchanged (the
// upload is then an in-place refresh of the same ID) and a parse failure leaves the slot EMPTY
// rather than stale: a picture that is visibly wrong beats one that is quietly wrong (D13).
struct SLutSlot
{
	string    requested;          //!< the path this slot was last asked for, failures included
	ELutSpace requestedSpace = ELutSpace::ACEScct; //!< the fallback space it was last asked with
	string    cacheName;          //!< the renderer-side name the texture was uploaded under
	int       texId  = 0;
	int       size   = 0;
	bool      loaded = false;

	// --- hot reload (S10 item 4.3) ------------------------------------------------------------
	// The file's identity as of the last successful load, and the one seen by the last poll. A
	// reload fires only when a CHANGED pair has been seen TWICE: Resolve writing a 65^3 cube
	// writes ~2.7 MB and a poll can land in the middle of it, so the second look is what makes
	// "the file stopped changing" the trigger rather than "the file is being written".
	uint64    fileTime = 0;
	uint64    fileSize = 0;
	uint64    pendingTime = 0;
	uint64    pendingSize = 0;
	bool      bPending = false;
};

SLutSlot s_lutODT;
SLutSlot s_lutLMT;

//! The forward and inverse halves of the output transform a Rec.709 look LUT was authored
//! against, held on the CPU and never uploaded. Fixed files, not the camera's own ODT: which
//! monitor a foreign look was made on is a property of the LOOK, so pointing the camera at a
//! different output transform must not change what that look means
//! (decisions/s10-grade-component.md 4.2).
//! The inverse lives under luts/system because nobody ever picks it: it is an input to the wrap,
//! and a per-type asset picker cannot hide one file (decisions/s10-grade-component.md 4b.3).
constexpr const char* kWrapForwardODTFile = CINECAM_FOLDER_LUTS "/odt_srgb_100nit_aces2_65.cube";
constexpr const char* kWrapInverseODTFile = CINECAM_FOLDER_SYSTEM "/odt_srgb_100nit_aces2_inv_65.cube";

//! The grid a wrapped LUT is resampled onto. See the note at the call site: it is the OUTPUT
//! TRANSFORM's curvature that sets this, not the look's.
constexpr int kWrapGridSize = 65;

struct SWrapTransforms
{
	SCubeLut forward;   //!< ACEScct -> sRGB display code values
	SCubeLut inverse;   //!< sRGB display code values -> ACEScct
	bool     bTried = false;
	bool     bReady = false;
};

SWrapTransforms s_wrap;

//! cinecam_LutHotReload: poll interval in frames. 0 = off. Default 30 in the editor (author in
//! Resolve on one monitor, watch the picture move on the other) and 0 in the launcher (a shipped
//! build's LUTs do not change, and a stat with no user is a cost with no user).
int s_lutHotReloadFrames = 0;
ICVar* s_pLutHotReloadCVar = nullptr;

//! The two 1D curves (S10 item 3b), as ONE N x 1 fp16 texture: R the master (luma) curve, G the
//! Sat vs Sat multiplier.
//!
//! Two curves in two CHANNELS rather than two rows because a two-row texture has to be sampled at
//! the row centres exactly or a linear sampler bleeds one curve into the other - and it would do
//! so invisibly. One row, v = 0.5, no hazard. RGBA16F rather than R16G16F because the upload path
//! does no format conversion and this is the format the two 3D LUTs already prove through this
//! same call; the difference is 4 KiB.
struct SCurveLutSlot
{
	string cacheName;
	int    texId = 0;
	int    size = 0;
	//! What the current texture was baked from, so that the common path is ten float compares.
	float  masterStops[SceneReferredCurves::kKnotCount] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float  satMult[SceneReferredCurves::kKnotCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	bool   bMasterActive = false;
	bool   bSatActive = false;
};

SCurveLutSlot s_lutCurve;

// IRenderer::RemoveTexture(id) is GetByID(id) followed by Release() - it trusts the id. Ours
// can go stale without us being told: a level change frees the renderer's resources BEFORE
// the entities are destroyed (editor.log: "*** Start clearing render resources ***" precedes
// "Ending game context"), so by the time OnShutDown gets here the id may already belong to
// somebody else's texture, and releasing it would drop a stranger's reference and leave a
// dangling CTexture for the render thread to trip over later.
//
// The name is the check: every upload here passes a fixed cache name ($ccamODT / $ccamLMT /
// $ccamCurve), so a texture still carrying it at that id is ours and nothing else can be.
void ReleaseOwnTexture(int& texId, string& cacheName)
{
	if (texId > 0 && gEnv && gEnv->pRenderer)
	{
		ITexture* const pTex = gEnv->pRenderer->EF_GetTextureByID(texId);
		const char* const szName = pTex ? pTex->GetName() : nullptr;
		if (szName != nullptr && !cacheName.empty() && cacheName == szName)
			gEnv->pRenderer->RemoveTexture((unsigned int)texId);
	}
	texId = 0;
	cacheName.clear();
}

void ClearLutSlot(SLutSlot& slot)
{
	ReleaseOwnTexture(slot.texId, slot.cacheName);
	slot.size = 0;
	slot.loaded = false;
	slot.cacheName.clear();
}

//! The file's identity, for the hot-reload poll. Goes through the SAME two-tree search the load
//! used (OpenLutFile), so the watcher cannot end up watching a different file from the one that
//! is in the picture. A cube inside a .pak returns the archive entry's time and simply never
//! changes, which is the right answer for a paked asset.
bool StatLutFile(const char* szPath, uint64& outTime, uint64& outSize)
{
	string usedPath, triedPaths;
	FILE* const pFile = OpenLutFile(szPath, usedPath, triedPaths);
	if (!pFile)
		return false;

	outTime = (uint64)gEnv->pCryPak->GetModificationTime(pFile);
	outSize = (uint64)gEnv->pCryPak->FGetSize(pFile);
	gEnv->pCryPak->FClose(pFile);
	return true;
}

//! Trilinear fetch of a parsed cube, on the CPU. The same arithmetic the GPU sampler does, and
//! deliberately the same clamp: the shader saturate()s the LUT input, so the wrap has to as well
//! or the two would disagree at the edges of the domain.
Vec3 SampleCubeTrilinear(const SCubeLut& lut, const Vec3& in)
{
	const int n = lut.size;
	const float scale = (float)(n - 1);
	const float x = clamp_tpl(in.x, 0.0f, 1.0f) * scale;
	const float y = clamp_tpl(in.y, 0.0f, 1.0f) * scale;
	const float z = clamp_tpl(in.z, 0.0f, 1.0f) * scale;

	const int ix = min((int)x, n - 2);
	const int iy = min((int)y, n - 2);
	const int iz = min((int)z, n - 2);
	const float fx = x - (float)ix;
	const float fy = y - (float)iy;
	const float fz = z - (float)iz;

	Vec3 acc(0.0f, 0.0f, 0.0f);
	for (int dz = 0; dz < 2; ++dz)
	{
		const float wz = dz ? fz : (1.0f - fz);
		for (int dy = 0; dy < 2; ++dy)
		{
			const float wy = dy ? fy : (1.0f - fy);
			for (int dx = 0; dx < 2; ++dx)
			{
				const float w = wz * wy * (dx ? fx : (1.0f - fx));
				// Red fastest, then green, then blue - the .cube order the parser preserved.
				const size_t idx = ((size_t)(iz + dz) * n * n + (size_t)(iy + dy) * n + (size_t)(ix + dx)) * 3;
				acc.x += w * lut.rgb[idx + 0];
				acc.y += w * lut.rgb[idx + 1];
				acc.z += w * lut.rgb[idx + 2];
			}
		}
	}
	return acc;
}

//! Load the fixed forward/inverse output-transform pair the Rec.709 wrap needs. Once per process;
//! a failure is remembered so a missing file does not re-read the disk every time a LUT changes.
bool EnsureWrapTransforms()
{
	if (s_wrap.bTried)
		return s_wrap.bReady;

	s_wrap.bTried = true;

	string error, resolvedPath;
	if (!ParseCubeFile(kWrapForwardODTFile, s_wrap.forward, error, resolvedPath))
	{
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
		           "[SceneReferred] cannot wrap a Rec.709 LUT: '%s' NOT loaded: %s",
		           kWrapForwardODTFile, error.c_str());
		return false;
	}
	if (!ParseCubeFile(kWrapInverseODTFile, s_wrap.inverse, error, resolvedPath))
	{
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
		           "[SceneReferred] cannot wrap a Rec.709 LUT: '%s' NOT loaded: %s. It is baked by "
		           "tools/ocio-bake (uv run bake.py --preset odt_srgb_100nit_inv) and installs "
		           "beside the other cubes.",
		           kWrapInverseODTFile, error.c_str());
		s_wrap.forward = SCubeLut();
		return false;
	}

	s_wrap.bReady = true;
	CryLog("[SceneReferred] LUT wrap transforms ready: forward %d^3, inverse %d^3",
	       s_wrap.forward.size, s_wrap.inverse.size);
	return true;
}

//! Turn a LUT that is NOT in the grading space into one that is (S10 item 4.2).
//!
//! The recipe is everybody's - ACES's own guidance for reusing a legacy show LUT, Baselight's and
//! Resolve's inverse-ODT node sandwiches, Nuke's OCIODisplay invert: forward output transform,
//! the foreign LUT, inverse output transform. What is ours is that it is applied as a RESIDUAL:
//!
//!     W(x) = x + [ inv(LUT(fwd(x))) - inv(fwd(x)) ]
//!
//! because inv(fwd(x)) is NOT the identity. An SDR output transform folds out-of-gamut colour
//! inwards and puts everything above display white on one code value, and neither can be undone -
//! so the plain sandwich turns an IDENTITY Rec.709 LUT into a visibly non-identity LMT (measured:
//! up to 0.95 in ACEScct, ~16 stops, at the extremes). In the residual form the two inv() calls
//! land on the same coordinate for an identity source and cancel BIT FOR BIT, so "neutral is an
//! exact identity" survives - the rule every other control in this component keeps. Above display
//! white the LUT's effect continues as a constant offset in ACEScct = a constant gain in linear,
//! which is what keeps highlight separation in the EXR (the export tap is downstream of here).
void WrapForeignLut(SCubeLut& lut, ELutSpace space, int outSize)
{
	std::vector<float> wrapped;
	wrapped.resize((size_t)outSize * outSize * outSize * 3);

	const float scale = 1.0f / (float)(outSize - 1);
	size_t out = 0;
	for (int b = 0; b < outSize; ++b)
	{
		for (int g = 0; g < outSize; ++g)
		{
			for (int r = 0; r < outSize; ++r, out += 3)
			{
				const Vec3 x((float)r * scale, (float)g * scale, (float)b * scale);

				Vec3 projected, looked;
				if (space == ELutSpace::Rec709Display)
				{
					const Vec3 display = SampleCubeTrilinear(s_wrap.forward, x);
					projected = SampleCubeTrilinear(s_wrap.inverse, display);
					looked = SampleCubeTrilinear(s_wrap.inverse, SampleCubeTrilinear(lut, display));
				}
				else
				{
					// ACEScg: the shaper is analytic on both sides, so no cube is involved. The
					// clamp is the honest part - a unit-domain LINEAR cube covers scene linear
					// [0,1] and ACEScctToLinear(1.0) is 222.86, so everything above 1.0 gets the
					// LUT's effect AT 1.0, carried up as a constant gain by the residual.
					Vec3 lin = SceneReferredCurves::ACEScctToLinear(x);
					lin.x = clamp_tpl(lin.x, 0.0f, 1.0f);
					lin.y = clamp_tpl(lin.y, 0.0f, 1.0f);
					lin.z = clamp_tpl(lin.z, 0.0f, 1.0f);
					projected = SceneReferredCurves::LinearToACEScct(lin);
					looked = SceneReferredCurves::LinearToACEScct(SampleCubeTrilinear(lut, lin));
				}

				wrapped[out + 0] = x.x + (looked.x - projected.x);
				wrapped[out + 1] = x.y + (looked.y - projected.y);
				wrapped[out + 2] = x.z + (looked.z - projected.z);
			}
		}
	}

	lut.rgb.swap(wrapped);
	lut.size = outSize;
}

//! Parse, wrap if the file is not already in the grading space, convert and upload. Called only
//! when something changed - a path, the declared space, or the file on disk.
//!
//! `bIsReload` splits the two failure policies apart, deliberately. A path the user just typed
//! that does not parse must CLEAR the slot, so the picture goes visibly wrong and the user sees
//! their mistake (design decision D13). A file that failed to re-read while nobody asked for
//! anything must keep the previous good LUT: the user did not request a change, so the picture
//! must not change under them - most often it is a half-written file and the next poll succeeds.
bool LoadLutIntoSlot(SLutSlot& slot, const char* szWanted, const char* szCacheName,
                     const char* szRole, ELutSpace fallbackSpace, bool bAllowWrap, bool bIsReload)
{
	SCubeLut lut;
	string error, resolvedPath;
	if (!ParseCubeFile(szWanted, lut, error, resolvedPath))
	{
		// Once per change, not once per frame. A WARNING and not a log line: without the output
		// transform the picture is a clamped encode - flat and washed - and the whole point of
		// D13 is that the user is told why instead of being left to guess at a look. It has to be
		// findable in a console full of asset spam, so it goes through CryWarning.
		if (bIsReload && slot.loaded)
		{
			CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
			           "[SceneReferred] %s LUT '%s' changed on disk but was NOT reloaded: %s. "
			           "KEEPING the last good one - the picture has not changed.",
			           szRole, szWanted, error.c_str());
			return false;
		}
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
		           "[SceneReferred] %s LUT '%s' NOT loaded: %s", szRole, szWanted, error.c_str());
		ClearLutSlot(slot);
		return false;
	}

	// ----- which space is this file in? (S10 item 4.1) ----------------------------------------
	// The file's own tag wins when it has one: it was written by whoever made the LUT, while the
	// property is a guess by whoever loaded it. Both present and disagreeing is not an error - it
	// is a thing the user has to be TOLD, once, with the file named.
	ELutSpace space = fallbackSpace;
	if (lut.bTagged)
	{
		space = lut.tagSpace;
		if (lut.tagSpace != fallbackSpace)
		{
			CryLog("[SceneReferred] %s LUT '%s' says it is %s; the %s Input Space property says "
			       "%s. THE FILE WINS.", szRole, szWanted, LutSpaceName(lut.tagSpace), szRole,
			       LutSpaceName(fallbackSpace));
		}
	}
	else if (!lut.tagText.empty())
	{
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
		           "[SceneReferred] %s LUT '%s' carries an unrecognised ReC-LUT-Space '%s'; "
		           "using the Input Space property (%s) instead. Known: ACEScct, ACEScg, Rec709.",
		           szRole, szWanted, lut.tagText.c_str(), LutSpaceName(fallbackSpace));
	}

	// ----- wrap it into the grading space if it is not there already ---------------------------
	int nSourceSize = lut.size;
	bool bWrapped = false;
	if (space != ELutSpace::ACEScct)
	{
		if (!bAllowWrap)
		{
			// The ODT slot. An output transform IS the thing that leaves the grading space, so a
			// space tag on it describes its input and there is nothing to wrap.
			space = ELutSpace::ACEScct;
		}
		else if (space == ELutSpace::Rec709Display && !EnsureWrapTransforms())
		{
			CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
			           "[SceneReferred] %s LUT '%s' is a Rec.709 display LUT but the wrap "
			           "transforms are missing, so it is being loaded RAW - the picture will be "
			           "wrong. Install %s.", szRole, szWanted, kWrapInverseODTFile);
		}
		else
		{
			// ALWAYS 65^3, whatever the source's grid was. The wrapped LUT does not only carry
			// the look - it carries the output transform's own curvature, which is far steeper
			// than any look, and that is what sets the grid it needs. Measured against a
			// third-party-style 709 look LUT, in gamut and in the display's range: 33^3 wrapped
			// costs ~0.96 8-bit code values of mean error against applying the LUT to the ODT
			// output directly, 65^3 costs ~0.44. It is the same 2.2 MiB the ODT already spends
			// and it runs once, on change.
			WrapForeignLut(lut, space, kWrapGridSize);
			bWrapped = true;
		}
	}

	// fp16 RGBA on the CPU. The upload path does NOT convert formats - source and destination
	// have to resolve to the same typeless DXGI format - and there is no three-channel fp16
	// texture format, so the alpha channel is the price of admission (25 % of 2.2 MiB at 65^3).
	const int nEntries = lut.size * lut.size * lut.size;
	std::vector<CryHalf> halfData;
	halfData.resize((size_t)nEntries * 4);
	const CryHalf one = CryConvertFloatToHalf(1.0f);
	for (int i = 0; i < nEntries; ++i)
	{
		halfData[(size_t)i * 4 + 0] = CryConvertFloatToHalf(lut.rgb[(size_t)i * 3 + 0]);
		halfData[(size_t)i * 4 + 1] = CryConvertFloatToHalf(lut.rgb[(size_t)i * 3 + 1]);
		halfData[(size_t)i * 4 + 2] = CryConvertFloatToHalf(lut.rgb[(size_t)i * 3 + 2]);
		halfData[(size_t)i * 4 + 3] = one;
	}

	// A different grid size needs a different texture: UploadToVideoMemory3D with an existing Id
	// refreshes the contents in place and does not re-describe the resource.
	if (slot.texId > 0 && slot.size != lut.size)
	{
		ClearLutSlot(slot);   // id-safe release; see the comment there
	}

	// repeat = false -> FT_STATE_CLAMP, which is what a LUT wants on all three axes.
	// FT_DONT_STREAM is forced by the upload path anyway and is stated here so the intent is on
	// the page. One mip: the update path walks the whole chain and would read past a
	// single-level buffer if it were told there were more.
	const int nNewId = (int)gEnv->pRenderer->UploadToVideoMemory3D(
		(unsigned char*)halfData.data(), lut.size, lut.size, lut.size,
		eTF_R16G16B16A16F, eTF_R16G16B16A16F, /*nummipmap*/ 1, /*repeat*/ false,
		FILTER_LINEAR, slot.texId, szCacheName,
		FT_NOMIPS | FT_DONT_STREAM | FT_STATE_CLAMP);

	if (nNewId <= 0)
	{
		CryLog("[SceneReferred] %s LUT '%s' NOT uploaded (the renderer returned no texture)", szRole, szWanted);
		slot.texId = 0;
		slot.size = 0;
		slot.loaded = false;
		return false;
	}

	slot.texId = nNewId;
	slot.size = lut.size;
	slot.loaded = true;
	slot.cacheName = szCacheName ? szCacheName : "";
	// What the watcher compares against from now on. Taken AFTER the read, so a file that was
	// still being written when it was parsed is re-read on the next poll rather than accepted.
	StatLutFile(szWanted, slot.fileTime, slot.fileSize);
	slot.bPending = false;

	// The ABSOLUTE path, not the requested one: "not found" and "found somewhere else than you
	// think" look identical in a relative path, and the two install trees (project assets and
	// %ENGINE%) are exactly the pair that gets confused.
	if (bWrapped)
	{
		CryLog("[SceneReferred] %s LUT '%s' %s: %d^3 %s source WRAPPED into an ACEScct LMT at "
		       "%d^3, texture id %d, from '%s'. For a shipping look, wrap it once offline instead: "
		       "tools/ocio-bake, uv run wrap.py --lut <file> --space %s",
		       szRole, szWanted, bIsReload ? "reloaded" : "loaded", nSourceSize,
		       LutSpaceName(space), lut.size, nNewId, resolvedPath.c_str(),
		       space == ELutSpace::Rec709Display ? "rec709" : "acescg");
	}
	else
	{
		CryLog("[SceneReferred] %s LUT '%s' %s: %d^3, texture id %d, from '%s'",
		       szRole, szWanted, bIsReload ? "reloaded" : "loaded", lut.size, nNewId,
		       resolvedPath.c_str());
	}
	return true;
}

//! Decide whether the slot needs re-reading, and re-read it if so.
//!
//! Cheap on every frame but the first after a change: the common path is one string compare and
//! one enum compare. The hot-reload poll adds an FOpen/FClose every cinecam_LutHotReload frames -
//! about 30 us, half a second apart, on two files.
void EnsureLutSlot(SLutSlot& slot, const char* szPath, const char* szCacheName, const char* szRole,
                   ELutSpace fallbackSpace = ELutSpace::ACEScct, bool bAllowWrap = false)
{
	const char* szWanted = szPath ? szPath : "";

	if (slot.requested != szWanted || slot.requestedSpace != fallbackSpace)
	{
		slot.requested = szWanted;
		slot.requestedSpace = fallbackSpace;
		slot.fileTime = 0;
		slot.fileSize = 0;
		slot.bPending = false;

		if (!szWanted[0])
		{
			// An empty path is a legitimate answer for the LMT slot (identity look, D10) and a
			// mistake for the ODT slot. The loader does not judge; the tone map does.
			ClearLutSlot(slot);
			return;
		}

		LoadLutIntoSlot(slot, szWanted, szCacheName, szRole, fallbackSpace, bAllowWrap, false);
		return;
	}

	// ----- hot reload (S10 item 4.3) -----------------------------------------------------------
	if (s_lutHotReloadFrames <= 0 || !szWanted[0] || !slot.loaded)
		return;

	if ((gEnv->nMainFrameID % (uint32)s_lutHotReloadFrames) != 0)
		return;

	uint64 fileTime = 0, fileSize = 0;
	if (!StatLutFile(szWanted, fileTime, fileSize))
		return;   // gone, or momentarily unopenable. The picture keeps the LUT it has.

	if (fileTime == slot.fileTime && fileSize == slot.fileSize)
	{
		slot.bPending = false;
		return;
	}

	// Seen changed once: remember it and look again next poll. Resolve writing a 65^3 cube writes
	// ~2.7 MB, and a poll can land in the middle of that; the second look is what makes the
	// trigger "the file has stopped changing" rather than "the file is being written".
	if (!slot.bPending || fileTime != slot.pendingTime || fileSize != slot.pendingSize)
	{
		slot.bPending = true;
		slot.pendingTime = fileTime;
		slot.pendingSize = fileSize;
		return;
	}

	slot.bPending = false;
	if (!LoadLutIntoSlot(slot, szWanted, szCacheName, szRole, fallbackSpace, bAllowWrap, true))
	{
		// A file that is broken and stays broken must be complained about ONCE, not twice a
		// second: adopt what was seen so the next poll compares equal and says nothing until the
		// file genuinely changes again.
		slot.fileTime = fileTime;
		slot.fileSize = fileSize;
	}
}

void ClearCurveLutSlot()
{
	ReleaseOwnTexture(s_lutCurve.texId, s_lutCurve.cacheName);
	s_lutCurve.size = 0;
	s_lutCurve.bMasterActive = false;
	s_lutCurve.bSatActive = false;
}

//! Bake the two 1D curves into the shared texture, if either of them is doing anything and if
//! anything about them has changed (S10 item 3b).
//!
//! The neutrality test is EXACT - a master knot's neutral is the literal 0.0f and a saturation
//! multiplier's is the literal 1.0f - so "nobody has touched this" needs no epsilon and, more to
//! the point, produces no texture, no bus size and no shader fetch. That is what makes an unused
//! curve free rather than cheap, and the neutral picture bit-identical to the build before this
//! existed rather than merely indistinguishable from it.
//!
//! Everything about the SHAPE of the curve - where the knots are, what happens between them, how
//! stops become knot heights - lives in CryRenderer/SceneReferredCurves.h, which the renderer's
//! EXR exporter includes too. This function only decides when to run it.
void EnsureCurveLut(const float* pMasterStops, const float* pSatMult)
{
	using namespace SceneReferredCurves;

	static const float kNeutralStops[kKnotCount] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	static const float kNeutralSat[kKnotCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

	const float* const pMaster = pMasterStops ? pMasterStops : kNeutralStops;
	const float* const pSat = pSatMult ? pSatMult : kNeutralSat;

	const bool bMasterActive = !IsMasterNeutral(pMaster);
	const bool bSatActive = !IsSatNeutral(pSat);

	if (!bMasterActive && !bSatActive)
	{
		if (s_lutCurve.texId > 0)
			ClearCurveLutSlot();
		// Remembered even when nothing is uploaded, so that going neutral and back is one compare.
		memcpy(s_lutCurve.masterStops, pMaster, sizeof(s_lutCurve.masterStops));
		memcpy(s_lutCurve.satMult, pSat, sizeof(s_lutCurve.satMult));
		return;
	}

	if (s_lutCurve.texId > 0
	    && memcmp(s_lutCurve.masterStops, pMaster, sizeof(s_lutCurve.masterStops)) == 0
	    && memcmp(s_lutCurve.satMult, pSat, sizeof(s_lutCurve.satMult)) == 0)
	{
		return;
	}

	memcpy(s_lutCurve.masterStops, pMaster, sizeof(s_lutCurve.masterStops));
	memcpy(s_lutCurve.satMult, pSat, sizeof(s_lutCurve.satMult));
	s_lutCurve.bMasterActive = bMasterActive;
	s_lutCurve.bSatActive = bSatActive;

	std::vector<float> master(kLutSize), sat(kLutSize);
	BakeMaster(pMaster, master.data(), kLutSize);
	BakeSat(pSat, sat.data(), kLutSize);

	// R = the master curve, G = the saturation multiplier, B = 0 and A = 1 because there is no
	// two-channel fp16 upload here and the alpha is the price of admission (the same 8 KiB
	// argument the 3D LUTs make at 2.2 MiB).
	std::vector<CryHalf> halfData;
	halfData.resize((size_t)kLutSize * 4);
	const CryHalf zero = CryConvertFloatToHalf(0.0f);
	const CryHalf one = CryConvertFloatToHalf(1.0f);
	for (int i = 0; i < kLutSize; ++i)
	{
		halfData[(size_t)i * 4 + 0] = CryConvertFloatToHalf(master[i]);
		halfData[(size_t)i * 4 + 1] = CryConvertFloatToHalf(sat[i]);
		halfData[(size_t)i * 4 + 2] = zero;
		halfData[(size_t)i * 4 + 3] = one;
	}

	// h = 1: the public interface has no 1D entry point, and an N x 1 eTT_2D texture is what it
	// resolves to (DriverD3D.cpp:4782). The width never changes, so this is an in-place refresh of
	// the same id after the first bake - which is the case that matters, because dragging a knot
	// runs it once per edit.
	const int nNewId = (int)gEnv->pRenderer->UploadToVideoMemory(
		(unsigned char*)halfData.data(), kLutSize, 1,
		eTF_R16G16B16A16F, eTF_R16G16B16A16F, /*nummipmap*/ 1, /*repeat*/ false,
		FILTER_LINEAR, s_lutCurve.texId, "$ccamCurve",
		FT_NOMIPS | FT_DONT_STREAM | FT_STATE_CLAMP);

	if (nNewId <= 0)
	{
		// Once per change, not once per frame: this is only reached when something moved.
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING,
		           "[SceneReferred] the grade curves were NOT uploaded (the renderer returned no "
		           "texture) - the picture is ungraded by them until they are edited again");
		s_lutCurve.texId = 0;
		s_lutCurve.size = 0;
		s_lutCurve.bMasterActive = false;
		s_lutCurve.bSatActive = false;
		return;
	}

	s_lutCurve.texId = nNewId;
	s_lutCurve.size = kLutSize;
	s_lutCurve.cacheName = "$ccamCurve";
}

} // namespace

// The two LUT slots, refreshed and published every frame. "Published" is two numbers per slot on
// the post-effect bus - the renderer texture ID and the grid size - because the grid size is what
// the shader's half-texel inset is computed from and a 33^3 LMT next to a 65^3 ODT has to work.
//
// Nothing here is gated on m_sceneReferred: loading a cube costs one string compare per frame
// once it is loaded, and having the texture ready before the switch is flipped means the first
// scene-referred frame is already correct.
namespace CineCam
{

// The LUT slots are process-wide (they are the renderer's textures, and only one driver publishes
// at a time), so this is a free function rather than a component method: the plugin-owned editor
// look publisher needs exactly the same two uploads for the shipped default ODT, and a second copy
// of the upload path would mean a second pair of textures fighting over the same two bus slots.
void ApplyDisplayLuts(const char* szODTFile, const char* szLMTFile,
                      const float* pMasterCurveStops, const float* pSatCurveMultipliers,
                      uint32 lmtSpace)
{
	if (!gEnv->pRenderer || !gEnv->p3DEngine)
		return;

	// An empty ODT field is not a request for "no output transform" - it is a field nobody has
	// touched, and the picture it produces (a clamped ACEScct encode) looks like a bug because
	// it is one. The shipped default stands in, so a freshly ticked switch is already correct.
	const char* szODT = szODTFile ? szODTFile : "";
	if (!szODT[0])
		szODT = kDefaultODTFile;

	// The ODT slot never wraps: an output transform IS the step that leaves the grading space, so
	// "what space is this in" has one answer for it and there is nothing to convert. Only the LMT
	// slot takes files from outside this pipeline, so only it is space-aware (S10 item 4).
	EnsureLutSlot(s_lutODT, szODT, "$ccamODT", "ODT");
	EnsureLutSlot(s_lutLMT, szLMTFile ? szLMTFile : "", "$ccamLMT", "LMT",
	              (ELutSpace)lmtSpace, /*bAllowWrap*/ true);

	gEnv->p3DEngine->SetPostEffectParam("Global_User_LutODT",     (float)s_lutODT.texId);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_LutODTSize", (float)s_lutODT.size);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_LutLMT",     (float)s_lutLMT.texId);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_LutLMTSize", (float)s_lutLMT.size);

	// The two 1D curves. The texture id and size are what the tone map samples; the ten control
	// points are published as well because the EXR exporter has to write the master curve out as
	// a Resolve 1D .cube beside a capture, and it re-evaluates them through the same header this
	// bake used. `.w` of the two B params is deliberately unused - `.y` is the per-curve ACTIVE
	// flag, which the renderer takes from here rather than deriving a second time from the control
	// points, because two answers to "is this curve on" is exactly the state that drifts.
	EnsureCurveLut(pMasterCurveStops, pSatCurveMultipliers);

	const float* const pM = s_lutCurve.masterStops;
	const float* const pS = s_lutCurve.satMult;
	gEnv->p3DEngine->SetPostEffectParam("Global_User_CurveLut",     (float)s_lutCurve.texId);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_CurveLutSize", (float)s_lutCurve.size);
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveMasterA", Vec4(pM[0], pM[1], pM[2], pM[3]));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveMasterB", Vec4(pM[4], s_lutCurve.bMasterActive ? 1.0f : 0.0f, 0.0f, 0.0f));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveSatA",    Vec4(pS[0], pS[1], pS[2], pS[3]));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveSatB",    Vec4(pS[4], s_lutCurve.bSatActive ? 1.0f : 0.0f, 0.0f, 0.0f));
}

// The display chain's file watcher (S10 item 4.3). Registered by the plugin rather than by a
// component: the LUT slots are process-wide and outlive any one camera.
//
// A POLL and not IFileChangeMonitor, on evidence: that interface is provided by Sandbox and by
// nothing else (EditorFileMonitor.cpp is the only SetIFileChangeMonitor in the tree), and even
// there it watches the project's game folder, the mod paths, Editor/ and Engine/Shaders - NOT
// <engine>/engine/Assets/ODT/, which is one of the two install trees this loader documents. A
// reload mechanism that works in half the installs is worse than none.
void RegisterLutCVars()
{
	if (s_pLutHotReloadCVar != nullptr || gEnv->pConsole == nullptr)
		return;

	s_pLutHotReloadCVar = REGISTER_CVAR2("cinecam_LutHotReload", &s_lutHotReloadFrames, 0, VF_NULL,
		"Re-read the display chain's .cube files (ODT and LMT) when they change on disk, every N "
		"frames. 0 = off. Author a look in Resolve on one monitor and watch the picture move on "
		"the other. A change has to be seen twice before it is acted on, so a half-written file is "
		"not loaded; a file that fails to parse keeps the previous good LUT and says why. Default "
		"30 in the editor, 0 in the launcher (a shipped build's LUTs do not change).");

	// The default is decided here rather than in the macro because it depends on which host we
	// are in. Set through the cvar so a user.cfg or a console line still overrides it afterwards.
	if (s_pLutHotReloadCVar != nullptr && gEnv->IsEditor())
		s_pLutHotReloadCVar->Set(30);
}

void UnregisterLutCVars()
{
	if (s_pLutHotReloadCVar != nullptr && gEnv->pConsole != nullptr)
		gEnv->pConsole->UnregisterVariable("cinecam_LutHotReload");

	s_pLutHotReloadCVar = nullptr;
}

// The bus is cleared first, so the renderer stops referring to the IDs before they die; the
// textures then go with the driver. There is nothing to save and restore here - off the
// scene-referred path nothing reads these params, and the stock path never had them.
void RestoreDisplayLuts()
{
	if (gEnv && gEnv->p3DEngine)
	{
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutODT",     0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutODTSize", 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutLMT",     0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutLMTSize", 0.0f);

		// The curve slot goes with them. Only the size and the two active flags matter to the
		// shader, but the control points are cleared too so that nothing downstream - the EXR
		// header, the sidecars - can describe a curve that is no longer in the picture.
		gEnv->p3DEngine->SetPostEffectParam("Global_User_CurveLut",     0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_CurveLutSize", 0.0f);
		gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveMasterA", Vec4(0.0f, 0.0f, 0.0f, 0.0f));
		gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveMasterB", Vec4(0.0f, 0.0f, 0.0f, 0.0f));
		gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveSatA",    Vec4(1.0f, 1.0f, 1.0f, 1.0f));
		gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_CurveSatB",    Vec4(1.0f, 0.0f, 0.0f, 0.0f));
	}

	ClearLutSlot(s_lutODT);
	ClearLutSlot(s_lutLMT);
	ClearCurveLutSlot();
	s_lutODT.requested.clear();
	s_lutLMT.requested.clear();
}

} // namespace CineCam

void CCinematicCameraComponent::ApplyDisplayLuts()
{
	if (!CanDriveRenderState())
		return;

	// The curves come from the same sibling as the look, by the same lookup, on the same frame:
	// nothing is cached, so a CineCam Grade added or removed at runtime is picked up on the very
	// next publish and its absence is the neutral (no texture, no fetch).
	const SCineCurveParams& curves = GetCurveParams();
	float masterStops[SceneReferredCurves::kKnotCount];
	float satMult[SceneReferredCurves::kKnotCount];
	curves.GetMasterStops(masterStops);
	curves.GetSatMultipliers(satMult);

	const SCineLookParams& look = GetLookParams();
	CineCam::ApplyDisplayLuts(m_output.odtFile.value.c_str(), look.lmtFile.value.c_str(),
	                          masterStops, satMult, (uint32)look.lmtSpace);
}

void CCinematicCameraComponent::RestoreDisplayLuts()
{
	CineCam::RestoreDisplayLuts();
}

// A level is going away (ESYSTEM_EVENT_LEVEL_UNLOAD_START / LEVEL_LOAD_PREPARE / LEVEL_LOAD_START
// / LEVEL_POST_UNLOAD, whichever the host fires first). This runs BEFORE the renderer frees its
// resources, which is the whole point: it is the last moment at which our LUT texture ids are
// still ours to release.
//
// It also takes the scene-referred request off the bus by hand. Leaving it standing means the
// pipeline is asked to run scene-referred across a load - on targets that are being destroyed and
// re-created, with the shader cache possibly invalidated behind it - and the engine then clears
// the bus itself at the end of the load (CRenderer::EF_ResetPostEffects, render thread), so the
// switch gets flipped twice before the first frame of the new level is drawn for no gain at all.
// The engine-side mirror goes with it as a PLAIN STORE: PublishSceneReferredConvention()'s forced
// ITimeOfDay::Update(true, true) recomputes the whole Nishita sky dome, and asking for that in
// the middle of a level teardown is exactly the kind of work this function exists to prevent. The
// time of day is reloaded by the level anyway.
void CCinematicCameraComponent::OnLevelTeardown()
{
	if (gEnv && gEnv->p3DEngine)
	{
		gEnv->p3DEngine->SetPostEffectParam("Global_User_SceneReferred", 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutODT", 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutODTSize", 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutLMT", 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("Global_User_LutLMTSize", 0.0f);
		gEnv->p3DEngine->SetGlobalParameter(E3DPARAM_SCENE_REFERRED, Vec3(0.0f, 0.0f, 0.0f));
	}

	ClearLutSlot(s_lutODT);
	ClearLutSlot(s_lutLMT);
	s_lutODT.requested.clear();
	s_lutLMT.requested.clear();

	// The baselines were captured from a bus that is about to be reset; writing them back later
	// would restore values from the previous level.
	for (CCinematicCameraComponent* p : s_all)
		p->ClearSavedBaselines();
}

// The level is up (ESYSTEM_EVENT_LEVEL_LOAD_END / LEVEL_GAMEPLAY_START). Nothing is applied here:
// CanDriveRenderState() has been refusing for the whole load and starts saying yes on the next
// frame, so the resolver and ENTITY_EVENT_UPDATE re-apply the camera by themselves. All this does
// is make sure they re-apply from scratch rather than from a cache that predates the load.
void CCinematicCameraComponent::OnLevelLoaded()
{
	s_lutODT.requested.clear();
	s_lutLMT.requested.clear();

	for (CCinematicCameraComponent* p : s_all)
		p->ClearSavedBaselines();
}

// WHERE THE GRADE COMES FROM (decisions/s10-grade-component.md section 2 A / section 3 P1).
// The ASC CDL and the look slot live on a separate CineCam Grade component - a look is a thing
// you add and delete, not a permanent part of every camera - but the camera stays the single
// writer of the bus. It looks the sibling up on the frame it publishes rather than caching a
// pointer: the lookup is a linear scan of a three-component entity with no allocation, and in
// exchange nothing here depends on which component was created first, a component removed at
// runtime falls back to neutral on the very next frame with no dangling pointer to clear, and
// the whole publication stays behind the one CanDriveRenderState() gate.
//
// The fallbacks are default-constructed, so their values are the reflected neutrals: bypass off,
// slope 1, offset 0, power 1, saturation 1, no LMT. That is what makes "no grade component" and
// "an untouched grade component" the same picture, and both of them exactly ACES 2.0 (D10).
static const SCineGradeParams s_neutralGrade;
static const SCineCurveParams s_neutralCurves;
static const SCineLookParams  s_neutralLook;

const SCineGradeParams& CCinematicCameraComponent::GetGradeParams() const
{
	if (m_pEntity != nullptr)
	{
		if (const CCineGradeComponent* pGrade = m_pEntity->GetComponent<CCineGradeComponent>())
			return pGrade->GetGrade();
	}

	return s_neutralGrade;
}

const SCineCurveParams& CCinematicCameraComponent::GetCurveParams() const
{
	if (m_pEntity != nullptr)
	{
		if (const CCineGradeComponent* pGrade = m_pEntity->GetComponent<CCineGradeComponent>())
			return pGrade->GetCurves();
	}

	return s_neutralCurves;
}

const SCineLookParams& CCinematicCameraComponent::GetLookParams() const
{
	if (m_pEntity != nullptr)
	{
		if (const CCineGradeComponent* pGrade = m_pEntity->GetComponent<CCineGradeComponent>())
			return pGrade->GetLook();
	}

	return s_neutralLook;
}

// The sibling grade component edited one of its properties. Both calls are the ordinary ones,
// behind the ordinary gate, so this is only about latency: without it the edit would be picked up
// by the next ENTITY_EVENT_UPDATE instead of immediately.
void CCinematicCameraComponent::RefreshGrade()
{
	ApplyDisplayLuts();
	ApplyGrade();
}

// The camera's grade (SceneReferredSpec.md S5). Everything here acts on scene data BEFORE the
// output transform, which is the whole difference between grading and retouching: the picture
// has not been squeezed onto a monitor yet, so a saturation push has somewhere to go and a
// white-balance change is a change of illuminant rather than a tint over a finished frame.
//
// All of it is published live, every frame, as plain numbers on the post-effect bus - the tone
// map builds the white-balance matrix and hands the CDL straight to the shader. There is no
// per-frame LUT bake to wait for, so dragging a slider moves the picture on the next frame.
//
// Neutral by construction: 6500 K + tint 0 is an exact identity (the tone map derives its
// reference white from the same locus), slope 1 / offset 0 / power 1 / saturation 1 is an exact
// identity, and with the Look slot empty the chain is nothing but the ACES 2.0 output transform.
// That is D10 - no hidden house look - and it is a property of the defaults, not a claim.
void CCinematicCameraComponent::ApplyGrade()
{
	if (!CanDriveRenderState())
		return;

	const SCineGradeParams& grade = GetGradeParams();
	const bool bBypass = grade.bBypassGrade;

	// The wheels, contrast and the ASC CDL base fold into ONE CDL on the way out
	// (SCineGradeParams::Resolve, decisions/s10-grade-component.md section 3a). The bus, the
	// shader, the EXR header and the .cdl sidecar all therefore see the same three triples, and
	// nothing downstream has to know that a colour wheel exists.
	const SCineEffectiveCdl cdl = grade.Resolve();

	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeBypass",   bBypass ? 1.0f : 0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeWBEnable", m_whiteBalance.bEnableWhiteBalance ? 1.0f : 0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeWBKelvin", (float)(int)m_whiteBalance.colorTemperature);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeWBTint",   (float)m_whiteBalance.tint);

	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_GradeSlope",  Vec4(cdl.slope.x, cdl.slope.y, cdl.slope.z, 0.0f));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_GradeOffset", Vec4(cdl.offset.x, cdl.offset.y, cdl.offset.z, 0.0f));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_GradePower",  Vec4(cdl.power.x, cdl.power.y, cdl.power.z, 0.0f));
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeSaturation", cdl.saturation);

	// Diagnostic, edge-triggered exactly like the white balance's below: one line per change of
	// the CDL this camera publishes. It is the answer to "I moved a grade slider and the picture
	// did not move" - a line means the value left the component, reached the bus and the reason
	// lies further down the chain (the Scene Referred switch, which is where the tone map reads
	// these params at all); no line means it never got here. Two float compares per frame.
	//
	// These are the FOLDED numbers, which is the point: with wheels in front of the CDL the raw
	// properties no longer say what is on the bus, and this line is where "what is actually being
	// sent" is readable without a debugger. It is also, verbatim, what the .cdl sidecar of a
	// capture taken now would contain.
	{
		const float sat = cdl.saturation;
		if (fabs_tpl(cdl.slope.x - m_lastLoggedGradeSlopeX) > 1e-4f ||
		    fabs_tpl(cdl.offset.x - m_lastLoggedGradeOffsetX) > 1e-4f ||
		    fabs_tpl(cdl.power.x - m_lastLoggedGradePowerX) > 1e-4f ||
		    fabs_tpl(sat - m_lastLoggedGradeSaturation) > 1e-4f)
		{
			m_lastLoggedGradeSlopeX = cdl.slope.x;
			m_lastLoggedGradeOffsetX = cdl.offset.x;
			m_lastLoggedGradePowerX = cdl.power.x;
			m_lastLoggedGradeSaturation = sat;
			CryLog("[CinematicCamera] grade published: slope %.4f/%.4f/%.4f offset %.4f/%.4f/%.4f "
			       "power %.4f/%.4f/%.4f saturation %.4f%s (scene referred %s)",
			       cdl.slope.x, cdl.slope.y, cdl.slope.z,
			       cdl.offset.x, cdl.offset.y, cdl.offset.z,
			       cdl.power.x, cdl.power.y, cdl.power.z, sat,
			       bBypass ? " BYPASSED" : "",
			       m_sceneReferred.bSceneReferred ? "on" : "OFF - the tone map does not read these");
		}
	}
}

void CCinematicCameraComponent::RestoreGrade()
{
	if (!gEnv || !gEnv->p3DEngine)
		return;

	// Back to neutral rather than to a saved baseline: nothing but this component ever writes
	// these params, so their neutral value IS the state the renderer had before the camera.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeBypass",     0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeWBEnable",   0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeWBKelvin",   6500.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeWBTint",     0.0f);
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_GradeSlope",  Vec4(1.0f, 1.0f, 1.0f, 0.0f));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_GradeOffset", Vec4(0.0f, 0.0f, 0.0f, 0.0f));
	gEnv->p3DEngine->SetPostEffectParamVec4("Global_User_GradePower",  Vec4(1.0f, 1.0f, 1.0f, 0.0f));
	gEnv->p3DEngine->SetPostEffectParam("Global_User_GradeSaturation", 1.0f);
}

// The scene-referred pipeline switch. Pattern A of the two ccam gating patterns: the component
// only ASKS, on the post-effect parameter bus, and r_SceneReferred decides whether the renderer
// listens. Nothing global is written, so a crash or a mode change can never strand the engine
// off the stock path - EF_ResetPostEffects() clears the request by itself.
void CCinematicCameraComponent::ApplySceneReferred()
{
	// Single writer, as for the lens character: without this every selected cinematic camera,
	// preview windows included, would fight over the request.
	if (!CanDriveRenderState())
		return;

	if (!m_bSceneReferredSaved)
	{
		gEnv->p3DEngine->GetPostEffectParam("Global_User_SceneReferred", m_savedSceneReferred);
		m_bSceneReferredSaved = true;
	}

	gEnv->p3DEngine->SetPostEffectParam("Global_User_SceneReferred",
	                                    m_sceneReferred.bSceneReferred ? 1.0f : 0.0f);

	CineCam::PublishSceneReferredConvention(m_sceneReferred.bSceneReferred);
}

void CCinematicCameraComponent::RestoreSceneReferred()
{
	if (!m_bSceneReferredSaved)
		return;

	gEnv->p3DEngine->SetPostEffectParam("Global_User_SceneReferred", m_savedSceneReferred);
	m_bSceneReferredSaved = false;

	// The engine-side mirror is restored with it, and by the same flip path, so a deactivating
	// camera leaves the 3D engine on the stock light-unit convention with the time of day
	// already re-evaluated for it.
	CineCam::PublishSceneReferredConvention(m_savedSceneReferred > 0.5f);
}

// The pre-exposure itself. It travels on the same bus as the request, in the same save/restore
// latch, and it is published every frame the component can drive render state - including in the
// editor, because with the scene-referred path on the exposure IS the picture.
//
// S1 item 1 publishes UNITY and nothing else: this commit is the plumbing (bus parameter, the
// render-thread latch, the five m_fAdaptedSceneScaleLBuffer sites), so the picture must not move
// yet. Item 2 replaces the body of ComputeSceneReferredExposureScale() with the real
// 1 / L_sat(EV100) and drops the tone map's own ComputeExposure() on the same path, which is the
// only way to change the two together without a frame of double exposure.
void CCinematicCameraComponent::ApplySceneExposure()
{
	// Single writer, exactly as for the request.
	if (!CanDriveRenderState())
		return;

	if (!m_bSceneExposureSaved)
	{
		gEnv->p3DEngine->GetPostEffectParam("Global_User_SceneExposure", m_savedSceneExposure);
		gEnv->p3DEngine->GetPostEffectParam("Global_User_SensorClipStops", m_savedSensorClipStops);
		m_bSceneExposureSaved = true;
	}

	gEnv->p3DEngine->SetPostEffectParam("Global_User_SceneExposure", ComputeSceneReferredExposureScale());

	// The sensor's full well (decisions/s8-sensor-clip.md). Published unconditionally: the tone
	// map only reads it on the scene-referred branch, and publishing it off the switch as well
	// keeps one writer for one number instead of two rules for when it is live.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_SensorClipStops", (float)m_exposure.sensorClipStops);

	// The metering mask the histogram pass accumulates with. An INPUT to the renderer, unlike the
	// three readouts below it, and published every frame the camera drives: the pass reads it on
	// the render thread and stamps the mode it used onto the bins it hands back, so a mode change
	// costs one frame of latency and can never be attributed to the wrong histogram.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterMode", (float)(uint32)m_exposure.meteringMode);
	// The sky's vote in the same histogram (decisions/s8-metering-sky.md): a second input to the
	// same pass, with the same one-frame latency and the same "the value used comes back with the
	// bins" rule.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterSkyWeight", (float)m_exposure.meterSkyWeight);

	// Diagnostics for r_HDRDebug 1 (decisions/s8-metering.md, "Diagnosability"). The camera owns
	// the reduction, so the renderer cannot print the band's edges unless the camera says what
	// they were. Published here, once a frame, rather than from inside ComputeSceneReferredEV100,
	// which is reached more than once per frame.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterBandLow", m_autoMeter.bandLowLum);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterBandHigh", m_autoMeter.bandHighLum);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterTargetEV", m_autoMeter.targetEV);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterBandMean", m_autoMeter.bandMeanStops);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterHold", (float)m_autoMeter.hold);
}

// The camera's identity and its exposure triangle, onto the same bus everything else travels on.
// Written every frame the camera owns the render state, and read once per CAPTURED frame - the
// renderer samples it at readback time, so a value that is a frame stale would attach the wrong
// numbers to a picture.
//
// Nothing here is drawn with. The one that matters most is the EV100: the pre-exposure has already
// been folded into the pixels by the time they reach the export tap, so without the exposure
// travelling in the header the file's relation to real luminance is gone.
void CCinematicCameraComponent::ApplyExportMetadata()
{
	if (!CanDriveRenderState())
		return;

	const I3DEngine* p3DEngine = gEnv->p3DEngine;

	p3DEngine->SetPostEffectParamString("Global_User_CamName", m_pEntity ? m_pEntity->GetName() : "");
	// Lens PRESETS do not exist yet - the preset camera is its own later item - so this is
	// deliberately empty rather than a made-up name, and the writer omits an empty attribute
	// instead of stamping every frame with a lie about which lens took it.
	p3DEngine->SetPostEffectParamString("Global_User_LensName", "");

	p3DEngine->SetPostEffectParam("Global_User_CamFocal", (float)m_lens.focalLength);

	// T-stop, not f-number: the T-stop is what the exposure is actually computed from here, and it
	// is the number a cinematographer reads off the barrel. f-number / sqrt(transmission).
	const float transmission = clamp_tpl((float)m_lens.transmission, 0.01f, 1.0f);
	p3DEngine->SetPostEffectParam("Global_User_CamTStop", (float)m_lens.aperture / sqrtf(transmission));

	p3DEngine->SetPostEffectParam("Global_User_CamISO", (float)(int)m_body.iso);
	p3DEngine->SetPostEffectParam("Global_User_CamShutter", 1.0f / (float)max((int)m_body.shutterDenominator, 1));
	p3DEngine->SetPostEffectParam("Global_User_CamND", (float)(int)m_lens.ndFilterStops);
	p3DEngine->SetPostEffectParam("Global_User_CamEV100", ComputeSceneReferredEV100());
}

void CCinematicCameraComponent::RestoreExportMetadata()
{
	const I3DEngine* p3DEngine = gEnv->p3DEngine;
	p3DEngine->SetPostEffectParamString("Global_User_CamName", "");
	p3DEngine->SetPostEffectParamString("Global_User_LensName", "");
	p3DEngine->SetPostEffectParam("Global_User_CamFocal", 0.0f);
	p3DEngine->SetPostEffectParam("Global_User_CamTStop", 0.0f);
	p3DEngine->SetPostEffectParam("Global_User_CamISO", 0.0f);
	p3DEngine->SetPostEffectParam("Global_User_CamShutter", 0.0f);
	p3DEngine->SetPostEffectParam("Global_User_CamND", 0.0f);
	p3DEngine->SetPostEffectParam("Global_User_CamEV100", 0.0f);
}

void CCinematicCameraComponent::RestoreSceneExposure()
{
	if (!m_bSceneExposureSaved)
		return;

	gEnv->p3DEngine->SetPostEffectParam("Global_User_SceneExposure", m_savedSceneExposure);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_SensorClipStops", m_savedSensorClipStops);

	// Back to the bus defaults rather than to a saved value: nothing but a Cinematic Camera ever
	// writes these, and the metering readouts describe a measurement that is no longer being made.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterMode", 1.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterSkyWeight", 0.25f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterBandLow", 0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterBandHigh", 0.0f);
	gEnv->p3DEngine->SetPostEffectParam("Global_User_MeterTargetEV", 0.0f);

	m_bSceneExposureSaved = false;
}

// EV100 the camera exposes at while it owns the exposure.
//
// Manual is the pure photographic triangle: aperture, shutter, ISO, transmission, ND and EV
// compensation, and nothing else. Exposure Response and Neutral EV, which used to compress the
// applied EV around a pivot, have no role there - that compression was a second tone curve, and
// under an output transform the S curve belongs to the ODT (spec D11).
//
// The two auto modes are the camera's AUTO with the meanings decisions/s1-auto-mode.md part 2
// gives them, which is what their names have said all along:
//   AutoBiased       = AUTO with an EV bias of (manual EV100 - Neutral EV). At the shipped
//                      defaults (f/5.6, 1/50, ISO 400 -> EV100 8.615; Neutral EV 9.0) that is a
//                      bias of -0.385 stops, so the mode stays very nearly transparent and the
//                      lens still visibly biases the picture, exactly as before.
//   AutoCompensation = AUTO with exposure compensation only.
// Neither can change the aperture's effect on brightness the way Manual does - that is the point
// of the distinction, and it is what a camera's mode dial does.

float CCinematicCameraComponent::ComputeSceneReferredEV100()
{
	const float manualEV = ComputeManualEV100();

	if (m_exposure.exposureMode == EExposureMode::Manual)
	{
		m_autoMeter.hold = CineCam::eMeterHold_Manual;
		m_autoMeter.targetEV = 0.0f;
		if (fabs_tpl((float)m_exposure.exposureResponse - 1.0f) > 0.001f && !m_bLoggedSceneRefResponseIgnored)
		{
			m_bLoggedSceneRefResponseIgnored = true;
			CryLog("[CinematicCamera] Scene Referred: Exposure Response (%.2f) and Neutral EV (%.1f) do not "
			       "apply in Manual - exposure is the physical triangle and the tone curve lives in the "
			       "output transform. They drive the AUTO modes instead.",
			       (float)m_exposure.exposureResponse, (float)m_exposure.neutralEV);
		}
		return manualEV;
	}

	// ----- AUTO -----------------------------------------------------------------------------
	// The meter itself lives in CineCamShared.cpp, because the plugin-owned editor-look publisher
	// (the Sandbox viewport's built-in "CineCam" camera) meters the same scene the same way. There
	// is one definition of what the scene measures; the mode dial, the bias and the EV limits
	// below are what a camera component adds to it.
	CineCam::SAutoMeterSettings meter;
	meter.bandLowPercent  = (float)m_exposure.meterMinPercent;
	meter.bandHighPercent = (float)m_exposure.meterMaxPercent;
	meter.tau             = (float)m_exposure.autoTau;
	meter.darkenTime      = (float)m_exposure.autoDarkenTime;
	meter.response        = (float)m_exposure.exposureResponse;
	meter.bLock           = m_exposure.bAutoLock;

	float ev = 0.0f;
	if (!CineCam::UpdateAutoMeteredEV(meter, m_autoMeter, ev))
	{
		// Nothing measured yet (first frames, or the meter is stalled): expose as if Manual rather
		// than at EV 0, which would be a white frame.
		return manualEV;
	}

	if (m_exposure.exposureMode == EExposureMode::AutoBiased)
		ev += manualEV - (float)m_exposure.neutralEV;   // EV compensation is already inside manualEV
	else
		ev -= (float)m_exposure.evCompensation;         // AutoCompensation: EC only

	// The camera's own EV limits. Unlike the time-of-day window these are the operator's, they
	// are applied AFTER the bias, and they are what stops AUTO walking off the end of the world
	// in a scene with real light ratios.
	const float evMin = min((float)m_exposure.autoEVMin, (float)m_exposure.autoEVMax);
	const float evMax = max((float)m_exposure.autoEVMin, (float)m_exposure.autoEVMax);
	return clamp_tpl(clamp_tpl(ev, evMin, evMax), -6.0f, 24.0f);
}

// The linear scale the renderer multiplies scene light by, so that a correctly exposed 18 %
// reflector lands on 0.18 in the HDR target (spec D6, decisions/s1-exposure-constant.md
// approach A).
//
// The saturation-based film sensitivity model, unchanged from CE's own tone mapper and from
// Frostbite: the luminance that just saturates the sensor is
//
//     L_sat = (78 / (q * S)) * N^2 / t = (120 / S) * 2^EV100   cd/m^2,   q = 0.65
//
// which at S = ISO 100 is L_sat = 1.2 * 2^EV100. Scene values are engine units, 1.0 = 10 000
// cd/m^2, so a value v is L = v * LIGHT_UNIT_SCALE and the scale that maps L_sat to 1.0 is
//
//     scale = LIGHT_UNIT_SCALE / (1.2 * 2^EV100).
//
// Where 0.18 comes in: a correctly metered camera reads EV100 = log2(E * S / C) from the scene
// illuminance E, C being the ISO 2720 incident-meter constant. We adopt C = 120*pi (CE's shader
// ships C = 330, a real dome-receptor value, which puts mid grey at 0.158). With C = 120*pi and
// S = 100,
//
//     2^EV100 = E * 100 / (120*pi)  =>  L_sat = 1.2 * E * 100 / (120*pi) = E / pi,
//
// and a Lambertian 18 % reflector under that illuminance has L = 0.18 * E / pi, so
// L / L_sat = 0.18 exactly. The 1.2 is left alone deliberately: it is 120/S, so ISO keeps
// meaning ISO. The constant itself only enters when the camera METERS (the AUTO loop); in
// Manual the EV comes from the triangle and C is the statement of what 'correctly exposed'
// means. C = 120*pi = 376.99 is an implementation detail - the contract is '18 % grey -> 0.18'.
float CCinematicCameraComponent::ComputeSceneReferredExposureScale()
{
	// Only while the component actually asks for the scene-referred path. Off it the renderer
	// latches 1.0 anyway, but publishing 1.0 keeps the bus value honest for anyone reading it.
	if (!m_sceneReferred.bSceneReferred)
		return 1.0f;

	const float ev100 = ComputeSceneReferredEV100();
	return RENDERER_LIGHT_UNIT_SCALE / (1.2f * powf(2.0f, ev100));
}

// Viewfinder overlays: frame guides and focus peaking. These are monitor markings, never part
// of the picture, so they are editor-live like the lens character and default to editor-only -
// a recording made in game mode is clean unless Overlays In Game Mode says otherwise.
void CCinematicCameraComponent::ApplyViewfinder()
{
	// Single writer: only the active cinematic camera may drive the shared render state.
	if (!CanDriveRenderState())
		return;

	ICVar* pGuides       = gEnv->pConsole->GetCVar("r_LensGuides");
	ICVar* pGuideAspect  = gEnv->pConsole->GetCVar("r_LensGuideAspect");
	ICVar* pGuideOpacity = gEnv->pConsole->GetCVar("r_LensGuideOpacity");
	ICVar* pPeaking      = gEnv->pConsole->GetCVar("r_LensPeaking");
	ICVar* pPeakingNear  = gEnv->pConsole->GetCVar("r_LensPeakingNear");
	ICVar* pPeakingFar   = gEnv->pConsole->GetCVar("r_LensPeakingFar");
	ICVar* pPeakingColor = gEnv->pConsole->GetCVar("r_LensPeakingColor");
	ICVar* pOverlays     = gEnv->pConsole->GetCVar("r_LensOverlays");
	if (pGuides == nullptr || pPeaking == nullptr || pOverlays == nullptr)
		return;

	if (!m_bViewfinderSaved)
	{
		m_savedGuides       = pGuides->GetIVal();
		m_savedGuideAspect  = pGuideAspect ? pGuideAspect->GetFVal() : 2.39f;
		m_savedGuideOpacity = pGuideOpacity ? pGuideOpacity->GetFVal() : 0.6f;
		m_savedPeaking      = pPeaking->GetFVal();
		m_savedPeakingNear  = pPeakingNear ? pPeakingNear->GetFVal() : 0.0f;
		m_savedPeakingFar   = pPeakingFar ? pPeakingFar->GetFVal() : 0.0f;
		m_savedPeakingColor = pPeakingColor ? pPeakingColor->GetIVal() : 1;
		m_savedOverlays     = pOverlays->GetIVal();
		m_bViewfinderSaved  = true;
	}

	int guides = 0;
	if (m_viewfinder.bShowGuides)
	{
		if (m_viewfinder.bThirds)      guides |= 1;
		if (m_viewfinder.bCentre)      guides |= 2;
		if (m_viewfinder.bSafeAreas)   guides |= 4;
		if (m_viewfinder.bAspectGuide) guides |= 8;
	}
	if (pGuides->GetIVal() != guides)
		pGuides->Set(guides);

	if (pGuideAspect && fabs_tpl(pGuideAspect->GetFVal() - (float)m_viewfinder.guideAspect) > 0.001f)
		pGuideAspect->Set((float)m_viewfinder.guideAspect);
	if (pGuideOpacity && fabs_tpl(pGuideOpacity->GetFVal() - (float)m_viewfinder.guideOpacity) > 0.001f)
		pGuideOpacity->Set((float)m_viewfinder.guideOpacity);

	const float peaking = (float)m_viewfinder.peaking;
	if (fabs_tpl(pPeaking->GetFVal() - peaking) > 0.001f)
		pPeaking->Set(peaking);
	if (pPeakingColor && pPeakingColor->GetIVal() != (int)m_viewfinder.peakingColour)
		pPeakingColor->Set((int)m_viewfinder.peakingColour);

	// The in-focus band IS this camera's thin-lens depth of field, so it has to be re-pushed
	// every frame: a focus pull moves both limits. Pushed whatever the strength, so that
	// turning peaking on from the console finds a real band instead of the 0/0 default -
	// which reads as "no band" and shows nothing.
	if (pPeakingNear && pPeakingFar)
	{
		float dofNear = 0.0f, dofFar = 0.0f;
		ComputeDOFBounds(dofNear, dofFar);
		if (fabs_tpl(pPeakingNear->GetFVal() - dofNear) > 0.0001f)
			pPeakingNear->Set(dofNear);
		if (fabs_tpl(pPeakingFar->GetFVal() - dofFar) > 0.0001f)
			pPeakingFar->Set(dofFar);
	}

	// The recording kill switch: overlays live in the editor, and only reach game / simulation
	// mode when the operator explicitly asks for them.
	const int overlays = (gEnv->IsEditing() || m_viewfinder.bOverlaysInGame) ? 1 : 0;
	if (pOverlays->GetIVal() != overlays)
		pOverlays->Set(overlays);

	if (overlays != 0)
		DrawViewfinderText();
}

// The legend a monitor prints next to its markings: what aspect the framing guide is, and
// one line of the state the operator is actually pulling. Aux text, so it is an overlay in
// exactly the same sense as the guides - never part of the picture.
void CCinematicCameraComponent::DrawViewfinderText() const
{
	if (!m_viewfinder.bShowGuides || gEnv->pRenderer == nullptr)
		return;

	const float w = (float)gEnv->pRenderer->GetOverlayWidth();
	const float h = (float)gEnv->pRenderer->GetOverlayHeight();
	if (w < 1.0f || h < 1.0f)
		return;

	const float opacity = clamp_tpl((float)m_viewfinder.guideOpacity, 0.0f, 1.0f);
	const float colour[4] = { 1.0f, 1.0f, 1.0f, opacity };
	const float fontSize = 1.3f;
	const float margin = 8.0f;

	// Aspect of the framing guide, printed just inside the top left corner of its band -
	// the same band the shader outlines, so the two cannot drift apart.
	if (m_viewfinder.bAspectGuide)
	{
		const float viewAspect  = w / h;
		const float guideAspect = max((float)m_viewfinder.guideAspect, 0.01f);
		const float heightFrac  = (guideAspect > viewAspect) ? (viewAspect / guideAspect) : 1.0f;
		const float widthFrac   = (guideAspect < viewAspect) ? (guideAspect / viewAspect) : 1.0f;
		const float bandLeft    = 0.5f * (1.0f - widthFrac) * w;
		const float bandTop     = 0.5f * (1.0f - heightFrac) * h;
		IRenderAuxText::Draw2dLabel(bandLeft + margin, bandTop + margin, fontSize, colour, false,
		                            "%.2f:1", guideAspect);
	}

	// Framing readout, bottom left. The stop follows the glass: a lens that loses light is
	// pulled by its T-stop, so that is what gets printed once transmission is not unity.
	const float transmission = clamp_tpl((float)m_lens.transmission, 0.25f, 1.0f) * ComputeMaskTransmission();
	const float N = max((float)m_lens.aperture, 0.7f);

	const char* szSensor = "Custom";
	switch (m_body.sensorPreset)
	{
	case ESensorPreset::FullFrame35mm:   szSensor = "Full Frame";        break;
	case ESensorPreset::APSC:            szSensor = "APS-C";             break;
	case ESensorPreset::MicroFourThirds: szSensor = "Micro Four Thirds"; break;
	default:                                                             break;
	}

	const float lineY = h - margin - 18.0f;
	if (transmission < 0.999f)
	{
		IRenderAuxText::Draw2dLabel(margin, lineY, fontSize, colour, false,
		                            "%.0fmm  T%.1f  focus %.2fm  %s",
		                            GetEffectiveFocalLength(), N / sqrtf(transmission),
		                            (float)m_lens.focusDistance, szSensor);
	}
	else
	{
		IRenderAuxText::Draw2dLabel(margin, lineY, fontSize, colour, false,
		                            "%.0fmm  f/%.1f  focus %.2fm  %s",
		                            GetEffectiveFocalLength(), N,
		                            (float)m_lens.focusDistance, szSensor);
	}
}

void CCinematicCameraComponent::RestoreViewfinder()
{
	if (!m_bViewfinderSaved)
		return;

	if (ICVar* pGuides = gEnv->pConsole->GetCVar("r_LensGuides"))
		pGuides->Set(m_savedGuides);
	if (ICVar* pGuideAspect = gEnv->pConsole->GetCVar("r_LensGuideAspect"))
		pGuideAspect->Set(m_savedGuideAspect);
	if (ICVar* pGuideOpacity = gEnv->pConsole->GetCVar("r_LensGuideOpacity"))
		pGuideOpacity->Set(m_savedGuideOpacity);
	if (ICVar* pPeaking = gEnv->pConsole->GetCVar("r_LensPeaking"))
		pPeaking->Set(m_savedPeaking);
	if (ICVar* pPeakingNear = gEnv->pConsole->GetCVar("r_LensPeakingNear"))
		pPeakingNear->Set(m_savedPeakingNear);
	if (ICVar* pPeakingFar = gEnv->pConsole->GetCVar("r_LensPeakingFar"))
		pPeakingFar->Set(m_savedPeakingFar);
	if (ICVar* pPeakingColor = gEnv->pConsole->GetCVar("r_LensPeakingColor"))
		pPeakingColor->Set(m_savedPeakingColor);
	if (ICVar* pOverlays = gEnv->pConsole->GetCVar("r_LensOverlays"))
		pOverlays->Set(m_savedOverlays);
	m_bViewfinderSaved = false;
}

void CCinematicCameraComponent::RestoreMotionBlur()
{
	if (!m_bMBParamsSaved)
		return;

	if (ICVar* pShutter = gEnv->pConsole->GetCVar("r_MotionBlurShutterSpeed"))
		pShutter->Set(m_savedMBShutter);
	if (ICVar* pMode = gEnv->pConsole->GetCVar("r_MotionBlur"))
		pMode->Set(m_savedMBMode);
	m_bMBParamsSaved = false;
}

// Sprite bokeh is a renderer mode switch (r_DepthOfFieldMode 0) plus the shape texture the
// splat shader intersects with the iris polygon. Saved and restored like the motion blur
// cvars, so leaving the camera never strands the renderer in a mode nobody asked for.
void CCinematicCameraComponent::ApplySpriteBokeh()
{
	// Single writer: only the active cinematic camera may drive the shared render state.
	// FinalizeGameCamera already only runs on the active component, so this is an assertion
	// of that invariant rather than a new gate - but the shape texture is a global post
	// param, and two cameras pushing different names would fight frame by frame.
	if (!CanDriveRenderState())
		return;

	ICVar* pDofMode = gEnv->pConsole->GetCVar("r_DepthOfFieldMode");
	if (pDofMode == nullptr)
		return;

	// The property drives the cvar, with the usual save/restore.
	if (m_lens.bUseSpriteBokeh)
	{
		if (!m_bSpriteBokehSaved)
		{
			m_savedDofModeCVar = pDofMode->GetIVal();
			m_bSpriteBokehSaved = true;
		}

		if (pDofMode->GetIVal() != 0)
			pDofMode->Set(0);
	}
	else if (m_bSpriteBokehSaved)
	{
		RestoreSpriteBokeh();
	}

	// The shape texture follows the PROPERTY whenever the hybrid splats are actually running.
	// Deliberately keyed off the effective cvar rather than off our own property, because
	// r_DepthOfFieldMode 0 set straight from the console is just as valid a way to get here.
	if (pDofMode->GetIVal() != 0)
		return;

	// Written EVERY frame, deliberately. CParamTexture keeps one slot per render command
	// buffer and hands the render thread whichever slot is current that frame, so a name
	// written once only ever reaches one of the two slots - the other keeps its previous
	// value indefinitely and the render thread then alternates between the two. Writing every
	// frame is what keeps both slots converged; Create() early-outs on a matching name, so
	// the steady state costs a single string compare.
	m_bBokehShapeTexPushed = !m_lens.bokehShapeTex.value.empty();
	gEnv->p3DEngine->SetPostEffectParamString("Dof_User_BokehShapeTex", m_lens.bokehShapeTex.value.c_str());
}

void CCinematicCameraComponent::RestoreSpriteBokeh()
{
	if (m_bSpriteBokehSaved)
	{
		if (ICVar* pDofMode = gEnv->pConsole->GetCVar("r_DepthOfFieldMode"))
			pDofMode->Set(m_savedDofModeCVar);
		m_bSpriteBokehSaved = false;
	}

	// Drop whatever shape texture we pushed. The previous value cannot be read back
	// meaningfully (the post param hands out the loaded texture's name, not the request), so
	// unbinding is the honest restore - the default state is the procedural shape anyway.
	// Only one thread slot receives this, but restore also puts r_DepthOfFieldMode back, so
	// the splat pass stops running entirely and a stale binding in the other slot is inert.
	// Re-entering mode 0 rewrites every frame and converges both slots again.
	if (m_bBokehShapeTexPushed)
	{
		m_bBokehShapeTexPushed = false;
		gEnv->p3DEngine->SetPostEffectParamString("Dof_User_BokehShapeTex", "");
	}
}

void CCinematicCameraComponent::RestoreFilmGrain()
{
	if (!m_bGrainParamSaved)
		return;

	gEnv->p3DEngine->SetPostEffectParam("FilterGrain_Amount", m_savedGrainAmount);
	m_bGrainParamSaved = false;
}

// ---------------------------------------------------------------------------
// Diffraction streaks (sunstars) — the renderer's HDR_Streaks_* post effect params.
//
// Light bends around the straight edges of the iris blades, so every blade edge throws a
// pair of rays. With an even blade count the opposing pairs overlap and you get one ray per
// blade; an odd count leaves them offset and you get twice the blade count. The ray set is
// clamped to [4, 24]; the renderer draws them as symmetric line pairs —
// that is the documented ceiling, not a defect.
//
// Stopping down is what makes the rays visible: the aperture edge becomes a larger share of
// the light path. Nothing below f/5.6, faintly visible from f/8, sharp and long by f/16.
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::ApplyStreaks()
{
	if (!CanDriveRenderState())
		return;

	// The baseline latch covers BOTH parameter sets: the anamorphic flare is pushed even when
	// the diffraction streaks are switched off, so it cannot be saved under the blade branch.
	if (!m_bStreakParamsSaved)
	{
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Active", m_savedStreaksActive);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Amount", m_savedStreaksAmount);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Count", m_savedStreaksCount);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Angle", m_savedStreaksAngle);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Length", m_savedStreaksLength);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Mode", m_savedStreaksMode);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_WaveAmount", m_savedStreaksWaveAmount);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_WaveReach", m_savedStreaksWaveReach);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_Dispersion", m_savedStreaksDispersion);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_AnamorphicAmount", m_savedAnamorphicAmount);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_AnamorphicLength", m_savedAnamorphicLength);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Streaks_AnamorphicThreshold", m_savedAnamorphicThreshold);
		gEnv->p3DEngine->GetPostEffectParamVec4("HDR_Streaks_AnamorphicTint", m_savedAnamorphicTint);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Filter_Amount", m_savedFilterAmount);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Filter_Size", m_savedFilterSize);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Filter_Rotation", m_savedFilterRotation);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Filter_Threshold", m_savedFilterThreshold);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Filter_Points", m_savedFilterPoints);
		m_bStreakParamsSaved = true;
	}

	if (m_streaks.bEnableStreaks)
	{
		// Diffraction physics: an even blade count folds opposing edges onto the same spikes
		// (N spikes); an odd count cannot, giving 2N. The renderer draws them as symmetric
		// line pairs, so high counts stay affordable.
		const int blades = (int)m_lens.apertureBlades;
		const float rayCount = (float)clamp_tpl((blades % 2 == 0) ? blades : blades * 2, 4, 24);

		// Brightness saturates early (fully lit by f/11) so that from there on stopping down
		// reads as the spikes GROWING, not fading in - the length curve owns f/7 to f/22.
		const float N = max((float)m_lens.aperture, 0.7f);
		const float amount = powf(clamp_tpl((N - 6.3f) / (11.0f - 6.3f), 0.0f, 1.0f), 1.2f);
		const float length = clamp_tpl((N - 7.0f) / (22.0f - 7.0f), 0.0f, 1.0f);

		// The ACTIVE flag is the renderer's master gate for the whole streak block, and the two
		// models do not agree on what switches it on.
		//
		//   Analytic - the f-number ramp IS the model's amount, so amount 0 means "draw nothing".
		//   Wave     - the ramp does not exist. The pattern's size on the sensor is lambda * N,
		//              so stopping down SPREADS the same energy instead of fading it in; the
		//              physics is continuous in N and there is no aperture below which the glare
		//              should be switched off. Gating Wave on the analytic ramp made the whole
		//              block dead below f/6.3 and switch on hard at f/6.4 - a pop that belongs
		//              to the old model and to nothing in this one.
		const bool  bWaveMode = (m_streaks.mode == EStreakMode::Wave);
		const bool  bActive = bWaveMode ? ((float)m_streaks.streakAmount > 0.001f)
		                                : (amount > 0.001f);

		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Active", bActive ? 1.0f : 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Amount", amount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Count", rayCount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Angle", DEG2RAD((float)m_streaks.bladeRotation));
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Length", length);

		// Wave optics (DiffractionKernelSpec.md). The renderer picks ONE of the two models from
		// the mode switch, but both parameter sets are pushed every frame: the wave kernel can be
		// refused (r_HDRDiffractionKernel 0), and when it is, the analytic line pairs above have
		// to be live and correct rather than a stale copy from whenever the mode last changed.
		//
		// Note what is deliberately absent here: the f-number ramp. In the wave model stopping
		// down does not fade the pattern in, it SPREADS it - the pattern's size on the sensor is
		// lambda * N - so the same energy over a larger star is what makes rays appear as you stop
		// down. Ramping the amount on top would be counting the aperture twice. The user's Streak
		// Amount is sent through untouched, with 1 as the physical answer.
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Mode", (m_streaks.mode == EStreakMode::Wave) ? 1.0f : 0.0f);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_WaveAmount", (float)m_streaks.streakAmount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_WaveReach", (float)m_streaks.reach);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Dispersion", (float)m_streaks.dispersion);
	}
	else
	{
		// Blade streaks off: hand the diffraction params back exactly as before, and keep going -
		// the anamorphic flare is a property of the front element, not of the iris.
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Active", m_savedStreaksActive);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Amount", m_savedStreaksAmount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Count", m_savedStreaksCount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Angle", m_savedStreaksAngle);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Length", m_savedStreaksLength);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Mode", m_savedStreaksMode);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_WaveAmount", m_savedStreaksWaveAmount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_WaveReach", m_savedStreaksWaveReach);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Dispersion", m_savedStreaksDispersion);
	}

	// Anamorphic horizontal flare. The cylindrical front element throws it hardest when the
	// whole element is in the light path, so it is strongest wide open and gone by f/11 -
	// the opposite curve to the iris diffraction above.
	{
		const float N = max((float)m_lens.aperture, 0.7f);
		const float wideOpenness = clamp_tpl((11.0f - N) / (11.0f - 2.0f), 0.0f, 1.0f);
		const ColorF tint = m_anamorphic.flareTint;

		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_AnamorphicAmount", (float)m_anamorphic.flareAmount * wideOpenness);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_AnamorphicLength", (float)m_anamorphic.flareLength);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_AnamorphicThreshold", (float)m_anamorphic.flareThreshold);
		gEnv->p3DEngine->SetPostEffectParamVec4("HDR_Streaks_AnamorphicTint", Vec4(tint.r, tint.g, tint.b, 0.0f));
	}

	// Front filter PSF. A plate screwed onto the front of the lens: it convolves every bright
	// point with one fixed pattern at EVERY f-number, so - unlike both effects above - nothing
	// here is modulated by the aperture and the properties are pushed exactly as authored.
	{
		gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Amount", (float)m_filter.starAmount);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Size", (float)m_filter.starSize);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Rotation", DEG2RAD((float)m_filter.starRotation));
		gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Threshold", (float)m_filter.starThreshold);
		gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Points", (float)(int)m_filter.starPoints);

		// Written EVERY frame, for the same reason the bokeh shape texture is: CParamTexture
		// keeps one slot per render command buffer and a name written once only ever reaches
		// one of them, leaving the render thread alternating between the new name and a stale
		// one. Create() early-outs on a matching name, so the steady state is a string compare.
		m_bFilterPSFTexPushed = !m_filter.psfTexture.value.empty();
		gEnv->p3DEngine->SetPostEffectParamString("HDR_Filter_PSFTex", m_filter.psfTexture.value.c_str());
	}
}

void CCinematicCameraComponent::RestoreStreaks()
{
	if (!m_bStreakParamsSaved)
		return;

	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Active", m_savedStreaksActive);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Amount", m_savedStreaksAmount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Count", m_savedStreaksCount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Angle", m_savedStreaksAngle);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Length", m_savedStreaksLength);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Mode", m_savedStreaksMode);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_WaveAmount", m_savedStreaksWaveAmount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_WaveReach", m_savedStreaksWaveReach);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_Dispersion", m_savedStreaksDispersion);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_AnamorphicAmount", m_savedAnamorphicAmount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_AnamorphicLength", m_savedAnamorphicLength);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Streaks_AnamorphicThreshold", m_savedAnamorphicThreshold);
	gEnv->p3DEngine->SetPostEffectParamVec4("HDR_Streaks_AnamorphicTint", m_savedAnamorphicTint);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Amount", m_savedFilterAmount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Size", m_savedFilterSize);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Rotation", m_savedFilterRotation);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Threshold", m_savedFilterThreshold);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Filter_Points", m_savedFilterPoints);

	// Drop whatever PSF image we pushed. The previous value cannot be read back meaningfully
	// (the post param hands out the loaded texture's name, not the request), so unbinding is
	// the honest restore - and the amount above has just gone back to its baseline, so with
	// amount 0 the pass does not run at all and a stale binding in the other thread slot is
	// inert until the next Apply rewrites both.
	if (m_bFilterPSFTexPushed)
	{
		m_bFilterPSFTexPushed = false;
		gEnv->p3DEngine->SetPostEffectParamString("HDR_Filter_PSFTex", "");
	}

	m_bStreakParamsSaved = false;
}

// ---------------------------------------------------------------------------
// Film halation
// ---------------------------------------------------------------------------
// A source bright enough to overload the emulsion sends light straight through it; the light
// reflects off the film base and re-exposes the emulsion from behind, in a halo far wider than
// the source itself. The red-sensitive layer sits deepest, so what comes back reads red-orange.
//
// This is a property of the FILM STOCK, not of the lens: unlike the diffraction streaks and the
// anamorphic flare nothing here is modulated by the f-number, and the properties are pushed to
// the renderer exactly as authored.
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::ApplyHalation()
{
	if (!CanDriveRenderState())
		return;

	if (!m_bHalationSaved)
	{
		gEnv->p3DEngine->GetPostEffectParam("HDR_Halation_Amount", m_savedHalationAmount);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Halation_Threshold", m_savedHalationThreshold);
		gEnv->p3DEngine->GetPostEffectParam("HDR_Halation_Radius", m_savedHalationRadius);
		gEnv->p3DEngine->GetPostEffectParamVec4("HDR_Halation_Tint", m_savedHalationTint);
		m_bHalationSaved = true;
	}

	const ColorF tint = m_halation.tint;

	gEnv->p3DEngine->SetPostEffectParam("HDR_Halation_Amount", (float)m_halation.amount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Halation_Threshold", (float)m_halation.threshold);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Halation_Radius", (float)m_halation.radius);
	gEnv->p3DEngine->SetPostEffectParamVec4("HDR_Halation_Tint", Vec4(tint.r, tint.g, tint.b, 0.0f));
}

void CCinematicCameraComponent::RestoreHalation()
{
	if (!m_bHalationSaved)
		return;

	gEnv->p3DEngine->SetPostEffectParam("HDR_Halation_Amount", m_savedHalationAmount);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Halation_Threshold", m_savedHalationThreshold);
	gEnv->p3DEngine->SetPostEffectParam("HDR_Halation_Radius", m_savedHalationRadius);
	gEnv->p3DEngine->SetPostEffectParamVec4("HDR_Halation_Tint", m_savedHalationTint);
	m_bHalationSaved = false;
}

// ---------------------------------------------------------------------------
// Sun shafts - retired on ReCS in favour of volumetric fog (2026-09-09).
//
// r_sunshafts defaults to 0 on ReCS, but the engine's sys_spec CVar groups
// (Engine/Config/CVarGroups/sys_spec_PostProcessing.cfg, `r_SunShafts=2` in the [default]
// block) re-apply the stock value at startup, so the code default alone is not seen. The cvar is
// VF_DUMPTODISK and global (decisions/s0-master-switch.md), so the camera does not write it.
//
// First attempt (92a2236c) pinned the TOD's `SunShafts_Active` to 0 on the bus. That did nothing:
// `CSunShaftsStage::IsStageActive` is `r_sunshafts && r_PostProcess` and never reads the param
// (the `CSunShafts::Preprocess` that once ANDed it with the cvar is not on the pipeline's path in
// 5.7 - only Init/Release/Reset survive), and the TOD's visibility sliders reach the picture
// through `SunShafts_Amount` / `SunShafts_RaysAmount`, which the TOD republishes on every
// environment update. So the camera now publishes its OWN request, `Global_User_SunShaftsSuppressed`,
// which the renderer ANDs into `IsStageActive` - the same shape as every other Global_User_
// request the lens model makes. Nothing of the TOD's is touched or fought over.
//
// Re-asserted per frame from the plugin's BeforeFinalizeCamera hook so a ResetPostEffects (level
// load, RESET into game) cannot leave the stage on under an active camera; cleared on deactivate.
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::ApplySunShaftsOff()
{
	if (!CanDriveRenderState())
		return;

	if (!m_bSunShaftsSaved)
	{
		m_bSunShaftsSaved = true;
		CryLog("[CinematicCamera] sun shafts off (retired in favour of volumetric fog)");
	}

	gEnv->p3DEngine->SetPostEffectParam("Global_User_SunShaftsSuppressed", 1.0f);
}

void CCinematicCameraComponent::RestoreSunShafts()
{
	if (!m_bSunShaftsSaved)
		return;

	// 0 = stock gate (cvars only); the TOD's own params were never written, nothing to give back.
	gEnv->p3DEngine->SetPostEffectParam("Global_User_SunShaftsSuppressed", 0.0f);
	m_bSunShaftsSaved = false;
}

// ---------------------------------------------------------------------------
// Exposure — called from CCinematicCameraPlugin::UpdateBeforeFinalizeCamera()
// which runs AFTER ITimeOfDay::Update() so our write is the last one before render.
// ---------------------------------------------------------------------------
// Manual exposure: the standard photographic equation.
//     EV100 = log2(N^2 / t) - log2(ISO / 100)
// EV compensation is subtracted so +1 EV means one stop brighter, matching a real camera.
// The stops of light the aperture and the shutter let through: log2(T^2 / t), where
// T^2 = N^2 / transmission. Radiometry follows the T-stop, not the f-stop, so glass losses
// darken the image exactly like a real lens while geometry (DOF, bokeh, diffraction) stays on N.
// A shape mask past its crossover is a smaller hole with part of it blocked off: it darkens the
// image through the same term, while N (the iris) keeps driving the renderer's domain call.
//
// This is the one definition. The exposure and the film-grain light term used to compute it
// separately, which meant a change to one silently desynchronised the other.
float CCinematicCameraComponent::ComputeApertureShutterStops() const
{
	const float N = max((float)m_lens.aperture, 0.7f);
	const float t = 1.0f / (float)max((int)m_body.shutterDenominator, 1);
	const float transmission = clamp_tpl((float)m_lens.transmission, 0.25f, 1.0f) * ComputeMaskTransmission();

	return log2f(N * N / (transmission * t));
}

float CCinematicCameraComponent::ComputeManualEV100() const
{
	const float iso = (float)max((int)m_body.iso, 1);

	const float ev100 = ComputeApertureShutterStops() - log2f(iso / 100.0f) - (float)m_exposure.evCompensation + (float)(int)m_lens.ndFilterStops;
	return clamp_tpl(ev100, -6.0f, 24.0f);
}

// Take over the view for the current frame. Runs after entity updates AND after the
// GameSDK ViewSystem, so this write is the one the renderer actually uses - this is what
// makes "view through the cinematic camera" work in game mode at all.
void CCinematicCameraComponent::FinalizeGameCamera()
{
	// In the editor (editing or simulation mode) the viewport owns the camera: it writes the
	// system view camera itself immediately before RenderWorld, so a write from here would be
	// discarded. Only the VIEW WRITE is game-only, though - the effect list below it is applied in
	// the editor too, for the viewport's preview owner, from the plugin's resolver
	// (decisions/s9-editor-cinecam-preview.md). The gate therefore stays exactly where it was and
	// this function is unchanged for game mode, step for step.
	if (gEnv->IsEditing())
		return;

	UpdateFocalMotor(gEnv->pTimer->GetFrameTime());

	CCamera camera = gEnv->pSystem->GetViewCamera();
	camera.SetFrustum(camera.GetViewSurfaceX(), camera.GetViewSurfaceZ(),
		ComputeVerticalFOV(), m_body.nearPlane, m_body.farPlane, camera.GetPixelAspectRatio());
	camera.SetMatrix(GetWorldTransformMatrix());
	gEnv->pSystem->SetViewCamera(camera);

	ApplyGameOnlyEffects();
}

// The effect list that used to sit inside FinalizeGameCamera(), in the same order. In the editor
// the focal motor and the sibling FOV are driven by ENTITY_EVENT_UPDATE instead, so what is left
// here really is only the effects, and the preview owner can be handed exactly this.
void CCinematicCameraComponent::ApplyGameOnlyEffects()
{
	// Keep DOF pinned every frame from here as well, so it no longer depends on whether
	// the sibling CCameraComponent won the camera-manager race.
	if (m_dof.bEnableDOF)
		ApplyDOF();

	ApplyFilmGrain();
	ApplyHalation();
	ApplyMotionBlur();
	ApplySpriteBokeh();
	ApplyWhiteBalance();
	ApplyStreaks();
	ApplyLensCharacter();
	ApplyViewfinder();
}

void CCinematicCameraComponent::ApplyExposureGlobalParam()
{
	if (!CanDriveRenderState())
		return;

	// Diagnostic: report the effective T-stop whenever the lens changes. The edge-trigger state is
	// per component - function statics would be shared by every cinematic camera in the level.
	{
		const float N = max((float)m_lens.aperture, 0.7f);
		const float trans = clamp_tpl((float)m_lens.transmission, 0.25f, 1.0f) * ComputeMaskTransmission();
		if (fabs_tpl(N - m_lastLoggedApertureN) > 0.05f || fabs_tpl(trans - m_lastLoggedTransmission) > 0.005f)
		{
			m_lastLoggedApertureN = N;
			m_lastLoggedTransmission = trans;
			if (m_lens.bUseSpriteBokeh && !m_lens.bokehShapeTex.value.empty())
				CryLog("[CinematicCamera] f/%.1f with mask f/%.1f (iris/mask radius %.2f, tint %.0f%%) -> effective transmission %.2f, T%.1f, CoC from f/%.1f",
				       N, (float)m_lens.maskAperture, (float)m_lens.maskAperture / N, (float)m_lens.maskOpenArea * 100.0f, trans, N / sqrtf(trans), GetGeometricAperture());
			else
				CryLog("[CinematicCamera] f/%.1f at transmission %.2f -> T%.1f", N, trans, N / sqrtf(trans));
		}
	}

	// With the scene-referred path requested the camera ALWAYS owns the exposure, whether or not
	// Enable Exposure Control is ticked: the tone map applies unit exposure on that path, so a
	// camera that published nothing would render raw scene radiance and the frame would be black.
	// Off the switch the tick keeps its old meaning exactly.
	const bool bSceneReferred = m_sceneReferred.bSceneReferred;
	if (!m_exposure.bEnableExposure && !bSceneReferred)
	{
		RestoreExposure();
		return;
	}

	// Read the full EyeAdaptation vector (EV_min, EV_max, EV_auto_compensation).
	Vec3 v;
	gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_HDR_EYEADAPTATION_PARAMS, v);

	if (!m_bExpParamsSaved)
	{
		m_savedEyeAdaptation = v;
		m_baseEyeAdaptationWindow = v;
		m_bExpParamsSaved    = true;
	}

	if (bSceneReferred)
	{
		// The camera owns the exposure outright: it is applied on the light side and the tone map
		// applies none of its own. This write is no longer what produces the picture - the tone
		// map's scene-referred branch pins its own window - but the EV triple is still read by the
		// particle system's ExposureValue domain, so it is kept meaningful and consistent.
		//
		// .z = 0 is mandatory (research/s1-exposure-model.md section 7, item 5): the shader's
		// :807 scene-key tilt is a non-photographic term worth up to +-2 stops of invisible,
		// per-scene drift, and today Manual mode silently inherits whatever the time of day wrote
		// there. A camera has no such term.
		const float ev100 = ComputeSceneReferredEV100();
		v.x = clamp_tpl(ev100, -6.0f, 24.0f);
		v.y = v.x;
		v.z = 0.0f;
	}
	else if (m_exposure.exposureMode == EExposureMode::AutoBiased)
	{
		// Game-engine exposure: the engine's night is only a few stops darker than its day
		// (artist-lit, not radiometric), so an absolute EV lock blows out night scenes. Keep
		// the scene's own adaptation window and SHIFT it by the camera's relative stops -
		// auto exposure keeps tracking the scene, the lens biases it brighter or darker.
		const float bias = (ComputeManualEV100() - (float)m_exposure.neutralEV) * (float)m_exposure.exposureResponse;

		// Base tracks external (TOD) writes so the per-frame shift never compounds.
		if (!v.IsEquivalent(m_lastWrittenEyeAdaptation, 1e-5f))
			m_baseEyeAdaptationWindow = v;

		v.x = clamp_tpl(m_baseEyeAdaptationWindow.x + bias, -6.0f, 24.0f);
		v.y = clamp_tpl(m_baseEyeAdaptationWindow.y + bias, -6.0f, 24.0f);
		v.z = m_baseEyeAdaptationWindow.z;
		m_lastWrittenEyeAdaptation = v;
	}
	else if (m_exposure.exposureMode == EExposureMode::Manual)
	{
		// The tone mapper clamps its auto EV100 to [EV_min, EV_max]; collapsing that window
		// onto a single value is what pins exposure to the lens instead of the scene, and is
		// the only way the f-number can change brightness.
		//
		// Response compression: physically every stop is exactly 2x, but a processed camera
		// image runs through an S curve that rolls extremes off, which is what photographic
		// intuition is calibrated against. Compressing the applied EV around Neutral EV
		// reproduces that feel; Exposure Response 1 restores strict physics.
		const float ev100 = ComputeManualEV100();
		const float applied = (float)m_exposure.neutralEV + (ev100 - (float)m_exposure.neutralEV) * (float)m_exposure.exposureResponse;
		v.x = clamp_tpl(applied, -6.0f, 24.0f);
		v.y = v.x;
	}
	else
	{
		// Legacy behaviour: drive the scene-key auto compensation strength only.
		v.z = (float)m_exposure.evCompensation;
	}

	gEnv->p3DEngine->SetGlobalParameter(E3DPARAM_HDR_EYEADAPTATION_PARAMS, v);
}

// ---------------------------------------------------------------------------
// ICinematicCameraOptics - live lens control (driven by CryPhoneTracker)
//
// Setters take effect on the next frame through UpdateCameraComponentParams()/ApplyDOF();
// they are called at pose rate, so they only do work when the value actually moved.
// ---------------------------------------------------------------------------
void CCinematicCameraComponent::SetFocalLength(float millimetres)
{
	const float clamped = clamp_tpl(millimetres, 1.0f, 2400.0f);
	if (fabs_tpl(clamped - (float)m_lens.focalLength) < 0.001f)
		return;

	m_lens.focalLength = clamped;
	UpdateCameraComponentParams();
}

void CCinematicCameraComponent::SetAperture(float fNumber)
{
	const float clamped = clamp_tpl(fNumber, 1.0f, 64.0f);
	if (fabs_tpl(clamped - (float)m_lens.aperture) < 0.001f)
		return;

	m_lens.aperture = clamped;
	if (CanApplyViewEffects() && m_dof.bEnableDOF)
		ApplyDOF();
}

void CCinematicCameraComponent::SetFocusDistance(float metres)
{
	const float clamped = clamp_tpl(metres, 0.01f, 10000.0f);
	if (fabs_tpl(clamped - (float)m_lens.focusDistance) < 0.001f)
		return;

	m_lens.focusDistance = clamped;
	if (CanApplyViewEffects() && m_dof.bEnableDOF)
		ApplyDOF();
}

void CCinematicCameraComponent::SetEVCompensation(float ev)
{
	m_exposure.evCompensation = clamp_tpl(ev, -3.0f, 3.0f);
}

void CCinematicCameraComponent::SetISO(float iso)
{
	m_body.iso = (int)clamp_tpl(iso, 25.0f, 25600.0f);
}

void CCinematicCameraComponent::SetDepthOfFieldEnabled(bool enable)
{
	if (m_dof.bEnableDOF == enable)
		return;

	m_dof.bEnableDOF = enable;
	if (CanApplyViewEffects())
	{
		if (enable)
			ApplyDOF();
		else
			RestoreDOF();
	}
}

void CCinematicCameraComponent::SetExposureEnabled(bool enable)
{
	m_exposure.bEnableExposure = enable;
	if (!enable)
		RestoreExposure();
}
