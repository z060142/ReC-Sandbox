// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "IPlugin.h"

//! The editor half of the Area modernisation (scene-notes/area/decisions/02-editor-bridge.md):
//! the shape edit tools, their class descs and, later, the create-panel presets under "Area".
//! It talks to the runtime shapes only through the reflected IShapeComponent / IShapeComponentEdit
//! interfaces, so there is deliberately NO link dependency on the AreaComponents DLL.
class CAreaComponentsEditorPlugin : public IPlugin
{
public:
	CAreaComponentsEditorPlugin();
	~CAreaComponentsEditorPlugin();

	int32       GetPluginVersion() override { return 1; }
	const char* GetPluginName() override    { return "AreaComponentsEditor"; }
	const char* GetPluginDescription() override
	{
		return "Editing tools for the Area shape components: point tools, creation tool and the "
		       "\"Area\" create-panel presets.";
	}
};
