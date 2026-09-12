// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "SplineShapeComponent.h"
#include "ShapeDisplay.h"
#include "ShapeGeometry.h"

#include <CryRenderer/IRenderAuxGeom.h>
#include <CrySystem/IConsole.h>
#include <CrySystem/ConsoleRegistration.h>
#include <CryMath/Random.h>

#include <algorithm>

namespace Cry
{
namespace AreaComponents
{

namespace
{
//! Two points closer than this to each other are the same point, the test legacy makes before it
//! accepts an inserted point (SplineObject.cpp:707-711 compares each axis against FLT_EPSILON).
const float kSplineDuplicatePointEpsilon = FLT_EPSILON;
//! How many times GetRandomPointInside draws from the footprint AABB before it gives up.
const int   kSplineRandomPointAttempts = 64;
}

namespace
{

//! `area_SplineSelfTest` - walks the spline maths through every degenerate shape the create tool can
//! produce mid-gesture and reports the first NaN, infinity or wrong count.
//!
//! It exercises SplineMath directly rather than a live component, because the shapes that matter
//! here are the ones with no entity yet: one point, two coincident points, two real points. Those
//! are exactly the states a user walks through between the first click and the third, and the
//! states in which a normalise, a division by a chord length or a subdivision stencil can produce a
//! NaN that only shows up as a crash several frames later.
void SplineSelfTest(IConsoleCmdArgs*)
{
	struct SCase
	{
		const char* szName;
		Vec3        points[4];
		int         count;
	};

	const SCase cases[] =
	{
		{ "1 point",            { Vec3(3, 4, 5) },                                                  1 },
		{ "2 coincident",       { Vec3(3, 4, 5), Vec3(3, 4, 5) },                                   2 },
		{ "2 points",           { Vec3(0, 0, 0), Vec3(10, 0, 0) },                                  2 },
		{ "3 collinear",        { Vec3(0, 0, 0), Vec3(5, 0, 0), Vec3(10, 0, 0) },                   3 },
		{ "3 points vertical",  { Vec3(0, 0, 0), Vec3(0, 0, 5), Vec3(0, 0, 10) },                   3 },
		{ "3 points",           { Vec3(0, 0, 0), Vec3(5, 5, 1), Vec3(10, 0, 0) },                   3 },
	};

	int failures = 0;
	int checks = 0;

	const auto check = [&failures, &checks](bool bOk, const char* szWhat, const char* szCase, int iterations, bool bClosed)
	{
		++checks;
		if (bOk)
			return;

		++failures;
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_ERROR,
		           "area_SplineSelfTest FAILED: %s [%s, %d iteration(s), %s]",
		           szWhat, szCase, iterations, bClosed ? "closed" : "open");
	};

	for (const SCase& testCase : cases)
	{
		for (int closed = 0; closed <= 1; ++closed)
		{
			const bool bClosed = closed != 0;

			for (int iterations = 0; iterations <= 2; ++iterations)
			{
				Vec3 poly[512];
				int  count = testCase.count;
				for (int i = 0; i < count; ++i)
					poly[i] = testCase.points[i];

				// Subdivision, the same loop BuildEvalPolygon runs.
				for (int pass = 0; pass < iterations && count >= 2; ++pass)
				{
					Vec3      scratch[512];
					const int written = SplineMath::SubdivideInterpolating(poly, count, bClosed, scratch, CRY_ARRAY_COUNT(scratch));
					if (written < 2)
						break;

					const int expected = bClosed ? (count * 2) : (count * 2 - 1);
					check(written == expected, "subdivision produced the wrong count", testCase.szName, iterations, bClosed);

					for (int i = 0; i < written; ++i)
						check(scratch[i].IsValid(), "subdivision produced a non-finite point", testCase.szName, iterations, bClosed);

					memcpy(poly, scratch, sizeof(Vec3) * written);
					count = written;
				}

				SplineMath::SHandles handles[512];
				SplineMath::DeriveHandles(poly, count, bClosed, handles, 1.0f);

				for (int i = 0; i < count; ++i)
				{
					check(handles[i].back.IsValid() && handles[i].forw.IsValid(),
					      "handle derivation produced a non-finite point", testCase.szName, iterations, bClosed);
				}

				const int segmentCount = (count < 2) ? 0 : (bClosed ? count : (count - 1));

				// Evaluation, lengths and both frame modes over every segment.
				Vec3 prevPos(ZERO), prevTangent(ZERO), normal(ZERO);
				bool bHavePrevious = false;

				for (int segment = 0; segment < segmentCount; ++segment)
				{
					const int i0 = segment % count;
					const int i1 = (segment + 1) % count;

					const float length = SplineMath::SegmentLength(poly[i0], handles[i0].forw, handles[i1].back, poly[i1]);
					check(NumberValid(length) && length >= 0.0f, "segment length is not a finite, non-negative number",
					      testCase.szName, iterations, bClosed);

					for (int step = 0; step <= 4; ++step)
					{
						const float t = static_cast<float>(step) / 4.0f;

						const Vec3 pos = SplineMath::EvalSegmentPos(poly[i0], handles[i0].forw, handles[i1].back, poly[i1], t);
						const Vec3 tangent = SplineMath::EvalSegmentTangent(poly[i0], handles[i0].forw, handles[i1].back, poly[i1], t);

						check(pos.IsValid(), "evaluated position is not finite", testCase.szName, iterations, bClosed);
						check(tangent.IsValid(), "evaluated tangent is not finite", testCase.szName, iterations, bClosed);

						// LegacyUp.
						const Vec3 legacyNormal = SplineMath::SegmentNormal(tangent, 0.0f, 30.0f, t);
						check(legacyNormal.IsValid(), "legacy normal is not finite", testCase.szName, iterations, bClosed);

						// ParallelTransport.
						if (!bHavePrevious)
						{
							normal = SplineMath::MakeSeedNormal(tangent);
							check(normal.IsValid(), "seed normal is not finite", testCase.szName, iterations, bClosed);
							bHavePrevious = true;
						}
						else
						{
							normal = SplineMath::PropagateNormal(prevPos, prevTangent, normal, pos, tangent);
							check(normal.IsValid(), "propagated normal is not finite", testCase.szName, iterations, bClosed);
						}

						const Vec3 rolled = SplineMath::ApplyRoll(normal, tangent, 30.0f);
						check(rolled.IsValid(), "rolled normal is not finite", testCase.szName, iterations, bClosed);

						prevPos = pos;
						prevTangent = tangent;
					}
				}
			}
		}
	}

