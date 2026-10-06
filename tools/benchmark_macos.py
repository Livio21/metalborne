#!/usr/bin/env python3
"""Launch an isolated macOS benchmark using bbport's existing pad-file controls."""
import argparse
import csv
import ctypes
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
NEUTRAL = "lx=128 ly=128 rx=128 ry=128"
FLIP = re.compile(r"Guest flip stats: ([\d.]+) FPS.*?; (\d+) shader/pipeline compiles, ([\d.]+) ms;.*? (\d+) draws/frame")
PACE = re.compile(r"Frame pacing: median ([\d.]+) ms, stddev ([\d.]+) ms, p99 ([\d.]+) ms, (\d+) frames over")
HOST = re.compile(r"Host present calls: interval p50 ([\d.]+) / p95 ([\d.]+) / p99 ([\d.]+) ms")
METAL_SCENE = re.compile(r"MetalFX scene #(\d+): (\d+)x(\d+) -> (\d+)x(\d+) before HUD; Vulkan release ([\d.]+) ms, Metal/completion ([\d.]+) ms, GPU upscale\+copy ([\d.]+|nan) ms")
METAL_PRESENT = re.compile(r"Native Metal GPU #(\d+): ([\d.]+) ms; post=(\d+) spatial=(\d+), frame=(\d+)x(\d+) target=(\d+)x(\d+) drawable=(\d+)x(\d+)(?: direct_post=(\d+))?")
THERMAL_STATES = ("nominal", "fair", "serious", "critical")


def thermal_reader():
    """Read the public Foundation properties without sudo or an extra dependency."""
    foundation = ctypes.CDLL("/System/Library/Frameworks/Foundation.framework/Foundation")
    objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
    objc.objc_getClass.argtypes, objc.objc_getClass.restype = [ctypes.c_char_p], ctypes.c_void_p
    objc.sel_registerName.argtypes, objc.sel_registerName.restype = [ctypes.c_char_p], ctypes.c_void_p
    send_id = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
    send_int = ctypes.CFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
    send_bool = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
    responds = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
    selector = objc.sel_registerName
    process = send_id(objc.objc_getClass(b"NSProcessInfo"), selector(b"processInfo"))
    thermal, power = selector(b"thermalState"), selector(b"isLowPowerModeEnabled")
    responds_selector = selector(b"respondsToSelector:")
    if not process or not responds(process, responds_selector, thermal):
        raise ValueError("Foundation thermal monitoring is unavailable")
    has_power = responds(process, responds_selector, power)
    objc.objc_autoreleasePoolPush.restype = ctypes.c_void_p
    objc.objc_autoreleasePoolPush.argtypes = []
    objc.objc_autoreleasePoolPop.argtypes = [ctypes.c_void_p]
    objc.objc_autoreleasePoolPop.restype = None

    def read():
        pool = objc.objc_autoreleasePoolPush()
        try:
            state = send_int(process, thermal)
            return dict(state=THERMAL_STATES[state] if 0 <= state < 4 else "unknown",
                        low_power_mode=send_bool(process, power) if has_power else None)
        finally:
            objc.objc_autoreleasePoolPop(pool)
    return read


def summarize_thermals(samples, measurement_start, seconds):
    running = [s for s in samples if s["at"] >= 0]
    measured = [s for s in running if measurement_start is not None and measurement_start <= s["at"] <= measurement_start + seconds]
    def worst(rows):
        if not rows or any(s["state"] not in THERMAL_STATES for s in rows):
            return "unknown"
        return max((s["state"] for s in rows), key=THERMAL_STATES.index)
    return dict(run_worst_state=worst(running), measurement_worst_state=worst(measured),
                comparison_flagged=not measured or any(s["state"] != "nominal" or s["low_power_mode"] is not False for s in running),
                caveat="Five-second OS thermal-pressure samples, not temperatures or CPU/GPU clocks. Nominal does not prove absence of throttling or other load.")


