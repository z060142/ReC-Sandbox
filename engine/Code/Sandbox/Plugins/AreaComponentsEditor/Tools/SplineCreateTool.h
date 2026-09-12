// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "ShapeCreateTool.h"

//! "Create Object -> Area -> Spline": the click-click-double-click draw of CShapeCreateTool with
//! the two things a spline needs differently from a polygon - it is created as the AreaShapeSpline
//! object class, and two points are already a spline (a polygon needs three to have an interior).
//!
//! Everything else is the base tool unchanged: the first click spawns the entity with its spline
//! shape component and makes that point 0, further clicks append, a double-click or Enter finishes,
//! Esc throws the entity away again, and the whole draw is one undo step. One tool class per object
//! class, because the Create panel hands a custom creation tool no user data (report 02).
class CSplineCreateTool : public CShapeCreateTool
{
	DECLARE_DYNCREATE(CSplineCreateTool)

public:
	CSplineCreateTool() = default;

	// CEditTool
	virtual string GetDisplayName() const override { return "Create Spline"; }
	// ~CEditTool

protected:
	virtual ~CSplineCreateTool() = default;

	// CShapeCreateTool
	virtual const char* GetObjectClassName() const override { return "AreaShapeSpline"; }
	//! CSplineObject::GetMinPoints() is 2 (SplineObject.h:83): a two-point spline is a curve, and
	//! refusing it would make the shortest useful path impossible to draw.
	virtual int         GetMinPointCount() const override { return 2; }
	// ~CShapeCreateTool
};