	if (failures == 0)
	{
		CryLog("area_SplineSelfTest: %d checks, all passed.", checks);
	}
	else
	{
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_ERROR,
		           "area_SplineSelfTest: %d of %d checks FAILED.", failures, checks);
	}
}

} // namespace

void CSplineShapeComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
{
	// No Schematyc functions yet - the same stage as the other kinds. The spline evaluators become
	// Schematyc nodes when something outside C++ needs them.

	// Registered here because Register() runs once, at Schematyc environment registration, by which
	// time the console exists. It costs nothing until somebody types it.
	// IConsole::AddCommand is protected and befriends ConsoleRegistrationHelper, so the registration
	// goes through REGISTER_COMMAND like every other console command in the engine. Guarded, because
	// Register() can run again when the Schematyc environment is reloaded.
	static bool s_bSelfTestRegistered = false;
	if (!s_bSelfTestRegistered && gEnv != nullptr && gEnv->pConsole != nullptr)
	{
		s_bSelfTestRegistered = true;
		REGISTER_COMMAND("area_SplineSelfTest", SplineSelfTest, VF_NULL,
		                 "Checks the spline maths against 1-, 2- and 3-point open and closed splines, "
		                 "0 to 2 smoothing passes, both frame modes. Reports any non-finite result.");
	}
}

// ---------------------------------------------------------------------------
// Basics and the derived cache
// ---------------------------------------------------------------------------

Matrix34 CSplineShapeComponent::GetShapeWorldTM() const
{
	return GetWorldTransformMatrix() * Matrix34::CreateTranslationMat(m_offset);
}

int CSplineShapeComponent::GetSegmentCount() const
{
	// Counted on the EVALUATION polygon, which is the authored points when smoothing is off and
	// 2^SmoothIterations times as many when it is on.
	const int count = static_cast<int>(GetCache().localPos.size());
	if (count < 2)
		return 0;

	// A closed spline has one more segment than an open one: the wrap from the last point back to
	// the first. That extra segment is the whole of what `Closed` means geometrically.
	return m_closed ? count : (count - 1);
}

void CSplineShapeComponent::SegmentPoints(int segment, int& i0, int& i1) const
{
	const int count = static_cast<int>(GetCache().localPos.size());

	i0 = (count > 0) ? (segment % count) : 0;
	i1 = (count > 0) ? ((segment + 1) % count) : 0;
}

int CSplineShapeComponent::GetSamplesPerSegment() const
{
	// Smoothing multiplies the number of segments, so the samples per segment come down to match:
	// the drawn curve stays just as smooth, the contour buffer stays the same size, and nothing
	// downstream has to learn about smoothing.
	const int iterations = clamp_tpl(m_smoothIterations, 0, kMaxSmoothIterations);
	return max(1, kSamplesPerSegment >> iterations);
}

void CSplineShapeComponent::BuildEvalPolygon(SCache& cache) const
{
	const int authoredCount = static_cast<int>(m_points.points.size());

	cache.evalStride = 1;
	cache.localPos.resize(authoredCount);
	cache.evalAngle.resize(authoredCount);
	cache.evalWidth.resize(authoredCount);

	for (int i = 0; i < authoredCount; ++i)
	{
		const SSplinePoint& point = m_points.points[i];
		cache.localPos[i] = m_offset + point.pos;
		cache.evalAngle[i] = point.angle;
		// The width is resolved here, once: a point flagged "default width" contributes a negative
		// sentinel, and GetLocalWidth() turns that back into the consumer's own width. Doing it this
		// way is what lets the width be subdivided as a plain float array beside the positions.
		cache.evalWidth[i] = point.defaultWidth ? -1.0f : point.width;
	}

	const int iterations = clamp_tpl(m_smoothIterations, 0, kMaxSmoothIterations);
	if (iterations <= 0 || authoredCount < 2)
		return;

	std::vector<Vec3>  posScratch;
	std::vector<float> angleScratch;
	std::vector<float> widthScratch;

	for (int pass = 0; pass < iterations; ++pass)
	{
		const int count = static_cast<int>(cache.localPos.size());
		const int outCount = m_closed ? (count * 2) : (count * 2 - 1);
		if (outCount > kMaxEvalPoints)
		{
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
			           "Shape: Spline - Smooth Iterations %d would need %d evaluation points (limit %d); "
			           "stopped after %d passes.",
			           iterations, outCount, kMaxEvalPoints, pass);
			break;
		}

		posScratch.resize(outCount);
		angleScratch.resize(outCount);
		widthScratch.resize(outCount);

		const int written = SplineMath::SubdivideInterpolating(cache.localPos.data(), count, m_closed,
		                                                       posScratch.data(), outCount);
		if (written != outCount)
			break;

		SplineMath::SubdivideAttribute(cache.evalAngle.data(), count, m_closed, angleScratch.data(), outCount);
		SplineMath::SubdivideAttribute(cache.evalWidth.data(), count, m_closed, widthScratch.data(), outCount);

		cache.localPos.swap(posScratch);
		cache.evalAngle.swap(angleScratch);
		cache.evalWidth.swap(widthScratch);

		cache.evalStride *= 2;
	}
}

