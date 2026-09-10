// Sandbox editor preview surface of the Cinematic Camera plugin ("cinecam").
//
// The level-editor viewport tells the plugin, once per rendered frame, what the user is
// looking through. The plugin resolves that into "who drives the render state this frame":
//
//   Default  - nobody. The picture is stock, exactly as an engine without cinecam draws it.
//   CineCam  - the plugin's own publisher (no entity): scene referred + automatic adaptation.
//   Entity   - the CCinematicCameraComponent on that entity, with its FULL effect set.
//
// The Sandbox side resolves this the same way CryPhoneTracker resolves the optics entry point:
// CryLoadLibraryDefName("CinematicCamera") + CryGetProcAddress(CINECAM_EDITOR_PREVIEW_ENTRY).
// A missing module or a null pointer is not an error - the editor then behaves exactly stock.
#pragma once

#include <CryEntitySystem/IEntityBasicTypes.h>

class CCamera;

//! What the level-editor viewport is looking through this frame.
enum class ECineCamEditorPreviewMode : int
{
	Default = 0,   //!< the stock editor fly camera; the plugin publishes nothing
	CineCam = 1,   //!< the built-in cinecam fly camera; the plugin's own publisher drives
	Entity  = 2,   //!< a camera entity; its CCinematicCameraComponent drives (if it has one)
};

struct ICineCamEditorPreview
{
	virtual ~ICineCamEditorPreview() {}

	//! Called by the level-editor viewport once per rendered frame, in edit mode, immediately
	//! before it hands its camera to the engine. entityId is valid only for Entity mode.
	//! camera is the viewport's CCamera for this frame (Default/CineCam: the fly camera;
	//! Entity: the copy taken from the entity).
	//! A frame without a call falls back to Default after one frame of grace, so a closed
	//! viewport cannot freeze the look.
	virtual void SetViewportPreview(ECineCamEditorPreviewMode mode, EntityId entityId, const CCamera& camera) = 0;

	//! The viewport tells the plugin the preview is gone (viewport destroyed, level unload).
	//! The plugin restores stock state.
	virtual void ClearViewportPreview() = 0;
};

//! Name of the C entry point exported by CinematicCamera.dll.
#define CINECAM_EDITOR_PREVIEW_ENTRY "CryGetCineCamEditorPreview"

//! Signature of that entry point. May return nullptr before the plugin is up.
typedef ICineCamEditorPreview* (* TGetCineCamEditorPreviewFn)();

extern "C" DLL_EXPORT ICineCamEditorPreview* CryGetCineCamEditorPreview();
