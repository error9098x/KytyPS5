#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITETICKMAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITETICKMAP_H_

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

namespace Libs::Graphics {

// KYTY_READBACK_PRODUCER_TICK bookkeeping: for guest VA bytes held in BufferCache buffers, an
// upper bound on the scheduler tick of the last GPU command that wrote those cache-buffer bytes.
// Intervals are disjoint, half-open [begin, end). kPending marks a write whose GPU command may
// not have been recorded yet (its tick is unknown). Stamping later than the real tick is always
// safe (it only forces the legacy drain); stamping earlier would not be, so writers stamp after
// recording, with CurrentTick() of that moment.
class WriteTickMap final {
public:
	static constexpr uint64_t kPending = ~uint64_t {0};

	struct Query {
		uint64_t max_tick = 0;
		bool     covered  = true; // every queried byte has a stamp
	};

	void Set(uint64_t address, uint64_t size, uint64_t tick) {
		if (size == 0) {
			return;
		}
		Erase(address, size);
		const uint64_t end = address + size;
		auto           it  = m_map.emplace(address, Entry {end, tick}).first;
		// Coalesce with equal-tick neighbours to keep the map small.
		if (it != m_map.begin()) {
			auto prev = std::prev(it);
			if (prev->second.end == address && prev->second.tick == tick) {
				prev->second.end = end;
				m_map.erase(it);
				it = prev;
			}
		}
		auto next = std::next(it);
		if (next != m_map.end() && next->first == it->second.end && next->second.tick == tick) {
			it->second.end = next->second.end;
			m_map.erase(next);
		}
	}

	void Erase(uint64_t address, uint64_t size) {
		if (size == 0) {
			return;
		}
		const uint64_t end = address + size;
		auto           it  = m_map.lower_bound(address);
		if (it != m_map.begin() && std::prev(it)->second.end > address) {
			it = std::prev(it);
		}
		while (it != m_map.end() && it->first < end) {
			const uint64_t b = it->first;
			const Entry    e = it->second;
			it               = m_map.erase(it);
			if (b < address) {
				m_map.emplace(b, Entry {address, e.tick});
			}
			if (e.end > end) {
				m_map.emplace(end, Entry {e.end, e.tick});
				break;
			}
		}
	}

	[[nodiscard]] Query Max(uint64_t address, uint64_t size) const {
		Query q;
		if (size == 0) {
			return q;
		}
		const uint64_t end    = address + size;
		uint64_t       cursor = address;
		auto           it     = m_map.lower_bound(address);
		if (it != m_map.begin() && std::prev(it)->second.end > address) {
			it = std::prev(it);
		}
		for (; it != m_map.end() && it->first < end; ++it) {
			if (it->first > cursor) {
				q.covered = false;
			}
			q.max_tick = std::max(q.max_tick, it->second.tick);
			cursor     = std::max(cursor, it->second.end);
		}
		if (cursor < end) {
			q.covered = false;
		}
		return q;
	}

	[[nodiscard]] bool   Empty() const noexcept { return m_map.empty(); }
	[[nodiscard]] size_t Size() const noexcept { return m_map.size(); }
	void                 Clear() noexcept { m_map.clear(); }

private:
	struct Entry {
		uint64_t end;
		uint64_t tick;
	};
	std::map<uint64_t, Entry> m_map;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITETICKMAP_H_
