# Step-0 frame-cost diagnosis for the Orion RT backend.
#
# Turns the existing PERF-L10 GPU timestamp scopes and the ORION_PERF_ABLATION
# arms into one attribution table, so the choice between the candidate perf levers
# (light-table memory residency vs per-ray cost vs guide stores) rests on our own
# measured milliseconds instead of on inference from Orion's profile.
#
# It adds no instrumentation.  `probe` prints exactly what this tree cannot measure
# yet, and what the smallest follow-up change would be.
#
# Log shapes consumed (kept in sync with _perf_l10.py, which cannot be imported
# because it calls main() at module scope):
#   [PERF L10 GPU] frame=<n> win=60 ablate=<stage> ser=.. aoSkip=.. dispatch=WxH
#                  avgMs frame=<ms> vli=.. far=.. setup=.. wsim=.. caus=.. sky=..
#                  rt=.. cloud=.. dlss=.. sdr=.. blit=.. fg=.. tail=..
#   [PERF L10 CPU] avgMs=.. minMs=.. maxMs=.. workAvgMs=.. workMaxMs=.. cpuFps=..
#   [PERF L10 ABLATE] / [BUILD] orion-rt / [OMM NATIVE] / [SER-1 PROBE]
#
# run/rt_debug.log is appended across sessions by multiple writers, so it is not a
# reliable time series.  `run` records the file size before launching and archives
# only the bytes that session appended.
#
#   python tools/perf_step0.py probe
#   python tools/perf_step0.py report perf/E_optim_accept.log
#   python tools/perf_step0.py diff perf/A_ablation.log perf/E_optim_accept.log
#   python tools/perf_step0.py run --stages 2,3,4,5,6,10,11,12 --windows 6 --dry-run
#   python tools/perf_step0.py run --stages sweep

import argparse
import bisect
import hashlib
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME_LOG = os.path.join(ROOT, "run", "rt_debug.log")
OUT_DIR = os.path.join(ROOT, "perf", "step0")

# 2026-10-28: 与 VulkanCore.cpp 的 perfL10GpuScopeNames（:2137-2160 附近）对齐。
# 旧表停在 2026-10-24 之前，缺 wfft/wcaus/cloudpipe/rtpipe/sfar/sharcup/gi/trace/
# snear/height —— 而主 RayGen 那 20+ ms 现在记在 **trace** 上，旧表把它整段漏掉，
# 于是 probe 报出 "rt=0.02ms 占 0.1%" 这种结论。缺段会让归因表直接指向错误的东西，
# 所以这张表必须跟着 C++ 的枚举走。
GPU_SCOPES = ["vli", "far", "setup", "wsim", "wfft", "wcaus", "caus",
              "cloudpipe", "rtpipe", "sfar", "sharcup", "sky", "gi", "trace",
              "rt", "snear", "height", "cloud", "dlss", "sdr", "blit", "fg", "tail"]

# 归因锚点。2026-10-28 之前这里是 "rt"，但 rt 段现在只剩 SHaRC resolve
# （实测 0.02 ms 恒定，SHaRC 默认关），主 RayGen 的 20+ ms 记在 **trace** 上。
# 用 rt 做锚点会让每条消融臂的 dRt 恒为 0，脚本却仍然打印"harness is behaving"——
# 这正是历史上"空臂得出错误结论"的同一种失效模式，所以锚点必须跟着 C++ 的枚举走。
PRIMARY_SCOPE = "trace"

# PERF_L10_ABLATION_STAGE_NAMES (VulkanCore.cpp:1470), which must stay in order with
# the stage predicates in rt_test.rgen:256-375.  Index order matters: `run --stages`
# sends these numbers straight into CameraUBO.perfAblationStage.x, and `report` reads
# the names back out of the log, so a wrong table here silently ablates the wrong
# feature while the report still looks right.
STAGE_NAMES = {
    0: "base",
    1: "noRtao",             # perfL10DisableRtao
    2: "noGlossy",           # perfL10DisableGlossy
    3: "noGi",               # perfL10DisableDiffuseGi
    4: "noRtaoGlossyGi",     # 1+2+3 together (the additivity check)
    5: "noG2bGuide",         # perfL10DisableG2bLocalGuide -- the guide STORE
    6: "noEmitterScan",      # perfL10DisableLocalEmitterHitScan
    7: "noG2bEmitter",       # 5+6 together
    8: "gi1Bounce",          # perfL10GiBounceCap -> 1
    9: "ao2of4",             # perfL10AoSampleCap -> 2
    10: "shadow1of2",        # perfL10ShadowSampleCap -> 1
    11: "rrGi",              # force GI roulette ON
    12: "march1",            # perfL10MarchSteps -> 1 unconditionally
    13: "tierHalf",          # halve every distance-LOD threshold
    14: "rrOff",             # force GI roulette OFF (the only arm that can ADD work)
    15: "spare15",           # 2026-10-21: 原 noIndirectPom 撤销（POM 不可砍），槽位保留
    16: "noVoxelDda",        # 2026-10-13: 体素 DDA 遍历（棱缝门 + 本地光二级弹射）
}

