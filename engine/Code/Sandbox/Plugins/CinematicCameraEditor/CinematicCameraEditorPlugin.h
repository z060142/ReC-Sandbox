// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "IPlugin.h"

//! The editor half of the cinematic camera (S10 item 4b): it registers the three asset types the
//! display chain's files need, and the per-file registration that turns a file someone dropped into
//! `assets/cinecam/` into one of them. Deliberately separate from the S9 editor work, which lives in
//! Sandbox itself (the viewport's CineCam camera) - this DLL has no UI, no viewport and no state, so
//! it can be deployed on its own and removed on its own.
class CCinematicCameraEditorPlugin : public IPlugin
{
public:
	CCinematicCameraEditorPlugin();
	~CCinematicCameraEditorPlugin();

	int32       GetPluginVersion() override     { return 1; }
	const char* GetPluginName() override        { return "Cinematic Camera Editor"; }
	const char* GetPluginDescription() override
	{
		return "Asset types for the cinematic camera's display chain: colour LUTs (.cube), "
		       "ASC CDLs (.cdl) and CineCam Grade presets (.cinegrade). Files dropped into "
		       "assets/cinecam register themselves.";
	}
};