void CSplineShapeComponent::BuildFrameNormals(SCache& cache) const
{
	cache.frameNormals.clear();

	if (m_frameMode != ESplineFrameMode::ParallelTransport)
		return;

	const int segmentCount = m_closed ? static_cast<int>(cache.localPos.size())
	                                  : (static_cast<int>(cache.localPos.size()) - 1);
	if (segmentCount <= 0)
		return;

	const int samplesPerSegment = GetSamplesPerSegment();
	const int sampleCount = segmentCount * samplesPerSegment + 1;

	cache.frameNormals.resize(sampleCount);

	// Local space: the frame rides the shape, so a rotated entity rotates it with everything else.
	const auto sampleAt = [&](int index, Vec3& pos, Vec3& tangent)
	{
		const float f = static_cast<float>(index) / static_cast<float>(samplesPerSegment);
		int         segment = static_cast<int>(floorf(f));
		float       t = f - static_cast<float>(segment);
		if (segment >= segmentCount)
		{
			segment = segmentCount - 1;
			t = 1.0f;
		}

		int i0, i1;
		i0 = segment % static_cast<int>(cache.localPos.size());
		i1 = (segment + 1) % static_cast<int>(cache.localPos.size());

		pos = SplineMath::EvalSegmentPos(cache.localPos[i0], cache.localHandles[i0].forw,
		                                 cache.localHandles[i1].back, cache.localPos[i1], t);
		tangent = SplineMath::EvalSegmentTangent(cache.localPos[i0], cache.localHandles[i0].forw,
		                                         cache.localHandles[i1].back, cache.localPos[i1], t);
	};

	Vec3 prevPos(ZERO), prevTangent(ZERO);
	sampleAt(0, prevPos, prevTangent);

	// A curve whose first two points coincide - which the create tool can produce for one frame -
	// has a zero tangent there. MakeSeedNormal answers a fixed perpendicular for that case rather
	// than normalising a zero vector, and PropagateNormal carries it until the tangent is real.

	// Seeded from legacy's own normal, so a flat spline looks identical in both frame modes and
	// only a curve that legacy cannot orient is changed.
	cache.frameNormals[0] = SplineMath::MakeSeedNormal(prevTangent);

	for (int i = 1; i < sampleCount; ++i)
	{
		Vec3 pos, tangent;
		sampleAt(i, pos, tangent);

		cache.frameNormals[i] = SplineMath::PropagateNormal(prevPos, prevTangent, cache.frameNormals[i - 1],
		                                                    pos, tangent);

		prevPos = pos;
		prevTangent = tangent;
	}
}

const CSplineShapeComponent::SCache& CSplineShapeComponent::GetCache() const
{
	const Matrix34 worldTM = GetShapeWorldTM();

	// Re-entry guard. The rule above - nothing inside a cache build may read the cache - is easy to
	// break again, because half the evaluation helpers legitimately read it. If it is ever broken,
	// this hands the caller the half-built cache instead of recursing until the stack ends: a wrong
	// answer for one frame is recoverable, a stack overflow is not.
	if (m_bBuildingCache)
		return m_cache;

	const bool bSameTM = m_cache.bValid &&
	                     IsEquivalent(m_cache.worldTM.GetColumn0(), worldTM.GetColumn0(), 0.0001f) &&
	                     IsEquivalent(m_cache.worldTM.GetColumn1(), worldTM.GetColumn1(), 0.0001f) &&
	                     IsEquivalent(m_cache.worldTM.GetColumn2(), worldTM.GetColumn2(), 0.0001f) &&
	                     IsEquivalent(m_cache.worldTM.GetColumn3(), worldTM.GetColumn3(), 0.0001f);

	if (bSameTM)
		return m_cache;

	m_bBuildingCache = true;

	// The evaluation polygon first: the authored points, optionally subdivided (BuildEvalPolygon).
	BuildEvalPolygon(m_cache);

	const int count = static_cast<int>(m_cache.localPos.size());

	m_cache.localHandles.resize(count);
	m_cache.worldPos.resize(count);
	m_cache.worldHandles.resize(count);

	SplineMath::DeriveHandles(m_cache.localPos.data(), count, m_closed, m_cache.localHandles.data(),
	                          clamp_tpl(m_tension, 0.0f, 1.0f));

	// A Bezier is affine invariant, so transforming the four control points of a segment transforms
	// the curve: the world copy is the local one put through the matrix, not a re-derivation (which
	// would differ, because the handle rule uses chord LENGTHS and a non-uniform scale changes them).
	for (int i = 0; i < count; ++i)
	{
		m_cache.worldPos[i] = worldTM.TransformPoint(m_cache.localPos[i]);
		m_cache.worldHandles[i].back = worldTM.TransformPoint(m_cache.localHandles[i].back);
		m_cache.worldHandles[i].forw = worldTM.TransformPoint(m_cache.localHandles[i].forw);
	}

	// The arc-length table, measured in WORLD metres so that PosByDistance and TotalLength answer in
	// the units every consumer places things with. Counted locally rather than through
	// GetSegmentCount(), which reads the cache this function is still filling.
	const int segmentCount = (count < 2) ? 0 : (m_closed ? count : (count - 1));
	m_cache.segmentLength.resize(segmentCount);
	m_cache.segmentStart.resize(segmentCount);
	m_cache.totalLength = 0.0f;

	for (int s = 0; s < segmentCount; ++s)
	{
		// The two end points of this segment, computed HERE and not through SegmentPoints().
		//
		// This is the bug that crashed spline creation on the second click. SegmentPoints() reads
		// GetCache().localPos.size() - it has to, because with smoothing on the evaluation polygon
		// is not the authored point list any more - and GetCache() is this function, still building,
		// with m_cache.bValid still false. So the call recursed into itself until the stack ran out.
		// It could only fire once there was at least one segment, i.e. from the second point on,
		// which is exactly when the crash appeared. Nothing inside a cache build may read the cache.
		const int i0 = s % count;
		const int i1 = (s + 1) % count;

		m_cache.segmentStart[s] = m_cache.totalLength;
		m_cache.segmentLength[s] = SplineMath::SegmentLength(m_cache.worldPos[i0], m_cache.worldHandles[i0].forw,
		                                                     m_cache.worldHandles[i1].back, m_cache.worldPos[i1]);
		m_cache.totalLength += m_cache.segmentLength[s];
	}

	// Last, because it samples the curve the lines above just finished describing.
	BuildFrameNormals(m_cache);

	m_cache.worldTM = worldTM;
	m_cache.bValid = true;
	m_bBuildingCache = false;

	return m_cache;
}