# 2026-10-08: 5/6/7 were NO-OPS in the deployed build.  Their predicates had no live
# call site (perfL10DisableLocalEmitterHitScan was never called at all) or were
# short-circuited by LL_P3_ENABLE_G2B_LOCAL_GUIDE = false, so the ~0 paired dRt those
# arms reported was "nothing was removed", not "this feature is free".  rt_test.rgen
# now wires them to the things their names claim:
#   5 -> the four unconditional RR guide imageStores (rt_test.rgen RR-6.2)
#   6 -> traceVptLocalLightVisibility(), the local-light visibility ray
#   7 -> both
# The log's arm labels still come from the DLL's string table (VulkanCore.cpp:1470),
# which was NOT rebuilt this round, so a row named "noG2bGuide" in a post-deploy log
# means "RR guide stores off".  Any future C++ change should rename those three
# entries to match.

DLL_SLOTS = [
    ("run (loaded)", os.path.join(ROOT, "run", "rtx_backend.dll")),
    ("bundle src (authoritative)", os.path.join(ROOT, "src", "main", "resources",
                                                "natives", "windows", "rtx_backend.dll")),
    ("cmake output", os.path.join(ROOT, "cpp", "build", "Release", "rtx_backend.dll")),
    ("repo root", os.path.join(ROOT, "rtx_backend.dll")),
]
SPV_DIRS = [("cpp", os.path.join(ROOT, "cpp", "shaders")),
            ("run", os.path.join(ROOT, "run", "shaders"))]
SPV_WATCH = ["rt_test.rgen.spv", "rt_test_ser.rgen.spv", "rt_test.rchit.spv",
             "rt_test.rmiss.spv", "volumetric_clouds.comp.spv", "rr_sdr_post.comp.spv"]

COUNTER_MARKER = "VPT_RT_STAGE_COUNTERS"

# runClient recipe.  JAVA_HOME/GRADLE_USER_HOME are the in-tree ones; --no-daemon
# is what lets the game JVM inherit the ORION_PERF_* environment at all.
GRADLE_ENV = {
    "JAVA_HOME": "D:/fabric-example-mod-26.2/voxy-26.3-dev/.toolchains/jdk-25.0.4.1",
    "GRADLE_USER_HOME": os.path.join(ROOT, ".gradle-user-home"),
}
GRADLE_ARGS = ["gradlew.bat", "runClient", "--no-daemon",
               "-Dorg.gradle.java.installations.auto-detect=false",
               "-Dorg.gradle.java.installations.auto-download=false"]

BLIND_SPOT = """
-- WHAT THIS TREE CANNOT MEASURE YET --
`rt` is one opaque scope.  Our heaviest paired capture put rt=33.8ms inside
frame=41.9ms (perf/C_altpair.log), so ~80% of the mod's GPU frame is a single
unlabelled block.  The 14 ablation arms do carve a lot out of it -- GI, glossy,
RTAO, the GI bounce cap, the G2b guide STORE (stage 5), the emitter hit scan
(stage 6), soft-shadow samples, the solar march and the distance-LOD tiers --
and in that capture all of those except GI/glossy/march land within noise.

What is still NOT separable, and it is the whole residual left after `noGi`:
  BVH traversal | material + texture evaluation | the local-light dependent READ
  (the arms touch the light table's producer side, never the consumer read that
  runs per pixel at shading time).
Those three are what the device-local-memory lever and the per-ray-cost lever both
argue about, so deciding between them needs 3 counters inside rt_test.rgen, one
atomicAdd per traced ray into a storage buffer, read back on the same 60-frame
window as the timestamps:
  [0] rays issued  [1] traversal steps / hits accepted  [2] material evals
Smallest change that gets them:
  1. rt_test.rgen: a `#ifdef VPT_RT_STAGE_COUNTERS` block at the 3 sites, plus a
     new storage-buffer binding (62 slots are declared, 58 distinct, so room exists).
  2. VulkanCore.cpp: one small device-local + host-visible counter buffer allocated
     beside the existing perf pool (VulkanCore.cpp:4219), zeroed per window and
     printed as an [RT-CTR] line right where the window line is emitted
     (VulkanCore.cpp:2187).
  3. One cached getenv gate so the counters cost nothing when off, copying the
     perfTimestampsEnabled style (VulkanCore.cpp:1371).
Then recompile BOTH rgen variants (SER and non-SER) with -D VPT_RT_STAGE_COUNTERS,
rebuild the DLL, and deploy the spv to cpp/shaders AND run/shaders, the DLL to the
bundle source AND run/ -- a DLL only copied into run/ silently reverts next launch.
Until then the ns/pixel figures are an upper bound on what per-ray work can explain.
"""


