# Controller input lag (~0.5–1 s) — root cause and fix

**Status:** Fixed in `56852717` (`src/libs/controller.cpp`)
**Title:** Animal Well (PPSA22520), macOS / Apple Silicon (x86-64 under Rosetta), MoltenVK 1.4.2
**Controller:** DUALSHOCK 4 Wireless Controller over Bluetooth

## Symptom

With a real controller, button presses took about 0.5–1 s to show up in-game.
The lag stayed the same during normal gameplay, not only at startup.

## Root cause

`GameController::AddState()` added **every** controller update to a
64-entry ring buffer (`m_states`). A DualShock 4 / DualSense sends motion-sensor
(gyro/accel) and stick packets continuously, far faster than the game polls.

Animal Well calls `scePadRead(handle, data, 1)` once per frame. `ReadStates()`
returns the **oldest** unread entry. So the ring stayed full of stale motion
samples, and each new button press waited behind them. At ~60 reads per second
with a backlog of tens of entries, that adds up to the 0.5–1 s delay.

## Why earlier measurements missed it

Earlier latency probes used synthetic keyboard input (`KYTY_INPUT_PROBE`). With
no physical controller sending motion data, the ring never filled up, and
those runs measured 7–25 ms. The bug only shows up when a real controller is
streaming. The user's hands-on report was correct; the measurement setup was
blind to the problem.

Evidence (same probe, physical DS4 connected):

| Run | Host → game sees button | Notes |
|---|---|---|
| `baseline-recheck-20261010` (before fix) | median 430 ms, p95 890 ms, max 944 ms | Arrow-Left mask `0x80` |
| `hack-smoke-20261010` (hard sync + 1 pending flip + 2 swapchain images) | median 813 ms, max 929 ms | presentation hacks don't help |
| `padfix-1-20261010` (after fix) | **median 14.2 ms, p95 16.3 ms, max 16.6 ms** | next present max 33.4 ms |

## Fix

`AddState()` now:

1. **Coalesces** continuous updates. If buttons and touch state didn't change
   and the newest entry hasn't been read yet, the new update replaces that
   entry instead of being added.
2. **Drops consumed entries**, so the ring holds only unread samples.
3. **Caps unread samples at 8**, so input can never fall far behind.

Button and touch *transitions* still get their own entries, so short taps
(press + release between two polls) are still delivered in order.

This fixes the actual cause; it is not a timing workaround. The emulated
PS5 vblank, swapchain, and GPU synchronization are unchanged.

## Generic "low-latency emulator" settings — tested, not needed

| Suggestion | Kyty equivalent | Result |
|---|---|---|
| Turn off vsync | `Immediate` present mode (default for PPSA22520 on Apple) | already on |
| GPU hard sync | `KYTY_GPU_HARD_SYNC=1` (opt-in) | no improvement |
| Max buffered frames 0/1 | `KYTY_MAX_PENDING_FLIPS=1`, `KYTY_SWAPCHAIN_IMAGE_COUNT=2` | no improvement; 0 is invalid in Vulkan |
| Turn off threaded video | n/a (separate guest-GPU and present threads) | not applied; would hurt pacing |

## How to re-verify

```bash
KYTY_INPUT_PROBE=1 KYTY_INPUT_STATS=1 KYTY_FRAME_TIMING=1 \
KYTY_INPUT_PROBE_BOOTSTRAP_KEY=J KYTY_INPUT_PROBE_BOOTSTRAP_COUNT=4 \
KYTY_INPUT_PROBE_KEY=Left KYTY_INPUT_PROBE_START_MS=2000 \
KYTY_INPUT_PROBE_BOOTSTRAP_WARMUP_MS=8000 KYTY_INPUT_PROBE_INTERVAL_MS=500 \
KYTY_INPUT_PROBE_HOLD_MS=100 KYTY_FRAME_STATS=0 KYTY_PRINTF_DIRECTION=File \
KYTY_TIMEOUT_SECONDS=25 ./harness/run.sh padfix-check
```

Keep a physical controller connected while it runs. Pass: the
`mask=0x00000080 host_to_observed_button` max stays under ~40 ms.

## Lessons

- Latency probes must run with a real controller connected (or simulated
  motion/stick streaming). Keyboard-only probes can't see queue backlog.
- Measure "host event → guest observes that exact sample", not just
  "host event → next present"; the second can look fine while the game is
  reading stale data.
