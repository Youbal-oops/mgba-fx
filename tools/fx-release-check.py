#!/usr/bin/env python3
"""Record repeatable FX performance checks using existing test executables."""
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def checked_path(value):
    path = (ROOT / value).resolve()
    if not path.is_relative_to(ROOT):
        raise ValueError("Builds and reports must stay inside the repository")
    return path


def run(build, arguments, destination, timeout=180):
    env = dict(os.environ)
    paths = [build, ROOT / "build-tools/qt/Tools/mingw1310_64/bin",
             ROOT / "build-tools/deps/bin", ROOT / "build-tools/deps/mingw64/bin"]
    env["PATH"] = os.pathsep.join(map(str, paths)) + os.pathsep + env.get("PATH", "")
    result = subprocess.run([str(build / "mgba-perf.exe"), *arguments], cwd=ROOT,
                            env=env, capture_output=True, text=True, timeout=timeout)
    destination.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Performance run failed: {destination}")
    match = re.search(r"^([^,\r\n]+),(\d+),(\d+),software$", result.stdout, re.M)
    if not match:
        raise RuntimeError(f"Missing performance data: {destination}")
    frames, elapsed = int(match[2]), int(match[3])
    metrics = {key: int(value) for key, value in
               re.findall(r"\b(bytes|samples|save_us|copy_us|crc_us|diff_us|interval|captures|capture_us|entries)=(\d+)", result.stderr)}
    return {"frames": frames, "elapsed_us": elapsed, "fps": frames * 1e6 / elapsed,
            "metrics": metrics, "command": arguments}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-fx/release")
    parser.add_argument("--rewind-control", default="build-fx/adaptive")
    parser.add_argument("--ordinary-control", default="build-fx")
    parser.add_argument("--output", default="build-fx/milestone-6-release/performance")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--soak-frames", type=int, default=36000)
    args = parser.parse_args()
    if not 1 <= args.runs <= 10 or not 120 <= args.frames <= 3600 or not 600 <= args.soak_frames <= 100000:
        parser.error("Invalid run count or frame range")
    build, rewind_control, ordinary_control, output = map(
        checked_path, (args.build, args.rewind_control, args.ordinary_control, args.output))
    output.mkdir(parents=True, exist_ok=True)
    rom = build / "diagnostics/adaptive-fast.gba"
    report = {"rom_sha256": hashlib.sha256(rom.read_bytes()).hexdigest(), "rewind": {}, "ordinary": {}}
    for interval in (0, 1, 6):
        groups = {"control": [], "candidate": []}
        for repeat in range(args.runs):
            for label, binary in (("control", rewind_control), ("candidate", build)):
                arguments = ["-P", "-Q", "-F", str(args.frames), "-C", "skipBios=1",
                             "-C", "gba.fxMemoryMiB=0", "-C", "gba.fxMemoryPercent=80"]
                if interval:
                    arguments += ["-R", str(interval)]
                arguments.append(str(rom))
                groups[label].append(run(binary, arguments, output / f"rewind-{interval}-{label}-{repeat}.log"))
        medians = {label: statistics.median(row["elapsed_us"] for row in rows) for label, rows in groups.items()}
        report["rewind"][interval] = {"runs": groups, "median_us": medians,
                                     "candidate_over_control": medians["candidate"] / medians["control"]}
        print(f"rewind interval {interval}: candidate/control time {medians['candidate'] / medians['control']:.3f}", flush=True)
    cases = ["cinema/gba/irq/keyirq/test.gba", "cinema/gba/obj/2d-wrap/test.gba",
             "cinema/gba/blend/disabled-bg-semitrans-blend/test.gba"]
    for index, case in enumerate(cases):
        groups = {"control": [], "candidate": []}
        for repeat in range(args.runs):
            for label, binary in (("control", ordinary_control), ("candidate", build)):
                groups[label].append(run(binary, ["-P", "-F", "3600", "-C", "skipBios=1", case],
                                         output / f"ordinary-{index}-{label}-{repeat}.log"))
        medians = {label: statistics.median(row["elapsed_us"] for row in rows) for label, rows in groups.items()}
        report["ordinary"][case] = {"runs": groups, "median_us": medians,
                                   "candidate_over_control": medians["candidate"] / medians["control"]}
        print(f"ordinary {index}: candidate/control time {medians['candidate'] / medians['control']:.3f}", flush=True)
    report["idle_soak"] = run(build, ["-P", "-F", str(args.soak_frames), "-C", "skipBios=1", str(rom)],
                               output / "idle-soak.log", timeout=300)
    report["limitations"] = ["Headless software-renderer measurements; desktop presentation and audio devices are not measured.",
                             "Rewind measurements use synchronous diffing; desktop diffing uses a worker.",
                             "Control binaries must be preserved pre-optimization builds.",
                             "The adaptive ROM finishes its active workload after 120 updates; the long run checks idle machine stability."]
    report["dll_sha256"] = {label: hashlib.sha256((directory / "libmgba.dll").read_bytes()).hexdigest()
                            for label, directory in (("candidate", build), ("rewind_control", rewind_control), ("ordinary_control", ordinary_control))}
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Performance report: {output / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
