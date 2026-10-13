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

#include <gtest/gtest.h>

#include "kernel/bitpattern.h"
#include "kernel/rtlil.h"
#include "kernel/stateops.h"
#include "libs/bigint/BigIntegerLibrary.hh"

#include <random>

YOSYS_NAMESPACE_BEGIN

namespace {

using RTLIL::State;
const State kStates[] = {State::S0, State::S1, State::Sx, State::Sz, State::Sa, State::Sm};

// The pre-kernel per-bit definitions, verbatim from calc.cc, as the oracle.
State ref_and(State a, State b)
{
	if (a == State::S0) return State::S0;
	if (b == State::S0) return State::S0;
	if (a != State::S1) return State::Sx;
	if (b != State::S1) return State::Sx;
	return State::S1;
}
State ref_or(State a, State b)
{
	if (a == State::S1) return State::S1;
	if (b == State::S1) return State::S1;
	if (a != State::S0) return State::Sx;
	if (b != State::S0) return State::Sx;
	return State::S0;
}
State ref_xor(State a, State b)
{
	if (a != State::S0 && a != State::S1) return State::Sx;
	if (b != State::S0 && b != State::S1) return State::Sx;
	return a != b ? State::S1 : State::S0;
}
State ref_xnor(State a, State b)
{
	if (a != State::S0 && a != State::S1) return State::Sx;
	if (b != State::S0 && b != State::S1) return State::Sx;
	return a == b ? State::S1 : State::S0;
}
State ref_not(State a)
{
	if (a == State::S0) return State::S1;
	if (a == State::S1) return State::S0;
	return State::Sx;
}

struct OpCase {
	stateops::Op op;
	State (*ref)(State, State);
};
const OpCase kOps[] = {{stateops::Op::And, ref_and}, {stateops::Op::Or, ref_or},
                       {stateops::Op::Xor, ref_xor}, {stateops::Op::Xnor, ref_xnor}};

// Every (a, b) state pair, placed in every lane of a 3-block vector, so each pair
// is exercised in the vector body at every alignment and in the scalar tail.
TEST(StateOpsTest, LogicEveryPairEveryLane)
{
	const size_t n = 100;  // 3 full 32-byte blocks + a 4-byte tail (or 6 + 4 at 16)
	for (const OpCase &c : kOps) {
		for (State a : kStates) {
			for (State b : kStates) {
				for (size_t lane = 0; lane < n; lane++) {
					std::vector<State> va(n, State::S1), vb(n, State::S0), out(n, State::Sm);
					va[lane] = a;
					vb[lane] = b;
					stateops::logic(c.op, out.data(), va.data(), vb.data(), n);
					for (size_t i = 0; i < n; i++)
						ASSERT_EQ(out[i], c.ref(va[i], vb[i]))
							<< "op " << int(c.op) << " a=" << int(a) << " b=" << int(b) << " lane " << lane << " at " << i;
				}
			}
		}
	}
}

TEST(StateOpsTest, LogicRandomVectorsAndAliasing)
{
	std::mt19937 rng(0x5eed);
	for (int rep = 0; rep < 400; rep++) {
		const size_t n = rng() % 300;
		std::vector<State> va(n), vb(n);
		for (size_t i = 0; i < n; i++) {
			va[i] = kStates[rng() % 6];
			vb[i] = kStates[rng() % 6];
		}
		for (const OpCase &c : kOps) {
			std::vector<State> out(n, State::Sm), expect(n);
			for (size_t i = 0; i < n; i++)
				expect[i] = c.ref(va[i], vb[i]);
			stateops::logic(c.op, out.data(), va.data(), vb.data(), n);
			ASSERT_EQ(out, expect);
			// in place: out aliases the first operand
			std::vector<State> inplace = va;
			stateops::logic(c.op, inplace.data(), inplace.data(), vb.data(), n);
			ASSERT_EQ(inplace, expect);
		}
		std::vector<State> notted(n, State::Sm), expect_not(n);
		for (size_t i = 0; i < n; i++)
			expect_not[i] = ref_not(va[i]);
		stateops::logic_not(notted.data(), va.data(), n);
		ASSERT_EQ(notted, expect_not);
	}
}

TEST(StateOpsTest, AllInEveryLane)
{
	const size_t n = 100;
	for (State c0 : kStates)
		for (State c1 : kStates)
			for (State odd : kStates) {
				const bool odd_ok = odd == c0 || odd == c1;
				for (size_t lane = 0; lane < n; lane++) {
					std::vector<State> v(n, c0);
					v[lane] = odd;
					ASSERT_EQ(stateops::all_in(v.data(), n, c0, c1), odd_ok)
						<< "c0=" << int(c0) << " c1=" << int(c1) << " odd=" << int(odd) << " lane " << lane;
				}
			}
	ASSERT_TRUE(stateops::all_in(nullptr, 0, State::S0, State::S1));
}

// The public entry points agree with their former per-bit results, including
// the extension/truncation to result_len and the signed padding.
TEST(StateOpsTest, ConstEntryPoints)
{
	std::mt19937 rng(0xc0ffee);
	for (int rep = 0; rep < 300; rep++) {
		const int la = rng() % 150, lb = rng() % 150;
		RTLIL::Const a(State::S0, la), b(State::S0, lb);
		for (int i = 0; i < la; i++) a.set(i, kStates[rng() % 6]);
		for (int i = 0; i < lb; i++) b.set(i, kStates[rng() % 6]);
		const bool sa = rng() & 1, sb = rng() & 1;
		const int rl = (rng() % 3 == 0) ? -1 : int(rng() % 180);
		const int len = rl < 0 ? std::max(la, lb) : rl;
		auto ext = [&](const RTLIL::Const &c, bool s) {
			RTLIL::Const e = c;
			State pad = (e.size() > 0 && s) ? e.back() : State::S0;
			e.resize(len, pad);
			return e;
		};
		const RTLIL::Const ea = ext(a, sa), eb = ext(b, sb);
		struct { RTLIL::Const (*fn)(const RTLIL::Const &, const RTLIL::Const &, bool, bool, int); State (*ref)(State, State); } fns[] = {
			{RTLIL::const_and, ref_and}, {RTLIL::const_or, ref_or}, {RTLIL::const_xor, ref_xor}, {RTLIL::const_xnor, ref_xnor}};
		for (auto &f : fns) {
			RTLIL::Const got = f.fn(a, b, sa, sb, rl);
			ASSERT_EQ(got.size(), len);
			for (int i = 0; i < len; i++)
				ASSERT_EQ(got[i], f.ref(ea[i], eb[i])) << "bit " << i;
		}
		// const_not defaults result_len to the first operand's width, not the max
		const int nlen = rl < 0 ? la : rl;
		RTLIL::Const na = a;
		na.resize(nlen, (na.size() > 0 && sa) ? na.back() : State::S0);
		RTLIL::Const got = RTLIL::const_not(a, b, sa, sb, rl);
		ASSERT_EQ(got.size(), nlen);
		for (int i = 0; i < nlen; i++)
			ASSERT_EQ(got[i], ref_not(na[i])) << "not bit " << i;
		// predicates against the definitions
		bool z = true, o = true, d = true, u = true;
		for (int i = 0; i < la; i++) {
			z &= a[i] == State::S0;
			o &= a[i] == State::S1;
			d &= a[i] == State::S0 || a[i] == State::S1;
			u &= a[i] == State::Sx || a[i] == State::Sz;
		}
		ASSERT_EQ(a.is_fully_zero(), z);
		ASSERT_EQ(a.is_fully_ones(), o);
		ASSERT_EQ(a.is_fully_def(), d);
		ASSERT_EQ(a.is_fully_undef(), u);
		RTLIL::SigSpec sig(a);
		ASSERT_EQ(sig.is_fully_zero(), z);
		ASSERT_EQ(sig.is_fully_ones(), o);
		ASSERT_EQ(sig.is_fully_def(), d);
		ASSERT_EQ(sig.is_fully_undef(), u);
	}
}

// --- second sweep: reductions, equality, shifts, mux family, cube match ------

// Sequential per-bit fold, as logic_reduce_wrapper did it.
State ref_reduce(stateops::Op op, const std::vector<State> &v)
{
	State (*f)(State, State) = op == stateops::Op::And ? ref_and : op == stateops::Op::Or ? ref_or : ref_xor;
	State t = op == stateops::Op::And ? State::S1 : State::S0;
	for (State b : v) t = f(t, b);
	if (op == stateops::Op::Xnor) t = t == State::S0 ? State::S1 : t == State::S1 ? State::S0 : t;
	return t;
}
State ref_eq_status(const std::vector<State> &a, const std::vector<State> &b)
{
	State matched = State::S1;
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i] == State::S0 && b[i] == State::S1) return State::S0;
		if (a[i] == State::S1 && b[i] == State::S0) return State::S0;
		if (a[i] > State::S1 || b[i] > State::S1) matched = State::Sx;
	}
	return matched;
}
// The former const_shift_worker, BigInteger position per bit.
RTLIL::Const ref_shift_worker(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool sign_ext, bool signed2, int direction,
                              int result_len, State vacant_bits = State::S0)
{
	int undef_bit_pos = -1;
	BigUnsigned mag;
	BigInteger::Sign sign = BigInteger::positive;
	State inv_sign_bit = State::S1;
	int num_bits = arg2.size();
	if (signed2 && num_bits && arg2[num_bits - 1] == State::S1) {
		inv_sign_bit = State::S0;
		sign = BigInteger::negative;
		num_bits--;
	}
	for (int i = 0; i < num_bits; i++)
		if (arg2[i] == State::S0 || arg2[i] == State::S1)
			mag.setBit(i, arg2[i] == inv_sign_bit);
		else if (undef_bit_pos < 0)
			undef_bit_pos = i;
	if (sign == BigInteger::negative) mag += 1;
	BigInteger offset = BigInteger(mag, sign) * direction;
	if (result_len < 0) result_len = arg1.size();
	RTLIL::Const result(State::Sx, result_len);
	if (undef_bit_pos >= 0) return result;
	for (int i = 0; i < result_len; i++) {
		BigInteger pos = BigInteger(i) + offset;
		if (pos < 0)
			result.set(i, vacant_bits);
		else if (pos >= BigInteger(arg1.size()))
			result.set(i, sign_ext && !arg1.empty() ? arg1.back() : vacant_bits);
		else
			result.set(i, arg1[pos.toInt()]);
	}
	return result;
}

