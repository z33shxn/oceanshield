"""
OceanShield - ThingSpeak log analyser
-------------------------------------
Downloads the full feed of the OceanShield ThingSpeak channel and checks it for
the faults that are invisible on a live dashboard:

  * upload cadence (is the band really posting every ~30 s?)
  * per-session sensor health (flat zeros, rail-pinned ECG, dead RFID)
  * two analog inputs that suddenly track each other (a shorted / moved wire)
  * an audit of the original dashboard rules replayed over the whole history
  * a software-in-the-loop simulation of the corrected risk engine

Usage:
    python oceanshield_log_analyzer.py                   # reads the channel
    python oceanshield_log_analyzer.py --file feeds.json # works offline
    python oceanshield_log_analyzer.py --simulate        # scenario test only

Set the channel ID and read key in the environment variables
OCEANSHIELD_CHANNEL_ID and OCEANSHIELD_READ_KEY rather than hard-coding them.
"""

import argparse
import json
import os
import statistics
import urllib.request
from collections import Counter
from datetime import datetime

CHANNEL_ID = int(os.environ.get("OCEANSHIELD_CHANNEL_ID", "0"))
FIELDS = {"field1": "mems", "field2": "ecg", "field3": "water", "field4": "rfid"}
WINDOW = 10                # points per dashboard refresh

# Rules of the original dashboard (SeafarerSafetyApp.m)
OLD_MEMS_LIMIT = 2000      # mean(MEMS) > 2000   -> can never fire (firmware max 1023)
OLD_ECG_LIMIT = 120        # mean(ECG)  > 120
OLD_WATER_LIMIT = 1        # any(water) > 1      -> can never fire while digital (0/1)

# Rules of the corrected dashboard (SeafarerSafetyApp_FIXED.m)
MEMS_PKPK = 400            # R1: raw 12-bit counts, peak-to-peak in the window
BPM_HIGH, BPM_LOW = 120, 45  # R2: tachycardia / bradycardia, 0 BPM = leads-off
WATER_WET = 350            # R3: dry baseline 112-140, wet 564-687 counts

LEVELS = ["Safe", "Moderate Risk", "High Risk", "High Risk"]


def load_feed(path=None, read_key=None):
    """Return the channel feed as a list of dicts with parsed values."""
    if path:
        with open(path, encoding="utf-8") as fh:
            raw = json.load(fh)
    else:
        url = (f"https://api.thingspeak.com/channels/{CHANNEL_ID}/feeds.json"
               f"?results=8000&api_key={read_key}")
        with urllib.request.urlopen(url, timeout=30) as resp:
            raw = json.load(resp)

    rows = []
    for entry in raw["feeds"]:
        row = {"id": entry["entry_id"],
               "t": datetime.strptime(entry["created_at"], "%Y-%m-%dT%H:%M:%SZ")}
        for key, name in FIELDS.items():
            value = entry.get(key)
            row[name] = float(value) if value not in (None, "") else None
        rows.append(row)
    return rows


def split_sessions(rows, gap_s=600):
    """A new session starts whenever the band was silent for more than gap_s."""
    sessions, current = [], [rows[0]]
    for prev, cur in zip(rows, rows[1:]):
        if (cur["t"] - prev["t"]).total_seconds() > gap_s:
            sessions.append(current)
            current = []
        current.append(cur)
    sessions.append(current)
    return sessions


def cadence(rows):
    """Upload intervals (seconds) inside sessions only."""
    gaps = []
    for s in split_sessions(rows):
        gaps += [(b["t"] - a["t"]).total_seconds() for a, b in zip(s, s[1:])]
    return gaps


