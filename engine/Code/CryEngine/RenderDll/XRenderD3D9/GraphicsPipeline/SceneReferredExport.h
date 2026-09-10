// Copyright 2026 ReC Sandbox. Scene-referred pipeline, stage S6 (SceneReferredSpec.md).

#pragma once

#include "Common/GraphicsPipeline.h"
#include "Common/UtilityPasses.h"
#include "Common/Textures/Image/ExrImage.h"

#include <CryThreading/IJobManager.h>
// The shared definition of the display chain's two 1D curves. The plugin bakes them into the LUT
// texture with this header; the master curve's .cube sidecar below is evaluated with the same one,
// so there is exactly one answer to what the curve between two knots is.
#include <CryRenderer/SceneReferredCurves.h>

class CTexture;

//! What the exported frame holds.
enum ESceneReferredExportTap
{
	//! The graded, pre-ODT value, taken off SV_Target1 of HDRFinalPass: post white balance, post
	//! CDL, post LMT, post bloom/shafts/vignette composite, and back in linear ACEScg. This is the
	//! ACCEPTANCE instrument - the only tap for which "apply the same ODT elsewhere and compare"
	//! is a test of the output transform rather than of every operator in front of it.
	eSceneReferredExportTap_Graded = 0,

	//! $HDRTarget as it enters the tone map: exposed, converted to ACEScg, ungraded, unvignetted,
	//! un-composited. The camera NEGATIVE - what a cinematographer actually takes into a DI suite.
	//! A different artefact from the graded tap, not a variant of it.
	eSceneReferredExportTap_Negative = 1,
};

//! The colour the file claims to be.
enum ESceneReferredExportEncoding
{
	//! ACES2065-1: AP0 primaries, ST 2065-4 container. The AP1 -> AP0 matrix runs on the CPU
	//! during the de-interleave. Default, because an untagged EXR in an ACES project is ASSUMED to
	//! be ACES2065-1 - the user does nothing and the file is read correctly.
	eSceneReferredExportEncoding_ACES2065 = 0,

	//! ACEScg: AP1, the buffer byte for byte, `chromaticities` only and NEVER the container flag -
	//! the flag is a conformance claim about AP0 data and would be a false one here.
	eSceneReferredExportEncoding_ACEScg = 1,
};

//! Everything about one captured frame that is not its pixels. Sampled on the render thread at
//! issue time and carried to the writer job with the pixels, because by the time the job runs the
//! camera has moved on.
struct SSceneReferredExportMetadata
{
	SSceneReferredExportMetadata()
		: focalLength(0.0f), tStop(0.0f), iso(0.0f), shutterAngle(0.0f), shutterTime(0.0f)
		, nd(0.0f), whiteBalanceK(0.0f), whiteBalanceTint(0.0f), ev100(0.0f), exposureScale(1.0f)
		, frameIndex(0), renderFrameId(0), engineTime(0.0f), fps(0.0f)
	{}

	string cameraName;
	string lensName;
	string odtName;
	string lmtName;

	//! The EFFECTIVE ASC CDL - what the tone map really evaluated for this frame, not what the
	//! camera was asked for: Bypass Grade folded in (a bypassed frame carries the identity,
	//! because that is what is in the picture) and the renderer's own slope/power floors applied,
	//! so that the numbers in the header and in the .cdl sidecar are the numbers the shader used.
	Vec3   cdlSlope = Vec3(1.0f, 1.0f, 1.0f);
	Vec3   cdlOffset = Vec3(0.0f, 0.0f, 0.0f);
	Vec3   cdlPower = Vec3(1.0f, 1.0f, 1.0f);
	float  cdlSaturation = 1.0f;
	bool   bCdlBypassed = false;

	//! The same numbers as one line, ASC order and ASC names, for the `rec/cdl` attribute.
	string cdl;

