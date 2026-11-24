#pragma once

// Cross-compiler helpers: assume, struct packing, alignment, restrict, force inline, bit scans,
// saturate. FFS64 is added for 64-bit slot masks. CLZ32 is guarded: core/math/random_pcg.h
// defines it and its MSVC helper too.

#include "core/error/error_macros.h"
#include "core/typedefs.h"

///================= ASSUME ===================

// Define a cross-platform macro to tell the compiler to assume a condition is true.
// This helps the optimizer and can silence warnings like "potential divide by 0".
#ifndef NDEBUG
#define ASSUME(expr)
#elif defined(_MSC_VER)
#define ASSUME(expr) __assume(expr)
#elif defined(__clang__) || defined(__GNUC__)
#define ASSUME(expr)                         \
	do {                                     \
		if (!(expr)) __builtin_unreachable(); \
	} while (0)
#else
#define ASSUME(expr)
#endif

// A helper function to both check a divisor at runtime (in debug)
// and tell the compiler to assume it's non-zero.
inline uint32_t checked_divisor(uint32_t divisor) {
	DEV_ASSERT(divisor != 0); // Your runtime check for debug builds
	ASSUME(divisor != 0); // Hint for the compiler in all builds
	return divisor;
}

///================= PACK ===================

// Define cross-platform macros for packed structures and aligned memory.
// PACK ensures no padding is added between struct members.
// ALIGN forces a specific byte alignment boundary for data.
#if defined(__clang__) || defined(__GNUC__)
// Clang and GCC
#define PACK(...) __VA_ARGS__ __attribute__((__packed__))
#elif defined(_MSC_VER)
// Microsoft Visual C++
#define PACK(...) __pragma(pack(push, 1)) __VA_ARGS__ __pragma(pack(pop))
#else
#error "Unsupported compiler for struct packing"
#endif

#if defined(__clang__) || defined(__GNUC__)
// Clang and GCC
#define ALIGN(alignment, ...) __VA_ARGS__ __attribute__((aligned(alignment)))
#elif defined(_MSC_VER)
// Microsoft Visual C++
#define ALIGN(alignment, ...) __declspec(align(alignment)) __VA_ARGS__
#else
#error "Unsupported compiler for memory alignment"
#endif

///================= RESTRICT ===================

// Define a cross-platform restrict keyword for pointer aliasing optimization.
// Tells the compiler that a pointer is the only way to access the object it points to.
#ifdef __cplusplus
#if defined(__GNUC__) || defined(__clang__)
#define RESTRICT __restrict__
#elif defined(_MSC_VER)
#define RESTRICT __restrict
#else
#define RESTRICT
#endif
#else
#define RESTRICT restrict
#endif

///================= INLINE ===================

// Force inline macro for performance-critical functions.
// Guarantees the function is inlined regardless of compiler heuristics.
#if defined(_MSC_VER)
#define FINLINE(x) __forceinline x noexcept
#define FINLINE_CE(x) __forceinline constexpr x noexcept
#elif defined(__clang__)
#define FINLINE(x) [[clang::always_inline]] inline x noexcept
#define FINLINE_CE(x) [[clang::always_inline]] constexpr x noexcept
#elif defined(__GNUC__)
#define FINLINE(x) [[gnu::always_inline]] inline x noexcept
#define FINLINE_CE(x) [[gnu::always_inline]] constexpr x noexcept
#else
#error "Unsupported compiler"
#endif

///================= BITS ===================

// Bit manipulation intrinsics for efficient bitwise operations.
// FFS - Find First Set (returns the index of the first set bit +1, 0 if not found)
// POPCOUNT - Population Count (number of set bits)
// CLZ32 - Count Leading Zeros (number of zero bits before the first set bit)
// clang-cl defines _MSC_VER too: it takes the builtins and skips <intrin.h>.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#ifndef CLZ32
// CLZ32 (Macro assumes x != 0)
[[maybe_unused]] static int __bsr_clz32(uint32_t x) {
	unsigned long index;
	_BitScanReverse(&index, x); // Undefined behavior if x == 0
	return 31 - index;
}
#define CLZ32(x) __bsr_clz32(x)
#endif
[[maybe_unused]] static int __bsr_clz64(uint64_t x) {
	unsigned long index;
	_BitScanReverse64(&index, x);
	return 63 - index;
}
// FFS (Macro assumes x != 0)
[[maybe_unused]] static int __bsf_ffs(uint32_t x) {
	unsigned long index;
	_BitScanForward(&index, x); // Undefined behavior if x == 0
	return index + 1; // Convert 0-based index to 1-based
}
[[maybe_unused]] static int __bsf_ffs64(uint64_t x) {
	unsigned long index;
	_BitScanForward64(&index, x); // Undefined behavior if x == 0
	return index + 1;
}

#define POPCOUNT(x) __popcnt(x)
#define FFS(x) __bsf_ffs(x)
#define FFS64(x) __bsf_ffs64(x)
#define CLZ64(x) __bsr_clz64(x)

#elif defined(__clang__) || defined(__GNUC__)
#define POPCOUNT(x) __builtin_popcount(x)
#ifndef CLZ32
#define CLZ32(x) __builtin_clz(x)
#endif
#define CLZ64(x) __builtin_clzll(x)
#define FFS(x) __builtin_ffs(x)
#define FFS64(x) __builtin_ffsll(x)
#else
#error "Unsupported compiler"
#endif

FINLINE_CE(uint8_t bit_single(const uint8_t v)) { return v && !(v & (v - 1)); }
FINLINE_CE(uint16_t bit_single(const uint16_t v)) { return v && !(v & (v - 1)); }
FINLINE_CE(uint32_t bit_single(const uint32_t v)) { return v && !(v & (v - 1)); }
FINLINE_CE(uint64_t bit_single(const uint64_t v)) { return v && !(v & (v - 1)); }

// Subtract 1 to handle case where n is already a power of 2
// CLZ32 returns leading zeros, so (32 - CLZ32(n)) gives us
// the position we need to shift to.
// Guard v <= 1: CLZ32(0) is UB on all platforms (_BitScanReverse leaves
// index uninitialised, __builtin_clz(0) is also undefined).
FINLINE(uint32_t next_pow2(const uint32_t v)) { return (v <= 1) ? 1 : (1u << (32 - CLZ32(v - 1))); }
FINLINE(uint64_t next_pow2(const uint64_t v)) { return (v <= 1) ? 1 : (1ull << (64 - CLZ64(v - 1))); }

// Note:: This is not the same as next_pow to because IF it is a pow2 you need the one up to store
// So unlike next pow2 we don't sub 1 also we return the number bits
FINLINE(uint8_t bits_needed(uint32_t v)) { return (v == 0) ? 1 : (32 - CLZ32(v)); }
FINLINE(uint8_t bits_needed_64(uint64_t v)) { return (v == 0) ? 1 : (64 - CLZ64(v)); }

///================= SATURATE ===================

// Clamp to [0, 1] (HLSL saturate). NaN passes through.
FINLINE_CE(float saturate(const float v)) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// saturate(v * mul + add). Remap lo -> 0, hi -> 1: mul = 1 / (hi - lo),
// add = -lo * mul (hi < lo inverts). Keep mul / add constexpr: no divide at run time.
FINLINE_CE(float satmuladd(const float v, const float mul, const float add)) { return saturate(v * mul + add); }

///================= SWAP ===================

#ifndef SWAP
#define SWAP(m_x, m_y) __swap_tmpl((m_x), (m_y))
template <typename T>
inline void __swap_tmpl(T &x, T &y) {
	T aux = x;
	x = y;
	y = aux;
}
#endif