class Stats:
    def __init__(self):
        self.guest, self.host = [], []
        self.native = []
        self.pending = None

    def feed(self, line, at):
        if match := FLIP.search(line):
            fps, compiles, compile_ms, draws = match.groups()
            self.pending = dict(at=at, fps=float(fps), compiles=int(compiles),
                                compile_ms=float(compile_ms), draws=int(draws))
        elif (match := PACE.search(line)) and self.pending is not None:
            median, deviation, p99, slow = match.groups()
            self.pending.update(median_ms=float(median), stddev_ms=float(deviation),
                                p99_ms=float(p99), slow_frames=int(slow))
            self.guest.append(self.pending)
            self.pending = None
        elif match := HOST.search(line):
            self.host.append(dict(at=at, p50_ms=float(match[1]),
                                  p95_ms=float(match[2]), p99_ms=float(match[3])))
        elif match := METAL_SCENE.search(line):
            gpu = float(match[8])
            self.native.append(dict(at=at, stage="scene", frame=int(match[1]),
                                    input=[int(match[2]), int(match[3])], output=[int(match[4]), int(match[5])],
                                    vulkan_release_ms=float(match[6]), metal_completion_ms=float(match[7]),
                                    gpu_ms=gpu if math.isfinite(gpu) else None))
        elif match := METAL_PRESENT.search(line):
            self.native.append(dict(at=at, stage="presentation", frame=int(match[1]), gpu_ms=float(match[2]),
                                    post=bool(int(match[3])), spatial=bool(int(match[4])),
                                    input=[int(match[5]), int(match[6])], output=[int(match[7]), int(match[8])],
                                    drawable=[int(match[9]), int(match[10])],
                                    direct_post=bool(int(match[11])) if match[11] is not None else None))


def summarize(guest, host):
    if not guest:
        raise ValueError("No complete guest timing windows in the measurement interval")
    draws = [row["draws"] for row in guest]
    return dict(windows=len(guest), mean_window_fps=statistics.mean(r["fps"] for r in guest),
                min_window_fps=min(r["fps"] for r in guest), max_window_fps=max(r["fps"] for r in guest),
                worst_window_p99_ms=max(r["p99_ms"] for r in guest),
                median_interval_range_ms=[min(r["median_ms"] for r in guest), max(r["median_ms"] for r in guest)],
                compiles=sum(r["compiles"] for r in guest), compile_ms=sum(r["compile_ms"] for r in guest),
                draws_range=[min(draws), max(draws)],
                workload_changed=max(draws) > max(1, min(draws)) * 1.10,
                host_windows=len(host),
                worst_host_window_p99_ms=max((r["p99_ms"] for r in host), default=None))


def command_output(*command):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_pad(path, buttons=""):
    temporary = path.with_suffix(".next")
    temporary.write_text(buttons + "\n")
    temporary.replace(path)