	//! The two 1D curves of S10 item 3b, exactly as the camera published them: the master curve's
	//! five knot offsets IN STOPS and the five Sat vs Sat multipliers. Zeroed to the neutral when
	//! Bypass Grade was on, for the same reason the CDL is - the file has to describe the picture
	//! that was rendered, not the numbers that were typed.
	float  curveMasterStops[SceneReferredCurves::kKnotCount] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float  curveSatMult[SceneReferredCurves::kKnotCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	bool   bCurveMaster = false;
	bool   bCurveSat = false;

	//! Both curves as one line, for the `rec/curves` attribute. Empty when neither is in the frame.
	string curves;

	float  focalLength;
	float  tStop;
	float  iso;
	float  shutterAngle;
	float  shutterTime;
	float  nd;
	float  whiteBalanceK;
	float  whiteBalanceTint;
	float  ev100;
	float  exposureScale;

	int    frameIndex;
	int    renderFrameId;
	float  engineTime;
	float  fps;
};

//! The capture. One at a time, global, and owned by whichever graphics pipeline's tone map runs
//! first while it is armed - the editor runs more than one pipeline and a capture that hopped
//! between them would silently interleave two views into one sequence.
//!
//! Deliberately NOT part of the stock capture path: `capture_frames`, `RT_ReadTexture`,
//! `SCaptureFormatInfo` and `ICaptureFrameListener` are untouched, so the byte-identity harness
//! that reads TGA out of exactly that code stays valid (research/s6-capture-path.md section 8).
class CSceneReferredExport
{
public:
	static CSceneReferredExport& Get();

	//! Ring depth. Two would mostly work; three is what makes "frame N+1's copy is never waiting on
	//! frame N's map" true at a frame rate that varies (SceneDepth.cpp uses a ring for the same
	//! reason). The cost is one more full-res RGBA16F staging surface while armed, and nothing at
	//! all while idle - the targets are created on arm and released on disarm.
	static const int kRingSize = 3;

	//! Arm a sequence. Returns false and logs if one is already running.
	bool StartSequence(const char* szFolder, const char* szPrefix);
	//! Arm exactly one frame. Unlike a sequence, the numbering CONTINUES across calls: single
	//! frames are taken one at a time all session long and every one of them landing on
	//! frame.000000.exr makes the command useless for the thing it exists for.
	bool RequestSingleFrame(const char* szFolder, const char* szPrefix);
	//! Disarm. Drains the writer jobs and releases the ring. Safe when nothing is armed.
	void Stop(const char* szReason);

	bool IsArmed() const     { return m_state != eState_Idle; }
	//! True only while a graded-tap capture is armed: the one condition under which HDRFinalPass
	//! grows its second render target.
	bool WantsGradedTap() const;

	// ---- render thread, driven by CToneMappingStage ----

	//! Consume anything the GPU has finished, then hand back the ring slot this frame should be
	//! written into (nullptr when nothing is armed, the pipeline is not the owner, or the ring is
	//! saturated). Creates the ring on first use.
	CTexture* BeginFrame(CGraphicsPipeline& pipeline, int width, int height);

	//! The copy is done (MRT write, or the negative's blit): issue the fence-carrying download.
	void EndFrame();

	//! Shut everything down - called when the pipeline that owns the capture goes away.
	void Shutdown();

private:
	CSceneReferredExport();

	enum EState
	{
		eState_Idle = 0,
		eState_Sequence,
		eState_SingleFrame,
	};

	struct SRingSlot
	{
		SRingSlot() : bIssued(false), fIssueTime(0.0f) {}

		_smart_ptr<CTexture>         pTex;
		bool                         bIssued;
		float                        fIssueTime;
		SSceneReferredExportMetadata metadata;
		string                       path;

		//! Where this frame's ASC .cdl sidecar goes, and the id it carries - EMPTY on every frame
		//! but the first of a capture. One CDL describes one grade and a take has one name, so the
		//! sidecar is written once per capture, from the first frame that actually came back (the
		//! first moment the numbers are known to belong to a file that exists).
		string                       cdlPath;
		string                       cdlId;