def pearson(xs, ys):
    mx, my = statistics.mean(xs), statistics.mean(ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / (sxx * syy) ** 0.5 if sxx and syy else float("nan")


def is_all_zero(row):
    return not any((row[n] or 0) for n in FIELDS.values())


def session_health(session):
    def vals(name):
        return [r[name] for r in session if r[name] is not None]

    mems, ecg, water, rfid = vals("mems"), vals("ecg"), vals("water"), vals("rfid")
    return {
        "start": session[0]["t"], "first_id": session[0]["id"],
        "last_id": session[-1]["id"], "n": len(session),
        "all_zero": sum(is_all_zero(r) for r in session),
        "mems": (min(mems), max(mems)) if mems else None,
        "ecg": (min(ecg), max(ecg)) if ecg else None,
        "ecg_rail": sum(1 for v in ecg if v in (0, 1023)),
        "water": (min(water), max(water)) if water else None,
        "rfid_scans": sum(1 for v in rfid if v > 0),
        "mems_ecg_r": pearson(mems, ecg) if len(mems) == len(ecg) > 2 else None,
    }


def old_score(window):
    """Risk score exactly as the original dashboard computed it."""
    mems = statistics.mean(r["mems"] or 0 for r in window)
    ecg = statistics.mean(r["ecg"] or 0 for r in window)
    water = any((r["water"] or 0) > OLD_WATER_LIMIT for r in window)
    return (mems > OLD_MEMS_LIMIT) + (ecg > OLD_ECG_LIMIT) + water


def shifted_label(score):
    """Status mapping of the later SeafarerSafetyApp.m (shifted by one level)."""
    return {1: "Safe", 2: "Moderate Risk"}.get(score, "High Risk")


def audit_old_dashboard(rows):
    """Replay the original rules over every dashboard-sized window."""
    tally = Counter()
    for s in split_sessions(rows):
        if all(is_all_zero(r) for r in s):
            continue                      # nothing to judge on a dead session
        for i in range(len(s) - WINDOW + 1):
            score = old_score(s[i:i + WINDOW])
            tally[(score, LEVELS[score], shifted_label(score))] += 1
    return tally


def classify(pkpk, bpm, water):
    """Corrected risk engine: three independent rules, score = rules true."""
    r1 = pkpk > MEMS_PKPK
    r2 = bpm > 0 and (bpm > BPM_HIGH or bpm < BPM_LOW)
    r3 = water >= WATER_WET
    score = r1 + r2 + r3
    return (r1, r2, r3), score, LEVELS[score]


SCENARIOS = [                     # (situation, motion pk-pk, BPM, water counts)
    ("Resting at a work station", 60, 76, 125),
    ("Walking on deck", 240, 92, 130),
    ("Heavy lifting (exertion)", 310, 128, 118),
    ("Slip and fall on deck", 1450, 104, 122),
    ("Sea spray on the probe", 180, 88, 540),
    ("Fall with panic response", 1600, 138, 128),
    ("Man overboard", 1800, 146, 660),
    ("Motionless in water", 40, 42, 650),
    ("Electrodes detached", 90, 0, 120),
]


def simulate():
    for i, (name, pk, bpm, water) in enumerate(SCENARIOS, 1):
        rules, score, level = classify(pk, bpm, water)
        flags = "".join("X" if r else "." for r in rules)
        print(f"S{i}  {name:<28} R1R2R3={flags}  score={score}  -> {level}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--file", help="read a saved feeds.json instead of the API")
    ap.add_argument("--simulate", action="store_true", help="run the scenario test only")
    args = ap.parse_args()

    if args.simulate:
        simulate()
        return

    rows = load_feed(args.file, os.environ.get("OCEANSHIELD_READ_KEY"))
    gaps = cadence(rows)
    print(f"entries           : {len(rows)}  ({rows[0]['t']} -> {rows[-1]['t']})")
    print(f"median interval   : {statistics.median(gaps):.1f} s  "
          f"(within 29-35 s: {100 * sum(29 <= g <= 35 for g in gaps) / len(gaps):.1f} %)")
    print(f"all-zero entries  : {sum(is_all_zero(r) for r in rows)}")
    print()
    for s in split_sessions(rows, gap_s=3600):
        h = session_health(s)
        r = f"{h['mems_ecg_r']:+.2f}" if h["mems_ecg_r"] is not None else "  n/a"
        print(f"{h['start']:%Y-%m-%d}  ids {h['first_id']:>3}-{h['last_id']:<3}  n={h['n']:>3}  "
              f"all-zero={h['all_zero']:>3}  MEMS={h['mems']}  ECG={h['ecg']}  "
              f"rail={h['ecg_rail']:>3}  water={h['water']}  RFID={h['rfid_scans']:>3}  r(M,E)={r}")
    print()
    print("original dashboard replay (score, correct label, label shown):")
    for key, n in sorted(audit_old_dashboard(rows).items()):
        print(f"   {key}: {n}")
    print()
    simulate()


if __name__ == "__main__":
    main()
