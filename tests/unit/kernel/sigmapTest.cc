#include <gtest/gtest.h>

#include "kernel/log.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"

#include <random>

YOSYS_NAMESPACE_BEGIN

class SigmapAndCheckTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		IdString::ensure_prepopulated();
		if (logger().empty())
			logger().add_sink<ConsoleLogSink>();
	}
};

TEST_F(SigmapAndCheckTest, UnaliasedChunkStaysCanonical)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *wide = mod->addWire("\\wide", 64);
	Wire *other = mod->addWire("\\other", 64);
	SigMap map(mod);

	SigSpec spec(wide);
	map.apply(spec);
	EXPECT_TRUE(spec.is_wire());
	EXPECT_EQ(spec, SigSpec(wide));

	mod->connect(wide, other);
	SigMap aliased(mod);
	SigBit kept = aliased(SigBit(wide, 63));
	EXPECT_EQ(kept, SigBit(other, 63));
	SigBit low = aliased(SigBit(wide, 0));
	EXPECT_EQ(low, SigBit(other, 0));
}

TEST_F(SigmapAndCheckTest, PartialAliasDoesNotRewriteOtherBits)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *a = mod->addWire("\\a", 4);
	Wire *b = mod->addWire("\\b", 4);
	mod->connect(SigSpec(a, 1, 1), SigSpec(b, 1, 1));
	SigMap map(mod);

	EXPECT_EQ(map(SigBit(a, 1)), SigBit(b, 1));
	EXPECT_EQ(map(SigBit(a, 0)), SigBit(a, 0));
	EXPECT_EQ(map(SigBit(a, 2)), SigBit(a, 2));
	// A partial alias is not one chunk, so the whole wire is not rewritten.
	SigSpec whole(a);
	map.apply(whole);
	EXPECT_EQ(whole[0], SigBit(a, 0));
	EXPECT_EQ(whole[1], SigBit(b, 1));
}

TEST_F(SigmapAndCheckTest, FullWireAliasIsOneChunk)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *a = mod->addWire("\\a", 32);
	Wire *b = mod->addWire("\\b", 32);
	Wire *c = mod->addWire("\\c", 32);
	mod->connect(a, b);
	mod->connect(b, c);
	SigMap map(mod);

	SigSpec image(a);
	map.apply(image);
	EXPECT_EQ(image, SigSpec(c));
	EXPECT_TRUE(image.is_wire());

	SigSpec slice(a, 4, 8);
	map.apply(slice);
	EXPECT_EQ(slice, SigSpec(c, 4, 8));

	Const eight(0x5a, 8);
	Wire *k = mod->addWire("\\k", 8);
	mod->connect(k, eight);
	SigMap consts(mod);
	SigSpec kimage(k);
	consts.apply(kimage);
	EXPECT_EQ(kimage, SigSpec(eight));
}

TEST_F(SigmapAndCheckTest, AliasCycleAndCutResolveLikeBits)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *a = mod->addWire("\\a", 4);
	Wire *b = mod->addWire("\\b", 4);
	mod->connect(a, b);
	mod->connect(b, a);
	SigMap cycled(mod);
	// The first connection makes b the representative. Joining b back to a
	// is the same set, so b stays the representative.
	EXPECT_EQ(cycled(SigBit(a, 2)), SigBit(b, 2));
	EXPECT_EQ(cycled(SigBit(b, 2)), SigBit(b, 2));

	Design cut;
	Module *cutm = cut.addModule("\\cut");
	Wire *p = cutm->addWire("\\p", 4);
	Wire *q = cutm->addWire("\\q", 4);
	Wire *r = cutm->addWire("\\r");
	cutm->connect(p, q);
	cutm->connect(SigBit(p, 1), r);
	SigMap mapped(cutm);
	EXPECT_EQ(mapped(SigBit(p, 0)), SigBit(q, 0));
	EXPECT_EQ(mapped(SigBit(p, 1)), SigBit(r, 0));
	EXPECT_EQ(mapped(SigBit(q, 1)), SigBit(r, 0));
	EXPECT_EQ(mapped(SigBit(p, 3)), SigBit(q, 3));
}

