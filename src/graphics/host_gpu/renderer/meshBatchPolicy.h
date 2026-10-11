#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHBATCHPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHBATCHPOLICY_H_

// KYTY_MESH_BATCH=1 (opt-in): batch consecutive emulated mesh draws.
//
// Baseline records every emulated mesh draw as
//   EndRendering, reuse barrier, [compute encoder] dispatch, produced barrier, BeginRendering, draw
// so N consecutive mesh draws cost N Metal compute encoders (each one re-walks every BDA buffer in
// MoltenVK, MVKCommandEncoderState.mm:654) and N render-encoder restarts.
//
// With the flag the compute half of each draw is recorded immediately into the open compute
// encoder, at a per-draw offset of the scratch buffer, and the graphics half is captured
// (descriptors, push constants, dynamic state, render state, pipeline, vertex count). The captured
// draws are replayed, in order, after one produced barrier, the first time anything else would be
// recorded into the command buffer (CommandBuffer::Handle/BeginRendering/EndRendering run the
// flush hook first) or when a non-batchable draw arrives. The command buffer state after a flush
// is the state baseline leaves after the last of those draws.
//
// This header holds the Vulkan-free policy (flag parsing, scratch ring, hazard set, counters) so
// that it can be compiled into the differential model test unchanged.
//
// KYTY_DEPTH_READ_UNION=1 (opt-in) is the second, independent change of entry 31: see
// renderDraw.cpp AcquireRenderTargets.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Libs::Graphics::MeshBatch {

[[nodiscard]] inline bool EnvFlag(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
}

[[nodiscard]] inline bool Enabled() {
	static const bool enabled = EnvFlag("KYTY_MESH_BATCH");
	return enabled;
}

[[nodiscard]] inline bool DepthReadUnionEnabled() {
	static const bool enabled = EnvFlag("KYTY_DEPTH_READ_UNION");
	return enabled;
}

// Upper bound of captured draws; a full batch flushes before the next one is added.
inline constexpr uint32_t MaxDraws = 64;

// Why a pending batch was replayed.
enum class FlushReason : uint32_t {
	Handle,         // someone asked the command buffer for its handle (any recording)
	EndRendering,   // an out-of-pass command (barrier, copy, dispatch, submit)
	BeginRendering, // a render pass begun outside the batch
	Draw,           // a draw that cannot join the batch
	Full,           // MaxDraws reached
	Overflow,       // the scratch ring is exhausted
	Count
};

inline constexpr std::array<const char*, static_cast<size_t>(FlushReason::Count)> FlushReasonNames {
    "handle", "end_rendering", "begin_rendering", "draw", "full", "overflow"};

// Why a mesh draw did not join a batch (it then runs the unchanged baseline path).
enum class RejectReason : uint32_t {
	Chunked,     // needs more than one scratch chunk
	Gds,         // a stage binds GDS (CommitBindings records a GDS barrier)
	Writes,      // a stage writes buffers (ShaderWriteBarrier follows the draw)
	Storage,     // a stage binds a storage image
	Hazard,      // the compute half samples an attachment of a pending draw
	Diagnostics, // KYTY_MESH_EMU_SKIP is set
	Count
};

inline constexpr std::array<const char*, static_cast<size_t>(RejectReason::Count)>
    RejectReasonNames {"chunked", "gds", "writes", "storage", "hazard", "diag"};

// Bump allocator over the scratch buffer. An epoch starts with one reuse barrier (all earlier
// scratch reads and writes before the new compute writes); inside an epoch every draw gets a
// disjoint region, so no barrier is needed between the dispatches of one batch.
struct ScratchRing {
	uint64_t capacity = 0;
	uint64_t align    = 256;
	uint64_t cursor   = 0;
	bool     epoch    = false; // regions handed out since the last reuse barrier are live

	enum class Result : uint32_t { Fits, NeedsNewEpoch, TooLarge };