		//! Where this capture's two curve sidecars go - the master curve as a Resolve 1D .cube and
		//! both curves' control points as text. Empty on every frame but the first of a capture,
		//! and empty for the whole capture when neither curve is doing anything: unlike the .cdl,
		//! an absent curve file is not ambiguous, because the .cdl beside it always dates the take.
		string                       curveCubePath;
		string                       curveTextPath;

		//! One writer job per ring slot, so the ring bounds the queue by construction. If a slot's
		//! job is still running when the ring comes round to it, the disk is not keeping up and the
		//! capture STOPS and says so - it never drops a frame, because a hole in a sequence is
		//! invisible until someone tries to conform it.
		JobManager::SJobState        jobState;
	};

	void ReleaseRing();
	void ConsumeCompleted();
	bool ConsumeSlot(SRingSlot& slot);
	void GatherMetadata(SSceneReferredExportMetadata& out) const;
	void SetFixedStep(bool bEnable);
	//! Arm the one-per-capture sidecars - the .cdl and the two curve files. Called from both
	//! arming paths.
	void ArmCdlSidecar();

	//! The first index for which <folder>/<prefix>.%06d.exr does not exist on disk, searched from
	//! nStartAt upwards. The session counter is only a HINT into this: files deleted between two
	//! captures should be reused, and files left over from a previous session must not be
	//! overwritten, and neither is knowable from a counter alone.
	static int FirstFreeIndex(const string& folder, const string& prefix, int nStartAt);

	EState                m_state;

	//! Tap and encoding are LATCHED on arm, not read per frame. Changing either half way through a
	//! sequence would leave a folder of files that look alike and mean different things, and the
	//! only clue would be the attributes - which are written at readback time, one to two frames
	//! after the pixels were produced.
	int                   m_tap;
	int                   m_encoding;

	string                m_folder;
	string                m_prefix;
	int                   m_frameIndex;
	int                   m_nextSlot;

	//! Where the NEXT single frame should start looking for a free number, and the folder/prefix
	//! that number belongs to. Session-lifetime (this is a function-local static singleton), reset
	//! to 0 whenever the destination changes, and only ever a starting point for the disk scan.
	string                m_singleFrameKey;
	int                   m_singleFrameNext = 0;

	//! The .cdl sidecar, one per capture: armed on Start/Request, cleared by the first frame that
	//! claims it. `m_cdlSidecarText` is what that frame wrote, kept only so that a LATER frame
	//! whose grade has moved (a TrackView-animated grade, which one CDL cannot represent) can be
	//! named once in a warning instead of silently disagreeing with the file.
	bool                  m_bCdlSidecarPending = false;
	bool                  m_bCdlDriftWarned = false;
	string                m_cdlSidecarText;

	//! The curve sidecars, armed and claimed the same way. Separate from the CDL's flag because the
	//! two are gated by different cvars AND because the curve files are only written when a curve
	//! is doing something - a capture whose curves are neutral must not consume the claim and then
	//! write nothing, or a later frame could not write them either.
	bool                  m_bCurveSidecarPending = false;
	int                   m_width;
	int                   m_height;
	const void*           m_pOwner;

	SRingSlot             m_ring[kRingSize];

	//! t_FixedStep and the lens-model jitter phase, saved on arm and put back on disarm. Nothing
	//! else locks frame time outside the editor's CryMovie path, so a sequence captured in game
	//! mode has no cadence at all unless the capture sets it itself.
	bool                  m_bTimeStepOverridden;
	float                 m_fSavedFixedStep;
	int                   m_nSavedJitter;

};

//! The job payload. Owns its pixels; the job frees it.
struct SSceneReferredExportJob
{
	std::vector<CryHalf>         pixels;      //!< interleaved RGBA, packed (already de-pitched)
	int                          width = 0;
	int                          height = 0;
	string                       path;
	SExrWriteDesc                desc;
	std::vector<SExrAttribute>   attributes;
};