TEST(StateOpsTest, ReduceAndEqEveryLane)
{
	const size_t n = 100;
	for (State odd : kStates) {
		for (State base : {State::S0, State::S1}) {
			for (size_t lane = 0; lane < n; lane++) {
				std::vector<State> v(n, base);
				v[lane] = odd;
				for (stateops::Op op : {stateops::Op::And, stateops::Op::Or, stateops::Op::Xor, stateops::Op::Xnor})
					ASSERT_EQ(stateops::reduce(op, v.data(), n), ref_reduce(op, v)) << "op " << int(op) << " lane " << lane;
				std::vector<State> w(n, base);
				for (State other : kStates) {
					w[lane] = other;
					ASSERT_EQ(stateops::eq_status(v.data(), w.data(), n), ref_eq_status(v, w)) << "lane " << lane;
					ASSERT_EQ(stateops::cubes_match(v.data(), w.data(), n),
					          !(v[lane] <= State::S1 && w[lane] <= State::S1 && v[lane] != w[lane]) || false)
						<< "cube lane " << lane;
				}
			}
		}
	}
	// parity across block boundaries: 1..300 ones
	for (size_t ones = 0; ones < 300; ones++) {
		std::vector<State> v(300, State::S0);
		for (size_t i = 0; i < ones; i++) v[i * 7 % 300] = State::S1;
		ASSERT_EQ(stateops::reduce(stateops::Op::Xor, v.data(), 300), ref_reduce(stateops::Op::Xor, v));
	}
}