Vec3 CSplineShapeComponent::SegmentPos(int segment, float t, bool world) const
{
	const SCache& cache = GetCache();

	// Fewer than two evaluation points is not a curve: answer the single point, or the origin when
	// there is not even one. The create tool drives exactly this state between the first click and
	// the second, so it is a normal case and not an error.
	const int count = static_cast<int>(cache.localPos.size());
	if (count < 2 || segment < 0)
		return (count == 0) ? Vec3(ZERO) : (world ? cache.worldPos[0] : cache.localPos[0]);

	int i0, i1;
	SegmentPoints(segment, i0, i1);

	const std::vector<Vec3>&                 pos = world ? cache.worldPos : cache.localPos;
	const std::vector<SplineMath::SHandles>& handles = world ? cache.worldHandles : cache.localHandles;

	return SplineMath::EvalSegmentPos(pos[i0], handles[i0].forw, handles[i1].back, pos[i1], t);
}

Vec3 CSplineShapeComponent::SegmentTangent(int segment, float t, bool world) const
{
	const SCache& cache = GetCache();

	// A single point has no direction. Returning zero rather than normalising nothing is what keeps
	// NaN out of every frame built downstream.
	const int count = static_cast<int>(cache.localPos.size());
	if (count < 2 || segment < 0)
		return ZERO;

	int i0, i1;
	SegmentPoints(segment, i0, i1);

	const std::vector<Vec3>&                 pos = world ? cache.worldPos : cache.localPos;
	const std::vector<SplineMath::SHandles>& handles = world ? cache.worldHandles : cache.localHandles;

	return SplineMath::EvalSegmentTangent(pos[i0], handles[i0].forw, handles[i1].back, pos[i1], t);
}

// ---------------------------------------------------------------------------
// ISplineShape
// ---------------------------------------------------------------------------

void CSplineShapeComponent::ParamToSegment(float t, int& indexOut, float& segmentTOut) const
{
	const int segmentCount = GetSegmentCount();
	if (segmentCount <= 0)
	{
		indexOut = 0;
		segmentTOut = 0.0f;
		return;
	}

	const float clamped = clamp_tpl(t, 0.0f, 1.0f) * segmentCount;

	int index = static_cast<int>(floorf(clamped));
	if (index >= segmentCount)
		index = segmentCount - 1;

	indexOut = index;
	segmentTOut = clamp_tpl(clamped - static_cast<float>(index), 0.0f, 1.0f);
}

Vec3 CSplineShapeComponent::EvalPos(float t) const
{
	int   segment = 0;
	float segmentT = 0.0f;
	ParamToSegment(t, segment, segmentT);

	return SegmentPos(segment, segmentT, true);
}

Vec3 CSplineShapeComponent::EvalTangent(float t) const
{
	int   segment = 0;
	float segmentT = 0.0f;
	ParamToSegment(t, segment, segmentT);

	return SegmentTangent(segment, segmentT, true);
}

Vec3 CSplineShapeComponent::GetLocalBezierNormal(float t) const
{
	const SCache& cache = GetCache();
	if (cache.localPos.size() < 2)
		return ZERO;

	int   segment = 0;
	float segmentT = 0.0f;
	ParamToSegment(t, segment, segmentT);

	int i0, i1;
	SegmentPoints(segment, i0, i1);

	const Vec3  tangent = SegmentTangent(segment, segmentT, false);
	const float angle0 = cache.evalAngle[i0];
	const float angle1 = cache.evalAngle[i1];

	if (m_frameMode == ESplineFrameMode::LegacyUp || cache.frameNormals.size() < 2)
	{
		// CSplineObject::GetLocalBezierNormal unchanged - the default, and byte for byte the legacy
		// look. It blends the two points' Angle with the cubic ease legacy uses.
		return SplineMath::SegmentNormal(tangent, angle0, angle1, segmentT);
	}

	// The carried frame, read at the same parameter. The normals are sampled uniformly along the
	// curve's parameter, so the lookup is a straight lerp between neighbouring samples.
	const int   sampleCount = static_cast<int>(cache.frameNormals.size());
	const float f = clamp_tpl(t, 0.0f, 1.0f) * static_cast<float>(sampleCount - 1);
	const int   index = clamp_tpl(static_cast<int>(floorf(f)), 0, sampleCount - 2);

	Vec3 normal = Vec3::CreateLerp(cache.frameNormals[index], cache.frameNormals[index + 1], f - static_cast<float>(index));

	// Re-orthogonalise against the real tangent at t: the lerp between two unit normals is neither
	// unit nor exactly perpendicular.
	if (!tangent.IsZero())
	{
		const Vec3 unitTangent = tangent.GetNormalized();
		normal -= unitTangent * normal.Dot(unitTangent);
	}

	if (normal.IsZero(SplineMath::kEps))
		return SplineMath::SegmentNormal(tangent, angle0, angle1, segmentT);

	normal.Normalize();

	// The authored roll goes on top, with the same blend legacy uses, so switching frame mode never
	// loses a banked point.
	const float blend = SplineMath::BlendAngleAlongSegment(segmentT);
	return SplineMath::ApplyRoll(normal, tangent, (1.0f - blend) * angle0 + blend * angle1);
}

