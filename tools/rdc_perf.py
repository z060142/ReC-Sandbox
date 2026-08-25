# RenderDoc per-pass GPU timing report. Run inside qrenderdoc's embedded Python:
#   qrenderdoc.exe --python rdc_perf.py
# Python 3.6 compatible on purpose.
import os
import sys
import traceback

LOG_PATH = os.path.join(r"S:\Crytek\crytek\cryengine-57-lts\renderdoc", "rdc_perf_log.txt")
_log = open(LOG_PATH, "w", encoding="utf-8")
_log.write("script started, python %s\n" % sys.version)
_log.flush()
try:
    import renderdoc as rd
    _log.write("renderdoc module OK: %s\n" % str(rd))
    _log.flush()
except Exception:
    _log.write(traceback.format_exc())
    _log.close()
    os._exit(1)

CAPTURE_DIR = r"S:\Crytek\crytek\cryengine-57-lts\renderdoc"
CAPTURES = sorted(f for f in os.listdir(CAPTURE_DIR) if f.lower().endswith(".rdc"))
OUT_PATH = os.path.join(CAPTURE_DIR, "rdc_perf_report.txt")
MAX_DEPTH = 4
MIN_MS = 0.02

out = open(OUT_PATH, "w", encoding="utf-8")


def log(s=""):
    out.write(s + "\n")
    out.flush()


def analyze(path):
    log("=" * 100)
    log("CAPTURE: %s" % path)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    if res != rd.ResultCode.Succeeded:
        log("  OpenFile failed: %s" % str(res))
        return
    if not cap.LocalReplaySupport():
        log("  local replay not supported")
        return
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        log("  OpenCapture failed: %s" % str(res))
        return

    props = ctrl.GetAPIProperties()
    log("  API: %s" % str(props.pipelineType))

    counters = ctrl.EnumerateCounters()
    if rd.GPUCounter.EventGPUDuration not in counters:
        log("  EventGPUDuration counter unavailable; counters: %s" % str(counters))
        ctrl.Shutdown()
        return

    # Replay timing is dominated by GPU clock state; fetch several times, keep the
    # per-event minimum (the run that had the clocks up).
    dur = {}
    for i in range(5):
        for r in ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration]):
            v = r.value.d * 1000.0  # ms
            if r.eventId not in dur or v < dur[r.eventId]:
                dur[r.eventId] = v

    roots = ctrl.GetRootActions()

    def subtree_time(a):
        t = 0.0
        n = 0
        if a.children:
            for c in a.children:
                ct, cn = subtree_time(c)
                t += ct
                n += cn
        else:
            t = dur.get(a.eventId, 0.0)
            n = 1 if (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear | rd.ActionFlags.Copy | rd.ActionFlags.Resolve)) else 0
        return t, n

    total = 0.0
    total_n = 0
    for a in roots:
        t, n = subtree_time(a)
        total += t
        total_n += n
    log("  Frame GPU total: %.3f ms over %d actions" % (total, total_n))
    log("")

    def name_of(a):
        try:
            return a.GetName(ctrl.GetStructuredFile())
        except Exception:
            return str(a.customName)

    def walk(a, depth):
        t, n = subtree_time(a)
        nm = name_of(a)
        is_marker = bool(a.flags & rd.ActionFlags.PushMarker) or bool(a.children)
        if t >= MIN_MS and (is_marker or depth <= 1):
            log("  %s%-60s %8.3f ms  (%d)" % ("  " * depth, nm[:60], t, n))
        if a.children and depth < MAX_DEPTH:
            for c in a.children:
                walk(c, depth + 1)

    for a in roots:
        walk(a, 0)

    # Dedicated DOF digest: any marker whose name contains these tokens, summed by name.
    log("")
    log("  -- DOF-related markers (summed by name) --")
    agg = {}
    def collect(a):
        nm = name_of(a)
        up = nm.upper()
        if a.children and any(k in up for k in ("DOF", "DEPTH OF FIELD", "BOKEH", "SPLAT", "FAR/NEAR", "COC")):
            t, n = subtree_time(a)
            e = agg.setdefault(nm, [0.0, 0, 0])
            e[0] += t
            e[1] += n
            e[2] += 1
        if a.children:
            for c in a.children:
                collect(c)
    for a in roots:
        collect(a)
    for nm in sorted(agg, key=lambda k: -agg[k][0]):
        t, n, cnt = agg[nm]
        log("  %-60s %8.3f ms  actions=%d  occurrences=%d" % (nm[:60], t, n, cnt))

    ctrl.Shutdown()
    cap.Shutdown()


for c in CAPTURES:
    try:
        _log.write("analyzing %s\n" % c)
        _log.flush()
        analyze(os.path.join(CAPTURE_DIR, c))
    except Exception:
        log("  EXCEPTION on %s:\n%s" % (c, traceback.format_exc()))
        _log.write(traceback.format_exc())
        _log.flush()

log("DONE")
out.close()
_log.write("done\n")
_log.close()
os._exit(0)
