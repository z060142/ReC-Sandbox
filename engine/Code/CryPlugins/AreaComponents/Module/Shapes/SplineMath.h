// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

//! The interpolation rule of EditorQt's CSplineObject, ported into this plugin so that a spline
//! SHAPE COMPONENT and a legacy CSplineObject built on the same points draw the same curve
//! (decision 04, interpolation I1).
//!
//! The family is a cubic Bezier whose two handles per point are DERIVED from the neighbouring
//! points, never authored:
//!
//!   * evaluation      CSplineObject::GetBezierPos       SplineObject.cpp:750-757
//!   * tangent         CSplineObject::GetBezierTangent   SplineObject.cpp:759-771
//!   * segment length  GetBezierSegmentLength            SplineObject.cpp:773-787  (32-step polyline)
//!   * handle rule     BezierAnglesCorrection            SplineObject.cpp:896-958
//!   * roll normal     GetLocalBezierNormal              SplineObject.cpp:841-885
//!   * width blend     CRoadObject::GetLocalWidth        RoadObject.cpp:125-151
//!
//! Because the handles are derived, storing only the point positions is lossless - which
//! CGravityVolumeObject already proves by writing its points without Back/Forw
//! (GravityVolumeObject.cpp:810-811) and re-deriving them on load.
//!
//! ORDER OF DERIVATION - the one place this differs from legacy, deliberately.
//! Legacy's BezierAnglesCorrection for the two END points reads the neighbour's already-computed
//! handle (`pOut[1].back` / `pOut[maxIndex-1].forw`), while an INTERIOR point's handles depend on
//! point positions only. Legacy walks the list in index order, so at index 0 it reads whatever
//! point 1's `back` happened to hold from the previous pass - the converged value, because the
//! handles are also serialized and re-corrected on every load. DeriveHandles() below therefore
//! computes all interior points first and the two ends afterwards: that IS the converged fixed
//! point legacy settles on, reached in one pass and independent of edit history.
//!
//! Everything here takes caller-owned arrays and allocates nothing (heap rule, IShapeComponent.h).

#include <CryMath/Cry_Math.h>
#include <CryMath/Cry_Geo.h>

