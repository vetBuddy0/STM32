"""Turn a recorded testing session into the numbers the report needs.

    python tools/analyse_trials.py trials/trial_20260926_181500.log

Reads a file made by tools/trial_capture.ps1 and works out, for each labelled
trial, what the detector did and how long it took. It prints a summary and
writes a CSV next to the log for pasting into slides.

What counts as what:

  * a label whose first word is "fall" is an attempted fall, and the detector
    should raise an incident during it;
  * every other label is ordinary movement or a near-fall, and the detector
    should raise nothing.

Response times are measured from the moment of strongest movement inside the
trial (the peak of the filtered acceleration), not from when the label was
typed, because a person types before they move.
"""
import csv
import os
import re
import sys

MARK = re.compile(r"^#\s*MARK\s+t=(\d+)\s+(.*)$")
INCIDENT = re.compile(r"^##\s*incident\s+(\d+)\s+(\S+):\s*(.+)$")
ROW = re.compile(r"^(\d+),(\w+),([-\d.]+),([-\d.]+),([-\d.]+)\s*$")


def read(path):
    """Pull the three kinds of line out of the log, in order."""
    marks, rows, events = [], [], []
    last_tick = 0

    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()

            hit = ROW.match(line)
            if hit:
                last_tick = int(hit.group(1))
                rows.append({
                    "t": last_tick,
                    "state": hit.group(2),
                    "acc_g": float(hit.group(3)),
                    "gyro_dps": float(hit.group(4)),
                    "tilt_deg": float(hit.group(5)),
                })
                continue

            hit = MARK.match(line)
            if hit:
                marks.append({"t": int(hit.group(1)), "label": hit.group(2).strip()})
                continue

            hit = INCIDENT.match(line)
            if hit:
                events.append({
                    "t": last_tick,          # the board's clock at this point
                    "id": int(hit.group(1)),
                    "type": hit.group(2),
                    "status": hit.group(3).strip(),
                })

    return marks, rows, events


def build_trials(marks, rows, events):
    """One entry per label, ending where the next label starts."""
    trials = []

    for index, mark in enumerate(marks):
        start = mark["t"]
        end = marks[index + 1]["t"] if index + 1 < len(marks) else (
            rows[-1]["t"] if rows else start)

        inside = [r for r in rows if start <= r["t"] <= end]
        during = [e for e in events if start <= e["t"] <= end]

        peak_t = None
        if inside:
            peak = max(inside, key=lambda r: r["acc_g"])
            peak_t = peak["t"]

        raised = [e for e in during if "response window" in e["status"]
                  or "assistance requested" in e["status"]]
        assisted = [e for e in during if "assistance requested" in e["status"]]

        trials.append({
            "label": mark["label"],
            "kind": mark["label"].split()[0].lower() if mark["label"] else "?",
            "start": start,
            "end": end,
            "seconds": (end - start) / 1000.0,
            "samples": len(inside),
            "peak_acc_g": max((r["acc_g"] for r in inside), default=0.0),
            "min_acc_g": min((r["acc_g"] for r in inside), default=0.0),
            "peak_gyro_dps": max((r["gyro_dps"] for r in inside), default=0.0),
            "max_tilt_deg": max((r["tilt_deg"] for r in inside), default=0.0),
            "detected": bool(raised),
            "incident_id": raised[0]["id"] if raised else "",
            "detect_ms": (raised[0]["t"] - peak_t) if (raised and peak_t is not None) else "",
            "assist_ms": (assisted[0]["t"] - raised[0]["t"]) if (raised and assisted) else "",
            "events": " -> ".join(e["status"] for e in during),
        })

    return trials


def report(trials, path):
    falls = [t for t in trials if t["kind"] == "fall"]
    others = [t for t in trials if t["kind"] != "fall"]

    caught = [t for t in falls if t["detected"]]
    missed = [t for t in falls if not t["detected"]]
    false_alarms = [t for t in others if t["detected"]]

    print("\n%-16s %7s %8s %8s %8s %8s  %s" % (
        "trial", "secs", "peak g", "min g", "max dps", "max deg", "detector"))
    print("-" * 86)
    for t in trials:
        verdict = "DETECTED" if t["detected"] else "-"
        if t["kind"] != "fall" and t["detected"]:
            verdict = "FALSE ALARM"
        if t["kind"] == "fall" and not t["detected"]:
            verdict = "MISSED"
        print("%-16s %7.1f %8.2f %8.2f %8.1f %8.1f  %s" % (
            t["label"][:16], t["seconds"], t["peak_acc_g"], t["min_acc_g"],
            t["peak_gyro_dps"], t["max_tilt_deg"], verdict))

    print("\nSummary")
    print("  simulated falls      %d" % len(falls))
    print("    detected           %d" % len(caught))
    print("    missed             %d" % len(missed))
    if falls:
        print("    detection rate     %.0f%%" % (100.0 * len(caught) / len(falls)))

    print("  non-fall actions     %d" % len(others))
    print("    false alarms       %d" % len(false_alarms))
    if others:
        print("    false alarm rate   %.0f%%" % (100.0 * len(false_alarms) / len(others)))

    times = [t["detect_ms"] for t in caught if t["detect_ms"] != ""]
    if times:
        print("  time from the strongest movement to the alarm")
        print("    fastest            %d ms" % min(times))
        print("    slowest            %d ms" % max(times))
        print("    average            %d ms" % (sum(times) / len(times)))

    assists = [t["assist_ms"] for t in caught if t["assist_ms"] != ""]
    if assists:
        print("  time from the alarm to help being requested")
        print("    average            %d ms" % (sum(assists) / len(assists)))

    if missed:
        print("\nMissed falls, with what the detector saw:")
        for t in missed:
            print("  %-14s peak %.2f g, min %.2f g, %.0f dps, tilt %.0f deg" % (
                t["label"], t["peak_acc_g"], t["min_acc_g"],
                t["peak_gyro_dps"], t["max_tilt_deg"]))
        print("  Compare those against IMPACT_G, FREEFALL_G and ROT_DPS in main.c.")

    out = os.path.splitext(path)[0] + "_summary.csv"
    with open(out, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(trials[0].keys()))
        writer.writeheader()
        writer.writerows(trials)
    print("\nwrote %s" % out)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    log = sys.argv[1]
    marks, rows, events = read(log)

    print("read %s: %d rows, %d labels, %d incident events"
          % (log, len(rows), len(marks), len(events)))

    if not marks:
        print("\nNo labels found. Type a name and press Enter before each trial"
              " while recording, so the trials can be told apart.")
        sys.exit(1)

    report(build_trials(marks, rows, events), log)
