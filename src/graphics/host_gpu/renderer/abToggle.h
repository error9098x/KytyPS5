#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ABTOGGLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ABTOGGLE_H_

// KYTY_AB_TOGGLE=<FLAG_A,FLAG_B,...> + KYTY_AB_PERIOD_S=<s> (default 20): in-run A/B test. Every
// listed flag that supports toggling ignores its own env value and follows one shared phase that
// flips on/off every period, so on- and off-windows of the same run and scene can be compared
// (harness/memstudy/fpsw.py splits FPS by phase). The phase starts "off" and is advanced only by the
// guest GPU thread between submissions/commands (AbToggle::Tick in GuestGpu::ThreadRun), never in the
// middle of a draw or dispatch, so a draw always sees one consistent value.
// Each flip logs:  ABToggle: on|off t_us=<steady_clock us, same clock as FrameTiming> n=<flip#>
// Only flags that are safe to switch between draws may opt in (they call Gate()). Currently:
//   KYTY_SPEC_READ_HOIST, KYTY_BDA_SYNC_SKIP, KYTY_CLEAR_META_NOSYNC (and new per-draw/per-wait flags that call Gate()).
// KYTY_STATE_CACHE is NOT toggleable (its shadow is not maintained while off).
// Header-only; off (no KYTY_AB_TOGGLE): Gate() returns the env value, one cached bool test.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/logging/log.h"