	[[nodiscard]] Result Try(uint64_t bytes, uint64_t& offset) {
		if (bytes == 0 || bytes > capacity) {
			return Result::TooLarge;
		}
		if (!epoch) {
			return Result::NeedsNewEpoch;
		}
		const uint64_t start = (cursor + align - 1u) / align * align;
		if (start > capacity || bytes > capacity - start) {
			return Result::NeedsNewEpoch;
		}
		offset = start;
		cursor = start + bytes;
		return Result::Fits;
	}
	void NewEpoch() {
		cursor = 0;
		epoch  = true;
	}
	void Close() { epoch = false; }
};

// Images written (as attachments) by the captured, not yet replayed draws: by cache identity and
// by guest memory range, so an alias of the same memory under another image id also matches.
struct PendingTargets {
	struct Range {
		uint64_t address = 0;
		uint64_t size    = 0;
	};
	std::vector<uint64_t> ids;
	std::vector<Range>    ranges;
	void Add(uint64_t id, uint64_t address, uint64_t size) {
		if (id != 0 && std::find(ids.begin(), ids.end(), id) == ids.end()) {
			ids.push_back(id);
		}
		if (size != 0) {
			ranges.push_back({address, size});
		}
	}
	[[nodiscard]] bool Conflicts(uint64_t id, uint64_t address, uint64_t size) const {
		if (id != 0 && std::find(ids.begin(), ids.end(), id) != ids.end()) {
			return true;
		}
		if (size == 0) {
			return false;
		}
		return std::any_of(ranges.begin(), ranges.end(), [&](const Range& r) {
			return address < r.address + r.size && r.address < address + size;
		});
	}
	[[nodiscard]] bool Empty() const { return ids.empty() && ranges.empty(); }
	void Clear() {
		ids.clear();
		ranges.clear();
	}
};

struct Stats {
	uint64_t batched = 0;
	uint64_t baseline = 0;
	uint64_t flushes = 0;
	uint64_t replayed = 0;
	uint64_t max_run = 0;
	std::array<uint64_t, static_cast<size_t>(FlushReason::Count)>  flush_by {};
	std::array<uint64_t, static_cast<size_t>(RejectReason::Count)> reject_by {};
	int64_t last_us = 0;

	void NoteFlush(FlushReason reason, uint64_t draws) {
		flushes++;
		replayed += draws;
		max_run = std::max(max_run, draws);
		flush_by[static_cast<size_t>(reason)]++;
	}
	void NoteReject(RejectReason reason) {
		baseline++;
		reject_by[static_cast<size_t>(reason)]++;
	}
	// One line every 2 s: batched draws / flushes is the mean run, i.e. draws per compute encoder.
	template <typename Log>
	void MaybePrint(Log&& log) {
		const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
		                     std::chrono::steady_clock::now().time_since_epoch())
		                     .count();
		if (last_us == 0) {
			last_us = now;
			return;
		}
		if (now - last_us < 2000000) {
			return;
		}
		char line[512];
		int  n = std::snprintf(line, sizeof(line),
		                       "MeshBatch: dt_ms=%" PRId64 " batched=%" PRIu64 " flushes=%" PRIu64
		                       " replayed=%" PRIu64 " mean_run=%.2f max_run=%" PRIu64
		                       " baseline=%" PRIu64,
		                       (now - last_us) / 1000, batched, flushes, replayed,
		                       flushes != 0 ? static_cast<double>(replayed) / flushes : 0.0,
		                       max_run, baseline);
		for (size_t i = 0; i < flush_by.size() && n > 0 && n < static_cast<int>(sizeof(line)); i++) {
			n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " f_%s=%" PRIu64,
			                   FlushReasonNames[i], flush_by[i]);
		}
		for (size_t i = 0; i < reject_by.size() && n > 0 && n < static_cast<int>(sizeof(line));
		     i++) {
			n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " r_%s=%" PRIu64,
			                   RejectReasonNames[i], reject_by[i]);
		}
		log(line);
		*this = {};
		last_us = now;
	}
};

} // namespace Libs::Graphics::MeshBatch

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHBATCHPOLICY_H_ */
