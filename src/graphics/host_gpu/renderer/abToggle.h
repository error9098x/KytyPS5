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

// Usage inside a flag's Enabled():  static const bool env = ...; static const bool ab = AbToggle::Listed("KYTY_X");
//                                   return ab ? AbToggle::On() : env;
inline bool Gate(bool listed, bool env_value) {
	return listed ? On() : env_value;
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