namespace Libs::Graphics::AbToggle {

inline const std::string& List() {
	static const std::string list = [] {
		const char* v = std::getenv("KYTY_AB_TOGGLE");
		return v != nullptr ? "," + std::string(v) + "," : std::string();
	}();
	return list;
}

inline bool Active() {
	static const bool active = !List().empty();
	return active;
}

// True when `name` (e.g. "KYTY_SPEC_READ_HOIST" or "SPEC_READ_HOIST") is in KYTY_AB_TOGGLE.
inline bool Listed(const char* name) {
	if (!Active()) {
		return false;
	}
	const auto& list = List();
	std::string full = name;
	std::string bare = full.rfind("KYTY_", 0) == 0 ? full.substr(5) : full;
	return list.find("," + full + ",") != std::string::npos ||
	       list.find("," + bare + ",") != std::string::npos ||
	       list.find(",KYTY_" + bare + ",") != std::string::npos;
}

inline std::atomic<bool>& Phase() {
	static std::atomic<bool> phase {false};
	return phase;
}

inline bool On() {
	return Phase().load(std::memory_order_relaxed);
}

// KYTY_CLEAR_META_NOSYNC=auto: the flag follows AutoPhase(), driven by AutoUpdate() from the GPU
// thread's own stats (see there). KYTY_AB_TOGGLE listing a flag still overrides auto.
inline std::atomic<bool>& AutoPhase() {
	static std::atomic<bool> phase {false};
	return phase;
}

inline bool AutoWanted() {
	static const bool wanted = [] {
		const char* v = std::getenv("KYTY_CLEAR_META_NOSYNC");
		return v != nullptr && std::strcmp(v, "auto") == 0;
	}();
	return wanted;
}

// Usage inside a flag's Enabled():  static const bool env = ...; static const bool ab = AbToggle::Listed("KYTY_X");
//                                   return ab ? AbToggle::On() : env;
inline bool Gate(bool listed, bool env_value, bool auto_mode = false) {
	if (listed) {
		return On();
	}
	return auto_mode ? AutoPhase().load(std::memory_order_relaxed) : env_value;
}

// GPU thread only, once per 2 s stats window (GpuThreadStats::MaybePrint). Shares are fractions of
// the window's wall time. Decides the KYTY_CLEAR_META_NOSYNC=auto phase.
//  Measured (Silent Hill): in game rb_clear drain is 12-33 % with the GPU thread saturated (idle
//  ~1 %) and skipping the clear readback helps (+17..59 % fps); in the intro rb_clear is even
//  higher (44 %) but the GPU thread idles ~27 %, so skipping only moves the wait and costs fps.
//  - ON after 2 off-windows with rb_clear >= 8 % and idle < 5 % (judged only while off, so the
//    skip's own effect never feeds back into the decision to enable it).
//  - Trial: the mean draws/s and total drain share of on-windows 2-3 are compared with the last 2
//    off-windows; if draws/s did not rise 5 % and drain did not fall 5 points it backs out and
//    stays off for 60 s.
//  - Re-probe: every 60 s while on it goes off for 2 windows and re-decides from fresh off data,
//    so a scene change (game -> cutscene/menu) is noticed. Idle while on is not used: a faster GPU
//    thread waiting on the guest RenderThread is the skip working, not a reason to stop.
inline void AutoUpdate(double rb_clear, double idle, double drain, double draws_per_s) {
	if (!AutoWanted() || Listed("KYTY_CLEAR_META_NOSYNC")) {
		return;
	}
	static bool    on = false;
	static int     hi = 0, on_windows = 0;
	static double  off_dps[2] = {0, 0}, off_drain[2] = {0, 0}, on_dps = 0, on_drain = 0;
	static int64_t lockout_until_us = 0, on_since_us = 0;
	const int64_t  now_us = std::chrono::duration_cast<std::chrono::microseconds>(
	                            std::chrono::steady_clock::now().time_since_epoch())
	                            .count();
	auto set = [&](bool v, const char* why) {
		on = v;
		AutoPhase().store(v, std::memory_order_relaxed);
		hi = on_windows = 0;
		on_dps = on_drain = 0;
		on_since_us       = now_us;
		LOGF("ClearMetaAuto: %s (%s) t_us=%" PRId64 " rb_clear=%.1f%% idle=%.1f%% drain=%.1f%% draws/s=%.0f\n",
		     v ? "on" : "off", why, now_us, rb_clear * 100, idle * 100, drain * 100, draws_per_s);
	};
	if (!on) {
		off_dps[0]   = off_dps[1];
		off_drain[0] = off_drain[1];
		off_dps[1]   = draws_per_s;
		off_drain[1] = drain;
		if (now_us < lockout_until_us) {
			return;
		}
		hi = (rb_clear >= 0.08 && idle < 0.05) ? hi + 1 : 0;
		if (hi >= 2) {
			set(true, "rb_clear heavy, gpu thread saturated");
		}
		return;
	}
	on_windows++;
	if (on_windows >= 2 && on_windows <= 3) { // window 1 still carries the switch-over
		on_dps += draws_per_s / 2;
		on_drain += drain / 2;
	}
	if (on_windows == 3) {
		const double base_dps   = (off_dps[0] + off_dps[1]) / 2;
		const double base_drain = (off_drain[0] + off_drain[1]) / 2;
		if (on_dps < base_dps * 1.05 && on_drain > base_drain - 0.05) {
			lockout_until_us = now_us + 60'000'000;
			set(false, "trial: no gain, locked out 60 s");
		}
		return;
	}
	if (now_us - on_since_us >= 60'000'000) {
		set(false, "periodic re-probe");
	}
}

// GPU thread, between submissions. Advances the phase from wall time.
inline void Tick() {
	if (!Active()) {
		return;
	}
	static const int64_t period_us = [] {
		const char* v = std::getenv("KYTY_AB_PERIOD_S");
		const double s = v != nullptr ? std::atof(v) : 20.0;
		return static_cast<int64_t>((s > 0.5 ? s : 20.0) * 1e6);
	}();
	static int64_t start_us = 0;
	static int64_t flips    = -1;
	const int64_t  now_us   = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
	if (start_us == 0) {
		start_us = now_us;
	}
	const int64_t k = (now_us - start_us) / period_us;
	if (k == flips) {
		return;
	}
	flips         = k;
	const bool on = (k % 2) == 1;
	Phase().store(on, std::memory_order_relaxed);
	LOGF("ABToggle: %s t_us=%" PRId64 " n=%" PRId64 " flags=%s\n", on ? "on" : "off", now_us, k, List().c_str());
}

} // namespace Libs::Graphics::AbToggle

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ABTOGGLE_H_
