# Diff RenderDoc capture pairs: marker regions (time / draw count) and GPU resources.
#   qrenderdoc.exe --python rdc_diff.py
# Python 3.6 compatible.
import os
import sys
import traceback

OUT_PATH = os.path.join(r"S:\Crytek\crytek\cryengine-57-lts\renderdoc", "rdc_diff_report.txt")
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

CAPTURE_DIR = r"S:\Crytek\crytek\cryengine-57-lts\renderdoc"
PAIRS = [("3.rdc", "4.rdc", "ours, no ccam", "stock sandbox"),
         ("1.rdc", "2.rdc", "shaped bokeh", "6-blade iris")]


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

    ctrl.Shutdown()
    cap.Shutdown()
    return {"ms": total[0], "draws": total[1], "markers": markers, "texs": texs, "bufs": bufs}


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