Vec3 CSplineShapeComponent::EvalNormal(float t) const
{
	const Vec3 localNormal = GetLocalBezierNormal(t);
	if (localNormal.IsZero())
		return ZERO;

	Matrix34 rotation = GetShapeWorldTM();
	rotation.SetTranslation(ZERO);

	Vec3 worldNormal = rotation.TransformVector(localNormal);
	if (!worldNormal.IsZero())
		worldNormal.Normalize();

	return worldNormal;
}

float CSplineShapeComponent::GetLocalWidth(float t, float defaultWidth) const
{
	const SCache& cache = GetCache();
	if (cache.localPos.size() < 2)
		return defaultWidth;

	int   segment = 0;
	float segmentT = 0.0f;
	ParamToSegment(t, segment, segmentT);

	int i0, i1;
	SegmentPoints(segment, i0, i1);

	// A negative stored width is the "use the consumer's own width" sentinel BuildEvalPolygon wrote
	// for a point flagged Default Width.
	const float width0 = (cache.evalWidth[i0] < 0.0f) ? defaultWidth : cache.evalWidth[i0];
	const float width1 = (cache.evalWidth[i1] < 0.0f) ? defaultWidth : cache.evalWidth[i1];

	return SplineMath::BlendWidthAlongSegment(width0, width1, segmentT);
}

float CSplineShapeComponent::TotalLength() const
{
	return GetCache().totalLength;
}

Vec3 CSplineShapeComponent::PosByDistance(float distance) const
{
	const SCache& cache = GetCache();
	const int     segmentCount = GetSegmentCount();
	if (segmentCount <= 0)
		return cache.worldPos.empty() ? ZERO : cache.worldPos[0];

	const float clamped = clamp_tpl(distance, 0.0f, cache.totalLength);

	int segment = segmentCount - 1;
	for (int s = 0; s < segmentCount; ++s)
	{
		if (cache.segmentStart[s] + cache.segmentLength[s] > clamped)
		{
			segment = s;
			break;
		}
	}

	// The fraction of the segment's LENGTH is used as its PARAMETER, which is what
	// CSplineObject::GetPosByDistance (SplineObject.cpp:799-814) does and therefore what every
	// legacy consumer was tuned against. It is exact on a straight segment and slightly uneven on a
	// tight curve; a true arc-length reparameterisation would change the spacing of existing content.
	const float length = cache.segmentLength[segment];
	const float t = (length > 0.0f) ? ((clamped - cache.segmentStart[segment]) / length) : 0.0f;

	return SegmentPos(segment, clamp_tpl(t, 0.0f, 1.0f), true);
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

int CSplineShapeComponent::GetSampleCount() const
{
	const int count = static_cast<int>(GetCache().localPos.size());
	if (count < 2)
		return count;

	// Every segment contributes its start point plus the samples inside it; an open curve adds the
	// final end point, a closed one does not (its closing edge is implicit, as GetContour promises).
	return GetSegmentCount() * GetSamplesPerSegment() + (m_closed ? 0 : 1);
}

int CSplineShapeComponent::SampleCurve(Vec3* pOut, int maxPoints, bool world) const
{
	const int total = GetSampleCount();
	if (pOut == nullptr || maxPoints <= 0)
		return total;

	const int count = static_cast<int>(m_points.points.size());
	if (count == 1)
	{
		pOut[0] = world ? GetCache().worldPos[0] : GetCache().localPos[0];
		return total;
	}

	const int segmentCount = GetSegmentCount();

	int written = 0;
	for (int s = 0; s < segmentCount && written < maxPoints; ++s)
	{
		for (int k = 0; k < GetSamplesPerSegment() && written < maxPoints; ++k)
		{
			pOut[written++] = SegmentPos(s, static_cast<float>(k) / static_cast<float>(GetSamplesPerSegment()), world);
		}
	}

	if (!m_closed && written < maxPoints && segmentCount > 0)
	{
		pOut[written++] = SegmentPos(segmentCount - 1, 1.0f, world);
	}

	return total;
}

float CSplineShapeComponent::DistanceToCurve(const Vec3& world, Vec3* pNearestOut) const
{
	Vec3      samples[kMaxStackPoints];
	const int total = SampleCurve(samples, kMaxStackPoints, true);
	const int count = min(total, kMaxStackPoints);

	if (count == 0)
		return FLT_MAX;

	if (count == 1)
	{
		if (pNearestOut != nullptr)
			*pNearestOut = samples[0];
		return world.GetDistance(samples[0]);
	}

	float minDist = FLT_MAX;
	Vec3  nearest = samples[0];

	for (int i = 0; i + 1 < count; ++i)
	{
		Vec3        closest;
		const float d = ShapeGeo::PointToSegmentDistance(samples[i], samples[i + 1], world, closest);
		if (d < minDist)
		{
			minDist = d;
			nearest = closest;
		}
	}

	if (m_closed)
	{
		Vec3        closest;
		const float d = ShapeGeo::PointToSegmentDistance(samples[count - 1], samples[0], world, closest);
		if (d < minDist)
		{
			minDist = d;
			nearest = closest;
		}
	}

	if (pNearestOut != nullptr)
		*pNearestOut = nearest;

	return minDist;
}

// ---------------------------------------------------------------------------
// IShapeComponent - geometry
// ---------------------------------------------------------------------------

void CSplineShapeComponent::GetLocalAABB(AABB& out) const
{
	out.Reset();

	if (m_points.points.empty())
	{
		out = AABB(m_offset, m_offset);
		return;
	}

	// The curve, not the control polygon: a Bezier stays inside the hull of its control points and
	// handles, so an AABB over those would be noticeably larger than the shape the user sees.
	Vec3      samples[kMaxStackPoints];
	const int count = min(SampleCurve(samples, kMaxStackPoints, false), kMaxStackPoints);

	for (int i = 0; i < count; ++i)
		out.Add(samples[i]);

	// The control points too: with one point there is no curve, and they are what the tool grabs.
	for (const SSplinePoint& point : m_points.points)
		out.Add(m_offset + point.pos);
}

void CSplineShapeComponent::GetWorldAABB(AABB& out) const
{
	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetWorldTransformMatrix(), local);
}