def md5(path):
    if not os.path.isfile(path):
        return None
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def kv(text):
    out = {}
    for tok in text.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def parse_log(path, offset=0):
    """Return (windows, cpu_rows, context_notes, cnt_rows) from `path`.

    cnt_rows: [RT STAGE CNT] window counter lines (rays / hits / mateval).
    """
    windows, cpu_rows, notes, cnt_rows = [], [], [], []
    if not os.path.isfile(path):
        return windows, cpu_rows, notes, cnt_rows
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        if offset:
            f.seek(offset)
        for line in f:
            if "[PERF L10 GPU]" in line:
                body = line.split("[PERF L10 GPU]", 1)[1].replace("avgMs frame=", "frameMs=")
                if "frameMs=" not in body:
                    continue          # the maxMs half of the pair, and noise lines
                d = kv(body)
                w = {"ablate": str(d.get("ablate", "?")), "dispatch": str(d.get("dispatch", "?")),
                     "ser": d.get("ser"), "aoSkip": d.get("aoSkip"), "clock": line[4:12],
                     "fidx": d.get("frame") if isinstance(d.get("frame"), float) else None,
                     "win": d.get("win") if isinstance(d.get("win"), float) else None}
                w["frame"] = d.get("frameMs") if isinstance(d.get("frameMs"), float) else None
                for s in GPU_SCOPES:
                    w[s] = d.get(s) if isinstance(d.get(s), float) else None
                if w["frame"] is not None:
                    windows.append(w)
            elif "[RT STAGE CNT]" in line:
                d = kv(line.split("[RT STAGE CNT]", 1)[1])
                cnt_rows.append({
                    "frame": d.get("frame"),
                    "win": d.get("win"),
                    "rays": d.get("rays"),
                    "hits": d.get("hits"),
                    "mateval": d.get("mateval"),
                    "frames": d.get("frames"),
                })
            elif "[PERF L10 CPU]" in line:
                cpu_rows.append(kv(line.split("[PERF L10 CPU]", 1)[1]))
            elif any(tag in line for tag in ("[PERF L10 ABLATE]", "[BUILD] orion-rt",
                                             "[OMM NATIVE]", "[SER-1 PROBE]", "[RR-7 SWITCH]",
                                             "[SL RESULT]")):
                notes.append(line.strip()[:190])
    return windows, cpu_rows, notes, cnt_rows


def sig_of(w):
    return "dispatch=%s ser=%s aoSkip=%s" % (w["dispatch"], w["ser"], w["aoSkip"])


def group_by_sig(windows):
    out = {}
    for w in windows:
        out.setdefault(sig_of(w), []).append(w)
    return out


def mean(rows, key):
    vals = [r[key] for r in rows if isinstance(r.get(key), float)]
    return (sum(vals) / len(vals), len(vals)) if vals else (None, 0)


# ---------------------------------------------------------------- probe --------

def probe():
    print("== perf_step0 probe ==")
    print("root           : %s" % ROOT)
    if not os.path.isfile(RUNTIME_LOG):
        print("runtime log    : MISSING at %s" % RUNTIME_LOG)
        print(BLIND_SPOT)
        return 1

    windows, cpu, notes, _cnt = parse_log(RUNTIME_LOG)
    print("runtime log    : %.1f MB, %.1f d old, %d GPU windows"
          % (os.path.getsize(RUNTIME_LOG) / 1048576.0,
             (time.time() - os.path.getmtime(RUNTIME_LOG)) / 86400.0, len(windows)))

    for pat, label in (("[BUILD] orion-rt", "build"), ("[OMM NATIVE]", "omm"),
                       ("[SER-1 PROBE]", "ser"), ("[RR-7 SWITCH]", "exposure"),
                       ("[SL RESULT]", "streamline")):
        hit = [n for n in notes if pat in n]
        if hit:
            print("%-15s: %s" % (label + " (latest)", hit[-1].split("] ", 1)[-1][:118]))

    if not windows:
        print("windows        : none. ORION_PERF_TIMESTAMPS=1 was not set for any"
              " recorded session -> nothing to attribute yet.  Use `run`.")
    else:
        sig = sig_of(windows[-1])
        last = group_by_sig(windows)[sig]
        print("\nsignature      : %s  (%d windows)" % (sig, len(last)))
        frame = mean(last, "frame")[0]
        for s, (v, n) in sorted(((s, mean(last, s)) for s in GPU_SCOPES),
                                key=lambda x: -(x[1][0] or 0)):
            if v is None:
                continue
            mark = "<== dominant" if v and frame and v / frame > 0.30 else ""
            print("  %-6s %7.2f ms  %5.1f%%  n=%d %s" % (s, v, 100.0 * v / frame, n, mark))
        print("  frame    %7.2f ms  = %.1f fps" % (frame, 1000.0 / frame))
        if cpu:
            c = cpu[-1]
            print("\ncpu            : avg=%s ownWork=%s max=%s cpuFps=%s"
                  % (c.get("avgMs"), c.get("workAvgMs"), c.get("maxMs"), c.get("cpuFps")))
            print("                 (avg includes vanilla; ownWork is the mod's CPU cost."
                  " ownWork << avg means CPU is not our bottleneck.)")
        arms = sorted(set(w["ablate"] for w in windows))
        print("ablation arms  : %s" % (", ".join(arms) if len(arms) > 1 else
                                       "only %s -> no ablation was recorded" % arms[0]))

    print("\ndeploy identity  : (a measurement on a stale DLL or spv is worthless)")
    digests = {}
    for label, p in DLL_SLOTS:
        d = md5(p)
        digests[label] = d
        print("  %-28s %s%s" % (label, (d[:12] + "..") if d else "ABSENT",
                                "  %.1f d" % ((time.time() - os.path.getmtime(p)) / 86400.0)
                                if os.path.isfile(p) else ""))
    if len(set(x for x in digests.values() if x)) > 1:
        print("  !! several different DLL builds present; the loaded one is run/,"
              " verify it is the build you intend to measure")
    for name in SPV_WATCH:
        a = md5(os.path.join(SPV_DIRS[0][1], name))
        b = md5(os.path.join(SPV_DIRS[1][1], name))
        print("  %-28s cpp=%s run=%s%s" % (name, (a or "")[:8] or "ABSENT", (b or "")[:8] or "ABSENT",
                                           "" if (a and a == b) else "  <-- DIFFER"))

    rgen = os.path.join(ROOT, "cpp", "shaders", "rt_test.rgen")
    instrumented = False
    if os.path.isfile(rgen):
        with open(rgen, "r", encoding="utf-8", errors="replace") as f:
            instrumented = COUNTER_MARKER in f.read()
    print("\nin-shader counters: %s" % ("present" if instrumented else "ABSENT"))
    if not instrumented:
        print(BLIND_SPOT)
    return 0