TEST(StateOpsTest, ShiftWorkerAgainstBigIntegerReference)
{
	std::mt19937 rng(0x5417);
	for (int rep = 0; rep < 2000; rep++) {
		const int la = rng() % 120;
		RTLIL::Const a(State::S0, la);
		for (int i = 0; i < la; i++) a.set(i, kStates[rng() % 4]);
		// shift amounts: small, around the width, huge (does not fit a word), undefined
		RTLIL::Const b;
		switch (rng() % 4) {
		case 0: b = RTLIL::Const(int(rng() % 140), 8 + rng() % 8); break;
		case 1: b = RTLIL::Const(int(rng() % 8), 3); break;
		case 2: {
			b = RTLIL::Const(State::S0, 70);
			b.set(69, State::S1);  // 2^69 (or very negative when signed)
			if (rng() & 1) b.set(rng() % 69, State::S1);
			break;
		}
		default:
			b = RTLIL::Const(int(rng() % 140), 10);
			b.set(rng() % 10, kStates[2 + rng() % 4]);  // an undefined select bit
			break;
		}
		const bool signed2 = rng() & 1, sign_ext = rng() & 1;
		const int direction = (rng() & 1) ? 1 : -1;
		// const_shl/shr/shift extend to result_len before the worker, so the public
		// contract is result_len >= 0 (a negative one asserts in Const::resize).
		const int result_len = int(rng() % 150);
		const State vacant = (rng() & 1) ? State::S0 : State::Sx;
		// public entry points cover the worker; compare each against the reference
		struct { RTLIL::Const (*fn)(const RTLIL::Const &, const RTLIL::Const &, bool, bool, int); } fns[] = {
			{RTLIL::const_shl}, {RTLIL::const_shr}, {RTLIL::const_sshl}, {RTLIL::const_sshr}, {RTLIL::const_shift}, {RTLIL::const_shiftx}};
		const bool s1 = rng() & 1;
		const int rl = result_len < 0 ? la : result_len;
		auto ext = [&](const RTLIL::Const &c, int w, bool s) { RTLIL::Const e = c; e.resize(w, (e.size() > 0 && s) ? e.back() : State::S0); return e; };
		RTLIL::Const exp[6] = {
			ref_shift_worker(ext(a, rl, s1), b, false, false, -1, rl),
			ref_shift_worker(ext(a, std::max(rl, la), s1), b, false, false, +1, rl),
			ref_shift_worker(a, b, s1, false, -1, rl),
			ref_shift_worker(a, b, s1, false, +1, rl),
			ref_shift_worker(ext(a, std::max(rl, la), s1), b, false, signed2, +1, rl),
			ref_shift_worker(a, b, false, signed2, +1, rl, State::Sx)};
		for (int k = 0; k < 6; k++) {
			RTLIL::Const got = fns[k].fn(a, b, s1, signed2, result_len);
			ASSERT_EQ(got.size(), exp[k].size()) << "fn " << k << " rep " << rep;
			for (int i = 0; i < got.size(); i++)
				ASSERT_EQ(got[i], exp[k][i]) << "fn " << k << " rep " << rep << " bit " << i;
		}
		(void)sign_ext; (void)direction; (void)vacant;
	}
}

