// Copyright 2026 ReC Sandbox. The scene-referred display chain's two 1D curves.
//
// ONE definition of what a curve IS, shared by the two modules that need it
// (SceneReferredSpec.md S10 item 3b, decisions/s10-grade-component.md section 3b.5):
//
//   * CinematicCamera (the plugin) bakes the control points into the 1D LUT texture the tone map
//     samples, and publishes the control points on the post-effect bus;
//   * CryRenderD3D11 (CSceneReferredExport) reads those control points back off the bus and
//     writes the master curve out as a Resolve 1D .cube beside a capture.
//
// Two copies of the interpolator would be exactly the CPU/GPU drift risk item 3a refused to take
// when it folded contrast on the CPU rather than adding a shader constant. This header is the same
// kind of shared numeric truth RENDERER_LIGHT_UNIT_SCALE and SSceneReferredMeterHistogram already
// are, so nothing new is invented for it: header only, no state, no allocation, no dependency
// beyond CryMath.
#pragma once

#include <CryMath/Cry_Math.h>

namespace SceneReferredCurves
{

//! Five control points per curve - the on-set norm (black, shadow, mid, highlight, white) and the
//! number a colourist's hands already know. Frozen: the bus layout, the reflected member names and
//! the sidecar format all assume it.
enum { kKnotCount = 5 };

//! Entries in the baked 1D LUT. 1024 over the ACEScct domain is ~0.1 stop per texel before the
//! sampler's own linear interpolation, i.e. far finer than the curve it is sampling, and the whole
//! texture is 8 KiB in RGBA16F. There is no reason to be stingy here and every reason not to be
//! visible.
enum { kLutSize = 1024 };

//! ACEScct(0.18) - 18 % grey, the one anchor the pre-exposure guarantees. Same landmark as
//! CommonMath.cfi:269 and kACEScctMidGrey in CineGradeComponent.h.
const float kACEScctMidGrey = 0.4135878f;

//! ACESCCT_LOG_SCALE (CommonMath.cfi:280): one photographic stop is 1/17.52 of an ACEScct code
//! value, above the toe. This is what makes "an offset in stops" an exact, linear thing to do to a
//! knot's height.
const float kACEScctStopsPerUnit = 17.52f;

//! The Sat vs Sat curve's x axis: sat = saturate(length(c - luma) * this), in ACEScct, with the
//! ASC Rec.709 luma weights the CDL saturation uses. x = 1 is therefore a chroma vector of length
//! 0.5 in the log working space = 8.8 stops of spread between the extreme channels; measured
//! against content a strong red reads ~0.54 and a near-primary ~0.96
//! (decisions/s10-grade-component.md section 3b.3). Published to the shader in
//! SceneCurveParams.w so that HLSL, C++ and the exported sidecar cannot disagree about it.
const float kSatFullScale = 2.0f;

// -----------------------------------------------------------------------------------------
// The ACEScct shaper, on the CPU (S10 item 4).
//
// One definition, and this is where it belongs: the constants and the two landmarks above are
// already here, and the HLSL half at CommonMath.cfi:271-300 was until now the only copy in the
// tree. The plugin needs it to wrap a foreign LUT into the grading space at load time
// (decisions/s10-grade-component.md 4.2); the renderer does not call it yet, and having it here
// is what stops a third copy from appearing when something does.
//
// Constants VERBATIM from CommonMath.cfi:271-276, which took them from OCIO's
// "CURVE - ACEScct-LOG_to_LINEAR" LogCameraTransform. Landmarks: LinearToACEScct(0.18) =
// 0.41358781 (= kACEScctMidGrey), ACEScctToLinear(1.0) = 222.86 (= +10.3 stops over grey), and
// -B/A = -0.00691688 is the most negative linear value the encode still represents.
// -----------------------------------------------------------------------------------------
const float kACEScctXBrk      = 0.0078125000f;
const float kACEScctYBrk      = 0.1552511416f;
const float kACEScctA         = 10.5402377417f;
const float kACEScctB         = 0.0729055342f;
const float kACEScctLogOffset = 9.7200000000f;
const float kACEScctLogScale  = 17.5200000000f;   //!< == kACEScctStopsPerUnit, by construction

inline float LinearToACEScct(float x)
{
	// log2 of a non-positive number is not defined; the toe segment covers every value the log
	// segment cannot, so the branch is the guard as well as the encoding.
	return (x < kACEScctXBrk)
	       ? (kACEScctA * x + kACEScctB)
	       : ((logf(x) / logf(2.0f) + kACEScctLogOffset) / kACEScctLogScale);
}

inline float ACEScctToLinear(float y)
{
	return (y < kACEScctYBrk)
	       ? ((y - kACEScctB) / kACEScctA)
	       : powf(2.0f, y * kACEScctLogScale - kACEScctLogOffset);
}

inline Vec3 LinearToACEScct(const Vec3& c)
{
	return Vec3(LinearToACEScct(c.x), LinearToACEScct(c.y), LinearToACEScct(c.z));
}

inline Vec3 ACEScctToLinear(const Vec3& c)
{
	return Vec3(ACEScctToLinear(c.x), ACEScctToLinear(c.y), ACEScctToLinear(c.z));
}

//! The master curve's knots, as ACEScct code values: the bottom of the encoding, four stops under
//! mid grey, mid grey, four stops over, and the top of the encoding (= +10.3 stops over grey,
//! since ACEScctToLinear(1.0) = 222.86).
inline const float* MasterKnotX()
{
	static const float s_x[kKnotCount] =
	{
		0.0f,
		kACEScctMidGrey - 4.0f / kACEScctStopsPerUnit,
		kACEScctMidGrey,
		kACEScctMidGrey + 4.0f / kACEScctStopsPerUnit,
		1.0f
	};
	return s_x;
}

//! The Sat vs Sat curve's knots, on the measured-saturation axis. Evenly spaced because that axis
//! has no landmarks: it is a normalised distance from the grey axis, not a photographic quantity.
inline const float* SatKnotX()
{
	static const float s_x[kKnotCount] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
	return s_x;
}

//! Monotone cubic Hermite interpolation - Fritsch & Carlson 1980, the PCHIP of every numerical
//! library, with the Fritsch-Butland weighted harmonic mean for the interior tangents.
//!
//! Chosen over Catmull-Rom because Catmull-Rom overshoots between close knots, and an overshoot in
//! a tone curve is visible ringing around a highlight. This one cannot overshoot: where two
//! neighbouring secants disagree in sign the tangent is pinned to zero, and where they agree the
//! harmonic mean is bounded by three times the smaller secant, which is Fritsch & Carlson's
//! sufficient condition for monotonicity.
//!
//! The property that matters most here: on COLLINEAR knots every secant is equal, every tangent
//! takes that value, and the cubic collapses to the straight line exactly (h00 + h01 = 1 and
//! h01 + h10 + h11 = s). So an identity curve interpolates to an identity, at any knot spacing.
//!
//! x must be strictly increasing. t outside [x[0], x[n-1]] is clamped to the ends.
inline float Evaluate(const float* x, const float* y, float t)
{
	float h[kKnotCount - 1];
	float d[kKnotCount - 1];
	for (int i = 0; i < kKnotCount - 1; ++i)
	{
		h[i] = max(x[i + 1] - x[i], 1e-6f);
		d[i] = (y[i + 1] - y[i]) / h[i];
	}

	float m[kKnotCount];
	m[0] = d[0];
	m[kKnotCount - 1] = d[kKnotCount - 2];
	for (int i = 1; i < kKnotCount - 1; ++i)
	{
		if (d[i - 1] * d[i] <= 0.0f)
		{
			// A local extremum: a flat tangent is what keeps the curve from running past the knot.
			m[i] = 0.0f;
		}
		else
		{
			const float w1 = 2.0f * h[i] + h[i - 1];
			const float w2 = h[i] + 2.0f * h[i - 1];
			m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
		}
	}

	if (t <= x[0])
		return y[0];
	if (t >= x[kKnotCount - 1])
		return y[kKnotCount - 1];

	int k = 0;
	while (k < kKnotCount - 2 && t > x[k + 1])
		++k;

	const float s = (t - x[k]) / h[k];
	const float s2 = s * s;
	const float s3 = s2 * s;
	const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
	const float h10 = s3 - 2.0f * s2 + s;
	const float h01 = -2.0f * s3 + 3.0f * s2;
	const float h11 = s3 - s2;

	return h00 * y[k] + h10 * h[k] * m[k] + h01 * y[k + 1] + h11 * h[k] * m[k + 1];
}

//! The master curve's knot HEIGHTS, from the five authored offsets in stops. Neutral (all zero) is
//! y == x, i.e. the identity, exactly - which is the whole reason the property is an offset in
//! stops rather than an absolute code value.
inline void MasterKnotY(const float* pStops, float* pOutY)
{
	const float* const px = MasterKnotX();
	for (int i = 0; i < kKnotCount; ++i)
		pOutY[i] = px[i] + pStops[i] / kACEScctStopsPerUnit;
}

//! Exact tests, deliberately: the neutral value of an offset in stops is the literal 0.0f and of a
//! multiplier the literal 1.0f, so "has the user touched this" needs no epsilon. A neutral curve's
//! fetch is not issued at all, which is what makes an unused curve free and the neutral picture
//! bit-identical to the build before this existed.
inline bool IsMasterNeutral(const float* pStops)
{
	for (int i = 0; i < kKnotCount; ++i)
		if (pStops[i] != 0.0f)
			return false;
	return true;
}

inline bool IsSatNeutral(const float* pMultipliers)
{
	for (int i = 0; i < kKnotCount; ++i)
		if (pMultipliers[i] != 1.0f)
			return false;
	return true;
}

//! Sample the master curve onto a uniform grid over the ACEScct domain [0,1]. Sample i sits at
//! i/(n-1), which is what a .cube's LUT_1D_SIZE means and what the shader's half-texel inset
//! reproduces.
//!
//! The output is clamped to [-1, 2]: a knot dragged several stops can put the interpolant well
//! outside anything the ODT can show, and an fp16 texel is not the place to find that out.
inline void BakeMaster(const float* pStops, float* pOut, int n)
{
	float y[kKnotCount];
	MasterKnotY(pStops, y);
	const float* const px = MasterKnotX();
	const float scale = 1.0f / (float)max(n - 1, 1);
	for (int i = 0; i < n; ++i)
		pOut[i] = clamp_tpl(Evaluate(px, y, (float)i * scale), -1.0f, 2.0f);
}

//! Sample the Sat vs Sat curve. Negative multipliers would invert the chroma vector - a colour
//! rotated 180 degrees about grey - which is never what "less saturation" means, so the floor is 0.
inline void BakeSat(const float* pMultipliers, float* pOut, int n)
{
	const float* const px = SatKnotX();
	const float scale = 1.0f / (float)max(n - 1, 1);
	for (int i = 0; i < n; ++i)
		pOut[i] = clamp_tpl(Evaluate(px, pMultipliers, (float)i * scale), 0.0f, 4.0f);
}

} // namespace SceneReferredCurves
