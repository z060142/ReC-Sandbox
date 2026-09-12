// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <CryCore/Project/CryModuleDefs.h>
#include <CryCore/Platform/platform.h>

// The CryCommon helper headers EditorCommon pulls in expect the MFC/ATL shims, exactly as every
// other Sandbox plugin's precompiled header does.
#define CRY_USE_MFC
#include <CryCore/Platform/CryAtlMfc.h>

#include "EditorCommon.h"

#include <CrySystem/ISystem.h>
#include <CryExtension/CryGUID.h>
