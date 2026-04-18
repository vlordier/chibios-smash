#!/usr/bin/env python3
"""
SMASH trace visualizer.

Reads a JSON trace (from smash_trace_dump_json) and produces:
  - A text timeline view
  - An optional HTML visualization

Usage:
    python3 viz.py trace.json
    python3 viz.py trace.json --html > timeline.html
"""

import json
import sys
import argparse

COLORS = [
    "#e6194b", "#3cb44b", "#4363d8", "#f58231",
    "#911eb4", "#42d4f4", "#f032e6", "#bfef45",
]

def load_trace(path):
    with open(path) as f:
        return json.load(f)

def text_timeline(data):
    events = data["events"]
    schedule = data.get("schedule", [])
    max_tid = max((e["tid"] for e in events if e["tid"] >= 0), default=0)

    print(f"Schedule: {schedule}")
    print()

    header = "Step  | " + " | ".join(f"  T{t:<3}" for t in range(max_tid + 1))
    print(header)
    print("-" * len(header))

    for e in events:
        tid = e["tid"]
        if tid < 0:
            tid = 0
        cols = ["     "] * (max_tid + 1)

        short = e["type"]
        if "LOCK_ACQ" in short:
            short = "LACQ"
        elif "LOCK_ATT" in short:
            short = "LATT"
        elif "LOCK_BLK" in short:
            short = "LBLK"
        elif "UNLOCK" in short:
            short = "UNLK"
        elif "SIGNAL" in short:
            short = "SIG"
        elif "WAIT_BLK" in short:
            short = "WBLK"
        elif "WAIT" in short:
            short = "WAIT"
        elif "WAKEUP" in short:
            short = "WAKE"
        elif "YIELD" in short:
            short = "YLD"
        elif "DONE" in short:
            short = "DONE"
        elif "DEAD" in short:
            short = "DEAD"
        elif "SCHED" in short:
            short = ">>>"
        elif "INVAR" in short:
            short = "FAIL"

        res = e.get("resource", "")
        if res != "":
            short = f"{short}{res}"

        cols[tid] = f"{short:>5}"
        line = f"{e['step']:4}  | " + " | ".join(cols)
        print(line)

def html_timeline(data):
    events = data["events"]
    max_tid = max((e["tid"] for e in events if e["tid"] >= 0), default=0)

    print("<!DOCTYPE html><html><head>")
    print("<style>")
    print("body { font-family: monospace; background: #1a1a2e; color: #eee; }")
    print("table { border-collapse: collapse; margin: 20px; }")
    print("th, td { border: 1px solid #333; padding: 4px 8px; text-align: center; min-width: 60px; }")
    print("th { background: #16213e; }")
    print(".evt { border-radius: 3px; padding: 2px 4px; font-size: 12px; }")
    print(".lock { background: #e6194b; }")
    print(".unlock { background: #3cb44b; }")
    print(".wait { background: #f58231; }")
    print(".signal { background: #4363d8; color: white; }")
    print(".dead { background: #ff0000; font-weight: bold; }")
    print(".done { background: #555; }")
    print(".sched { color: #666; font-size: 10px; }")
    print("</style></head><body>")
    print("<h2>SMASH Trace Timeline</h2>")
    print(f"<p>Schedule: {data.get('schedule', [])}</p>")
    print("<table><tr><th>Step</th>")
    for t in range(max_tid + 1):
        print(f"<th style='color:{COLORS[t % len(COLORS)]}'>T{t}</th>")
    print("</tr>")

    for e in events:
        tid = e["tid"]
        if tid < 0:
            tid = 0
        typ = e["type"]

        cls = "evt"
        label = typ
        if "LOCK" in typ:
            cls += " lock"
            label = "LOCK"
        elif "UNLOCK" in typ:
            cls += " unlock"
            label = "UNLK"
        elif "WAIT" in typ:
            cls += " wait"
            label = "WAIT"
        elif "SIGNAL" in typ or "WAKEUP" in typ:
            cls += " signal"
            label = "SIG" if "SIGNAL" in typ else "WAKE"
        elif "DEAD" in typ:
            cls += " dead"
            label = "DEADLOCK"
        elif "DONE" in typ:
            cls += " done"
            label = "DONE"
        elif "SCHED" in typ:
            cls += " sched"
            label = ">>>"
        elif "INVAR" in typ:
            cls += " dead"
            label = "FAIL"

        res = e.get("resource", "")
        if res != "":
            label += f"({res})"

        print(f"<tr><td>{e['step']}</td>")
        for t in range(max_tid + 1):
            if t == tid:
                print(f"<td><span class='{cls}'>{label}</span></td>")
            else:
                print("<td></td>")
        print("</tr>")

    print("</table></body></html>")

def main():
    parser = argparse.ArgumentParser(description="SMASH trace visualizer")
    parser.add_argument("trace", help="JSON trace file")
    parser.add_argument("--html", action="store_true", help="Output HTML timeline")
    args = parser.parse_args()

    data = load_trace(args.trace)

    if args.html:
        html_timeline(data)
    else:
        text_timeline(data)

if __name__ == "__main__":
    main()
