/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2026  Etienne Cimon
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Byte-vector kernels over RTLIL::State arrays (one byte per bit, S0=0 S1=1
// Sx=2 Sz=3 Sa=4 Sm=5). Two things recur across the kernel and the opt passes:
// a predicate over every bit of a constant (is_fully_zero/ones/def/undef), and a
// four-state logic operation between two constants (const_and/or/xor/xnor/not).
// Both were one branch per bit; here they are 16- or 32-byte blocks through the
// compiler's portable vector extension, with the scalar form as the tail and as
// the whole path when the extension is unavailable. Results are defined to be
// identical to the scalar definitions in calc.cc / rtlil.cc, which the unit test
// stateopsTest.cc checks exhaustively per lane and randomly per vector.

#ifndef YOSYS_KERNEL_STATEOPS_H
#define YOSYS_KERNEL_STATEOPS_H

#include "kernel/rtlil.h"

#include <cstdint>
#include <cstring>

YOSYS_NAMESPACE_BEGIN

namespace stateops {

#if !defined(YOSYS_NO_SIMD) && (defined(__GNUC__) || defined(__clang__)) && \
	(defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__))
#if defined(__AVX2__)
constexpr int kBlock = 32;
#else
constexpr int kBlock = 16;
#endif
typedef unsigned char Block __attribute__((vector_size(kBlock)));
typedef uint64_t Block64 __attribute__((vector_size(kBlock)));

inline Block load(const RTLIL::State *p)
{
	Block v;
	std::memcpy(&v, p, sizeof(v));
	return v;
}
inline void store(RTLIL::State *p, Block v) { std::memcpy(p, &v, sizeof(v)); }
inline Block splat(unsigned char c) { return Block{} + c; }
// Comparisons yield a signed-lane mask; view it as bytes.
template <typename M> inline Block mask(M m) { return (Block)m; }
// True iff some lane of a (0 / 0xff) byte mask is set.
inline bool any(Block m)
{
	Block64 w;
	std::memcpy(&w, &m, sizeof(w));
	uint64_t r = 0;
	for (int k = 0; k < kBlock / 8; k++)
		r |= w[k];
	return r != 0;
}
#define YOSYS_STATEOPS_SIMD 1
#endif

// All n states are in {c0, c1} (pass c1 == c0 for a single value).
inline bool all_in(const RTLIL::State *p, size_t n, RTLIL::State c0, RTLIL::State c1)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block v0 = splat(c0), v1 = splat(c1);
	for (; i + kBlock <= n; i += kBlock) {
		const Block v = load(p + i);
		const Block ok = mask(v == v0) | mask(v == v1);
		if (any(~ok))
			return false;
	}
#endif
	for (; i < n; i++)
		if (p[i] != c0 && p[i] != c1)
			return false;
	return true;
}

enum class Op { And, Or, Xor, Xnor };

inline RTLIL::State scalar(Op op, RTLIL::State a, RTLIL::State b)
{
	// The reference definitions (calc.cc logic_and/or/xor/xnor), kept here so the
	// vector lanes and the tail cannot drift apart.
	switch (op) {
	case Op::And:
		if (a == RTLIL::State::S0 || b == RTLIL::State::S0) return RTLIL::State::S0;
		if (a != RTLIL::State::S1 || b != RTLIL::State::S1) return RTLIL::State::Sx;
		return RTLIL::State::S1;
	case Op::Or:
		if (a == RTLIL::State::S1 || b == RTLIL::State::S1) return RTLIL::State::S1;
		if (a != RTLIL::State::S0 || b != RTLIL::State::S0) return RTLIL::State::Sx;
		return RTLIL::State::S0;
	case Op::Xor:
		if (a > RTLIL::State::S1 || b > RTLIL::State::S1) return RTLIL::State::Sx;
		return a != b ? RTLIL::State::S1 : RTLIL::State::S0;
	case Op::Xnor:
		if (a > RTLIL::State::S1 || b > RTLIL::State::S1) return RTLIL::State::Sx;
		return a == b ? RTLIL::State::S1 : RTLIL::State::S0;
	}
	return RTLIL::State::Sx;
}

// out[i] = op(a[i], b[i]) for i < n. out may alias a or b.
inline void logic(Op op, RTLIL::State *out, const RTLIL::State *a, const RTLIL::State *b, size_t n)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block zero = splat(0), one = splat(1), x = splat(2);
	for (; i + kBlock <= n; i += kBlock) {
		const Block va = load(a + i), vb = load(b + i);
		// lanes where both operands are two-state take the plain bitwise result
		const Block two = mask(va <= one) & mask(vb <= one);
		Block r2, rx;
		switch (op) {
		case Op::And: {
			const Block z = mask(va == zero) | mask(vb == zero);
			r2 = va & vb;
			rx = ~z & x;  // a controlling 0 wins, otherwise Sx
			break;
		}
		case Op::Or: {
			const Block o = mask(va == one) | mask(vb == one);
			r2 = va | vb;
			rx = (o & one) | (~o & x);  // a controlling 1 wins, otherwise Sx
			break;
		}
		case Op::Xor:
			r2 = va ^ vb;
			rx = x;
			break;
		default:
			r2 = (va ^ vb) ^ one;
			rx = x;
			break;
		}
		store(out + i, (two & r2) | (~two & rx));
	}
#endif
	for (; i < n; i++)
		out[i] = scalar(op, a[i], b[i]);
}

// out[i] = !a[i]: S0 <-> S1, anything else Sx.
inline void logic_not(RTLIL::State *out, const RTLIL::State *a, size_t n)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block one = splat(1), x = splat(2);
	for (; i + kBlock <= n; i += kBlock) {
		const Block va = load(a + i);
		const Block two = mask(va <= one);
		store(out + i, (two & (va ^ one)) | (~two & x));
	}
#endif
	for (; i < n; i++)
		out[i] = a[i] == RTLIL::State::S0 ? RTLIL::State::S1
		       : a[i] == RTLIL::State::S1 ? RTLIL::State::S0 : RTLIL::State::Sx;
}

} // namespace stateops

YOSYS_NAMESPACE_END

#endif