# --------------------------------------------------------------- report --------

def stage_table(rows):
    by_stage = {}
    for r in rows:
        by_stage.setdefault(r["ablate"], []).append(r)
    base = by_stage.get("base")
    if not base or len(base) < 2:
        return [], None, 0.0
    rt_base = mean(base, PRIMARY_SCOPE)[0]
    if rt_base is None:
        return [], None, 0.0
    base_spread = pctl(rt_spread(base))
    out = []
    for stage, rs in by_stage.items():
        if stage == "base" or len(rs) < 2:
            continue
        rt = mean(rs, PRIMARY_SCOPE)[0]
        if rt is None:
            continue
        out.append((stage, rt, rt - rt_base, mean(rs, "cloud")[0] or 0.0,
                    mean(rs, "frame")[0] or 0.0, len(rs), pctl(rt_spread(rs))))
    return sorted(out, key=lambda x: x[2]), rt_base, base_spread


def rt_spread(rows):
    """A single arm re-measured over its own windows: how far apart those windows are.

    Scene content drifts while the player looks around, so this is the floor under any
    delta the harness produces -- a dRt smaller than it is a camera move, not a saving.
    """
    vals = sorted(r[PRIMARY_SCOPE] for r in rows
                  if isinstance(r.get(PRIMARY_SCOPE), float))
    if len(vals) < 4:
        return None
    return vals[int(0.10 * (len(vals) - 1))], vals[int(0.90 * (len(vals) - 1))], len(vals)


def pctl(spread):
    if not spread:
        return 0.0
    lo, hi, _ = spread
    return hi - lo


# ---------------------------------------------------------- paired deltas ------

def med(vals):
    v = sorted(vals)
    if not v:
        return None
    i = len(v) // 2
    return v[i] if len(v) % 2 else 0.5 * (v[i - 1] + v[i])


def paired_deltas(rows, key=PRIMARY_SCOPE):
    """Each arm window measured against the base window(s) adjacent to it in frame order.

    `alt:` mode alternates arm,base,arm,base, so an arm's neighbours are the same scene
    one 60-frame window away.  Comparing an arm to its own neighbours instead of to the
    session-wide base mean cancels camera/scene drift -- and it has to be done that way:
    in perf/A_ablation.log the camera moved during the session and mean-vs-mean put 7 of
    the 13 sign-correct arms above zero, i.e. "removing GI made rt slower".

    Returns {arm: [(delta, ref_ms, arm_ms), ...]}.
    """
    seq = sorted((r for r in rows if r.get("fidx") is not None), key=lambda r: r["fidx"])
    bases = [i for i, r in enumerate(seq) if r["ablate"] == "base"]
    out = {}
    for i, r in enumerate(seq):
        if r["ablate"] == "base" or not isinstance(r.get(key), float):
            continue
        k = bisect.bisect_left(bases, i)
        cand = []
        for j in (k - 1, k):
            if 0 <= j < len(bases):
                b = seq[bases[j]]
                span = abs(b["fidx"] - r["fidx"])
                win = r.get("win") or b.get("win") or 60.0
                if isinstance(b.get(key), float) and span <= 2.5 * win:
                    cand.append(b)
        if not cand:
            continue
        ref = sum(b[key] for b in cand) / len(cand)
        out.setdefault(r["ablate"], []).append((r[key] - ref, ref, r[key]))
    return out


