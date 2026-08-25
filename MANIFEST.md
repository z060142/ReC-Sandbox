# Sync manifest

Baseline : main @ a34100a9 (pristine CRYENGINE 5.7.1)
Dev      : dev @ b0e9fd87
Synced   : 2026-08-25 13:34

## Commits (newest first)

- b0e9fd87 fix: halation halo is round - isotropic gaussian at 1/16 resolution instead of a separable exponential spread
- 403eb41e feat: viewfinder overlays - frame guides and focus peaking in the final composite
- 6deb64a2 feat: Mask Rotation - turn the bokeh shape mask independently of the iris phase
- 9e79a6d0 feat: film halation - red-orange halo around emulsion-overloading sources (bloom domain)
- bef856de chore: ignore the third-party SDK drop under Code/SDKs
- e383db58 chore: keep engine textures tracked, ignore generated solutions_cmake
- 4e418950 feat: anamorphic lens - squeeze (hFOV + gate mask / squeezed frame), oval bokeh, horizontal tinted flare
- ab390af2 feat: shape mask as an optical element - pupil = mask INTERSECT iris, continuous
- d894fd31 fix: sprite bokeh sizes from the gather CoC and fades in step with the halo
- 02599dfa feat: Hybrid DOF 2.0 two-domain mask, effect authority, camera slots + tracker follow
- 961be44a feat: revive Cinebox sprite bokeh with shaped-mask intersection (v1)
- 734623df feat: lens character (vignette + distortion) and documentation pass
- b4973af9 feat: lens transmission (T-stop)
- 47418cd0 fix: polygonal bokeh visible for the first time since the CE3 era
- 4d2c2a19 feat: streak coupling, AutoBiased exposure, sensor grain, grouped inspector
- ac23eb45 feat(renderer): polygonal bokeh and aperture diffraction streaks
- d4fce3d2 feat: focus breathing and colour-temperature white balance
- c346eed7 feat: shutter-linked motion blur, ND filter, bokeh forward wiring
- a6788060 feat: ISO passthrough, centre autofocus, yaw-aligned anchoring
- efd7558d feat: ISO film grain, exposure response, servo zoom for CinematicCamera
- 851a085e fix: FocusDistance serialization name corrupted every XML it touched
- 64c9499f feat: add CryPhoneTracker plugin (ARCore phone 6DoF + lens control)
- 9c725efe feat: add CinematicCamera plugin with physically-based optics
- 71a87cb0 fix: actually stage the .gitignore content update
- aca5c8fe chore: update .gitignore and untrack generated/oversized files
- 5a2cd526 fix: VS2022/C++17 compatibility fixes for full engine + Sandbox build

## Changed files

- M	.gitignore
- M	CMakeSettings.json
- M	Code/CryEngine/RenderDll/Common/PostProcess/PostEffects.cpp
- M	Code/CryEngine/RenderDll/Common/PostProcess/PostEffects.h
- M	Code/CryEngine/RenderDll/Common/PostProcess/PostProcess.cpp
- M	Code/CryEngine/RenderDll/Common/PostProcess/PostProcess.h
- M	Code/CryEngine/RenderDll/Common/RendererCVars.cpp
- M	Code/CryEngine/RenderDll/Common/RendererCVars.h
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/Bloom.cpp
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/Bloom.h
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/DepthOfField.cpp
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/DepthOfField.h
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/MotionBlur.cpp
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/PostAA.cpp
- M	Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/ToneMapping.cpp
- M	Code/CryEngine/RenderDll/XRenderD3D9/PostProcessDOF.cpp
- M	Code/CryPlugins/CMakeLists.txt
- A	Code/CryPlugins/CinematicCamera/AnamorphicSpec.md
- A	Code/CryPlugins/CinematicCamera/DiffractionStreaksSpec.md
- A	Code/CryPlugins/CinematicCamera/EffectAuthoritySpec.md
- A	Code/CryPlugins/CinematicCamera/HalationSpec.md
- A	Code/CryPlugins/CinematicCamera/HybridDofSpec.md
- A	Code/CryPlugins/CinematicCamera/Interface/ICinematicCameraOptics.h
- A	Code/CryPlugins/CinematicCamera/Module/CMakeLists.txt
- A	Code/CryPlugins/CinematicCamera/Module/CinematicCameraComponent.cpp
- A	Code/CryPlugins/CinematicCamera/Module/CinematicCameraComponent.h
- A	Code/CryPlugins/CinematicCamera/Module/CinematicCameraPlugin.cpp
- A	Code/CryPlugins/CinematicCamera/Module/CinematicCameraPlugin.h
- A	Code/CryPlugins/CinematicCamera/Module/StdAfx.cpp
- A	Code/CryPlugins/CinematicCamera/Module/StdAfx.h
- A	Code/CryPlugins/CinematicCamera/Module/resource.h
- A	Code/CryPlugins/CinematicCamera/README.md
- A	Code/CryPlugins/CinematicCamera/SpriteBokehSpec.md
- A	Code/CryPlugins/CinematicCamera/ViewfinderSpec.md
- A	Code/CryPlugins/CinematicCamera/docs/ConsoleReference.md
- A	Code/CryPlugins/CryPhoneTracker/Module/CMakeLists.txt
- A	Code/CryPlugins/CryPhoneTracker/Module/PhoneTrackerComponent.cpp
- A	Code/CryPlugins/CryPhoneTracker/Module/PhoneTrackerComponent.h
- A	Code/CryPlugins/CryPhoneTracker/Module/PhoneTrackerPlugin.cpp
- A	Code/CryPlugins/CryPhoneTracker/Module/PhoneTrackerPlugin.h
- A	Code/CryPlugins/CryPhoneTracker/Module/PhoneTrackerProtocol.h
- A	Code/CryPlugins/CryPhoneTracker/Module/PoseFilter.h
- A	Code/CryPlugins/CryPhoneTracker/Module/StdAfx.cpp
- A	Code/CryPlugins/CryPhoneTracker/Module/StdAfx.h
- A	Code/CryPlugins/CryPhoneTracker/Module/resource.h
- A	Code/CryPlugins/CryPhoneTracker/PoseFilterSpec.md
- A	Code/CryPlugins/CryPhoneTracker/README.md
- M	Code/Tools/CryCommonTools/FileUtil.cpp
- M	Engine/Shaders/HWScripts/CryFX/DepthOfField.cfx
- M	Engine/Shaders/HWScripts/CryFX/HDRPostProcess.cfx
- M	Engine/Shaders/HWScripts/CryFX/PostAA.cfx
- M	Tools/CMake/CRYENGINE-MSVC.cmake