def benchmark(args):
    if sys.platform != "darwin":
        raise ValueError("Launching the benchmark requires macOS")
    running = subprocess.run(["pgrep", "-x", "bb-probe"], capture_output=True, text=True)
    if running.returncode == 0:
        raise ValueError("Bloodborne is already running. Close its window before benchmarking.")
    if running.returncode != 1:
        raise ValueError("Cannot check for an existing game process")
    if not (args.game / "eboot.bin").is_file() or not args.probe.is_file():
        raise ValueError("Game or probe missing; set --game/--probe and build first")
    if not (args.save_dir / "savedata").is_dir():
        raise ValueError("An existing save is required for the Continue benchmark")
    if args.replay:
        previous = -1
        for line in args.replay.read_text().splitlines():
            fields = [int(value) for value in line.split()]
            if len(fields) != 8 or not previous <= fields[0] <= 0xffffffff or not 0 <= fields[1] <= 0x10ffff or any(not 0 <= v <= 255 for v in fields[2:]):
                raise ValueError("Invalid replay: expected ordered ms/buttons and six byte values")
            previous = fields[0]
        if previous < 0 or previous < args.seconds * 1000:
            raise ValueError("Replay must cover the entire measurement interval")

    read_thermal = thermal_reader()
    thermal_origin = time.monotonic()
    thermal_samples = []
    nominal_since = None
    if args.cooldown:
        print(f"Waiting for {args.cooldown:g} seconds of nominal thermal pressure", flush=True)
    while True:
        now = time.monotonic()
        sample = dict(at=now - thermal_origin, **read_thermal())
        thermal_samples.append(sample)
        nominal_since = (nominal_since if nominal_since is not None else now) if sample["state"] == "nominal" else None
        if not args.cooldown or (nominal_since is not None and now - nominal_since >= args.cooldown):
            break
        if now - thermal_origin > args.startup_timeout:
            raise ValueError(f"Cooldown timed out with thermal state {sample['state']}; no game launched")
        time.sleep(min(5, args.cooldown))

    run = ROOT / "out/benchmarks" / (datetime.now().strftime("%Y%m%d-%H%M%S-%f") + "-" + args.label)
    run.mkdir(parents=True)
    shutil.copytree(args.save_dir, run / "user")
    config = run / "bbport.ini"
    source_config = ROOT / "out/macos-run/bbport.ini"
    if source_config.is_file():
        shutil.copy2(source_config, config)
    else:
        config.write_text("")
    pad, quit_file = run / "pad.txt", run / "quit"
    write_pad(pad, NEUTRAL)
    env = dict(os.environ)
    env.update(BB_PRESENT_BACKEND="metal", BB_METALFX="spatial", BB_RENDER_RES="1280x720",
               BB_UPSCALER="off", BB_FSR1="0", BB_RCAS="0", BB_FPS="30", BB_FPS_LIMIT="0",
               BB_VBLANK_HZ="60", BB_PRESENT_MODE="Fifo", BB_PREP_WORKERS="0",
               BB_MACOS_CONSERVATIVE_GPU="1", BB_METAL_BUFFER_CACHE="1", BB_METAL_BUFFER_COPY="1",
               BB_METAL_IMAGE_CACHE="1", BB_METAL_IMAGE_COPY="0", BB_METAL_IMAGE_TRANSFER="0",
               BB_METAL_IMAGE_CLEAR="0", BB_METAL_POST_PROCESS="0", BB_METALFX_SCENE="0", BB_FULLSCREEN="0")
    env.update(args.overrides)
    env.setdefault("BB_PATCHES", "Skip Intro + warning message")
    env.setdefault("BB_GPU_USER_DIR", str(ROOT / "out/macos-run/user"))
    env.update(BB_DATA_DIR=str(run), BB_USER_DIR=str(run / "user"), BB_CONFIG=str(config),
               BB_PAD_FILE=str(pad), BB_QUIT_FILE=str(quit_file), BB_TIMEOUT="0",
               BB_GAME_DIR=str(args.game), BB_PROBE=str(args.probe), BB_PREBUILT="1",
               BB_FRAME_STATS="1", BB_PRESENT_STATS="1", BB_MODS_ENABLED="0",
               BB_INPUT_MODE="auto" if args.manual else "gamepad", BB_MOUSE_CAPTURE="0")
    if not args.manual:
        env["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"] = "0xffff/0xffff"
    env.pop("BB_TOGGLE_FILE", None)  # launcher creates a private conservative toggle file
    env.pop("BB_AUTO_RENDER_RES", None)
    if args.replay:
        env["BB_PAD_REPLAY"] = str(args.replay)
    else:
        env.pop("BB_PAD_REPLAY", None)
    env.pop("BB_PAD_RECORD", None)
    env.setdefault("VK_DRIVER_FILES", str(ROOT / "out/vendor/kosmickrisp-0.19.0/kosmickrisp_mesa_icd.json"))
    metadata = dict(revision=command_output("git", "rev-parse", "HEAD"),
                    working_tree=command_output("git", "status", "--short"),
                    chip=command_output("sysctl", "-n", "machdep.cpu.brand_string"),
                    macos=command_output("sw_vers", "-productVersion"),
                    power_source=command_output("pmset", "-g", "batt"),
                    power_settings=command_output("pmset", "-g"),
                    probe_sha256=digest(args.probe),
                    workspace_gpu_sha256=digest(ROOT / "out/macos/gpu/libbbgpu.dylib"),
                    save_hashes={str(p.relative_to(args.save_dir)): digest(p) for p in sorted((args.save_dir / "savedata").rglob("*")) if p.is_file()},
                    environment={k: v for k, v in env.items() if k.startswith(("BB_", "VK_", "MESA_", "SDL_"))},
                    seconds=args.seconds, warmup=args.warmup, cooldown=args.cooldown, min_draws=args.min_draws,
                    readiness="Three consecutive timing windows above the draw threshold; heuristic, not a game-state API")
    (run / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Benchmark artifacts: {run}", flush=True)
    stats, log_buffer, all_log = Stats(), "", ""
    started = time.monotonic()
    for sample in thermal_samples:
        sample["at"] -= started - thermal_origin
    next_thermal = 0
    measurement_start = None
    error = None
    forced = False
    with (run / "game.log").open("wb") as output, (run / "game.log").open("r") as reader:
        process = subprocess.Popen(["bash", "macos/run.sh"], cwd=ROOT, env=env,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)

        def poll():
            nonlocal log_buffer, all_log, next_thermal
            at = time.monotonic() - started
            if at >= next_thermal:
                thermal_samples.append(dict(at=at, **read_thermal()))
                next_thermal = at + 5
            chunk = reader.read()
            all_log += chunk
            log_buffer += chunk
            while "\n" in log_buffer:
                line, log_buffer = log_buffer.split("\n", 1)
                stats.feed(line, time.monotonic() - started)
            if process.poll() is not None:
                raise RuntimeError(f"Game exited early with status {process.returncode}")

        def wait(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                poll()
                time.sleep(0.2)

        def press(button):
            print(f"Menu input: {button}", flush=True)
            write_pad(pad, f"{NEUTRAL} {button}")
            wait(0.18)
            write_pad(pad, NEUTRAL)
            wait(0.5)

        try:
            deadline = started + args.startup_timeout
            while "Runtime: pad opened" not in all_log:
                poll()
                if time.monotonic() > deadline:
                    raise RuntimeError("Startup timed out before controller initialization")
                time.sleep(0.2)
            if not args.manual:
                wait(args.menu_delay)
                # ponytail: timed standard menu; use --manual for other dialogs.
                press("up")  # Clamp to Online before selecting Offline, regardless of saved selection.
                press("down")
                press("cross")
                wait(6)
                press("cross")  # Offline acknowledgement.
                wait(3)
                press("cross")
            else:
                print("Manual navigation enabled; load the save in the game window.", flush=True)
            navigation_end = time.monotonic() - started
            while True:
                poll()
                rows = [r for r in stats.guest if r["at"] >= navigation_end + 5]
                if len(rows) >= 3 and all(r["draws"] >= args.min_draws for r in rows[-3:]):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("No qualifying gameplay workload before startup timeout; inspect game.log")
                time.sleep(0.2)
            print(f"Workload gate passed; warming up for {args.warmup:g} seconds", flush=True)
            wait(args.warmup)
            measurement_start = time.monotonic() - started
            if args.replay:
                write_pad(pad, "replay")
            print(f"Measuring {args.seconds:g} seconds", flush=True)
            deadline = time.monotonic() + args.seconds + 1
            next_progress = time.monotonic() + 10
            while time.monotonic() < deadline:
                poll()
                if time.monotonic() - started - stats.guest[-1]["at"] > 20:
                    raise RuntimeError("No completed guest timing window for 20 seconds; possible freeze")
                if time.monotonic() >= next_progress:
                    print(f"Latest window: {stats.guest[-1]['fps']:.1f} FPS", flush=True)
                    next_progress += 10
                time.sleep(0.2)
        except (RuntimeError, KeyboardInterrupt) as failure:
            error = str(failure) or "Interrupted"
        finally:
            thermal_samples.append(dict(at=time.monotonic() - started, **read_thermal()))
            write_pad(pad, NEUTRAL)
            quit_file.touch()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                forced = True
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()

    # Exclude the first partially overlapping five-second reporting window.
    guest = [r for r in stats.guest if measurement_start is not None and measurement_start + 5 <= r["at"] <= measurement_start + args.seconds]
    host = [r for r in stats.host if measurement_start is not None and measurement_start + 5 <= r["at"] <= measurement_start + args.seconds]
    if not error and (forced or process.returncode != 0):
        error = f"Shutdown required termination or returned {process.returncode}"
    if not error and (len(guest) < max(1, int(args.seconds / 5) - 2) or any(r["draws"] < args.min_draws for r in guest)):
        error = "Insufficient gameplay timing windows; possible menu/loading transition"
    result = dict(status="failed" if error else "complete", error=error,
                  exit_code=process.returncode, forced_termination=forced,
                  measurement_start=measurement_start, guest_windows=guest, host_windows=host,
                  summary=summarize(guest, host) if guest else None,
                  caveat="Guest flip throughput and host API timings, not display scanout. Window p99 values are not pooled percentiles.")
    result["native_gpu_samples"] = [r for r in stats.native if measurement_start is not None and measurement_start <= r["at"] <= measurement_start + args.seconds]
    result["native_gpu_caveat"] = "Sparse completed Metal command-buffer samples, not per-frame or isolated shader timings. Scene includes private-output copy; presentation includes optional post/scaler/overlay. Vulkan release and Metal completion are CPU wall time."
    result["thermal_samples"] = thermal_samples
    result["thermal"] = summarize_thermals(thermal_samples, measurement_start, args.seconds)
    result["power_source_end"] = command_output("pmset", "-g", "batt")
    if result["summary"]:
        result["summary"]["thermal_comparison_flagged"] = result["thermal"]["comparison_flagged"]
    (run / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    with (run / "windows.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=["at", "fps", "compiles", "compile_ms", "draws", "median_ms", "stddev_ms", "p99_ms", "slow_frames"])
        writer.writeheader()
        writer.writerows(guest)
    print(json.dumps(result["summary"], indent=2), flush=True)
    print(json.dumps(result["thermal"], indent=2), flush=True)
    if error:
        print(f"Benchmark failed: {error}", file=sys.stderr)
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--cooldown", type=float, default=30, help="Seconds of nominal thermal pressure before launch; 0 skips waiting")
    parser.add_argument("--startup-timeout", type=float, default=180)
    parser.add_argument("--menu-delay", type=float, default=12)
    parser.add_argument("--min-draws", type=int, default=500)
    parser.add_argument("--label", default="metal-spatial")
    parser.add_argument("--manual", action="store_true", help="Skip scripted title-menu navigation")
    parser.add_argument("--replay", type=Path, help="Recorded BB_PAD_RECORD route covering the measurement interval")
    parser.add_argument("--game", type=Path, default=ROOT / "out/game/CUSA03173")
    parser.add_argument("--probe", type=Path, default=ROOT / "out/bb-probe")
    parser.add_argument("--save-dir", type=Path, default=ROOT / "out/macos-metalfx-run/user")
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--self-check", action="store_true", help="Check the log parser without launching a game")
    args = parser.parse_args()
    if args.self_check:
        stats = Stats()
        stats.feed("Frame pacing: median 1 ms, stddev 1 ms, p99 1 ms, 1 frames over", 0)
        assert not stats.guest
        for fps, draws, compiles, at in ((20, 1000, 1, 5), (30, 1200, 0, 10)):
            stats.feed(f"Guest flip stats: {fps} FPS, worst interval 50 ms; {compiles} shader/pipeline compiles, 2.5 ms; 12 recorder syncs; GPU thread 8 us/draw, {draws} draws/frame", at)
            stats.feed("unrelated output", at)
            stats.feed("Frame pacing: median 33.33 ms, stddev 8 ms, p99 50 ms, 2 frames over", at)
            stats.feed("Host present calls: interval p50 33 / p95 40 / p99 55 ms", at)
        result = summarize(stats.guest, stats.host)
        assert result["mean_window_fps"] == 25 and result["compiles"] == 1
        assert result["workload_changed"] and result["worst_window_p99_ms"] == 50
        assert result["worst_host_window_p99_ms"] == 55
        stats.feed("MetalFX scene #300: 1280x720 -> 1920x1080 before HUD; Vulkan release 3.805 ms, Metal/completion 0.966 ms, GPU upscale+copy 0.593 ms", 11)
        stats.feed("Native Metal GPU #600: 0.188 ms; post=0 spatial=0, frame=1920x1080 target=1710x961 drawable=1710x1041", 12)
        stats.feed("MetalFX scene #900: 1280x720 -> 1920x1080 before HUD; Vulkan release 1.0 ms, Metal/completion 1.0 ms, GPU upscale+copy nan ms", 13)
        assert len(stats.native) == 3 and stats.native[0]["gpu_ms"] == 0.593
        assert stats.native[0]["input"] == [1280,720] and stats.native[0]["output"] == [1920,1080]
        assert not stats.native[1]["spatial"] and stats.native[1]["drawable"] == [1710,1041]
        assert stats.native[2]["gpu_ms"] is None
        assert stats.native[1]["direct_post"] is None
        stats.feed("Native Metal GPU #1200: 0.302 ms; post=1 spatial=0, frame=1920x1080 target=1710x961 drawable=1710x1041 direct_post=1", 14)
        assert stats.native[-1]["direct_post"] and stats.native[-1]["post"]
        json.dumps(stats.native, allow_nan=False)
        cool = [dict(at=t, state="nominal", low_power_mode=False) for t in (0, 5, 10)]
        assert not summarize_thermals(cool, 5, 10)["comparison_flagged"]
        hot = cool + [dict(at=8, state="serious", low_power_mode=False)]
        assert summarize_thermals(hot, 5, 10)["comparison_flagged"]
        assert summarize_thermals(hot, 5, 10)["measurement_worst_state"] == "serious"
        assert summarize_thermals([dict(at=5, state="nominal", low_power_mode=True)], 0, 10)["comparison_flagged"]
        assert summarize_thermals([], 0, 10)["comparison_flagged"]
        try:
            summarize([], [])
            assert False, "empty measurements accepted"
        except ValueError:
            pass
        print("Benchmark log parser, native GPU samples, summary and thermal flags PASS")
        return 0
    if not all(math.isfinite(v) and 0 <= v <= 3600 for v in (args.seconds, args.warmup, args.cooldown, args.startup_timeout, args.menu_delay)) or args.seconds < 10 or args.startup_timeout < max(15, args.cooldown) or args.min_draws < 1:
        parser.error("Use finite durations, seconds >= 10, startup-timeout >= max(15, cooldown), and min-draws >= 1")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.label):
        parser.error("Label must contain only letters, digits, dots, underscores or hyphens")
    args.overrides = {}
    reserved = {"BB_DATA_DIR", "BB_USER_DIR", "BB_CONFIG", "BB_PAD_FILE", "BB_PAD_REPLAY",
                "BB_PAD_RECORD", "BB_QUIT_FILE", "BB_TIMEOUT", "BB_GAME_DIR", "BB_PROBE",
                "BB_PREBUILT", "BB_FRAME_STATS", "BB_PRESENT_STATS", "BB_MODS_ENABLED",
                "BB_INPUT_MODE", "BB_MOUSE_CAPTURE", "BB_TOGGLE_FILE", "BB_AUTO_RENDER_RES"}
    for entry in args.env:
        key, separator, value = entry.partition("=")
        if not separator or not re.fullmatch(r"(?:BB|VK|MESA|SDL)_[A-Z0-9_]+", key):
            parser.error("--env expects a BB_, VK_, MESA_ or SDL_ variable assignment")
        if key in reserved:
            parser.error(f"{key} is managed by the benchmark; use the corresponding path option")
        args.overrides[key] = value
    for name in ("game", "probe", "save_dir", "replay"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).expanduser().resolve())
    try:
        return benchmark(args)
    except (ValueError, OSError) as failure:
        parser.exit(1, f"Benchmark: {failure}\n")


if __name__ == "__main__":
    sys.exit(main())
