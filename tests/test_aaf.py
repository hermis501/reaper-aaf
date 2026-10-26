"""AAF import and export tests.

    python tests/test_aaf.py [build/Release folder]

1. Export: writes a REAPER project, exports it with rpp2aaf (embedded and external media), reads the
   AAF back with aaf2rpp and compares. If pyaaf2 is importable, the files are also opened with it.
2. Import against LibAAF (optional): set LIBAAF_TEST_DIR to LibAAF's test/aaf folder and AAFTOOL to
   its aaftool executable (https://github.com/agfline/LibAAF). Every file is imported with aaf2rpp
   and each clip's position, length, source offset, gain and fades are compared with aaftool's
   output (with Pro Tools true fades and sample accurate edit removal, as aaf2rpp does by default).
"""

import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# build/Release of this folder's own build, else of the repository's top-level build
DEFAULT_BIN = next((d for d in (os.path.join(ROOT, "build", "Release"), os.path.join(os.path.dirname(ROOT), "build", "Release"))
                    if os.path.isdir(d)), os.path.join(ROOT, "build", "Release"))


# --- RPP parsing ---------------------------------------------------------------------------

def parse_rpp(text):
    """Returns a tree of (tokens, children) nodes."""
    root = (["ROOT"], [])
    stack = [root]
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line == ">":
            stack.pop()
            continue
        if line.startswith("<"):
            node = (tokenize(line[1:]), [])
            stack[-1][1].append(node)
            stack.append(node)
        else:
            stack[-1][1].append((tokenize(line), None))
    assert len(stack) == 1, "unbalanced RPP blocks"
    return root[1][0]


def tokenize(line):
    out, i = [], 0
    while i < len(line):
        c = line[i]
        if c == " ":
            i += 1
        elif c in "\"'`":
            j = line.index(c, i + 1)
            out.append(line[i + 1:j])
            i = j + 1
        else:
            j = line.find(" ", i)
            j = len(line) if j < 0 else j
            out.append(line[i:j])
            i = j
    return out


def children(node, name):
    return [c for c in node[1] if c[0][0] == name]


def first(node, name):
    found = children(node, name)
    assert found, f"{name} not found in {node[0]}"
    return found[0]


failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def exe(bin_dir, name):
    p = os.path.join(bin_dir, name + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(p):
        sys.exit(f"{p} not found - build first")
    return p


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout, r.stderr


# --- RPP side ----------------------------------------------------------------------------------

def rpp_tracks(text):
    """[(name, [item dicts])] plus the project offset."""
    root = parse_rpp(text)
    offset = 0.0
    for c in root[1]:
        if c[0][0] == "PROJOFFS":
            offset = float(c[0][1])
    tracks = []
    for t in children(root, "TRACK"):
        name = ""
        for c in t[1]:
            if c[0][0] == "NAME":
                name = c[0][1] if len(c[0]) > 1 else ""
        items = []
        for it in children(t, "ITEM"):
            d = {"vol": 1.0, "mute": False, "soffs": 0.0, "name": "", "file": "", "chanmode": 0, "volenv": [],
                 "rate": 1.0}
            for c in it[1]:
                k = c[0][0]
                if k == "POSITION":
                    d["pos"] = float(c[0][1])
                elif k == "LENGTH":
                    d["len"] = float(c[0][1])
                elif k == "SOFFS":
                    d["soffs"] = float(c[0][1])
                elif k == "FADEIN":
                    d["fadein"] = float(c[0][2])
                    d["fadeinshape"] = int(c[0][1])
                elif k == "FADEOUT":
                    d["fadeout"] = float(c[0][2])
                    d["fadeoutshape"] = int(c[0][1])
                elif k == "VOLPAN":
                    d["vol"] = float(c[0][1])
                elif k == "MUTE":
                    d["mute"] = c[0][1] == "1"
                elif k == "NAME":
                    d["name"] = c[0][1]
                elif k == "PLAYRATE":
                    d["rate"] = float(c[0][1])
                elif k == "CHANMODE":
                    d["chanmode"] = int(c[0][1])
                elif k == "SOURCE" and c[1] is not None:
                    for s in c[1]:
                        if s[0][0] == "FILE":
                            d["file"] = s[0][1]
                elif k == "VOLENV" and c[1] is not None:
                    d["volenv"] = [(float(p[0][1]), float(p[0][2])) for p in c[1] if p[0][0] == "PT"]
            items.append(d)
        tracks.append((name, items, t))
    return tracks, offset


# --- Export -------------------------------------------------------------------------------------

def write_wav(path, channels, rate, bits, seconds):
    n = int(seconds * rate)
    frames = bytearray()
    for i in range(n):
        for c in range(channels):
            v = math.sin(i * (0.01 + 0.02 * c))
            if bits == 16:
                frames += struct.pack("<h", int(v * 12000))
            else:
                frames += struct.pack("<i", int(v * 3000000))[:3]
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def write_aiff(path, rate, seconds):
    n = int(seconds * rate)
    data = b"".join(struct.pack(">h", int(math.sin(i * 0.03) * 10000)) for i in range(n))
    # 80-bit extended sample rate
    exp = 16383 + 63
    mant = rate
    while mant < (1 << 63):
        mant <<= 1
        exp -= 1
    comm = struct.pack(">hIh", 1, n, 16) + struct.pack(">HQ", exp, mant)
    ssnd = struct.pack(">II", 0, 0) + data
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    with open(path, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)


def export_rpp():
    def item(pos, length, file, extra="", soffs=0.0, tail=""):
        return f"""    <ITEM
      POSITION {pos}
      LENGTH {length}
      {extra}
      SOFFS {soffs}
      <SOURCE {'AIFF' if file.endswith('.aif') else 'WAVE'}
        FILE "{file}"
      >
{tail}    >
"""
    dialog = (item(1.0, 2.0, "media/mono.wav", "FADEIN 0 0.25 0 0 0 0 0\n      FADEOUT 1 0.5 0 1 0 0 0\n      VOLPAN 0.7 0 0.5 -1", 0.5)
              + item(4.0, 2.0, "media/mono.wav", "FADEOUT 0 0.01 0.3 0 0 0 0", 1.0)
              + item(5.7, 2.0, "media/mono.wav", "FADEIN 0 0.01 0.3 0 0 0 0", 2.0)
              + item(8.0, 1.0, "media/mono.wav", "MUTE 1 0"))
    music = (item(0.0, 4.0, "media/stereo.wav", "", 0.0,
                  "      <VOLENV\n        ACT 1 -1\n        PT 0 1 0\n        PT 2 0.5 0\n        PT 4 1 0\n      >\n")
             + item(1.0, 1.0, "media/stereo.wav", "", 3.0)
             + item(5.0, 2.0, "media/stereo.wav", "CHANMODE 4", 1.0)
             + item(8.0, 2.0, "media/stereo.wav", "PLAYRATE 2 1 0 -1 0 0.0025", 1.0))
    return f"""<REAPER_PROJECT 0.1 "7.0/win64" 1700000000
  PROJOFFS 3600 0 0
  TIMEMODE 1 5 -1 25 0 0 -1 0
  SAMPLERATE 48000 0 0
  MARKER 1 2 Intro 0 0
  MARKER 2 3 Verse 1 0
  MARKER 2 6 "" 1
  <TRACK
    NAME Dialog
    VOLPAN 0.5 -0.5 -1 -1 1
    MUTESOLO 0 2 0
{dialog}  >
  <TRACK
    NAME Music
    VOLPAN 1 0 -1 -1 1
    <VOLENV2
      ACT 1 -1
      PT 0 1 0
      PT 5 0.25 0
    >
    <PANENV2
      ACT 1 -1
      PT 0 1 0
      PT 6 -1 0
    >
{music}  >
  <TRACK
    NAME Loop
    MUTESOLO 1 0 0
{item(0.0, 12.0, "media/mono44.aif", "LOOP 1")}  >
  <TRACK
    NAME Bus
    ISBUS 1 1
  >
  <TRACK
    NAME Child
    ISBUS 2 -1
{item(2.0, 1.0, "media/mono.wav", "CHANMODE 3")}  >
  <TRACK
    NAME Keys
    <ITEM
      POSITION 0
      LENGTH 2
      <SOURCE MIDI
        HASDATA 1 960 QN
        E 960 90 3c 60
        E 960 80 3c 00
        E 0 b0 7b 00
      >
    >
  >
>
"""


def item_at(items, pos):
    for it in items:
        if abs(it["pos"] - pos) < 1e-4:
            return it
    return None


def check_export(bin_dir, aaftool):
    rpp2aaf, aaf2rpp = exe(bin_dir, "rpp2aaf"), exe(bin_dir, "aaf2rpp")
    tmp = tempfile.mkdtemp(prefix="aafexport-")
    media = os.path.join(tmp, "media")
    os.makedirs(media)
    write_wav(os.path.join(media, "mono.wav"), 1, 48000, 16, 10)
    write_wav(os.path.join(media, "stereo.wav"), 2, 48000, 24, 10)
    write_aiff(os.path.join(media, "mono44.aif"), 44100, 5)
    rpp = os.path.join(tmp, "export test.rpp")
    with open(rpp, "w", encoding="utf-8") as f:
        f.write(export_rpp())

    for embed in (False, True):
        kind = "embedded" if embed else "external"
        aaf = os.path.join(tmp, f"out-{kind}.aaf")
        code, out, err = run([rpp2aaf, rpp, aaf] + (["--embed"] if embed else []))
        check(code == 0, f"rpp2aaf ({kind}) failed: {err.strip()}")
        if code != 0:
            continue
        check("MIDI items were not exported" in err, f"{kind}: no MIDI warning: {err}")
        check("Overlapping items were placed on extra tracks" in err, f"{kind}: no overlap warning: {err}")
        check("(3 embedded)" in err if embed else "(0 embedded)" in err, f"{kind}: report: {err}")
        back = os.path.join(tmp, f"back-{kind}.rpp")
        code, out, err = run([aaf2rpp, aaf, back, "--media", os.path.join(tmp, f"extracted-{kind}")])
        check(code == 0, f"aaf2rpp ({kind}) failed: {err.strip()}")
        if code != 0:
            continue
        with open(back, encoding="utf-8") as f:
            text = f.read()
        tracks, offset = rpp_tracks(text)
        names = [t[0] for t in tracks]
        check(names == ["Dialog", "Music", "Music (2)", "Music (3)", "Loop", "Child"], f"{kind}: tracks {names}")
        check(abs(offset - 3600) < 1e-6, f"{kind}: project offset {offset}")
        if len(tracks) != 6:
            continue
        sr = 48000.0

        def near(a, b, what, tol=1.5 / sr):
            check(abs(a - b) <= tol, f"{kind}: {what}: {a} != {b}")

        # media: external files are referenced in place, embedded ones extracted unchanged
        for t in tracks:
            for it in t[1]:
                f = it["file"]
                if not embed:
                    check(os.path.normcase(os.path.dirname(f)) == os.path.normcase(media), f"{kind}: media path {f}")
                else:
                    check(os.path.isfile(f) and os.path.normcase(os.path.dirname(f)) != os.path.normcase(media),
                          f"{kind}: extracted media {f}")
                    orig = os.path.join(media, {"mono": "mono.wav", "stereo": "stereo.wav", "mono44": "mono44.aif"}[
                        os.path.splitext(os.path.basename(f))[0].split(" ")[0].split("-")[0]])
                    with open(f, "rb") as a, open(orig, "rb") as b:
                        check(a.read() == b.read(), f"{kind}: {f} differs from {orig}")

        dialog = tracks[0]
        volpan = first(dialog[2], "VOLPAN")[0]
        mutesolo = first(dialog[2], "MUTESOLO")[0]
        near(float(volpan[1]), 0.5, "Dialog volume", 1e-6)
        near(float(volpan[2]), -0.5, "Dialog pan", 1e-6)
        check(mutesolo[2] != "0" and mutesolo[1] == "0", f"{kind}: Dialog mute/solo {mutesolo}")
        items = dialog[1]
        check(len(items) == 4, f"{kind}: Dialog has {len(items)} items")
        a, b, c, d = (item_at(items, p) for p in (1.0, 4.0, 5.7, 8.0))
        check(None not in (a, b, c, d), f"{kind}: Dialog items at {[i['pos'] for i in items]}")
        if None not in (a, b, c, d):
            near(a["len"], 2.0, "a length")
            near(a["soffs"], 0.5, "a offset")
            near(a["vol"], 0.35, "a gain", 1e-6)
            near(a.get("fadein", 0), 0.25, "a fade in")
            near(a.get("fadeout", 0), 0.5, "a fade out")
            check(a.get("fadeinshape") == 0 and a.get("fadeoutshape") == 1, f"{kind}: a fade shapes {a}")
            near(b["len"], 2.0, "b length")
            near(b.get("fadeout", 0), 0.3, "crossfade out")
            near(c.get("fadein", 0), 0.3, "crossfade in")
            near(c["soffs"], 2.0, "c offset")
            near(c["len"], 2.0, "c length")
            check(d["mute"] and not a["mute"], f"{kind}: item mute")
        music = tracks[1]
        check(bool(children(music[2], "VOLENV2")) and bool(children(music[2], "PANENV2")), f"{kind}: Music envelopes")
        pan = children(music[2], "PANENV2")
        if pan:
            pts = [(float(p[0][1]), float(p[0][2])) for p in pan[0][1] if p[0][0] == "PT"]
            check(len(pts) == 2 and abs(pts[0][1] - 1) < 1e-6 and abs(pts[-1][1] + 1) < 1e-6 and abs(pts[-1][0] - 6) < 1e-4,
                  f"{kind}: Music pan envelope {pts}")
        vol = children(music[2], "VOLENV2")
        if vol:
            pts = [(float(p[0][1]), float(p[0][2])) for p in vol[0][1] if p[0][0] == "PT"]
            check(any(abs(t - 5) < 1e-4 and abs(v - 0.25) < 1e-4 for t, v in pts), f"{kind}: Music volume envelope {pts}")
        # the right-channel item is mono: it goes to its own track ("Music (3)")
        items = music[1] + tracks[3][1]
        check(len(music[1]) == 2 and len(tracks[3][1]) == 1, f"{kind}: Music items {len(music[1])}, {len(tracks[3][1])}")
        m1, m2, m3 = (item_at(items, p) for p in (0.0, 5.0, 8.0))
        if None not in (m1, m2, m3):
            check(len(m1["volenv"]) >= 3, f"{kind}: take envelope {m1['volenv']}")
            if m1["volenv"]:
                check(any(abs(t - 2) < 1e-3 and abs(v - 0.5) < 1e-4 for t, v in m1["volenv"]), f"{kind}: take envelope {m1['volenv']}")
            check(m1["chanmode"] == 0 and m2["chanmode"] == 4, f"{kind}: channel modes {m1['chanmode']} {m2['chanmode']}")
            near(m3["rate"], 2.0, "playback rate", 1e-6)
            near(m3["len"], 2.0, "rated item length")
            near(m3["soffs"], 1.0, "rated item offset")
        else:
            check(False, f"{kind}: Music items at {[i['pos'] for i in items]}")
        lane2 = tracks[2][1]
        check(len(lane2) == 1 and abs(lane2[0]["pos"] - 1.0) < 1e-4 and abs(lane2[0]["soffs"] - 3.0) < 1e-4,
              f"{kind}: overlapping item {lane2}")
        loop = tracks[4]
        check(first(loop[2], "MUTESOLO")[0][1] == "1", f"{kind}: Loop track mute")
        got = [(round(i["pos"], 4), round(i["len"], 4), round(i["soffs"], 4)) for i in loop[1]]
        check(got == [(0.0, 5.0, 0.0), (5.0, 5.0, 0.0), (10.0, 2.0, 0.0)], f"{kind}: loop items {got}")
        child = tracks[5][1]
        check(len(child) == 1 and child[0]["chanmode"] == 0, f"{kind}: Child items {child}")

        # markers and the region
        markers = [tokenize(l.strip()) for l in text.splitlines() if l.strip().startswith("MARKER")]
        got = [(m[2], m[3] if len(m) > 3 else "") for m in markers]
        check(len(markers) == 3 and abs(float(markers[0][2]) - 2) < 1e-4 and markers[0][3] == "Intro"
              and markers[1][3] == "Verse" and abs(float(markers[2][2]) - 6) < 1e-4, f"{kind}: markers {got}")

        if aaftool:
            info = aaftool_info(aaftool, aaf)
            check(info["start"] == 3600 * 48000, f"{kind}: aaftool composition start {info['start']}")
            counts = [len(t["clips"]) for t in info["tracks"]]
            check(counts == [4, 2, 1, 1, 3, 1], f"{kind}: aaftool clips per track {counts}")
            if counts == [4, 2, 1, 1, 3, 1]:
                dl = info["tracks"][0]["clips"]
                check(abs(dl[0]["gain"] - 20 * math.log10(0.35)) < 0.06, f"{kind}: aaftool gain {dl[0]['gain']}")
                check(dl[0]["fadein"] == 12000 and dl[0]["fadeout"] == 24000, f"{kind}: aaftool fades {dl[0]['line']}")
                check(dl[1]["fadeout"] == 14400 and dl[2]["fadein"] == 14400, f"{kind}: aaftool crossfade {dl[1]['line']}")
                check(dl[3]["mute"], f"{kind}: aaftool mute {dl[3]['line']}")
                check(info["tracks"][1]["clips"][0]["auto"], f"{kind}: aaftool clip gain automation")
                muted = [c["mute"] for t in info["tracks"] for c in t["clips"]]
                check(muted.count(True) == 1, f"{kind}: aaftool muted clips {muted}")
            check(len(info["markers"]) == 2 and (3 * 48000 + 3600 * 48000, 3 * 48000) in info["markers"],
                  f"{kind}: aaftool markers {info['markers']}")
        if pyaaf2_ok:
            check_pyaaf2(aaf, kind)


try:
    import aaf2  # noqa: F401
    pyaaf2_ok = True
except ImportError:
    pyaaf2_ok = False


def check_pyaaf2(path, kind):
    import aaf2
    with aaf2.open(path, "r") as f:
        comps = list(f.content.toplevel())
        check(len(comps) == 1, f"{kind}: pyaaf2 sees {len(comps)} top-level compositions")
        masters = list(f.content.mastermobs())
        check(len(masters) == 3, f"{kind}: pyaaf2 sees {len(masters)} master mobs")
        for mob in f.content.mobs:
            for slot in mob.slots:
                seg = slot.segment
                seg.length  # every segment readable
        for m in masters:
            for slot in m.slots:
                desc = slot.segment.mob.descriptor
                check(desc.name in ("WAVEDescriptor", "AIFCDescriptor") or desc.classdef.class_name in
                      ("WAVEDescriptor", "AIFCDescriptor"), f"{kind}: pyaaf2 descriptor {desc}")


# --- LibAAF comparison --------------------------------------------------------------------------

# aaftool shows every DaVinci Resolve marker at position 0
KNOWN_MARKER_DIFFERENCES = {"DR_Markers.aaf"}

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def aaftool_info(aaftool, path):
    code, out, err = run([aaftool, "--aaf-summary", "--aaf-clips", "--pos-format", "samples", "--show-automation",
                          "--pt-true-fades", "--pt-remove-sae", path])
    text = ANSI.sub("", out + err)
    info = {"start": 0, "rate": 48000, "tracks": [], "markers": []}
    m = re.search(r"Composition Start \(samples\)\s*:\s*(-?\d+)", text)
    if m:
        info["start"] = int(m.group(1))
    m = re.search(r"Dominant Sample Rate\s*:\s*(\d+)", text)
    if m:
        info["rate"] = int(m.group(1))
    track = None
    pending_xfade = None
    for line in text.splitlines():
        m = re.match(r"\s*Marker\[\d+\]:.*?Start: (-?\d+)\s+Length: (\d+)", line)
        if m:
            info["markers"].append((int(m.group(1)), int(m.group(2))))
            continue
        if re.match(r"\s*AudioTrack\[\d+\]", line):
            track = {"line": line.strip(), "clips": []}
            info["tracks"].append(track)
            pending_xfade = None
            continue
        if re.match(r"\s*VideoTrack", line):
            track = None
            continue
        if track is None:
            continue
        m = re.search(r"Clip \(\d+\):\s+Start: (-?\d+)\s+Len: (-?\d+).*?SourceOffset: (-?\d+)", line)
        if m:
            c = {"start": int(m.group(1)), "len": int(m.group(2)), "offset": int(m.group(3)), "gain": 0.0,
                 "fadein": 0, "fadeout": 0, "mute": "Mute: MUTE" in line, "auto": "VolumeAutomation: YES" in line,
                 "line": line.strip()}
            g = re.search(r"Gain: ([+-]?[\d.]+) dB", line)
            if g:
                c["gain"] = float(g.group(1))
            f = re.search(r"FadeIn: \w+ \((\d+)\)", line)
            if f:
                c["fadein"] = int(f.group(1))
            f = re.search(r"FadeOut: \w+ \((\d+)\)", line)
            if f:
                c["fadeout"] = int(f.group(1))
            if pending_xfade:
                c["fadein"] = pending_xfade
                pending_xfade = None
            c["points"] = []
            track["clips"].append(c)
            continue
        m = re.search(r"_time: ([\d.]+)\s+_value: ([\d.]+)", line)
        if m and track["clips"]:
            track["clips"][-1]["points"].append(float(m.group(2)))
            continue
        m = re.search(r"X-FADE: \w+\s+Length: (\d+)", line)
        if m and track["clips"]:
            track["clips"][-1]["fadeout"] = int(m.group(1))
            pending_xfade = int(m.group(1))
    for t in info["tracks"]:
        for c in t["clips"]:
            # a flat gain curve is a constant gain (aaf2rpp converts it; aaftool lists it as automation)
            if c["auto"] and c["points"] and max(c["points"]) - min(c["points"]) < 1e-6:
                c["auto"] = False
                c["gain"] += 20 * math.log10(c["points"][0]) if c["points"][0] > 0 else -150
    return info


def compare_with_libaaf(bin_dir, aaf_dir, aaftool):
    aaf2rpp = exe(bin_dir, "aaf2rpp")
    files = sorted(f for f in os.listdir(aaf_dir) if f.lower().endswith(".aaf"))
    tmp = tempfile.mkdtemp(prefix="aaftest-")
    print(f"comparing {len(files)} files with aaftool")
    for name in files:
        path = os.path.join(aaf_dir, name)
        rpp = os.path.join(tmp, name + ".rpp")
        code, out, err = run([aaf2rpp, path, rpp, "--media", os.path.join(tmp, "media")])
        check(code == 0, f"{name}: aaf2rpp failed: {err.strip()}")
        if code != 0:
            continue
        with open(rpp, encoding="utf-8") as f:
            text = f.read()
        tracks, offset = rpp_tracks(text)
        info = aaftool_info(aaftool, path)
        sr = info["rate"]
        # LibAAF rounds 23.976 / 119.88 fps timecode slightly differently
        check(abs(offset * sr - info["start"]) <= 6, f"{name}: project offset {offset * sr:.0f} != {info['start']}")
        check(len(tracks) == len(info["tracks"]),
              f"{name}: {len(tracks)} tracks, aaftool has {len(info['tracks'])}")
        # markers: "MARKER index position name flags ...", regions as a start/end pair
        markers = []
        open_regions = {}
        for line in text.splitlines():
            tok = tokenize(line.strip())
            if not tok or tok[0] != "MARKER":
                continue
            idx, pos, region = int(tok[1]), float(tok[2]), len(tok) > 4 and tok[4] == "1"
            if region and idx in open_regions:
                k = open_regions.pop(idx)
                markers[k] = (idx, markers[k][1], pos)
            else:
                if region:
                    open_regions[idx] = len(markers)
                markers.append((idx, pos, None))
        markers.sort(key=lambda m: m[1])
        info["markers"].sort()
        if name in KNOWN_MARKER_DIFFERENCES:
            check(len(markers) == len(info["markers"]), f"{name}: {len(markers)} markers, aaftool has {len(info['markers'])}")
        else:
            check(len(markers) == len(info["markers"]), f"{name}: {len(markers)} markers, aaftool has {len(info['markers'])}")
            for (idx, pos, end), (start, length) in zip(markers, info["markers"]):
                check(abs(pos * sr - (start - info["start"])) <= 2, f"{name}: marker {idx} at {pos * sr:.0f} != {start - info['start']}")
                ours_len = (end - pos) * sr if end is not None else 0
                check(abs(ours_len - length) <= 2, f"{name}: marker {idx} length {ours_len:.0f} != {length}")
        for ti, (ours, theirs) in enumerate(zip(tracks, info["tracks"])):
            line = theirs["line"]
            mutesolo = first(ours[2], "MUTESOLO")[0]
            volpan = first(ours[2], "VOLPAN")[0]
            check((mutesolo[1] == "1") == ("Mute: MUTE" in line), f"{name} track {ti + 1}: mute differs")
            check((mutesolo[2] != "0") == ("Solo: SOLO" in line), f"{name} track {ti + 1}: solo differs")
            pm = re.search(r"Pan: ([\d.]+)", line)
            if pm:
                check(abs(float(volpan[2]) - (float(pm.group(1)) * 2 - 1)) < 1e-3, f"{name} track {ti + 1}: pan {volpan[2]} != {pm.group(1)}")
            if "Pan: (A)" in line:
                check(bool(children(ours[2], "PANENV2")), f"{name} track {ti + 1}: pan automation missing")
            # several items at the same place = one clip split into channels (media missing)
            items = []
            for it in ours[1]:
                if not items or not (abs(items[-1]["pos"] - it["pos"]) < 1e-9 and abs(items[-1]["len"] - it["len"]) < 1e-9):
                    items.append(it)
            clips = theirs["clips"]
            where = f"{name} track {ti + 1}"
            check(len(items) == len(clips), f"{where}: {len(items)} items, aaftool has {len(clips)} clips")
            if len(items) != len(clips):
                continue
            for k, (it, c) in enumerate(zip(items, clips)):
                w = f"{where} clip {k + 1}"
                tol = 2 if "Audio_Warp" not in name else sr / 25  # LibAAF truncates warped clips to frames
                pos = it["pos"] * sr
                check(abs(pos - (c["start"] - info["start"])) <= tol, f"{w}: pos {pos:.1f} != {c['start'] - info['start']}")
                check(abs(it["len"] * sr - c["len"]) <= tol, f"{w}: len {it['len'] * sr:.1f} != {c['len']}")
                # LibAAF ignores where a Media Composer subclip starts in its master clip (shown with ClipName)
                if "ClipName:" not in c["line"]:
                    check(abs(it["soffs"] * sr - c["offset"]) <= tol, f"{w}: offset {it['soffs'] * sr:.1f} != {c['offset']}")
                db = 20 * math.log10(it["vol"]) if it["vol"] > 0 else -150
                check(abs(db - c["gain"]) < 0.06, f"{w}: gain {db:.2f} dB != {c['gain']} dB")
                check(abs(it.get("fadein", 0) * sr - c["fadein"]) <= 2, f"{w}: fade in {it.get('fadein', 0) * sr:.0f} != {c['fadein']}")
                check(abs(it.get("fadeout", 0) * sr - c["fadeout"]) <= 2, f"{w}: fade out {it.get('fadeout', 0) * sr:.0f} != {c['fadeout']}")
                check(it["mute"] == c["mute"], f"{w}: mute {it['mute']} != {c['mute']}")
                check(bool(it["volenv"]) == c["auto"], f"{w}: gain automation {bool(it['volenv'])} != {c['auto']}")


def main():
    bin_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    aaf_dir = os.environ.get("LIBAAF_TEST_DIR")
    aaftool = os.environ.get("AAFTOOL")
    check_export(bin_dir, aaftool)
    if not pyaaf2_ok:
        print("pyaaf2 not importable: skipping the pyaaf2 check of exported files")
    if aaf_dir and aaftool:
        compare_with_libaaf(bin_dir, aaf_dir, aaftool)
    else:
        print("LIBAAF_TEST_DIR / AAFTOOL not set: skipping the LibAAF comparison")
    if failures:
        print(f"{len(failures)} failure(s)")
        sys.exit(1)
    print("all AAF tests passed")


if __name__ == "__main__":
    main()
