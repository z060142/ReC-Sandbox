# Diff RenderDoc capture pairs: marker regions (time / draw count), GPU resources, and - for
# the byte-identity harness - a per-pass hash of every bound colour target, so a pair of
# captures that should be identical can be told WHERE they first stopped being identical.
#
#   qrenderdoc.exe --python rdc_diff.py [capture_dir] [A.rdc B.rdc ["label A" "label B"]]
#
# The capture directory can also come from the RDC_CAPTURE_DIR environment variable. With no
# arguments the legacy path and the legacy PAIRS list below are used unchanged.
#
# Procedure: ReC-Sandbox/tools/README-bitcompare.md.
# Python 3.6 compatible (qrenderdoc embeds its own interpreter).
import hashlib
import os
import sys
import traceback

LEGACY_CAPTURE_DIR = r"S:\Crytek\crytek\cryengine-57-lts\renderdoc"

# argv[0] is the script; qrenderdoc passes everything after it through.
_args = sys.argv[1:]
CAPTURE_DIR = os.environ.get("RDC_CAPTURE_DIR", LEGACY_CAPTURE_DIR)
if _args and os.path.isdir(_args[0]):
    CAPTURE_DIR = _args.pop(0)

OUT_PATH = os.path.join(CAPTURE_DIR, "rdc_diff_report.txt")
out = open(OUT_PATH, "w", encoding="utf-8")


def log(s=""):
    out.write(s + "\n")
    out.flush()


try:
    import renderdoc as rd
except Exception:
    log(traceback.format_exc())
    out.close()
    os._exit(1)

PAIRS = [("3.rdc", "4.rdc", "ours, no ccam", "stock sandbox"),
         ("1.rdc", "2.rdc", "shaped bokeh", "6-blade iris")]

if len(_args) >= 2:
    _labelA = _args[2] if len(_args) >= 4 else _args[0]
    _labelB = _args[3] if len(_args) >= 4 else _args[1]
    PAIRS = [(_args[0], _args[1], _labelA, _labelB)]

# Per-pass content hashing. Off by default: it re-replays the frame once per marker region and
# downloads every bound colour target. Set RDC_HASH_PASSES=1 for a byte-identity run.
HASH_PASSES = os.environ.get("RDC_HASH_PASSES", "0") not in ("0", "", "false", "False")
# How deep into the marker tree to hash. 1 = only the top-level regions; 2 also names the
# individual passes inside them, which is usually where the answer is.
HASH_DEPTH = int(os.environ.get("RDC_HASH_DEPTH", "2"))
# Hard cap, so a pathological frame cannot turn this into an overnight job.
HASH_MAX_REGIONS = int(os.environ.get("RDC_HASH_MAX_REGIONS", "400"))


def hash_passes(ctrl, sf):
    """Replay the frame once per marker region and hash every colour target bound at the end of
    it. Returns an ordered list of (region_key, {target_name: sha1}).

    This is what turns "the two captures differ" into "they first differ at TONEMAPPING": the
    metadata diff below only proves the pass list and the render-target FORMATS match, never
    that the pixels do. GetTextureData() gives the raw subresource bytes, so the comparison is
    exact and happens at fp16 in the middle of the chain rather than at 8 bits on the back
    buffer.
    """
    regions = []

    def walk(a, prefix, depth):
        if not a.children or depth > HASH_DEPTH or len(regions) >= HASH_MAX_REGIONS:
            return
        key = prefix + "/" + a.GetName(sf)
        last = a.children[-1]
        while last.children:
            last = last.children[-1]
        regions.append((key, last.eventId))
        for c in a.children:
            walk(c, key, depth + 1)

    for a in ctrl.GetRootActions():
        walk(a, "", 1)

    result = []
    for key, eid in regions:
        ctrl.SetFrameEvent(eid, True)
        state = ctrl.GetPipelineState()
        hashes = {}
        try:
            targets = state.GetOutputTargets()
        except Exception:
            targets = []
        for t in targets:
            rid = getattr(t, "resource", None)
            if rid is None:
                rid = getattr(t, "resourceId", None)
            if rid is None or rid == rd.ResourceId.Null():
                continue
            try:
                data = ctrl.GetTextureData(rid, rd.Subresource(0, 0, 0))
            except Exception:
                continue
            hashes[str(rid)] = hashlib.sha1(bytes(data)).hexdigest()
        result.append((key, hashes))
    return result


def gather(path):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("OpenFile failed " + path)
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        raise RuntimeError("OpenCapture failed " + path)

    dur = {}
    for r in ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration]):
        dur[r.eventId] = r.value.d * 1000.0
    sf = ctrl.GetStructuredFile()

    markers = {}
    total = [0.0, 0]

    def subtree(a):
        t, n = 0.0, 0
        if a.children:
            for c in a.children:
                ct, cn = subtree(c)
                t += ct
                n += cn
        else:
            t = dur.get(a.eventId, 0.0)
            n = 1 if (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear | rd.ActionFlags.Copy | rd.ActionFlags.Resolve)) else 0
        return t, n

    def walk(a, prefix):
        if a.children:
            key = prefix + "/" + a.GetName(sf)
            t, n = subtree(a)
            e = markers.setdefault(key, [0.0, 0, 0])
            e[0] += t
            e[1] += n
            e[2] += 1
            for c in a.children:
                walk(c, key)

    for a in ctrl.GetRootActions():
        t, n = subtree(a)
        total[0] += t
        total[1] += n
        walk(a, "")

    names = {}
    for r in ctrl.GetResources():
        names[r.resourceId] = r.name
    texs = {}
    for t in ctrl.GetTextures():
        name = names.get(t.resourceId, str(t.resourceId))
        key = "%s %dx%dx%d mips=%d arr=%d %s" % (name, t.width, t.height, t.depth, t.mips, t.arraysize, str(t.format.Name()))
        texs[key] = texs.get(key, 0) + t.byteSize
    bufs = {}
    for b in ctrl.GetBuffers():
        name = names.get(b.resourceId, str(b.resourceId))
        key = "%s %d bytes" % (name, b.length)
        bufs[key] = bufs.get(key, 0) + b.length

    passes = []
    if HASH_PASSES:
        try:
            passes = hash_passes(ctrl, sf)
        except Exception:
            log("  per-pass hashing failed for %s:" % path)
            log(traceback.format_exc())

    ctrl.Shutdown()
    cap.Shutdown()
    return {"ms": total[0], "draws": total[1], "markers": markers, "texs": texs, "bufs": bufs,
            "passes": passes}