namespace Cry
{
namespace AreaComponents
{
namespace SplineMath
{

//! How many chords GetBezierSegmentLength measures a segment with (SplineObject.cpp:775).
constexpr int   kLengthSteps = 32;
//! The degeneracy epsilon legacy compares its lengths and tangents against.
constexpr float kEps = 0.00001f;

//! The two absolute Bezier control handles of one point. `back` reaches towards the previous
//! point, `forw` towards the next one; a segment uses the START point's `forw` and the END point's
//! `back`, which is the asymmetry GetBezierPos encodes.
struct SHandles
{
	Vec3 back = ZERO;
	Vec3 forw = ZERO;
};

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

//! CSplineObject::GetBezierPos, SplineObject.cpp:750-757.
inline Vec3 EvalSegmentPos(const Vec3& p0, const Vec3& forw0, const Vec3& back1, const Vec3& p1, float t)
{
	const float invT = 1.0f - t;
	return p0 * (invT * invT * invT) +
	       forw0 * (3.0f * t * invT * invT) +
	       back1 * (3.0f * t * t * invT) +
	       p1 * (t * t * t);
}

//! CSplineObject::GetBezierTangent, SplineObject.cpp:759-771 - the analytic derivative, normalised.
inline Vec3 EvalSegmentTangent(const Vec3& p0, const Vec3& forw0, const Vec3& back1, const Vec3& p1, float t)
{
	const float invT = 1.0f - t;

	Vec3 tangent = -p0 * (invT * invT)
	               + forw0 * (invT * (invT - 2.0f * t))
	               + back1 * (t * (2.0f * invT - t))
	               + p1 * (t * t);

	if (!tangent.IsZero())
		tangent.Normalize();

	return tangent;
}

//! CSplineObject::GetBezierSegmentLength, SplineObject.cpp:773-787: the length of the first
//! fraction `t` of the segment, approximated by kLengthSteps chords.
inline float SegmentLength(const Vec3& p0, const Vec3& forw0, const Vec3& back1, const Vec3& p1, float t = 1.0f)
{
	const float kn = t * kLengthSteps + 1.0f;

	float length = 0.0f;
	Vec3  pos = EvalSegmentPos(p0, forw0, back1, p1, 0.0f);

	for (float k = 1.0f; k <= kn; k += 1.0f)
	{
		const Vec3 nextPos = EvalSegmentPos(p0, forw0, back1, p1, t * k / kn);
		length += (nextPos - pos).GetLength();
		pos = nextPos;
	}

	return length;
}

// ---------------------------------------------------------------------------
// Handle derivation - BezierAnglesCorrection, SplineObject.cpp:896-958
// ---------------------------------------------------------------------------

//! The interior rule (SplineObject.cpp:946-957): a Catmull-Rom style tangent parallel to the chord
//! p1->p3, expressed as two Bezier handles and scaled by the two chord lengths.
inline void DeriveInteriorHandles(const Vec3& p1, const Vec3& p2, const Vec3& p3, SHandles& out, float tension = 1.0f)
{
	const float lenOsn = (p3 - p1).GetLength();

	if (lenOsn <= kEps)
	{
		// The two neighbours coincide: legacy would divide by zero here. A zero-length handle pair
		// degenerates the segment into a straight line, which is the only sane answer.
		out.back = p2;
		out.forw = p2;
		return;
	}

	const float lenB = (p1 - p2).GetLength();
	const float lenF = (p3 - p2).GetLength();

	// `tension` scales the handle LENGTH only, never its direction, so the curve keeps legacy's
	// tangent everywhere and only changes how far it bulges between the points. 1 is legacy exactly;
	// 0 collapses every handle onto its point, which turns the Bezier into the control polygon.
	out.back = p2 + (p1 - p3) * (tension * lenB / lenOsn / 3.0f);
	out.forw = p2 + (p3 - p1) * (tension * lenF / lenOsn / 3.0f);
}

//! Fills `pOut` (count entries) with the derived handles of the whole point list. A CLOSED spline
//! has no end points at all - every point is interior with wrapped neighbours - which is what makes
//! the closing segment join the first one smoothly instead of forming a corner.
inline void DeriveHandles(const Vec3* pPos, int count, bool bClosed, SHandles* pOut, float tension = 1.0f)
{
	if (pPos == nullptr || pOut == nullptr || count <= 0)
		return;

	if (count == 1)
	{
		pOut[0].back = pPos[0];
		pOut[0].forw = pPos[0];
		return;
	}

	if (bClosed)
	{
		for (int i = 0; i < count; ++i)
		{
			DeriveInteriorHandles(pPos[(i + count - 1) % count], pPos[i], pPos[(i + 1) % count], pOut[i], tension);
		}
		return;
	}

	const int maxIndex = count - 1;

	// Interior first: these depend on point positions only (see the ORDER note at the top).
	for (int i = 1; i < maxIndex; ++i)
	{
		DeriveInteriorHandles(pPos[i - 1], pPos[i], pPos[i + 1], pOut[i], tension);
	}

	// The first point (SplineObject.cpp:908-925).
	{
		const Vec3& p2 = pPos[0];
		pOut[0].back = p2;

		if (maxIndex == 1)
		{
			pOut[0].forw = p2 + (pPos[1] - p2) * (tension / 3.0f);
		}
		else
		{
			const Vec3& pb3 = pOut[1].back;

			const float lenOsn = (pb3 - p2).GetLength();
			const float lenB = (pPos[1] - p2).GetLength();

			pOut[0].forw = (lenOsn > kEps && lenB > kEps) ? (p2 + (pb3 - p2) / (lenOsn / lenB * 3.0f)) : p2;
		}
	}

	// The last point (SplineObject.cpp:927-944).
	{
		const Vec3& p2 = pPos[maxIndex];
		pOut[maxIndex].forw = p2;

		const Vec3& pf1 = pOut[maxIndex - 1].forw;
		const Vec3& p1 = pPos[maxIndex - 1];

		const float lenOsn = (pf1 - p2).GetLength();
		const float lenF = (p1 - p2).GetLength();

		pOut[maxIndex].back = (lenOsn > kEps && lenF > kEps) ? (p2 + (pf1 - p2) / (lenOsn / lenF * 3.0f)) : p2;
	}
}

// ---------------------------------------------------------------------------
// Per-point attributes blended along a segment
// ---------------------------------------------------------------------------

//! The blend GetLocalBezierNormal uses for the roll angle (SplineObject.cpp:866-875): an
//! ease-in-ease-out on [0,1] built from a cubic mirrored about the midpoint, so that the banking of
//! a road settles at each point instead of changing linearly across the segment.
inline float BlendAngleAlongSegment(float t)
{
	float       af = t * 2.0f - 1.0f;
	const float ed = (af < 0.0f) ? -1.0f : 1.0f;

	af = ed - af;
	af = af * af * af;
	af = ed - af;

	return (af + 1.0f) / 2.0f;
}

//! The roll-aware normal of a segment, in the same space as the tangent
//! (CSplineObject::GetLocalBezierNormal, SplineObject.cpp:841-885). `angle0` / `angle1` are the two
//! points' roll in degrees, `t` the position within the segment.
inline Vec3 SegmentNormal(const Vec3& tangent, float angle0, float angle1, float t)
{
	Vec3 e = tangent;
	if (e.IsZero(kEps))
		return ZERO;

	Vec3 n;

	if (-kEps > angle0 || angle0 > kEps || -kEps > angle1 || angle1 > kEps)
	{
		const float af = BlendAngleAlongSegment(t);
		const float angle = DEG2RAD((1.0f - af) * angle0 + af * angle1);

		e.Normalize();
		n = Vec3(0.0f, 0.0f, 1.0f).Cross(e);
		n = n.GetRotated(e, angle);
	}
	else
	{
		n = Vec3(0.0f, 0.0f, 1.0f).Cross(e);
	}

	if (!n.IsZero(kEps))
		n.Normalize();

	return n;
}

//! The width blend of CRoadObject::GetLocalWidth (RoadObject.cpp:125-151), which is LINEAR - the
//! cubic ease above is deliberately not used for width; legacy computes `ed` there and then throws
//! it away, which leaves a plain lerp.
inline float BlendWidthAlongSegment(float width0, float width1, float t)
{
	if (width0 == width1)
		return width0;

	return (1.0f - t) * width0 + t * width1;
}

// ---------------------------------------------------------------------------
// Smoothing - one pass of the 4-point INTERPOLATING subdivision scheme
// ---------------------------------------------------------------------------

//! Why this scheme and not Chaikin. Chaikin corner cutting is an APPROXIMATING scheme: it replaces
//! every point with two points inside its corner, so after one pass none of the authored points is
//! on the curve any more and the whole shape shrinks towards the inside of the control polygon.
//! That is the wrong trade here, because those points are what the user drags - a smoothing slider
//! that walks the curve off its own handles is a slider nobody can aim.
//!
//! The 4-point scheme (Dyn-Levin-Gregory, the subdivision form of a Catmull-Rom) KEEPS every
//! existing point and only inserts one new point per edge, at
//!     m = (-p0 + 9*p1 + 9*p2 - p3) / 16
//! so the curve still passes through every authored point and only the corners are rounded off.
//! Each pass doubles the number of edges, which is also what makes the mapping back to the authored
//! points exact: after n passes, authored point i is subdivided point i * 2^n.
inline int SubdivideInterpolating(const Vec3* pIn, int count, bool bClosed, Vec3* pOut, int maxOut)
{
	if (pIn == nullptr || pOut == nullptr || count < 2)
		return 0;

	const int outCount = bClosed ? (count * 2) : (count * 2 - 1);
	if (outCount > maxOut)
		return 0;

	const int edgeCount = bClosed ? count : (count - 1);

	for (int e = 0; e < edgeCount; ++e)
	{
		const int i1 = e;
		const int i2 = (e + 1) % count;
		// The two outer points of the four-point stencil, clamped at the ends of an open polygon,
		// which is what makes an end segment keep its own straight direction.
		const int i0 = bClosed ? ((e + count - 1) % count) : max(e - 1, 0);
		const int i3 = bClosed ? ((e + 2) % count) : min(e + 2, count - 1);

		pOut[e * 2] = pIn[i1];
		pOut[e * 2 + 1] = (pIn[i0] * -1.0f + pIn[i1] * 9.0f + pIn[i2] * 9.0f + pIn[i3] * -1.0f) / 16.0f;
	}

	if (!bClosed)
		pOut[outCount - 1] = pIn[count - 1];

	return outCount;
}

//! Blends a per-point attribute (Angle, Width) the same way SubdivideInterpolating blends the
//! positions, so that the two arrays stay in lockstep: existing values are kept where they are and
//! an inserted point gets the midpoint of the edge it was inserted into. A plain midpoint, not the
//! four-point stencil - overshooting a width into a negative number would be a bug, not a nicety.
inline int SubdivideAttribute(const float* pIn, int count, bool bClosed, float* pOut, int maxOut)
{
	if (pIn == nullptr || pOut == nullptr || count < 2)
		return 0;

	const int outCount = bClosed ? (count * 2) : (count * 2 - 1);
	if (outCount > maxOut)
		return 0;

	const int edgeCount = bClosed ? count : (count - 1);

	for (int e = 0; e < edgeCount; ++e)
	{
		pOut[e * 2] = pIn[e];
		pOut[e * 2 + 1] = (pIn[e] + pIn[(e + 1) % count]) * 0.5f;
	}

	if (!bClosed)
		pOut[outCount - 1] = pIn[count - 1];

	return outCount;
}

// ---------------------------------------------------------------------------
// Rotation-minimizing frames (double reflection)
// ---------------------------------------------------------------------------

//! Legacy's normal is Vec3(0,0,1).Cross(tangent) - SegmentNormal above, and
//! CSplineObject::GetLocalBezierNormal (SplineObject.cpp:874 and :880). That is a fine normal for a
//! road lying on the ground and a bad one anywhere else: as the tangent approaches world up the
//! cross product's LENGTH goes to zero, so its direction comes to be decided by whatever component
//! of the tangent is still horizontal, and it swings through half a turn as the curve passes
//! vertical. Everything oriented by that frame - every mesh the distributor places with "Follow
//! Tangent" - spins with it.
//!
//! A rotation-minimizing frame has no preferred up vector at all: it carries one normal along the
//! curve and turns it only as much as the curve itself turns. This is the double-reflection step of
//! Wang, Juttler, Zheng and Liu (2008), the standard method, exact to second order: reflect the
//! previous frame in the plane between the two positions, then again in the plane between the two
//! tangents.
//!
//! The price, worth writing down: a CLOSED curve's frame does not generally return to where it
//! started - the holonomy of a closed space curve is not zero - so a closed spline can show one
//! twist at its seam. Legacy has no seam because it recomputes its frame from world up at every
//! point, which is exactly the property that makes it flip.
inline Vec3 PropagateNormal(const Vec3& prevPos, const Vec3& prevTangent, const Vec3& prevNormal,
                            const Vec3& pos, const Vec3& tangent)
{
	const Vec3  v1 = pos - prevPos;
	const float c1 = v1.Dot(v1);
	if (c1 <= kEps * kEps)
		return prevNormal;

	// First reflection, in the plane bisecting the two positions.
	const Vec3 reflectedNormal = prevNormal - v1 * ((2.0f / c1) * v1.Dot(prevNormal));
	const Vec3 reflectedTangent = prevTangent - v1 * ((2.0f / c1) * v1.Dot(prevTangent));

	// Second reflection, in the plane bisecting the reflected tangent and the real one.
	const Vec3  v2 = tangent - reflectedTangent;
	const float c2 = v2.Dot(v2);
	if (c2 <= kEps * kEps)
		return reflectedNormal;

	Vec3 result = reflectedNormal - v2 * ((2.0f / c2) * v2.Dot(reflectedNormal));

	// Keep it perpendicular and unit: the two reflections are exact in theory and drift in floats
	// over a few hundred samples.
	result -= tangent * result.Dot(tangent);
	if (!result.IsZero(kEps))
		result.Normalize();

	return result;
}

//! A normal to seed a rotation-minimizing frame with: legacy's world-up cross product where that is
//! well conditioned, and any perpendicular where it is not (a curve that starts out vertical).
//! Seeding from legacy is what makes a flat spline look identical in both frame modes.
inline Vec3 MakeSeedNormal(const Vec3& tangent)
{
	if (tangent.IsZero(kEps))
		return Vec3(0.0f, 1.0f, 0.0f);

	Vec3 normal = Vec3(0.0f, 0.0f, 1.0f).Cross(tangent);
	if (normal.GetLength() < 0.1f)
	{
		normal = tangent.GetOrthogonal();
	}

	if (!normal.IsZero(kEps))
		normal.Normalize();

	return normal;
}

//! Rolls a frame normal about the tangent by `angleDegrees` - the per-point Angle that road banking
//! and instance roll are authored with. It applies ON TOP of whichever frame mode produced `normal`,
//! so switching frame mode never loses the authored banking.
inline Vec3 ApplyRoll(const Vec3& normal, const Vec3& tangent, float angleDegrees)
{
	if (fabs_tpl(angleDegrees) <= kEps || normal.IsZero(kEps) || tangent.IsZero(kEps))
		return normal;

	Vec3 rolled = normal.GetRotated(tangent.GetNormalized(), DEG2RAD(angleDegrees));
	if (!rolled.IsZero(kEps))
		rolled.Normalize();

	return rolled;
}

} // namespace SplineMath
} // namespace AreaComponents
} // namespace Cry