bool CSplineShapeComponent::IsPointInside(const Vec3& world) const
{
	// An open spline is a path: it has no interior at all, which is exactly what the contract says
	// an open shape answers. A closed one is treated as the footprint it draws on XY.
	if (!m_closed || static_cast<int>(m_points.points.size()) < 3)
		return false;

	const Vec3 local = GetShapeWorldTM().GetInverted().TransformPoint(world);

	Vec3      samples[kMaxStackPoints];
	const int count = min(SampleCurve(samples, kMaxStackPoints, false), kMaxStackPoints);

	return ShapeGeo::IsPointInPolygon2D(samples, count, local);
}

float CSplineShapeComponent::DistanceToHull(const Vec3& world) const
{
	if (m_points.points.empty())
		return FLT_MAX;

	const float distance = DistanceToCurve(world, nullptr);

	// Negative inside, as the contract promises for a closed shape; an open spline has no inside and
	// its distance is always the plain distance to the curve.
	return IsPointInside(world) ? -distance : distance;
}

bool CSplineShapeComponent::IntersectRay(const Ray& ray, float& dist) const
{
	Vec3      samples[kMaxStackPoints];
	const int total = SampleCurve(samples, kMaxStackPoints, true);
	const int count = min(total, kMaxStackPoints);
	if (count < 2)
		return false;

	Vec3 direction = ray.direction;
	if (direction.IsZero())
		return false;
	direction.Normalize();

	const Vec3 rayLineP1 = ray.origin;
	const Vec3 rayLineP2 = ray.origin + direction * ShapeGeo::kRayDistance;

	float best = FLT_MAX;
	Vec3  bestPoint(ZERO);

	const int chordCount = m_closed ? count : (count - 1);
	for (int i = 0; i < chordCount; ++i)
	{
		const int j = (i + 1) % count;

		float d = 0.0f;
		Vec3  intPnt(ZERO);
		if (!ShapeGeo::RayToLineDistance(rayLineP1, rayLineP2, samples[i], samples[j], d, intPnt))
			continue;

		if (d > kRayTolerance)
			continue;

		const float alongRay = ray.origin.GetDistance(intPnt);
		if (alongRay < best)
		{
			best = alongRay;
			bestPoint = intPnt;
		}
	}

	if (best == FLT_MAX)
		return false;

	dist = best;
	return true;
}

bool CSplineShapeComponent::GetRandomPointInside(Vec3& out) const
{
	if (!m_closed || static_cast<int>(m_points.points.size()) < 3)
		return false;

	Vec3      samples[kMaxStackPoints];
	const int count = min(SampleCurve(samples, kMaxStackPoints, false), kMaxStackPoints);
	if (count < 3)
		return false;

	AABB footprint;
	footprint.Reset();
	for (int i = 0; i < count; ++i)
		footprint.Add(samples[i]);

	// Rejection sampling in the footprint AABB, the same method the polygon uses and the only one
	// that stays uniform over a concave contour without triangulating it.
	for (int attempt = 0; attempt < kSplineRandomPointAttempts; ++attempt)
	{
		const Vec3 candidate(cry_random(footprint.min.x, footprint.max.x),
		                     cry_random(footprint.min.y, footprint.max.y),
		                     cry_random(footprint.min.z, footprint.max.z));

		if (!ShapeGeo::IsPointInPolygon2D(samples, count, candidate))
			continue;

		out = GetShapeWorldTM().TransformPoint(candidate);
		return true;
	}

	return false;
}

int CSplineShapeComponent::GetContour(Vec3* pOutPoints, int maxPoints, bool world) const
{
	// The contour of a spline is the sampled curve - that is what a consumer drawing or fencing it
	// wants, and it is what the point tool draws as the hull.
	return SampleCurve(pOutPoints, maxPoints, world);
}

bool CSplineShapeComponent::IsSegmentObstructing(int index) const
{
	if (index < 0 || index >= static_cast<int>(m_points.points.size()))
		return false;

	return m_points.points[index].obstructSound;
}

// ---------------------------------------------------------------------------
// IEditorShapeComponent
// ---------------------------------------------------------------------------

bool CSplineShapeComponent::GetEditorLocalBounds(AABB& out) const
{
	if (m_points.points.empty())
		return false;

	AABB local;
	GetLocalAABB(local);
	out = AABB::CreateTransformedAABB(GetTransformMatrix(), local);
	return true;
}