def report(A, B, labelA, labelB):
    a = gather(os.path.join(CAPTURE_DIR, A))
    b = gather(os.path.join(CAPTURE_DIR, B))
    log("#" * 100)
    log("A = %s (%s)   B = %s (%s)" % (A, labelA, B, labelB))
    for tag, d in (("A", a), ("B", b)):
        hdr = [k for k in d["texs"] if ("HDRTarget" in k or "SceneDiffuse" in k or "BackBuffer" in k or "SceneTarget" in k or "SceneDepth" in k)]
        log("  %s render targets:" % tag)
        for k in sorted(hdr)[:10]:
            log("      " + k[:120])
    log("Frame GPU: A %.3f ms / %d draws    B %.3f ms / %d draws" % (a["ms"], a["draws"], b["ms"], b["draws"]))
    log("Texture bytes: A %.1f MB    B %.1f MB   (count %d / %d)" % (sum(a["texs"].values()) / 1e6, sum(b["texs"].values()) / 1e6, len(a["texs"]), len(b["texs"])))
    log("Buffer  bytes: A %.1f MB    B %.1f MB   (count %d / %d)" % (sum(a["bufs"].values()) / 1e6, sum(b["bufs"].values()) / 1e6, len(a["bufs"]), len(b["bufs"])))

    ma, mb = a["markers"], b["markers"]
    log("")
    log("== Marker regions ONLY in A ==")
    for k in sorted(set(ma) - set(mb), key=lambda k: -ma[k][0]):
        log("  %-90s %8.3f ms  draws=%d  x%d" % (k[-90:], ma[k][0], ma[k][1], ma[k][2]))
    log("")
    log("== Marker regions ONLY in B ==")
    for k in sorted(set(mb) - set(ma), key=lambda k: -mb[k][0]):
        log("  %-90s %8.3f ms  draws=%d  x%d" % (k[-90:], mb[k][0], mb[k][1], mb[k][2]))
    log("")
    log("== Common regions, sorted by |delta| (A - B), top 40 ==")
    common = set(ma) & set(mb)
    rows = [k for k in common if abs(ma[k][0] - mb[k][0]) >= 0.02 or ma[k][1] != mb[k][1]]
    for k in sorted(rows, key=lambda k: -abs(ma[k][0] - mb[k][0]))[:40]:
        d = ma[k][0] - mb[k][0]
        log("  %-90s A %8.3f  B %8.3f  delta %+8.3f ms  draws %d/%d" % (k[-90:], ma[k][0], mb[k][0], d, ma[k][1], mb[k][1]))

    if HASH_PASSES:
        log("")
        log("== Per-pass colour target hashes (RDC_HASH_PASSES=1) ==")
        pa, pb = a["passes"], b["passes"]
        if not pa or not pb:
            log("  no hashes were collected - see the traceback above, or the RenderDoc build")
            log("  does not expose GetTextureData / GetOutputTargets in Python.")
        else:
            ka = [k for k, _ in pa]
            kb = [k for k, _ in pb]
            if ka != kb:
                log("  REGION LISTS DIFFER - the pass list is not the same, so there is no")
                log("  meaningful 'first differing pass'. Read the marker sections above.")
            first = None
            hb = dict(pb)
            for key, ha in pa:
                if key not in hb:
                    continue
                names = set(ha) | set(hb[key])
                bad = sorted(n for n in names if ha.get(n) != hb[key].get(n))
                if bad:
                    first = (key, bad, ha, hb[key])
                    break
            if first is None:
                log("  every hashed region produced identical target contents in A and B.")
            else:
                key, bad, ha, hbk = first
                log("  FIRST DIFFERING REGION: %s" % key)
                for n in bad:
                    log("    %-60s A %s  B %s" % (n[:60], str(ha.get(n))[:16], str(hbk.get(n))[:16]))
                log("  (regions after this one are not reported: once a target diverges,")
                log("   everything downstream of it does too.)")

    log("")
    log("== Textures ONLY in A (top 40 by size) ==")
    for k in sorted(set(a["texs"]) - set(b["texs"]), key=lambda k: -a["texs"][k])[:40]:
        log("  %-115s %8.2f MB" % (k[:115], a["texs"][k] / 1e6))
    log("")
    log("== Textures ONLY in B (top 40 by size) ==")
    for k in sorted(set(b["texs"]) - set(a["texs"]), key=lambda k: -b["texs"][k])[:40]:
        log("  %-115s %8.2f MB" % (k[:115], b["texs"][k] / 1e6))
    log("")


for A, B, la, lb in PAIRS:
    try:
        report(A, B, la, lb)
    except Exception:
        log(traceback.format_exc())

log("DONE")
out.close()
os._exit(0)
