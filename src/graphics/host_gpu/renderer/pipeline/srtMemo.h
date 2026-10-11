#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTMEMO_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTMEMO_H_

// KYTY_SRT_MEMO=1: memoize ShaderRecompiler::IR::MaterializeResources per program-cache entry.
//
// MaterializeResources is a deterministic function of
//   (immutable ResourcePlan, user-data SGPRs, shader base, workgroup counts,
//    the values returned by every guest-memory read the SRT walk performs).
// The plan's mutable scratch (active_sources, visited/pending blocks, material keys, evaluation
// contexts) is always (re)initialized before it is read, so it is output-only.
//
// A slot records the inputs and the ordered list of guest reads (address, size, reader kind,
// values) of one materialization. A later call with identical inputs replays exactly those reads
// through the same readers (same addresses, same sizes, same order, so the same GPU-dirty
// readback and page-fault side effects) and compares the values. If every value is unchanged,
// the walk would take the same path and produce the same snapshot and specialization by
// induction over its reads, so the stored result is returned without re-walking the IR graph.
//
// The "write generation" of the pages the walk read is therefore content-defined: any write by
// any agent (guest CPU, CP/DMA, GPU write-back) that changes a word the walk depended on fails
// the replay. A counter-based page generation is not used because guest CPU writes to pages the
// buffer cache does not write-protect raise no event at all (see PROOF.md section 6).
//
// Header-only so the sandbox harness compiles this exact code against the real walker.

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics::SrtMemo {

namespace IR = ShaderRecompiler::IR;

// Beyond this many logged words a slot is not memoized (replay would approach walk cost).
inline constexpr uint32_t MaxLoggedWords = 2048;
inline constexpr uint32_t SlotsPerEntry  = 4;
// Adaptive bypass: an entry whose slots hit in fewer than 1/4 of a 32-call window skips the memo
// (default path, zero logging overhead) for the next 480 calls, then probes again.
inline constexpr uint32_t WindowCalls    = 32;
inline constexpr uint32_t BypassCalls    = 480;
// Largest single read replayed from the stack buffer (SRT reads are at most 16 dwords).
inline constexpr uint32_t MaxReadWords   = 64;

struct Stats {
	uint64_t calls       = 0;
	uint64_t hits        = 0;
	uint64_t key_misses  = 0; // no slot with equal user data / shader base / workgroup counts
	uint64_t value_misses = 0; // equal key, but a replayed guest word changed
	uint64_t read_failures = 0; // a replayed read failed (unmapped); full path decides
	uint64_t uncacheable = 0; // log overflow
	uint64_t bypassed    = 0; // calls routed to the default path by the adaptive bypass
	uint64_t replayed_words = 0;
};

class ReadLog {
public:
	// One record per logged dword. The first dword of a read carries the read's dword count and
	// reader kind; the following dwords of the same read are continuation records (count 0).
	struct Record {
		uint64_t address = 0;
		uint32_t value   = 0;
		uint16_t count   = 0;
		uint8_t  strict  = 0;
	};

	// Readers installed into the SrtRuntime handed to MaterializeResources. They perform exactly
	// the access the unwrapped runtime would (strict: the original read_specialization_memory;
	// ordinary: the walker's direct memcpy used when read_memory == nullptr) and log the result.
	IR::SrtRuntime Wrap(const IR::SrtRuntime& runtime) {
		m_inner_strict   = runtime.read_specialization_memory;
		m_inner_ordinary = runtime.read_memory;
		m_inner_userdata = runtime.userdata;
		m_records.clear();
		m_overflow = false;
		IR::SrtRuntime wrapped = runtime;
		wrapped.userdata       = this;
		wrapped.read_memory    = &ReadLog::Ordinary;
		wrapped.read_specialization_memory =
		    runtime.read_specialization_memory != nullptr ? &ReadLog::Strict : nullptr;
		return wrapped;
	}

	[[nodiscard]] bool Overflowed() const { return m_overflow; }
	[[nodiscard]] size_t WordCount() const { return m_records.size(); }
	[[nodiscard]] size_t ReadCount() const {
		size_t n = 0;
		for (const auto& r: m_records) n += r.count != 0 ? 1u : 0u;
		return n;
	}

	// Replays every logged read, in order, through the same reader and compares. Returns +1
	// when all values are unchanged, 0 at the first changed value, -1 when a read failed.
	// Stopping at the first change means every replayed access is one the fresh walk would
	// also perform (the walk's next address only depends on the values read before it).
	int Replay(const IR::SrtRuntime& runtime, uint64_t& replayed_words) const {
		std::array<uint32_t, MaxReadWords> scratch;
		const Record* r   = m_records.data();
		const Record* end = r + m_records.size();
		while (r < end) {
			const uint32_t n = r->count;
			replayed_words += n;
			if (r->strict != 0u) {
				// Identical (address, size) call through the identical reader, so a GPU-dirty
				// range takes the same readback path as in the recorded walk.
				if (runtime.read_specialization_memory == nullptr ||
				    !runtime.read_specialization_memory(runtime.userdata, r->address,
				                                        std::span<uint32_t>(scratch.data(), n))) {
					return -1;
				}
			} else if (runtime.read_memory != nullptr) {
				if (!runtime.read_memory(runtime.userdata, r->address,
				                         std::span<uint32_t>(scratch.data(), n))) {
					return -1;
				}
			} else {
				// Same direct host access the walker performs (SrtWalker::EvaluateRawRead,
				// CaptureOrdinaryRead): a protected GPU-dirty page faults here exactly as there.
				std::memcpy(scratch.data(), reinterpret_cast<const void*>(r->address), n * sizeof(uint32_t));
			}
			for (uint32_t i = 0; i < n; ++i) {
				if (scratch[i] != r[i].value) return 0;
			}
			r += n;
		}
		return 1;
	}

private:
	void Log(uint64_t address, std::span<const uint32_t> values, bool strict) {
		const size_t n = values.size();
		if (m_overflow || n == 0) return;
		if (m_records.size() + n > MaxLoggedWords || n > MaxReadWords) {
			m_overflow = true;
			return;
		}
		// Drop an exact consecutive single-dword duplicate (the clean and the ordinary walker
		// often read the same dword back to back). Re-reading an unchanged word through the same
		// reader has no further side effect: a GPU-dirty range was read back by the first call.
		if (n == 1 && !m_records.empty()) {
			const auto& last = m_records.back();
			if (last.count == 1 && last.address == address && last.value == values[0] &&
			    last.strict == static_cast<uint8_t>(strict)) {
				return;
			}
		}
		for (size_t i = 0; i < n; ++i) {
			m_records.push_back({.address = address + i * sizeof(uint32_t),
			                     .value   = values[i],
			                     .count   = static_cast<uint16_t>(i == 0 ? n : 0),
			                     .strict  = static_cast<uint8_t>(strict)});
		}
	}

	static bool Strict(void* userdata, uint64_t address, std::span<uint32_t> values) {
		auto& self = *static_cast<ReadLog*>(userdata);
		if (!self.m_inner_strict(self.m_inner_userdata, address, values)) return false;
		self.Log(address, values, true);
		return true;
	}

	static bool Ordinary(void* userdata, uint64_t address, std::span<uint32_t> values) {
		auto& self = *static_cast<ReadLog*>(userdata);
		if (self.m_inner_ordinary != nullptr) {
			if (!self.m_inner_ordinary(self.m_inner_userdata, address, values)) return false;
		} else {
			std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
		}
		self.Log(address, values, false);
		return true;
	}

	std::vector<Record> m_records;
	IR::SrtMemoryReader m_inner_strict   = nullptr;
	IR::SrtMemoryReader m_inner_ordinary = nullptr;
	void*               m_inner_userdata = nullptr;
	bool                m_overflow       = false;
};

struct Slot {
	bool                       valid    = false;
	uint64_t                   last_use = 0;
	std::vector<uint32_t>      user_data;
	uint64_t                   shader_base = 0;
	std::array<uint32_t, 3>    workgroup_counts {};
	uint32_t                   workgroup_count_size = 0;
	ReadLog                    log;
	IR::ResourceSnapshot       resources;
	IR::ResourceSpecialization specialization;
	// Cached permutation lookup: valid for the push-data cursor it was resolved with.
	uint32_t                   permutation_cursor = UINT32_MAX;
	uint32_t                   permutation        = UINT32_MAX;

	[[nodiscard]] bool KeyEquals(const IR::SrtRuntime& runtime) const {
		if (!valid || shader_base != runtime.shader_base ||
		    user_data.size() != runtime.user_data.size() ||
		    workgroup_count_size != runtime.workgroup_counts.size()) {
			return false;
		}
		for (uint32_t i = 0; i < workgroup_count_size; ++i) {
			if (workgroup_counts[i] != runtime.workgroup_counts[i]) return false;
		}
		return user_data.empty() ||
		       std::memcmp(user_data.data(), runtime.user_data.data(),
		                   user_data.size() * sizeof(uint32_t)) == 0;
	}

	void SetKey(const IR::SrtRuntime& runtime) {
		user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
		shader_base          = runtime.shader_base;
		workgroup_count_size = static_cast<uint32_t>(
		    std::min<size_t>(runtime.workgroup_counts.size(), workgroup_counts.size()));
		for (uint32_t i = 0; i < workgroup_count_size; ++i) {
			workgroup_counts[i] = runtime.workgroup_counts[i];
		}
	}
};

class EntryMemo {
public:
	// The whole memo step for one Get() on an existing entry. Returns the slot holding resources
	// valid for `runtime` (memoized or freshly materialized), or nullptr when the entry is in
	// adaptive bypass: the caller then runs the default (unmemoized) path. `ok` is false only
	// when MaterializeResources itself failed (the default path EXITs in that case as well).
	Slot* Acquire(const IR::ResourcePlan& plan, const IR::SrtRuntime& runtime, Stats& stats,
	              bool& ok) {
		ok = true;
		++stats.calls;
		if (m_bypass != 0u) {
			--m_bypass;
			++stats.bypassed;
			return nullptr;
		}
		Slot* slot = Find(runtime, stats);
		const bool hit = slot != nullptr;
		if (slot == nullptr) {
			slot = Materialize(plan, runtime, stats, ok);
		}
		m_window_hits += hit ? 1u : 0u;
		if (++m_window_calls == WindowCalls) {
			if (m_window_hits * 4u < m_window_calls) m_bypass = BypassCalls;
			m_window_calls = 0;
			m_window_hits  = 0;
		}
		return slot;
	}

	// Returns a slot whose stored materialization is valid for `runtime`, or nullptr.
	// Materialize() reuses the slot with an equal key, so at most one valid slot matches.
	Slot* Find(const IR::SrtRuntime& runtime, Stats& stats) {
		for (auto& slot: m_slots) {
			if (!slot.KeyEquals(runtime)) continue;
			const int result = slot.log.Replay(runtime, stats.replayed_words);
			if (result > 0) {
				slot.last_use = ++m_clock;
				++stats.hits;
				return &slot;
			}
			++(result < 0 ? stats.read_failures : stats.value_misses);
			return nullptr;
		}
		++stats.key_misses;
		return nullptr;
	}

	// Materializes into a slot (the matching-key slot if one exists, else the LRU) and returns
	// it, or nullptr when the result cannot be memoized; then the caller must use the default
	// path. A failed materialization leaves the slot invalid.
	Slot* Materialize(const IR::ResourcePlan& plan, const IR::SrtRuntime& runtime, Stats& stats,
	                  bool& ok) {
		Slot* victim = nullptr;
		for (auto& slot: m_slots) {
			if (slot.KeyEquals(runtime)) {
				victim = &slot;
				break;
			}
		}
		if (victim == nullptr) {
			victim = &m_slots[0];
			for (auto& slot: m_slots) {
				if (!slot.valid) {
					victim = &slot;
					break;
				}
				if (slot.last_use < victim->last_use) victim = &slot;
			}
		}
		victim->valid = false;
		const auto logged = victim->log.Wrap(runtime);
		ok = IR::MaterializeResources(plan, logged, victim->resources, victim->specialization);
		if (!ok) return nullptr;
		if (victim->log.Overflowed()) {
			++stats.uncacheable;
			// The result is still correct for this call; it just cannot be replayed later.
			victim->SetKey(runtime);
			victim->valid              = false;
			victim->permutation_cursor = UINT32_MAX;
			victim->last_use           = ++m_clock;
			return victim;
		}
		victim->SetKey(runtime);
		victim->valid              = true;
		victim->permutation_cursor = UINT32_MAX;
		victim->permutation        = UINT32_MAX;
		victim->last_use           = ++m_clock;
		return victim;
	}

private:
	std::array<Slot, SlotsPerEntry> m_slots {};
	uint64_t                        m_clock        = 0;
	uint32_t                        m_window_calls = 0;
	uint32_t                        m_window_hits  = 0;
	uint32_t                        m_bypass       = 0;
};

inline bool SnapshotEquals(const IR::ResourceSnapshot& a, const IR::ResourceSnapshot& b) {
	return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
	       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data &&
	       a.specialization_reads == b.specialization_reads && a.uniform_fill == b.uniform_fill;
}

} // namespace Libs::Graphics::SrtMemo

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTMEMO_H_ */