// wreduce ties the unused high bits of an output through a temporary:
//   connect x temp; connect temp[9:2] 0; connect add_y temp[1:0]
// opt_clean then promotes the low bits onto the output. The high bits must
// stay constant, including through a whole-wire apply.
TEST_F(SigmapAndCheckTest, ConstTailStaysConstWhenLowBitsPromoted)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *x = mod->addWire("\\x", 10);
	Wire *temp = mod->addWire("\\temp", 10);
	Wire *add_y = mod->addWire("\\add_y", 2);
	mod->connect(x, temp);
	mod->connect(SigSpec(temp, 2, 8), SigSpec(State::S0, 8));
	mod->connect(add_y, SigSpec(temp, 0, 2));
	SigMap map(mod);

	SigSpec image(x);
	map.apply(image);
	for (int i = 0; i < 2; i++)
		EXPECT_EQ(image[i], SigBit(temp, i));
	for (int i = 2; i < 10; i++) {
		EXPECT_EQ(image[i], SigBit(State::S0));
		EXPECT_EQ(map(SigBit(temp, i)), SigBit(State::S0));
	}
	EXPECT_EQ(map(SigBit(add_y, 0)), SigBit(temp, 0));
	EXPECT_EQ(map(SigBit(add_y, 1)), SigBit(temp, 1));

	for (int i = 0; i < 2; i++)
		map.add(SigBit(x, i));
	for (int i = 2; i < 10; i++)
		map.add(SigBit(x, i));

	SigSpec after(x);
	map.apply(after);
	for (int i = 0; i < 2; i++)
		EXPECT_EQ(after[i], SigBit(x, i));
	for (int i = 2; i < 10; i++)
		EXPECT_EQ(after[i], SigBit(State::S0));
	EXPECT_EQ(map(SigBit(temp, 0)), SigBit(x, 0));
	EXPECT_EQ(map(SigBit(temp, 9)), SigBit(State::S0));
	EXPECT_EQ(map(SigBit(add_y, 1)), SigBit(x, 1));
}

// A high slice tied to another wire has to survive promote of that slice.
// Promoting inside the union-find alone would leave the output bit as itself.
TEST_F(SigmapAndCheckTest, SliceAliasSurvivesPromote)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *x = mod->addWire("\\x", 4);
	Wire *temp = mod->addWire("\\temp", 4);
	Wire *other = mod->addWire("\\other", 4);
	mod->connect(x, temp);
	mod->connect(SigSpec(temp, 2, 2), SigSpec(other, 2, 2));
	SigMap map(mod);

	EXPECT_EQ(map(SigBit(x, 0)), SigBit(temp, 0));
	EXPECT_EQ(map(SigBit(x, 2)), SigBit(other, 2));
	EXPECT_EQ(map(SigBit(temp, 3)), SigBit(other, 3));

	map.add(SigBit(x, 2));
	map.add(SigBit(x, 3));
	EXPECT_EQ(map(SigBit(x, 2)), SigBit(other, 2));
	EXPECT_EQ(map(SigBit(temp, 2)), SigBit(other, 2));
	EXPECT_EQ(map(SigBit(other, 2)), SigBit(other, 2));

	map.add(SigBit(x, 0));
	EXPECT_EQ(map(SigBit(x, 0)), SigBit(x, 0));
	EXPECT_EQ(map(SigBit(temp, 0)), SigBit(x, 0));
}

// opt_clean used to add one bit at a time. Adjacent bits of one wire must
// stay one alias, and a whole-wire apply must still name the target wire.
TEST_F(SigmapAndCheckTest, BitAtATimeAliasMatchesWholeWire)
{
	Design design;
	Module *mod = design.addModule("\\m");
	Wire *a = mod->addWire("\\a", 64);
	Wire *b = mod->addWire("\\b", 64);
	SigMap bits;
	for (int i = 0; i < 64; i++)
		bits.add(SigBit(a, i), SigBit(b, i));
	SigMap whole(mod);
	mod->connect(a, b);
	whole.set(mod);

	for (int i = 0; i < 64; i++)
		EXPECT_EQ(bits(SigBit(a, i)), whole(SigBit(a, i)));
	SigSpec spec(a);
	bits.apply(spec);
	for (int i = 0; i < 64; i++)
		EXPECT_EQ(spec[i], SigBit(b, i));
}

