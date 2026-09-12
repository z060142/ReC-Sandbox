start_sources()

sources_platform(ALL)
add_sources("AreaComponents_uber_0.cpp"
	SOURCE_GROUP "Root"
		"StdAfx.cpp"
		"StdAfx.h"
		"PluginDll.h"
		"PluginDll.cpp"
		"resource.h"
	SOURCE_GROUP "Interface"
		"../Interface/IShapeComponent.h"
		"../Interface/IDistributorBake.h"
	SOURCE_GROUP "Shapes"
		"Shapes/ShapeGeometry.h"
		"Shapes/ShapeKinds.h"
		"Shapes/ShapeDisplay.h"
		"Shapes/ShapeEditorActions.h"
		"Shapes/BoxShapeComponent.h"
		"Shapes/BoxShapeComponent.cpp"
		"Shapes/PolygonShapeComponent.h"
		"Shapes/PolygonShapeComponent.cpp"
		"Shapes/SphereShapeComponent.h"
		"Shapes/SphereShapeComponent.cpp"
		"Shapes/SplineMath.h"
		"Shapes/SplineShapeComponent.h"
		"Shapes/SplineShapeComponent.cpp"
	SOURCE_GROUP "Functions"
		"Functions/AreaFunctionComponent.h"
		"Functions/AreaFunctionComponent.cpp"
		"Functions/TriggerBoundsComponent.h"
		"Functions/TriggerBoundsComponent.cpp"
		"Functions/GravityVolumeComponent.h"
		"Functions/GravityVolumeComponent.cpp"
		"Functions/DistributorComponent.h"
		"Functions/DistributorComponent.cpp"
)

end_sources()

CryEngineModule(AreaComponents PCH "StdAfx.cpp" SOLUTION_FOLDER "CryPlugins")

target_include_directories(${THIS_PROJECT}
	PRIVATE
		"${CRYENGINE_DIR}/Code/CryEngine/CryCommon"
)
