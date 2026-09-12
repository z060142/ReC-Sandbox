// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "PluginDll.h"

#include "Shapes/BoxShapeComponent.h"
#include "Shapes/PolygonShapeComponent.h"
#include "Shapes/SphereShapeComponent.h"
#include "Shapes/SplineShapeComponent.h"

#include "Functions/AreaFunctionComponent.h"
#include "Functions/TriggerBoundsComponent.h"
#include "Functions/GravityVolumeComponent.h"
#include "Functions/DistributorComponent.h"

#include <CrySchematyc/Env/IEnvRegistry.h>
#include <CrySchematyc/Env/EnvPackage.h>
#include <CrySchematyc/Env/Elements/EnvComponent.h>
#include <CrySchematyc/Utils/SharedString.h>

#include <CryCore/StaticInstanceList.h>

// Included only once per DLL module.
#include <CryCore/Platform/platform_impl.inl>

namespace Cry
{
namespace AreaComponents
{

CAreaComponentsPlugin::~CAreaComponentsPlugin()
{
	if (gEnv->pSchematyc != nullptr)
	{
		gEnv->pSchematyc->GetEnvRegistry().DeregisterPackage(CAreaComponentsPlugin::GetCID());
	}

	if (ISystem* pSystem = GetISystem())
	{
		pSystem->GetISystemEventDispatcher()->RemoveListener(this);
	}
}

bool CAreaComponentsPlugin::Initialize(SSystemGlobalEnvironment& env, const SSystemInitParams& initParams)
{
	env.pSystem->GetISystemEventDispatcher()->RegisterListener(this, "CAreaComponentsPlugin");
	return true;
}

void CAreaComponentsPlugin::OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam)
{
	switch (event)
	{
	case ESYSTEM_EVENT_REGISTER_SCHEMATYC_ENV:
		{
			if (gEnv->pSchematyc != nullptr)
			{
				gEnv->pSchematyc->GetEnvRegistry().RegisterPackage(
					stl::make_unique<Schematyc::CEnvPackage>(
						CAreaComponentsPlugin::GetCID(),
						"AreaComponents",
						"ReC Sandbox",
						"Shape and area components",
						[this](Schematyc::IEnvRegistrar& registrar) { RegisterComponents(registrar); }
						)
					);
			}
		}
		break;
	default:
		break;
	}
}

void CAreaComponentsPlugin::RegisterComponents(Schematyc::IEnvRegistrar& registrar)
{
	Schematyc::CEnvRegistrationScope scope = registrar.Scope(IEntity::GetEntityScopeGUID());
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CBoxShapeComponent));
		CBoxShapeComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CPolygonShapeComponent));
		CPolygonShapeComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CSphereShapeComponent));
		CSphereShapeComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CSplineShapeComponent));
		CSplineShapeComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CAreaFunctionComponent));
		CAreaFunctionComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CTriggerBoundsComponent));
		CTriggerBoundsComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CGravityVolumeComponent));
		CGravityVolumeComponent::Register(componentScope);
	}
	{
		Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CDistributorComponent));
		CDistributorComponent::Register(componentScope);
	}
}

CRYREGISTER_SINGLETON_CLASS(CAreaComponentsPlugin)

} // namespace AreaComponents
} // namespace Cry

#include <CryCore/CrtDebugStats.h>
