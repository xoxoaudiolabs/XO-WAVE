#!/usr/bin/env python3
"""Levels the factory presets: renders them all (xowave --render), measures each one's
loudness while sounding (loudest quarter of 50 ms windows), and writes dB trims to
tools/preset_trims.txt so make_presets.py brings each to the median. Repeats a few times.
Run from the repo after `make`:  python3 tools/level_presets.py"""
import array, math, os, re, subprocess, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
TRIMS = os.path.join(HERE, "preset_trims.txt")
WAV = "/tmp/xowave-level.wav"

def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")

def measure():
    subprocess.run([os.path.join(ROOT, "xowave"), "-c", os.path.join(ROOT, "xowave.conf"), "--render", WAV],
                   cwd=ROOT, check=True, capture_output=True)
    w = wave.open(WAV); sr = w.getframerate(); d = array.array("h", w.readframes(w.getnframes()))
    files = sorted(f for f in os.listdir(os.path.join(ROOT, "presets")) if f.endswith(".xowave"))
    levels = {}
    for i, f in enumerate(files):
        name = next(l.split("=", 1)[1].strip() for l in open(os.path.join(ROOT, "presets", f)) if l.startswith("name"))
        seg = d[i * 2 * sr * 2:(i + 1) * 2 * sr * 2]
        win = int(0.05 * sr) * 2
        rms = sorted((math.sqrt(sum(x * x for x in seg[k:k + win:3]) / len(seg[k:k + win:3])) / 32768
                      for k in range(0, len(seg) - win, win)), reverse=True)
        top = rms[:max(1, len(rms) // 4)]
        levels[(int(f[:2]), slug(name))] = (20 * math.log10(max(1e-9, sum(top) / len(top))), max(abs(x) for x in seg) / 32768)
    return levels

trims = {}
if os.path.exists(TRIMS):
    trims = {k: float(v) for k, v in (l.split() for l in open(TRIMS) if l.strip())}
for rnd in range(4):
    lv = measure()
    target = sorted(v[0] for v in lv.values())[len(lv) // 2]
    worst = 0.0
    for (num, s), (db, pk) in lv.items():
        if num < 9:            # hand-made presets 01-08: report only
            continue
        delta = target - db
        if pk > 0.95: delta = min(delta, -1.0)   # keep off the limiter
        trims[s] = max(-20.0, min(20.0, trims.get(s, 0.0) + delta * 0.9))
        worst = max(worst, abs(target - db))
    with open(TRIMS, "w") as f:
        for k in sorted(trims): f.write(f"{k} {trims[k]:.2f}\n")
    subprocess.run([sys.executable, os.path.join(HERE, "make_presets.py")], check=True, capture_output=True)
    print(f"round {rnd + 1}: target {target:.1f} dBFS, worst preset was {worst:.1f} dB off")
lv = measure()
target = sorted(v[0] for v in lv.values())[len(lv) // 2]
off = sorted(((abs(db - target), s, db - target, pk) for (n, s), (db, pk) in lv.items()), reverse=True)
print(f"final: target {target:.1f} dBFS; furthest: " + ", ".join(f"{s} {d:+.1f} dB (peak {pk:.2f})" for _, s, d, pk in off[:5]))
