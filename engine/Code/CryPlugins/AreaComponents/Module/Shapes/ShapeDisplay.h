// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! What the shape previewers draw with, and the one scale rule every shape kind obeys.
//!
//! The colours are CShapeObject's, reproduced here because an engine module cannot reach
//! SDisplayContext: a component previewer only gets SEntityPreviewContext::bSelected
//! (EntityObject.cpp:2839 sets it from CBaseObject::IsSelected()) and an IRenderAuxGeom.
//!
//! Unselected: ColorB(0, 204, 255) at alpha 0.8 - CShapeObject's constructor colour
//! (ShapeObject.cpp:922) drawn with the alpha DisplayNormal sets (:1311).
//! Selected:   SDisplayContext::GetSelectedColor() (DisplayContext.cpp:629), which is a magenta
//!             pulsing on the blue channel at 8 rad/s. Reproducing the pulse rather than picking a
//!             static pink is what makes a selected component shape look like a selected legacy
//!             shape sitting next to it.

#include <CryMath/Cry_Math.h>
#include <CryMath/Cry_Color.h>
#include <CrySystem/ITimer.h>

namespace Cry
{
namespace AreaComponents
{
namespace ShapeDisplay
{

//! CShapeObject's own colour, the one the Area shapes have had since 5.3.
//! A function and not a constant: Color_tpl's constructor is not constexpr.
inline ColorB GetAreaShapeColour() { return ColorB(0, 204, 255, 204); }

//! The colour a shape outline is drawn in, given the entity's selection state.
inline ColorB GetOutlineColour(bool bSelected)
{
	if (!bSelected)
		return GetAreaShapeColour();

	// SDisplayContext::GetSelectedColor(): RGB(255, 0, |sin(t * 8)| * 255).
	const float time = (gEnv != nullptr && gEnv->pTimer != nullptr) ? gEnv->pTimer->GetAsyncCurTime() : 0.0f;
	const uint8 blue = static_cast<uint8>(clamp_tpl(fabs_tpl(sinf(time * 8.0f)), 0.0f, 1.0f) * 255.0f);
	return ColorB(255, 0, blue, 255);
}

//! Legacy's highlight orange, used by the polygon previewer for a sound-obstructing edge.
inline ColorB GetHighlightColour(bool bSelected)
{
	return bSelected ? ColorB(255, 160, 0, 255) : ColorB(200, 120, 0, 204);
}

} // namespace ShapeDisplay

//! ---------------------------------------------------------------------------------------------
//! The scale rule (decision recorded in scene-notes/area/reports/03-sphere-and-creation.md).
//!
//! Entity scale is honoured by every kind whose maths can represent it exactly, and only by those:
//!  - Box     - an axis-aligned box under an affine transform is still a box; non-uniform scale is
//!              exact, so it is honoured with no complaint (UE's UBoxComponent is the one shape
//!              that does the same, research/05 section 2.4).
//!  - Polygon - the points go through the same matrix; footprint and extrusion scale exactly.
//!  - Sphere  - an ellipsoid is NOT a sphere, and every analytic answer the contract owes
//!              (IsPointInside, DistanceToHull, IntersectRay, GetRandomPointInside) would have to
//!              become an ellipsoid solver to stay correct. O3DE's documented behaviour is taken
//!              instead: collapse the scale to its LARGEST axis and say so once (research/05
//!              section 1.6 - "rendering and intersection tests use the largest vector of the
//!              Transform component's Scale property").
//!
//! The warning is LATCHED per component instance: a user dragging a non-uniform scale gizmo would
//! otherwise fill the console at frame rate.
//! ---------------------------------------------------------------------------------------------

//! Largest axis scale of a transform, the uniform scale a radial kind uses.
inline float GetLargestAxisScale(const Matrix34& tm)
{
	const float scaleX = tm.GetColumn0().GetLength();
	const float scaleY = tm.GetColumn1().GetLength();
	const float scaleZ = tm.GetColumn2().GetLength();
	return max(scaleX, max(scaleY, scaleZ));
}

//! True when the three axis scales of `tm` differ by more than a float's worth of noise.
inline bool IsNonUniformlyScaled(const Matrix34& tm)
{
	const float scaleX = tm.GetColumn0().GetLength();
	const float scaleY = tm.GetColumn1().GetLength();
	const float scaleZ = tm.GetColumn2().GetLength();

	const float largest = max(scaleX, max(scaleY, scaleZ));
	const float smallest = min(scaleX, min(scaleY, scaleZ));

	// Relative, so that a 100 m shape is not reported for a rounding error in its transform.
	return largest > 0.0f && (largest - smallest) > largest * 0.001f;
}

} // namespace AreaComponents
} // namespace Cry
