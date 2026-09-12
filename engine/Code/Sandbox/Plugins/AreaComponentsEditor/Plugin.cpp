// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "Plugin.h"

#include <CryCore/Platform/platform_impl.inl>

// Nothing to install yet: the edit tool registers itself through REGISTER_CLASS_DESC, which the
// editor drains after REGISTER_PLUGIN has installed GetIEditor() and ISystem.
CAreaComponentsEditorPlugin::CAreaComponentsEditorPlugin()
{
}

CAreaComponentsEditorPlugin::~CAreaComponentsEditorPlugin()
{
}

REGISTER_PLUGIN(CAreaComponentsEditorPlugin);
