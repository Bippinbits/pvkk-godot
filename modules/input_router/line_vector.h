#pragma once

// Growable array whose storage starts on a cache line, so a 64 B element never straddles two.
// Elements relocate by memcpy, as in LocalVector.

#include "core/error/error_macros.h"
#include "core/os/memory.h"

namespace InputRouterInternal {

constexpr uint32_t CACHE_LINE = 64;

template <typename T>
class LineVector {
	T *data = nullptr;
	uint32_t count = 0;
	uint32_t capacity = 0;

public:
	_FORCE_INLINE_ uint32_t size() const { return count; }
	_FORCE_INLINE_ T &operator[](uint32_t p_index) {
		CRASH_BAD_UNSIGNED_INDEX(p_index, count);
		return data[p_index];
	}
	_FORCE_INLINE_ const T &operator[](uint32_t p_index) const {
		CRASH_BAD_UNSIGNED_INDEX(p_index, count);
		return data[p_index];
	}
	void push_back(const T &p_value) {
		if (count == capacity) {
			const uint32_t grown = MAX(capacity * 2, 16u);
			data = (T *)Memory::realloc_aligned_static(data, grown * sizeof(T), count * sizeof(T), CACHE_LINE);
			capacity = grown;
		}
		memnew_placement(&data[count++], T(p_value));
	}
	void clear() {
		if constexpr (!std::is_trivially_destructible_v<T>) {
			for (uint32_t i = 0; i < count; i++) {
				data[i].~T();
			}
		}
		count = 0;
	}

	LineVector() = default;
	LineVector(const LineVector &) = delete;
	LineVector &operator=(const LineVector &) = delete;
	~LineVector() {
		clear();
		if (data) {
			Memory::free_aligned_static(data);
		}
	}
};

} // namespace InputRouterInternal
