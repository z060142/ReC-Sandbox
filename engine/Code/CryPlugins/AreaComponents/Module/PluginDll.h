// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include <CrySystem/ICryPlugin.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>

namespace Cry
{
namespace AreaComponents
{

//! The engine half of the Area modernisation (scene-notes/area/decisions/01-architecture.md):
//! the shape components (Box, Sphere, Polygon, Spline) and, from stage 2, the function components
//! that bind to them (Area, water, road, ...). Everything the editor needs to talk to is declared
//! in ../Interface/IShapeComponent.h; this DLL is deliberately free of any Sandbox dependency.
class CAreaComponentsPlugin final : public Cry::IEnginePlugin
	, public ISystemEventListener
{
public:
	CRYINTERFACE_SIMPLE(Cry::IEnginePlugin)
	CRYGENERATE_SINGLETONCLASS_GUID(CAreaComponentsPlugin, "AreaComponents",
		"{4D2E9C61-8B0A-4F3D-95C7-1E6B2A70D3F4}"_cry_guid)

	virtual ~CAreaComponentsPlugin();

	// Cry::IEnginePlugin
	virtual const char* GetName()     const override { return "AreaComponents"; }
	virtual const char* GetCategory() const override { return "CryPlugins"; }
	virtual bool        Initialize(SSystemGlobalEnvironment& env, const SSystemInitParams& initParams) override;
	// ~Cry::IEnginePlugin

	// ISystemEventListener
	virtual void OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam) override;
	// ~ISystemEventListener

	//! The one Schematyc registration hook of this DLL, called from the env package callback.
	//! Same shape as CPlugin_CryDefaultEntities::RegisterComponents.
	void RegisterComponents(Schematyc::IEnvRegistrar& registrar);
};

} // namespace AreaComponents
} // namespace Cry