def paired_table(rows, rt_base):
    """Print the drift-free attribution: median paired delta per arm, with its own scatter."""
    got = {}
    for key, base_ms in ((PRIMARY_SCOPE, rt_base),
                         ("frame", mean([r for r in rows if r["ablate"] == "base"],
                                        "frame")[0])):
        pd = paired_deltas(rows, key)
        if not pd:
            print("      paired      : no adjacent base windows found -- was this session run"
                  " with ORION_PERF_ABLATION=alt:...?  (a cross-session sweep cannot be paired)")
            return {}
        got.update(pd)
        arms = len(set(r["ablate"] for r in rows if r["ablate"] != "base"))
        if len(pd) < arms:
            print("\n      paired drt : only %d of %d arms had a base window within one window of"
                  " themselves." % (len(pd), arms))
            print("                 This log is a sequential sweep (120 frames per stage), so most"
                  " arms sit far from any base window and cannot be paired.")
            print("                 Its unpaired numbers above are scene-drift contaminated."
                  " Re-measure with `run --stages ...` (alt: interleaving) for paired deltas.")
            return got
        if base_ms:
            print("\n      paired d%s : each arm against the base window(s) bracketing it in frame"
                  " order (drift-free). This is the table to read." % key)
            print("      %-18s %9s %9s %8s %8s %4s" %
                  ("stage", "d"+key+" med", "mean", "scatter", "pct", "n"))
            for arm, items in sorted(pd.items(), key=lambda x: med([d for d, _, _ in x[1]])):
                ds = [d for d, _, _ in items]
                scatter = (max(ds) - min(ds)) if len(ds) > 1 else 0.0
                print("      %-18s %9.2f %9.2f %8.2f %+7.0f%% %4d"
                      % (arm, med(ds), sum(ds) / len(ds), scatter,
                         100.0 * med(ds) / base_ms, len(ds)))
            ok = [arm for arm in pd if arm != "rrOff" and med([d for d, _, _ in pd[arm]]) <= 0.0]
            bad = [arm for arm in pd if arm != "rrOff" and med([d for d, _, _ in pd[arm]]) > 0.0]
            print("      sign check  : %d/%d arms land <= 0 as their definition requires.%s"
                  % (len(ok), len(ok) + len(bad),
                     ("  Still positive: " + ", ".join(bad)) if bad else
                     "  Harness is behaving -- paired deltas are usable."))
    return paired_deltas(rows, PRIMARY_SCOPE)


