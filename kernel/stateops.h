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

// Summary of a whole vector in one pass: does it hold any S0, any S1, any
// state above S1, and the parity of its S1 count. Everything the reductions and
// the equality predicates need, so each of them is one scan.
struct Summary {
	bool any_zero = false, any_one = false, any_other = false;
	unsigned ones = 0;  // count of S1 lanes modulo 2^32 (parity is what is used)
};
inline Summary summarize(const RTLIL::State *p, size_t n)
{
	Summary s;
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block zero = splat(0), one = splat(1);
	Block z = zero, o = zero, h = zero;
	Block ones_acc = zero;  // per-lane 0/1 accumulator, flushed before it can wrap
	unsigned flushed = 0;
	int pending = 0;
	for (; i + kBlock <= n; i += kBlock) {
		const Block v = load(p + i);
		z |= mask(v == zero);
		const Block is_one = mask(v == one);
		o |= is_one;
		h |= mask(v > one);
		ones_acc += is_one & one;
		if (++pending == 255) {
			for (int k = 0; k < kBlock; k++) flushed += ones_acc[k];
			ones_acc = zero;
			pending = 0;
		}
	}
	for (int k = 0; k < kBlock; k++) flushed += ones_acc[k];
	s.any_zero = any(z);
	s.any_one = any(o);
	s.any_other = any(h);
	s.ones = flushed;
#endif
	for (; i < n; i++) {
		const RTLIL::State b = p[i];
		if (b == RTLIL::State::S0) s.any_zero = true;
		else if (b == RTLIL::State::S1) { s.any_one = true; s.ones++; }
		else s.any_other = true;
	}
	return s;
}

// Fold of op over p[0..n) starting from the identity, as the sequential
// per-bit fold defined it: for And an S0 anywhere gives S0, else any non-S1
// gives Sx, else S1; Or mirrors that with S1; Xor is Sx on any non-two-state
// lane, else the parity.
inline RTLIL::State reduce(Op op, const RTLIL::State *p, size_t n)
{
	const Summary s = summarize(p, n);
	switch (op) {
	case Op::And:
		if (s.any_zero) return RTLIL::State::S0;
		return s.any_other ? RTLIL::State::Sx : RTLIL::State::S1;
	case Op::Or:
		if (s.any_one) return RTLIL::State::S1;
		return s.any_other ? RTLIL::State::Sx : RTLIL::State::S0;
	case Op::Xor:
		if (s.any_other) return RTLIL::State::Sx;
		return (s.ones & 1) ? RTLIL::State::S1 : RTLIL::State::S0;
	default:
		if (s.any_other) return RTLIL::State::Sx;
		return (s.ones & 1) ? RTLIL::State::S0 : RTLIL::State::S1;
	}
}

// $eq over two equal-length vectors: S0 if some lane is a definite 0/1
// conflict, else Sx if some lane is not two-state, else S1.
inline RTLIL::State eq_status(const RTLIL::State *a, const RTLIL::State *b, size_t n)
{
	bool other = false;
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block zero = splat(0), one = splat(1);
	Block conflict = zero, h = zero;
	for (; i + kBlock <= n; i += kBlock) {
		const Block va = load(a + i), vb = load(b + i);
		conflict |= (mask(va == zero) & mask(vb == one)) | (mask(va == one) & mask(vb == zero));
		h |= mask(va > one) | mask(vb > one);
	}
	if (any(conflict))
		return RTLIL::State::S0;
	other = any(h);
#endif
	for (; i < n; i++) {
		if ((a[i] == RTLIL::State::S0 && b[i] == RTLIL::State::S1) ||
		    (a[i] == RTLIL::State::S1 && b[i] == RTLIL::State::S0))
			return RTLIL::State::S0;
		if (a[i] > RTLIL::State::S1 || b[i] > RTLIL::State::S1)
			other = true;
	}
	return other ? RTLIL::State::Sx : RTLIL::State::S1;
}

// out[i] = a[i] == b[i] ? a[i] : Sx  ($mux with an undefined select).
inline void merge_eq(RTLIL::State *out, const RTLIL::State *a, const RTLIL::State *b, size_t n)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block x = splat(2);
	for (; i + kBlock <= n; i += kBlock) {
		const Block va = load(a + i), vb = load(b + i);
		const Block same = mask(va == vb);
		store(out + i, (same & va) | (~same & x));
	}
#endif
	for (; i < n; i++)
		out[i] = a[i] == b[i] ? a[i] : RTLIL::State::Sx;
}

// out[i] = a[i] == b[i] ? S1 : S0  ($bweqx).
inline void bweqx(RTLIL::State *out, const RTLIL::State *a, const RTLIL::State *b, size_t n)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block one = splat(1);
	for (; i + kBlock <= n; i += kBlock)
		store(out + i, mask(load(a + i) == load(b + i)) & one);
#endif
	for (; i < n; i++)
		out[i] = a[i] == b[i] ? RTLIL::State::S1 : RTLIL::State::S0;
}

// $bwmux: s == S1 -> b; s == Sx and a != b -> Sx; otherwise a.
inline void bwmux(RTLIL::State *out, const RTLIL::State *a, const RTLIL::State *b, const RTLIL::State *s, size_t n)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block one = splat(1), x = splat(2);
	for (; i + kBlock <= n; i += kBlock) {
		const Block va = load(a + i), vb = load(b + i), vs = load(s + i);
		const Block sel1 = mask(vs == one);
		const Block usex = mask(vs == x) & ~mask(va == vb);
		const Block pick = (sel1 & vb) | (~sel1 & va);
		store(out + i, (usex & x) | (~usex & pick));
	}
#endif
	for (; i < n; i++) {
		if (s[i] != RTLIL::State::Sx || a[i] == b[i])
			out[i] = s[i] == RTLIL::State::S1 ? b[i] : a[i];
		else
			out[i] = RTLIL::State::Sx;
	}
}

// Two cubes intersect unless some lane is two-state in both and differs
// (BitPatternPool::match; Sa and the other states are wildcards).
inline bool cubes_match(const RTLIL::State *a, const RTLIL::State *b, size_t n)
{
	size_t i = 0;
#ifdef YOSYS_STATEOPS_SIMD
	const Block one = splat(1);
	Block clash = splat(0);
	for (; i + kBlock <= n; i += kBlock) {
		const Block va = load(a + i), vb = load(b + i);
		clash |= mask(va <= one) & mask(vb <= one) & ~mask(va == vb);
	}
	if (any(clash))
		return false;
#endif
	for (; i < n; i++)
		if (a[i] <= RTLIL::State::S1 && b[i] <= RTLIL::State::S1 && a[i] != b[i])
			return false;
	return true;
}

} // namespace stateops

YOSYS_NAMESPACE_END

#endif
