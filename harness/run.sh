#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${KYTY_BUILD_DIR:-$root/_Build/macos-x86_64}
game_dir=${KYTY_GAME_DIR:-$root/../Animal.Well-extracted-unar/[SuperPSX]-Animal.Well-PPSA22520-JPN-Game\ \(v01.000.011\)-PS5}
timeout_seconds=${KYTY_TIMEOUT_SECONDS:-30}
run_id=${1:-$(date +%Y%m%d-%H%M%S)}
out_dir=${KYTY_HARNESS_OUT:-$root/harness/runs/$run_id}
mkdir -p "$out_dir/shaders"

if [ ! -x "$build_dir/kyty_emulator" ]; then
	printf '%s\n' "missing emulator: $build_dir/kyty_emulator" >&2
	exit 2
fi
if [ ! -d "$game_dir" ]; then
	printf '%s\n' "missing game directory: $game_dir" >&2
	exit 2
fi

export KYTY_BORDERLESS=${KYTY_BORDERLESS:-1}
# Sampled readbacks stall the GPU; use KYTY_FRAME_STATS=0 for performance runs.
if [ "${KYTY_FRAME_STATS:-1}" = "0" ]; then
	unset KYTY_FRAME_STATS
else
	export KYTY_FRAME_STATS=1
fi
if [ "${KYTY_FRAME_DUMP:-0}" = "1" ]; then
	mkdir -p "$out_dir/frames"
	export KYTY_FRAME_DUMP_DIR="$out_dir/frames"
else
	unset KYTY_FRAME_DUMP_DIR
fi
# Default Vulkan loader for the x86-64 build (override with SDL_VULKAN_LIBRARY).
if [ -z "${SDL_VULKAN_LIBRARY:-}" ] && [ -f "$root/harness/tools/MoltenVK-1.4.2/libMoltenVK.dylib" ]; then
	export SDL_VULKAN_LIBRARY="$root/harness/tools/MoltenVK-1.4.2/libMoltenVK.dylib"
fi
python3 - "$build_dir/kyty_emulator" "$game_dir" "$timeout_seconds" "$out_dir" <<'PY'
import json, os, pathlib, signal, subprocess, sys, time

exe, game, timeout_s, out_dir = sys.argv[1], sys.argv[2], float(sys.argv[3]), pathlib.Path(sys.argv[4])
shader_log_direction = os.environ.get("KYTY_SHADER_LOG_DIRECTION", "Silent")
log = out_dir / "kyty.log"
args = [exe, "--game", game,
        "--shader-log-direction", shader_log_direction, "--shader-log-folder", str(out_dir / "shaders"),
        "--printf-direction", "File", "--printf-output-file", str(out_dir / "guest-printf.log"),
        "--vulkan-validation", "false",
        "--graphics-debug-dump", os.environ.get("KYTY_GRAPHICS_DEBUG_DUMP", "false"),
        "--command-buffer-dump", os.environ.get("KYTY_COMMAND_BUFFER_DUMP", "false"),
        "--command-buffer-dump-folder", str(out_dir / "command-dumps")]
audio_device = os.environ.get("KYTY_AUDIO_DEVICE")
if audio_device:
    args += ["--audio-device", audio_device]
present_mode = os.environ.get("KYTY_PRESENT_MODE")
if present_mode:
    args += ["--present-mode", present_mode]
started = time.time()
with log.open("w") as stream:
    proc = subprocess.Popen(args, stdout=stream, stderr=subprocess.STDOUT,
                            start_new_session=True, env=os.environ.copy())
    try:
        rc = proc.wait(timeout=timeout_s)
        timed_out = False
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            rc = proc.wait()
elapsed = time.time() - started
metadata = {"exit_code": rc, "timed_out": timed_out, "elapsed_seconds": round(elapsed, 3)}
if os.environ.get("KYTY_ROUTE"):
    metadata["route"] = os.environ["KYTY_ROUTE"]
(out_dir / "run.json").write_text(json.dumps(metadata) + "\n")
print("run_dir=%s exit=%d timeout=%s elapsed=%.3f" % (out_dir, rc, timed_out, elapsed))
PY
python3 "$root/harness/scan_log.py" "$out_dir/kyty.log" "$out_dir/guest-printf.log" > "$out_dir/scan.txt"
python3 "$root/harness/stage.py" "$out_dir" >> "$out_dir/scan.txt"
python3 "$root/harness/latency.py" "$out_dir" >> "$out_dir/scan.txt"
python3 "$root/harness/fps.py" "$out_dir" >> "$out_dir/scan.txt"
python3 "$root/harness/audio.py" "$out_dir" >> "$out_dir/scan.txt"
cat "$out_dir/scan.txt"