def self_check(table, rt_base, base_spread):
    """Sanity-check the harness before letting anyone read deltas as attribution.

    Every arm except rrOff can only take work away, so a positive dRt for one of
    them is harness bias or non-orthogonality, not a measurement.
    """
    should_reduce = [(s, d) for s, _, d, _, _, _, _ in table if s != "rrOff"]
    upside = [(s, d) for s, d in should_reduce if d > 0]
    deltas = sorted(d for _, _, d, _, _, _, _ in table)
    spreads = [base_spread] + [sp for _, _, _, _, _, _, sp in table]
    mid = deltas[len(deltas) // 2] if deltas else 0.0
    sign_floor = sum(abs(d) for d in deltas) / len(deltas) if deltas else 0.0
    drift = max(spreads) if spreads else 0.0
    floor = max(sign_floor, drift)
    print("      harness check : %d of %d arms that can only REMOVE work report a positive dRt"
          % (len(upside), len(should_reduce)))
    if upside:
        print("                      (%s)" % ", ".join("%s %+.2f" % (s, d) for s, d in upside[:6]))
    print("                      the `base` arm alone spreads %.2f ms of rt p10-p90 over its own"
          " windows; the widest arm spreads %.2f ms" % (base_spread, drift))
    print("                      (camera/scene content drifts inside one session; mean |dRt| = %.2f ms)"
          % sign_floor)
    print("                      -> usable noise floor = %.2f ms (%.0f%% of the %.2f ms base rt);"
          " anything under it is a camera move, not a saving"
          % (floor, 100.0 * floor / rt_base, rt_base))
    print("                      median dRt=%+.2f ms  spread=%+.2f..%+.2f ms" % (mid, deltas[0], deltas[-1]))
    biggest = [(s, d, sp) for s, _, d, _, _, _, sp in table if d < -floor]
    if biggest:
        print("      only these clear the floor: %s"
              % ", ".join("%s %.2f ms = %.0f%% of rt (own-window drift %.2f)"
                          % (s, d, 100.0 * -d / rt_base, sp) for s, d, sp in biggest))
    else:
        print("      nothing clears the floor -> this session cannot attribute rt")
    print("      stage 5 does price the guide STORE and stage 6 the emitter scan, but nothing"
          " here separates traversal from material eval from the local-light READ;")
    print("      that split needs [RT-CTR] in-shader counters (see `probe`).")
    return floor


def report(paths, top=1):
    print("== perf_step0 attribution ==")
    rc = 1
    for path in paths:
        windows, cpu, notes, cnt = parse_log(path)
        print("\n--- %s" % path)
        if not windows:
            print("    no [PERF L10 GPU] windows here")
            continue
        rc = 0
        for n in notes[:4]:
            print("    ctx %s" % n[:150])
        groups = sorted(group_by_sig(windows).items(), key=lambda x: -len(x[1]))[:top]
        for sig, rows in groups:
            frame = mean(rows, "frame")[0]
            print("\n  [%s]  windows=%d" % (sig, len(rows)))
            for s, (v, n) in sorted(((s, mean(rows, s)) for s in GPU_SCOPES),
                                    key=lambda x: -(x[1][0] or 0)):
                if v is None:
                    continue
                print("      %-6s %7.2f ms  %5.1f%%" % (s, v, 100.0 * v / frame))
            m = re.search(r"dispatch=(\d+)x(\d+)", sig)
            if m and frame:
                px = int(m.group(1)) * int(m.group(2))
                rt = mean(rows, PRIMARY_SCOPE)[0] or 0.0
                print("      -> %.2f fps | %.1f ns per output-pixel-frame | %s alone %.1f ns/px"
                      % (1000.0 / frame, 1e6 * frame / px, PRIMARY_SCOPE, 1e6 * rt / px))
            table, rt_base, base_spread = stage_table(rows)
            if not table:
                print("      ablation    : needs >=2 windows tagged base plus >=1 other arm"
                      " (a plain render session has only base)")
                continue
            base_frame = mean([r for r in rows if r["ablate"] == "base"], "frame")[0]
            print("      ablation    : UNPAIRED, arm mean minus session base mean. Shown only"
                  " to expose the bias; read the paired table below for attribution.")
            print("      %-18s %7s %8s %7s %6s %8s %6s %6s %4s" %
                  ("stage", "rt", "dRt", "dRt%", "spr", "dFrame", "cloud", "frame", "n"))
            for stage, rt, d, cloud, fr, n, sp in table:
                print("      %-18s %7.2f %+8.2f %+6.1f%% %6.2f %+7.2f %6.2f %6.2f %4d"
                      % (stage, rt, d, 100.0 * d / rt_base, sp,
                         (fr - base_frame) if base_frame else 0.0, cloud, fr, n))
            self_check(table, rt_base, base_spread)
            print("")
            paired_table(rows, rt_base)

        # RT 阶段三计数器（[RT STAGE CNT]）：每窗三计数 + 每帧/每像素派生。
        # 口径见着色器 rt_stage_counters.glsl；hits 不是 traversal steps。
        if cnt:
            px = None
            if windows:
                m = re.search(r"(\d+)x(\d+)", windows[0]["dispatch"])
                if m:
                    px = int(m.group(1)) * int(m.group(2))
            print("\n  [RT STAGE CNT] %d window(s)" % len(cnt))
            for c in cnt:
                fr = c["frames"] if isinstance(c["frames"], float) else 60.0
                rays = c["rays"] if isinstance(c["rays"], float) else 0.0
                hits = c["hits"] if isinstance(c["hits"], float) else 0.0
                mate = c["mateval"] if isinstance(c["mateval"], float) else 0.0
                print("    win=%-3s rays=%-9d hits=%-9d mateval=%-9d frames=%d"
                      % (c["win"], int(rays), int(hits), int(mate), int(fr)))
                print("      per-frame: rays=%.2f hits=%.2f mateval=%.2f"
                      % (rays / fr, hits / fr, mate / fr))
                if px:
                    print("      per-pixel: rays=%.3f hits=%.3f mateval=%.3f"
                          % (rays / px, hits / px, mate / px))
    return rc


# ------------------------------------------------------------------- run -------

def launch_cmd():
    # .bat files cannot be spawned directly; go through cmd.exe.
    return ["cmd.exe", "/c"] + GRADLE_ARGS


def run(stages, windows_per_arm, extra_env, tag, kill_on_timeout, timeout_min, dry_run):
    if not os.path.isdir(os.path.join(ROOT, "run")):
        print("run/ not found; cannot launch")
        return 2
    offset = os.path.getsize(RUNTIME_LOG) if os.path.isfile(RUNTIME_LOG) else 0

    env = dict(os.environ)
    env.update(GRADLE_ENV)
    # 2026-10-11：给游戏 JVM 打开原生内存跟踪（NMT）。
    # 背景：进程私有提交那条 +42MB/s 的曲线在 15:54~15:55 那一局已排除 Java 堆、
    # 原生 Vulkan 存活量、slab 与几何记账；直接内存口径由 [RT MEM JAVA] 的 direct=
    # 负责。若 direct= 也是平的而进程仍涨，就用
    #   jcmd <pid> VM.native_memory summary scale=mb
    # 把 JVM 侧各原生类别（Arena/Internal/Thread/Code/GC/Other...）一次看全，从而把
    # "JVM 内"与"第三方原生模块（nrd.dll / nvngx_dlss*.dll 在自己模块内分配）"分开。
    # JAVA_TOOL_OPTIONS 由任何 JVM 启动时读取，所以不必改 gradle 的 runs 配置。
    env["JAVA_TOOL_OPTIONS"] = "-XX:NativeMemoryTracking=summary"
    env["ORION_PERF_TIMESTAMPS"] = "1"
    env["ORION_PERF_ABLATION"] = ("1" if stages == "sweep"
                                     else "alt:" + ",".join(str(s) for s in stages))
    for item in extra_env:
        key, _, value = item.partition("=")
        env[key] = value

    print("== launching a measurement session ==")
    print("  cwd         : %s" % ROOT)
    print("  command     : %s" % " ".join(launch_cmd()))
    print("  ABLATION    : %s" % env["ORION_PERF_ABLATION"])
    print("                alt mode interleaves each arm against base inside ONE")
    print("                session, so resolution/upscaler/settings cannot drift")
    print("                between arms -- that is what makes the deltas comparable.")
    print("  TIMESTAMPS  : 1")
    print("  extra env   : %s" % (", ".join(extra_env) or "-"))
    print("  target      : %d windows per arm (1 window = 60 frames)" % windows_per_arm)
    print("  log offset  : %d bytes (only this session's tail gets archived)" % offset)
    if dry_run:
        print("\n  --dry-run: nothing launched.  The arms requested resolve to: %s"
              % (", ".join(STAGE_NAMES.get(int(s), "?") for s in stages)
                 if stages != "sweep" else "the built-in 120-frame-per-stage sweep"))
        return 0
    print("\nMinecraft will start and take over the screen.  Close the game window when")
    print("the count below is high enough; the script then archives and reports.")
    print("Ctrl-C detaches and leaves the game running.  Force-kill is %s -- it can"
          % ("ENABLED" if kill_on_timeout else "off"))
    print("roll back unsaved world edits, so it is off unless you pass --kill-on-timeout.")

    tmp = os.path.join(ROOT, ".tmp")
    if not os.path.isdir(tmp):
        os.makedirs(tmp)
    gradle_log = os.path.join(tmp, "perf_step0_%s_gradle.log" % tag)
    started = time.time()
    detached = False
    with open(gradle_log, "wb") as gl:
        proc = subprocess.Popen(launch_cmd(), cwd=ROOT, env=env,
                                stdout=gl, stderr=subprocess.STDOUT)
        try:
            while proc.poll() is None:
                time.sleep(15.0)
                mins = (time.time() - started) / 60.0
                tail, _, _, _ = parse_log(RUNTIME_LOG, offset)
                arms = {}
                for w in tail:
                    arms[w["ablate"]] = arms.get(w["ablate"], 0) + 1
                if arms:
                    worst = min(arms.values())
                    print("  [%5.1fm] windows=%d  min/arm=%d  %s"
                          % (mins, len(tail), worst,
                             "  ".join("%s:%d" % (k, v) for k, v in sorted(arms.items()))))
                    if worst >= windows_per_arm:
                        print("  target reached -- close the game window (timeout in %.0f min)"
                              % max(0.0, timeout_min - mins))
                if mins > timeout_min:
                    if kill_on_timeout:
                        print("  timeout; force-killing the process tree")
                        subprocess.call(["taskkill", "/PID", str(proc.pid), "/T", "/F"])
                    else:
                        print("  timeout with the game still running; detaching.")
                        print("  after you close it:  python tools/perf_step0.py archive --offset %d --tag %s"
                              % (offset, tag))
                    detached = True
                    break
        except KeyboardInterrupt:
            print("\ndetached; the game keeps running.  After you close it:")
            print("  python tools/perf_step0.py archive --offset %d --tag %s" % (offset, tag))
            return 0
        if not detached:
            proc.wait()
    return archive(offset, tag)


def archive(offset, tag):
    out_dir = OUT_DIR
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    out = os.path.join(out_dir, "%s_%s.log" % (tag, time.strftime("%Y%m%d-%H%M%S")))
    with open(RUNTIME_LOG, "r", encoding="utf-8", errors="replace") as f:
        f.seek(offset)
        data = f.read()
    with open(out, "w", encoding="utf-8") as f:
        f.write(data)
    print("\narchived this session only: %.1f MB -> %s" % (len(data) / 1048576.0, out))
    return report([out])


# ----------------------------------------------------------------- diff --------

def budget(path):
    """Largest settings group in `path` -> (sig, rows, {scope: ms}, frame, px)."""
    windows, _, _, _ = parse_log(path)
    if not windows:
        return None
    sig, rows = max(group_by_sig(windows).items(), key=lambda x: len(x[1]))
    frame = mean(rows, "frame")[0]
    scopes = {s: mean(rows, s)[0] for s in GPU_SCOPES}
    m = re.search(r"dispatch=(\d+)x(\d+)", sig)
    return sig, rows, scopes, frame, (int(m.group(1)) * int(m.group(2)) if m else None)


def diff(paths):
    """Compare two sessions' GPU budgets per output pixel.

    Absolute ms across sessions is not comparable -- resolution and upscaler differ.
    ns/pixel is, so this answers "did the newer build get cheaper per pixel" without
    pretending the settings match.
    """
    print("== perf_step0 cross-session diff (normalised per output pixel) ==")
    got = []
    for p in paths:
        b = budget(p)
        if b is None:
            print("  %s: no [PERF L10 GPU] windows" % p)
            continue
        got.append((p, b))
    if len(got) < 2:
        print("  need two readable logs")
        return 2
    (pa, (sa, ra, ka, fa, pxa)), (pb, (sb, rb, kb, fb, pxb)) = got[0], got[1]
    print("\n  A %s\n     %s  windows=%d" % (pa, sa, len(ra)))
    print("  B %s\n     %s  windows=%d" % (pb, sb, len(rb)))
    if pxa and pxb and pxa != pxb:
        print("  resolutions differ (%dx%d vs %dx%d); per-pixel columns are the comparable ones"
              % (int(pxa ** 0.5), int(pxa ** 0.5), int(pxb ** 0.5), int(pxb ** 0.5)))
    print("\n  %-7s %8s %8s %9s %9s %8s" %
          ("scope", "A ms", "B ms", "A ns/px", "B ns/px", "delta%"))
    for s in sorted(GPU_SCOPES, key=lambda x: -(ka.get(x) or 0)):
        a, b = ka.get(s), kb.get(s)
        if a is None or b is None:
            continue
        na = 1e6 * a / pxa if pxa else None
        nb = 1e6 * b / pxb if pxb else None
        d = (100.0 * (nb - na) / na) if (na and nb) else (100.0 * (b - a) / a if a else 0.0)
        print("  %-7s %8.2f %8.2f %9s %9s %+7.1f%%"
              % (s, a, b, ("%.1f" % na) if na else "-", ("%.1f" % nb) if nb else "-", d))
    if fa and fb and pxa and pxb:
        print("  %-7s %8.2f %8.2f %9.1f %9.1f %+7.1f%%"
              % ("TOTAL", fa, fb, 1e6 * fa / pxa, 1e6 * fb / pxb,
                 100.0 * (1e6 * fb / pxb - 1e6 * fa / pxa) / (1e6 * fa / pxa)))
    print("\n  caveat: two sessions never share a settings signature, so a delta here is"
          "\n  (real change + scene/res drift).  Only the in-session `alt:` arms in `report`"
          "\n  isolate one variable; use this to see direction, `report` to attribute.")
    return 0


def main():
    if hasattr(sys.stdout, "reconfigure"):      # the console here is GBK
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(prog="perf_step0", add_help=True)
    sub = parser.add_subparsers(dest="mode")

    sub.add_parser("probe", help="what can be measured now, and what cannot")

    rp = sub.add_parser("report", help="logs -> attribution table")
    rp.add_argument("logs", nargs="*", default=None)
    rp.add_argument("--top", type=int, default=1, help="settings groups to show")

    up = sub.add_parser("run", help="launch a session, archive only its log tail")
    up.add_argument("--stages", default="2,3,4,5,6,10,11,12",
                    help="arm indices, or 'sweep' for the built-in 120-frame sweep")
    up.add_argument("--windows", type=int, default=6, help="windows wanted per arm")
    up.add_argument("--env", action="append", default=[], metavar="K=V")
    up.add_argument("--tag", default="s0")
    up.add_argument("--kill-on-timeout", action="store_true")
    up.add_argument("--timeout-min", type=float, default=20.0)
    up.add_argument("--dry-run", action="store_true", help="print the launch, start nothing")

    ap = sub.add_parser("archive", help="archive a finished session's tail and report")
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--tag", default="manual")

    dp = sub.add_parser("diff", help="two logs -> per-output-pixel budget comparison")
    dp.add_argument("logs", nargs=2)

    a = parser.parse_args()
    if a.mode == "probe":
        sys.exit(probe())
    if a.mode == "report":
        sys.exit(report(a.logs or [RUNTIME_LOG], top=a.top))
    if a.mode == "run":
        stages = "sweep" if a.stages == "sweep" else [int(x) for x in a.stages.split(",") if x.strip()]
        sys.exit(run(stages, a.windows, a.env, a.tag, a.kill_on_timeout,
                     a.timeout_min, a.dry_run))
    if a.mode == "archive":
        sys.exit(archive(a.offset, a.tag))
    if a.mode == "diff":
        sys.exit(diff(a.logs))
    parser.print_help()


if __name__ == "__main__":
    main()