bool CSplineShapeComponent::EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const
{
	// CSplineObject::HitTest (SplineObject.cpp:1376-1428): a spline is picked by the CURVE, chord by
	// chord, never by a volume - it has none.
	Vec3      samples[kMaxStackPoints];
	const int total = SampleCurve(samples, kMaxStackPoints, true);
	const int count = min(total, kMaxStackPoints);
	if (count < 2)
		return false;

	const Vec3 rayDir = worldRay.direction.GetNormalized();
	const Vec3 rayLineP1 = worldRay.origin;
	const Vec3 rayLineP2 = worldRay.origin + rayDir * ShapeGeo::kRayDistance;

	bool  bHit = false;
	float minDist = FLT_MAX;
	Vec3  bestPnt(ZERO);

	const int chordCount = m_closed ? count : (count - 1);
	for (int i = 0; i < chordCount; ++i)
	{
		const int j = (i + 1) % count;

		float d = 0.0f;
		Vec3  intPnt(ZERO);
		if (ShapeGeo::RayToLineDistance(rayLineP1, rayLineP2, samples[i], samples[j], d, intPnt) && d < minDist)
		{
			minDist = d;
			bestPnt = intPnt;
			bHit = true;
		}
	}

	if (!bHit || minDist > tolerance)
		return false;

	distOut = worldRay.origin.GetDistance(bestPnt);
	return true;
}

// ---------------------------------------------------------------------------
// IShapeComponentEdit
// ---------------------------------------------------------------------------

int CSplineShapeComponent::GetPointCount() const
{
	return static_cast<int>(m_points.points.size());
}

Vec3 CSplineShapeComponent::GetPoint(int index) const
{
	if (index < 0 || index >= static_cast<int>(m_points.points.size()))
		return ZERO;

	return m_offset + m_points.points[index].pos;
}

void CSplineShapeComponent::SetPoint(int index, const Vec3& local)
{
	if (index < 0 || index >= static_cast<int>(m_points.points.size()))
		return;

	m_points.points[index].pos = local - m_offset;
	RecordChange(EShapeChangeReason::Geometry);
}

int CSplineShapeComponent::InsertPoint(int index, const Vec3& local)
{
	const Vec3 pos = local - m_offset;
	const int  count = static_cast<int>(m_points.points.size());

	// Legacy refuses a point that lands on an existing one rather than creating a zero-length
	// segment nobody can grab again (SplineObject.cpp:707-711).
	for (int i = 0; i < count; ++i)
	{
		const Vec3 diff = m_points.points[i].pos - pos;
		if (fabs_tpl(diff.x) < kSplineDuplicatePointEpsilon &&
		    fabs_tpl(diff.y) < kSplineDuplicatePointEpsilon &&
		    fabs_tpl(diff.z) < kSplineDuplicatePointEpsilon)
		{
			CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING, "Shape: Spline - the point is too close to another point!");
			return i;
		}
	}

	SSplinePoint point;
	point.pos = pos;

	int newIndex;
	if (index < 0 || index >= count)
	{
		m_points.points.push_back(point);
		newIndex = static_cast<int>(m_points.points.size()) - 1;
	}
	else
	{
		m_points.points.insert(m_points.points.begin() + index, point);
		newIndex = index;
	}

	RecordChange(EShapeChangeReason::Topology);
	return newIndex;
}

void CSplineShapeComponent::RemovePoint(int index)
{
	const int count = static_cast<int>(m_points.points.size());
	if (index < 0 || index >= count)
		return;

	if (count <= kMinPoints)
	{
		CryWarning(VALIDATOR_MODULE_ENTITYSYSTEM, VALIDATOR_WARNING,
		           "Shape: Spline - a spline needs at least %d points, the point was not removed.", kMinPoints);
		return;
	}

	m_points.points.erase(m_points.points.begin() + index);
	RecordChange(EShapeChangeReason::Topology);
}

int CSplineShapeComponent::GetEdgePoints(int index, Vec3* pOut, int maxPoints) const
{
	// The tool asks this so that Ctrl+click inserts on the CURVE and the highlight follows it.
	// Samples are in the shape's local space, both end points included.
	//
	// `index` is an AUTHORED edge - the tool only ever knows the authored points. With smoothing on,
	// that edge is 2^SmoothIterations evaluation segments wide, and the subdivision is interpolating,
	// so authored point i is exactly evaluation point i * stride (SplineMath.h). That is the whole
	// mapping, and it is why the scheme had to be an interpolating one.
	const SCache& cache = GetCache();

	const int authoredCount = static_cast<int>(m_points.points.size());
	const int authoredEdges = (authoredCount < 2) ? 0 : (m_closed ? authoredCount : (authoredCount - 1));
	if (pOut == nullptr || maxPoints < 2 || index < 0 || index >= authoredEdges)
		return 0;

	const int stride = max(1, cache.evalStride);
	const int firstSegment = index * stride;
	const int segmentCount = GetSegmentCount();
	if (firstSegment >= segmentCount)
		return 0;

	const int usedSegments = min(stride, segmentCount - firstSegment);
	const int stepsPerSegment = max(1, min(GetSamplesPerSegment(), (maxPoints - 1) / usedSegments));

	int written = 0;
	for (int segment = 0; segment < usedSegments; ++segment)
	{
		for (int k = 0; k < stepsPerSegment && written < maxPoints; ++k)
		{
			const float t = static_cast<float>(k) / static_cast<float>(stepsPerSegment);
			pOut[written++] = SegmentPos(firstSegment + segment, t, false);
		}
	}

	if (written < maxPoints)
	{
		pOut[written++] = SegmentPos(firstSegment + usedSegments - 1, 1.0f, false);
	}

	return (written >= 2) ? written : 0;
}

void CSplineShapeComponent::BeginEdit()
{
	m_bInEdit = true;
	m_bChangedDuringEdit = false;
	m_editReason = EShapeChangeReason::Geometry;
}

void CSplineShapeComponent::EndEdit()
{
	const bool               bChanged = m_bChangedDuringEdit;
	const EShapeChangeReason reason = m_editReason;

	m_bInEdit = false;
	m_bChangedDuringEdit = false;
	m_editReason = EShapeChangeReason::Geometry;

	if (bChanged)
		NotifyListeners(reason);
}

