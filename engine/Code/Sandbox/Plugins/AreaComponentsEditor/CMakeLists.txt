start_sources()

sources_platform(ALL)
add_sources("NoUberFile"
	SOURCE_GROUP "Root"
		"Plugin.h"
		"Plugin.cpp"
		"StdAfx.h"
		"StdAfx.cpp"
	SOURCE_GROUP "Tools"
		"Tools/ShapeToolCommon.h"
		"Tools/ShapeToolCommon.cpp"
		"Tools/ShapeEditTool.h"
		"Tools/ShapeEditTool.cpp"
		"Tools/ShapeCreateTool.h"
		"Tools/ShapeCreateTool.cpp"
		"Tools/ShapeDragCreateTool.h"
		"Tools/ShapeDragCreateTool.cpp"
		"Tools/SplineCreateTool.h"
		"Tools/SplineCreateTool.cpp"
		"Tools/ShapeRecenterTool.h"
		"Tools/ShapeRecenterTool.cpp"
		"Tools/DistributorBakeTool.h"
		"Tools/DistributorBakeTool.cpp"
	SOURCE_GROUP "Objects"
		"Objects/AreaShapeObjectClassDescs.cpp"
)

end_sources()

CryEditorPlugin(AreaComponentsEditor PCH "StdAfx.cpp" SOLUTION_FOLDER "Sandbox/Plugins")

# EditorCommon is linked by CryEditorPlugin() itself and is all this plugin needs: CEditTool,
# IClassDesc and REGISTER_CLASS_DESC all live there. The AreaComponents runtime DLL is reached
# only through the reflected IShapeComponent / IShapeComponentEdit interfaces at run time, so
# there is deliberately no link dependency on it - and no EditorQt / MFCToolsPlugin dependency
# either, which keeps the DLL deployable on its own.
target_include_directories(${THIS_PROJECT}
	PRIVATE
		"${CRYENGINE_DIR}/Code/CryPlugins/AreaComponents/Interface"
)