TEST(StateOpsTest, MuxFamilyAndCubes)
{
	std::mt19937 rng(0x3a3a);
	for (int rep = 0; rep < 300; rep++) {
		const int n = rng() % 200;
		RTLIL::Const a(State::S0, n), b(State::S0, n), s(State::S0, n);
		for (int i = 0; i < n; i++) { a.set(i, kStates[rng() % 6]); b.set(i, kStates[rng() % 6]); s.set(i, kStates[rng() % 6]); }
		RTLIL::Const mux = RTLIL::const_mux(a, b, RTLIL::Const(State::Sx, 1));
		RTLIL::Const weq = RTLIL::const_bweqx(a, b);
		RTLIL::Const wmux = RTLIL::const_bwmux(a, b, s);
		ASSERT_EQ(mux.size(), n);
		for (int i = 0; i < n; i++) {
			ASSERT_EQ(mux[i], a[i] == b[i] ? a[i] : State::Sx) << "mux bit " << i;
			ASSERT_EQ(weq[i], a[i] == b[i] ? State::S1 : State::S0) << "bweqx bit " << i;
			State ref = State::Sx;
			if (s[i] != State::Sx || a[i] == b[i]) ref = s[i] == State::S1 ? b[i] : a[i];
			ASSERT_EQ(wmux[i], ref) << "bwmux bit " << i;
		}
		// $eq / $ne / $eqx / $nex entry points
		const bool sa = rng() & 1, sb = rng() & 1;
		const int w = std::max(a.size(), b.size());
		auto ext = [&](const RTLIL::Const &c) { RTLIL::Const e = c; e.resize(w, (e.size() > 0 && (sa && sb)) ? e.back() : State::S0); return e; };
		const State eq = ref_eq_status(ext(a).to_bits(), ext(b).to_bits());
		ASSERT_EQ(RTLIL::const_eq(a, b, sa, sb, 1)[0], eq);
		const State ne = eq == State::S0 ? State::S1 : eq == State::S1 ? State::S0 : eq;
		ASSERT_EQ(RTLIL::const_ne(a, b, sa, sb, 1)[0], ne);
		ASSERT_EQ(RTLIL::const_eqx(a, b, sa, sb, 1)[0], ext(a).to_bits() == ext(b).to_bits() ? State::S1 : State::S0);
		// BitPatternPool::match through the pool API: the full pool intersects any
		// two-state cube; after taking that cube out, the cube itself is gone.
		if (n > 0) {
			RTLIL::Const two(State::S0, n);
			for (int i = 0; i < n; i++) two.set(i, (rng() & 1) ? State::S1 : State::S0);
			BitPatternPool pool(n);
			RTLIL::SigSpec sig(two);
			ASSERT_TRUE(pool.has_any(sig));
			ASSERT_TRUE(pool.has_all(sig));
			ASSERT_TRUE(pool.take(sig));
			ASSERT_FALSE(pool.has_any(sig));
		}
	}
}

} // namespace

YOSYS_NAMESPACE_END
