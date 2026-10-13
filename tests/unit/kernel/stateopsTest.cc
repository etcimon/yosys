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

#include "kernel/rtlil.h"
#include "kernel/stateops.h"

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

} // namespace

YOSYS_NAMESPACE_END
