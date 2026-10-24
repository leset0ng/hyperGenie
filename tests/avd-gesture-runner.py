#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Repeat launcher gestures on emulator-5580 using one persistent input process.

Coordinates must come from the current launcher, for example:
  python3 tests/avd-gesture-runner.py --phone 166,2202 --settings 955,1769 \
      --home-swipe 560,2420,560,1710 --cycles 10 --dry-run

The example icon coordinates were observed on the 1120x2436 HyperOS AVD. They
are deliberately not defaults. By default each tap requires launcher focus.
Explicit scheduled mode checks only the first tap and skips later focus queries,
for interruption reproduction with verified icon coordinates. The optional
native-close gate waits for the current Phone close before the next icon tap.
No activities, phone numbers, or call actions are launched by this script.
"""

import argparse
import datetime
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time
import zipfile


SERIAL = "emulator-5580"
SOURCE = Path(__file__).with_name("AvdGestureInput.java")


def coordinates(value):
    try:
        return tuple(int(part) for part in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("Use comma-separated integer coordinates") from error


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", default=SERIAL, help="Only emulator-5580 is permitted")
    parser.add_argument("--phone", type=coordinates, help="Observed Phone launcher icon: x,y")
    parser.add_argument("--settings", type=coordinates, help="Observed Settings launcher icon: x,y")
    parser.add_argument("--home-swipe", type=coordinates, help="Home gesture: start_x,start_y,end_x,end_y")
    parser.add_argument("--size", type=coordinates, default=(1120, 2436), help="Display width,height for coordinate validation")
    parser.add_argument("--display", type=int, default=0)
    parser.add_argument("--launcher-package", default="com.miui.home")
    parser.add_argument("--focus-mode", choices=("window", "scheduled"), default="window",
                        help="window checks every tap; scheduled checks only the first tap and skips later focus queries")
    parser.add_argument("--pattern", choices=("alternate", "same-phone", "same-settings", "mixed"), default="alternate")
    parser.add_argument("--cycles", type=int, default=10, help="Repeat the selected pattern this many times")
    parser.add_argument("--tap-ms", type=int, default=32)
    parser.add_argument("--open-dwell-ms", type=int, default=200, help="Time from icon touch-up to home gesture")
    parser.add_argument("--swipe-ms", type=int, default=120)
    parser.add_argument("--home-gap-ms", type=int, default=120, help="Time from home touch-up to the next icon touch-down")
    parser.add_argument("--cycle-gap-ms", type=int, default=0, help="Extra time after a pattern's final home gesture before the next cycle")
    parser.add_argument("--native-close-gate-ms", type=int,
                        help="Optional Phone close-start to the next Phone/Settings touch-down delay; requires scheduled mode")
    parser.add_argument("--native-close-timeout-ms", type=int, default=2000,
                        help="Abort the dependent next tap if this round's Phone close is not logged in time")
    parser.add_argument("--native-close-tolerance-px", type=int, default=32,
                        help="Maximum distance from the native close endpoint center to the explicit Phone icon")
    parser.add_argument("--sample-ms", type=int, default=8, help="Swipe MOVE interval")
    parser.add_argument("--start-delay-ms", type=int, default=1000)
    parser.add_argument("--settle-ms", type=int, default=1500, help="Wait after the last gesture before exiting")
    parser.add_argument("--focus-timeout-ms", type=int, default=2000, help="Abort if launcher focus does not return in time")
    parser.add_argument("--initial-home", action="store_true", help="Optional HOME key reset before the touch-only sequence")
    parser.add_argument("--initial-home-settle-ms", type=int, default=1000)
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_SDK_ROOT", "~/Library/Android/sdk")).expanduser())
    parser.add_argument("--java-home", type=Path, default=Path("~/Library/Java/JavaVirtualMachines/liberica-21.0.12.1").expanduser())
    parser.add_argument("--adb", type=Path, help="Defaults to SDK platform-tools/adb")
    parser.add_argument("--output", type=Path, help="Device event JSONL; default is a unique file under the system temporary directory")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", "--plan", action="store_true", help="Print the plan without compiling, connecting, or injecting")
    mode.add_argument("--compile-only", action="store_true", help="Compile the helper locally without contacting ADB")
    mode.add_argument("--probe-focus", action="store_true", help="Read the OEM focused-window bundle and task display without injecting input")
    args = parser.parse_args()
    if args.serial != SERIAL:
        parser.error(f"Refusing serial {args.serial!r}; only {SERIAL} is permitted")
    if args.display != 0:
        parser.error("This runner is restricted to display 0")
    if len(args.size) != 2 or min(args.size) < 1:
        parser.error("--size must be positive width,height")
    for name in ("cycles", "tap_ms", "swipe_ms", "sample_ms", "focus_timeout_ms",
                 "native_close_timeout_ms", "native_close_tolerance_px"):
        if getattr(args, name) < 1:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    for name in ("open_dwell_ms", "home_gap_ms", "cycle_gap_ms", "start_delay_ms", "settle_ms", "initial_home_settle_ms"):
        if getattr(args, name) < 0:
            parser.error(f"--{name.replace('_', '-')} must not be negative")
    if args.sample_ms >= args.swipe_ms:
        parser.error("--sample-ms must be shorter than --swipe-ms so the gesture includes MOVE events")
    if args.native_close_gate_ms is not None:
        if args.native_close_gate_ms < 0:
            parser.error("--native-close-gate-ms must not be negative")
        if args.focus_mode != "scheduled":
            parser.error("--native-close-gate-ms requires --focus-mode scheduled")
        if args.pattern not in ("alternate", "same-phone", "mixed"):
            parser.error("--native-close-gate-ms requires a pattern containing Phone home gestures")
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", args.launcher_package):
        parser.error("Invalid launcher package")
    if not args.compile_only and not args.probe_focus:
        required = ["home_swipe"]
        if args.pattern != "same-settings":
            required.append("phone")
        if args.pattern != "same-phone":
            required.append("settings")
        for name in required:
            points = getattr(args, name)
            expected = 4 if name == "home_swipe" else 2
            if points is None or len(points) != expected:
                parser.error(f"--{name.replace('_', '-')} requires {expected} explicit coordinates")
            for index, coordinate in enumerate(points):
                if not 0 <= coordinate < args.size[index % 2]:
                    parser.error(f"--{name.replace('_', '-')} is outside --size")
        if args.home_swipe[3] >= args.home_swipe[1]:
            parser.error("--home-swipe must move upward")
    return args


def make_plan(args):
    patterns = {"alternate": ("phone", "settings"), "same-phone": ("phone",),
                "same-settings": ("settings",), "mixed": ("phone", "phone", "settings", "settings")}
    events = []
    offset = 0
    if args.initial_home:
        events.append({"action": "home-key", "label": "initial-home", "offset_ms": offset, "duration_ms": args.tap_ms})
        offset += args.tap_ms + args.initial_home_settle_ms
    for cycle in range(0 if args.probe_focus else args.cycles):
        for target in patterns[args.pattern]:
            x, y = getattr(args, target)
            tap = {"action": "tap", "label": f"{cycle + 1}:{target}:open", "target": target,
                   "offset_ms": offset, "duration_ms": args.tap_ms, "x": x, "y": y}
            if (args.native_close_gate_ms is not None and events
                    and events[-1].get("target") == "phone" and events[-1]["action"] == "swipe"):
                tap["native_close_gate"] = {
                    "dependency": events[-1]["label"], "delay_ms": args.native_close_gate_ms,
                    "timeout_ms": args.native_close_timeout_ms,
                    "center_x": args.phone[0], "center_y": args.phone[1],
                    "tolerance_px": args.native_close_tolerance_px,
                }
            events.append(tap)
            offset += args.tap_ms + args.open_dwell_ms
            x1, y1, x2, y2 = args.home_swipe
            events.append({"action": "swipe", "label": f"{cycle + 1}:{target}:home", "target": target,
                           "offset_ms": offset, "duration_ms": args.swipe_ms,
                           "x": x1, "y": y1, "end_x": x2, "end_y": y2})
            offset += args.swipe_ms + args.home_gap_ms
        offset += args.cycle_gap_ms
    total = events[-1]["offset_ms"] + events[-1]["duration_ms"] + args.settle_ms if events else 0
    return {"serial": args.serial, "display": args.display, "size": args.size, "pattern": args.pattern,
            "cycles": args.cycles, "launcher_package": args.launcher_package,
            "focus_mode": args.focus_mode,
            "cycle_gap_ms": args.cycle_gap_ms,
            "start_delay_ms": args.start_delay_ms, "settle_ms": args.settle_ms,
            "focus_timeout_ms": args.focus_timeout_ms, "sample_ms": args.sample_ms,
            "planned_duration_ms": args.start_delay_ms + total, "events": events}


def version_key(path):
    return tuple(int(number) for number in re.findall(r"\d+", path.name))


def compile_helper(args, directory):
    platforms = [path for path in (args.sdk / "platforms").glob("android-*") if (path / "android.jar").is_file()]
    build_tools = [path for path in (args.sdk / "build-tools").glob("*") if (path / "d8").is_file() and "rc" not in path.name]
    if not platforms or not build_tools:
        raise RuntimeError(f"Android platform and d8 are required under {args.sdk}")
    android_jar = max(platforms, key=version_key) / "android.jar"
    d8 = max(build_tools, key=version_key) / "d8"
    java_home = args.java_home
    if (java_home / "Contents/Home/bin/javac").is_file():
        java_home = java_home / "Contents/Home"
    javac = java_home / "bin/javac"
    if not javac.is_file():
        raise RuntimeError(f"javac not found at {javac}")
    classes = directory / "classes"
    dex = directory / "dex"
    classes.mkdir()
    dex.mkdir()
    env = dict(os.environ, JAVA_HOME=str(java_home))
    subprocess.run([str(javac), "--release", "11", "-Xlint:all", "-classpath", str(android_jar),
                    "-d", str(classes), str(SOURCE)], check=True, env=env)
    subprocess.run([str(d8), "--min-api", "35", "--lib", str(android_jar), "--output", str(dex),
                    *map(str, sorted(classes.glob("*.class")))], check=True, env=env)
    archive = directory / "gesture-input.jar"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as jar:
        jar.write(dex / "classes.dex", "classes.dex")
    return archive


def run(args, plan, archive, directory):
    adb = args.adb or args.sdk / "platform-tools/adb"
    base = [str(adb), "-s", args.serial]
    # This check is intentionally independent of the serial-name allowlist.
    emulator = subprocess.run([*base, "shell", "getprop", "ro.kernel.qemu"], check=True,
                              capture_output=True, text=True).stdout.strip()
    if emulator != "1":
        raise RuntimeError("Refusing a target without ro.kernel.qemu=1")
    output = args.output or Path(tempfile.gettempdir()) / f"hypergenie-gestures-{datetime.datetime.now():%Y%m%d-%H%M%S}-{os.getpid()}.jsonl"
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    plan_file = directory / "plan.json"
    plan_file.write_text(json.dumps(plan) + "\n")
    device_dir = f"/data/local/tmp/hypergenie-gestures-{os.getpid()}-{time.monotonic_ns()}"
    subprocess.run([*base, "shell", "mkdir", "-p", device_dir], check=True)
    try:
        subprocess.run([*base, "push", str(archive), str(plan_file), device_dir + "/"], check=True, stdout=sys.stderr)
        device_archive = device_dir + "/" + archive.name
        mode = "--probe-plan" if args.probe_focus else "--plan"
        command = (f"CLASSPATH={shlex.quote(device_archive)} app_process /system/bin AvdGestureInput "
                   f"{mode} {shlex.quote(device_dir + '/plan.json')}")
        tap_count = sum(event["action"] == "tap" for event in plan["events"])
        gate_budget = sum(event["native_close_gate"]["timeout_ms"] + event["native_close_gate"]["delay_ms"]
                          for event in plan["events"] if "native_close_gate" in event)
        timeout = (plan["planned_duration_ms"] + tap_count * args.focus_timeout_ms + gate_budget) / 1000 + 30
        with output.open("x") as log:
            metadata = {"type": "host", "wall_time_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns(),
                        "serial": args.serial, "plan": plan}
            log.write(json.dumps(metadata) + "\n")
            log.flush()
            print(f"Event log: {output}", file=sys.stderr, flush=True)
            result = subprocess.run([*base, "shell", command], stdout=log, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"Device injector failed with exit {result.returncode}; see {output}")
        print(f"Completed: {output}", file=sys.stderr)
    finally:
        subprocess.run([*base, "shell", "rm", device_dir + "/" + archive.name, device_dir + "/plan.json"],
                       check=False, stdout=subprocess.DEVNULL)
        subprocess.run([*base, "shell", "rmdir", device_dir], check=False, stdout=subprocess.DEVNULL)


def main():
    args = parse_args()
    plan = None if args.compile_only else make_plan(args)
    if args.dry_run:
        print(json.dumps(plan, indent=2))
        return
    with tempfile.TemporaryDirectory(prefix="hypergenie-gesture-build-") as temporary:
        directory = Path(temporary)
        archive = compile_helper(args, directory)
        if args.compile_only:
            print("Java compilation and D8 conversion passed; no ADB connection was made.")
        else:
            run(args, plan, archive, directory)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