void CSplineShapeComponent::RecordChange(EShapeChangeReason reason)
{
	InvalidateCache();

	if (!m_bInEdit)
	{
		NotifyListeners(reason);
		return;
	}

	// Coarsest reason wins: a gesture that both moved and inserted points is a topology change.
	if (!m_bChangedDuringEdit || static_cast<int>(reason) > static_cast<int>(m_editReason))
		m_editReason = reason;

	m_bChangedDuringEdit = true;
}

// ---------------------------------------------------------------------------
// Listeners and events
// ---------------------------------------------------------------------------

void CSplineShapeComponent::AddListener(IShapeListener* pListener)
{
	if (pListener == nullptr)
		return;
	if (std::find(m_listeners.begin(), m_listeners.end(), pListener) == m_listeners.end())
		m_listeners.push_back(pListener);
}

void CSplineShapeComponent::RemoveListener(IShapeListener* pListener)
{
	m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), pListener), m_listeners.end());
}

void CSplineShapeComponent::NotifyListeners(EShapeChangeReason reason)
{
	m_lastNotifiedPointCount = static_cast<int>(m_points.points.size());

	// Copy first: a listener is allowed to remove itself while being told.
	const std::vector<IShapeListener*> listeners = m_listeners;
	for (IShapeListener* pListener : listeners)
		pListener->OnShapeChanged(*this, reason);
}

void CSplineShapeComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_XFORM:
		InvalidateCache();
		NotifyListeners(EShapeChangeReason::Transform);
		break;
	case ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED:
		{
			InvalidateCache();

			const int pointCount = static_cast<int>(m_points.points.size());
			NotifyListeners(pointCount != m_lastNotifiedPointCount ? EShapeChangeReason::Topology
			                                                       : EShapeChangeReason::Geometry);
		}
		break;
	default:
		break;
	}
}

Cry::Entity::EventFlags CSplineShapeComponent::GetEventMask() const
{
	return ENTITY_EVENT_XFORM | ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED;
}

void CSplineShapeComponent::OnShutDown()
{
	m_listeners.clear();
}

// ---------------------------------------------------------------------------
// Previewer
// ---------------------------------------------------------------------------

#ifndef RELEASE
void CSplineShapeComponent::Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const
{
	IRenderAuxGeom* pAux = gEnv->pAuxGeomRenderer;
	if (pAux == nullptr)
		return;

	Vec3      samples[kMaxStackPoints];
	const int total = SampleCurve(samples, kMaxStackPoints, true);
	const int count = min(total, kMaxStackPoints);
	if (count < 2)
		return;

	// The legacy colours, so a component spline standing next to a legacy one looks the same
	// (ShapeDisplay.h): the Area blue unselected, the editor's pulsing selection colour selected.
	const ColorB colour = ShapeDisplay::GetOutlineColour(context.bSelected);
	const ColorB obstructColour = ShapeDisplay::GetHighlightColour(context.bSelected);

	const int chordCount = m_closed ? count : (count - 1);
	for (int i = 0; i < chordCount; ++i)
	{
		const int j = (i + 1) % count;

		// Which AUTHORED segment this chord belongs to, so a sound-obstructing segment reads as one.
		// One authored segment is `evalStride` evaluation segments, each of GetSamplesPerSegment()
		// chords, so the divisor is the product.
		const int    chordsPerAuthoredSegment = max(1, GetSamplesPerSegment() * max(1, GetCache().evalStride));
		const int    segment = i / chordsPerAuthoredSegment;
		const ColorB chordColour = IsSegmentObstructing(segment) ? obstructColour : colour;

		pAux->DrawLine(samples[i], chordColour, samples[j], chordColour);
	}

	// With smoothing on, the curve no longer hugs the control polygon, so the polygon is drawn
	// faintly beside it: the user can see both what they authored and what it evaluates to, and the
	// point handles the edit tool shows sit on the faint one.
	if (m_smoothIterations > 0)
	{
		const Matrix34 shapeTM = GetShapeWorldTM();
		const int      authoredCount = static_cast<int>(m_points.points.size());
		const ColorB   polygonColour(colour.r, colour.g, colour.b, 70);

		const int authoredEdges = m_closed ? authoredCount : (authoredCount - 1);
		for (int i = 0; i < authoredEdges; ++i)
		{
			const Vec3 a = shapeTM.TransformPoint(m_offset + m_points.points[i].pos);
			const Vec3 b = shapeTM.TransformPoint(m_offset + m_points.points[(i + 1) % authoredCount].pos);
			pAux->DrawLine(a, polygonColour, b, polygonColour);
		}
	}

	// A short tick across the curve wherever a point carries an explicit width, so that authored
	// width is visible without opening the inspector. Half a width to each side, as a road is built.
	const int pointCount = static_cast<int>(m_points.points.size());
	const int segmentCount = GetSegmentCount();
	if (segmentCount <= 0)
		return;

	for (int i = 0; i < pointCount; ++i)
	{
		const SSplinePoint& point = m_points.points[i];
		if (point.defaultWidth || point.width <= 0.0f)
			continue;

		// Authored point i is evaluation point i * stride (the subdivision interpolates), so its
		// parameter along the whole curve is that index over the number of evaluation segments.
		const int   evalIndex = min(i * max(1, GetCache().evalStride), segmentCount);
		const float t = static_cast<float>(evalIndex) / static_cast<float>(segmentCount);

		const Vec3 normal = EvalNormal(t);
		if (normal.IsZero())
			continue;

		const Vec3 centre = EvalPos(t);
		const Vec3 half = normal * (point.width * 0.5f);

		pAux->DrawLine(centre - half, colour, centre + half, colour);
	}
}
#endif

} // namespace AreaComponents
} // namespace Cry