// The span walk of apply(SigSpec&) must return, bit for bit, what the per-bit
// loop of apply(SigBit&) returns, on random mixes of whole-wire, partial,
// reversed, constant and self-referencing connections, including chains deep
// enough to cross the span hop budget.
TEST_F(SigmapAndCheckTest, SpanWalkMatchesPerBitLoop)
{
	std::mt19937 rng(0x51b);
	for (int rep = 0; rep < 60; rep++) {
		Design design;
		Module *mod = design.addModule("\\m");
		const int n = 6 + rng() % 40;
		const int width = 1 + rng() % 70;
		std::vector<Wire *> wires;
		for (int i = 0; i < n; i++)
			wires.push_back(mod->addWire(stringf("\\w%d", i), width));
		const int conns = rng() % (3 * n);
		for (int c = 0; c < conns; c++) {
			Wire *dst = wires[rng() % n];
			Wire *src = wires[rng() % n];
			switch (rng() % 6) {
			case 0: mod->connect(dst, src); break;
			case 1: {
				const int off = rng() % width, w = 1 + rng() % (width - off);
				const int soff = rng() % (width - w + 1);
				mod->connect(SigSpec(dst, off, w), SigSpec(src, soff, w));
				break;
			}
			case 2: {
				const int off = rng() % width;
				mod->connect(SigSpec(dst, off, 1), SigSpec((rng() & 1) ? State::S1 : State::Sx));
				break;
			}
			case 3: {
				// two chunks from different wires onto one range
				const int w = 2 + rng() % std::max(1, width - 2);
				if (w > width) break;
				const int off = rng() % (width - w + 1);
				SigSpec rhs;
				rhs.append(SigSpec(src, 0, w / 2));
				rhs.append(SigSpec(wires[rng() % n], 0, w - w / 2));
				mod->connect(SigSpec(dst, off, w), rhs);
				break;
			}
			case 4: {
				// deep chain: every wire's low half from the previous one
				for (int i = 1; i < n; i++)
					mod->connect(SigSpec(wires[i], 0, width / 2), SigSpec(wires[i - 1], width - width / 2, width / 2));
				break;
			}
			default:
				// self-referencing partial connection
				if (width > 1) mod->connect(SigSpec(dst, 1, width - 1), SigSpec(dst, 0, width - 1));
				break;
			}
		}
		SigMap map(mod);
		for (int q = 0; q < 40; q++) {
			SigSpec sig;
			const int pieces = 1 + rng() % 4;
			for (int p = 0; p < pieces; p++) {
				Wire *w = wires[rng() % n];
				const int off = rng() % width;
				sig.append(SigSpec(w, off, 1 + rng() % (width - off)));
				if (rng() % 5 == 0) sig.append(SigSpec(State::S0, 1 + rng() % 3));
			}
			SigSpec spec = sig;
			map.apply(spec);
			ASSERT_EQ(spec.size(), sig.size());
			for (int i = 0; i < sig.size(); i++) {
				SigBit bit = sig[i];
				map.apply(bit);
				if (spec[i] != bit) {
					std::string trace;
					SigBit hopbit = sig[i];
					for (int hop = 0; hop < 8; hop++) {
						SigBit next = map.resolve_step(hopbit);
						trace += stringf(" -> %s", log_signal(next));
						if (next == hopbit) break;
						hopbit = next;
					}
					for (auto &it : map.alias_ranges)
						for (auto &r : it.second)
							trace += stringf(" | alias %s[%d+%d] => %s", log_id(it.first), r.offset, r.width, log_signal(r.image));
					ADD_FAILURE() << "rep " << rep << " query " << q << " bit " << i << " sig " << log_signal(sig)
						<< " got " << log_signal(spec[i]) << " want " << log_signal(bit) << " trace" << trace;
					return;
				}
			}
		}
	}
}

YOSYS_NAMESPACE_END
