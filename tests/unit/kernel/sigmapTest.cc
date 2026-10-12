#include <gtest/gtest.h>

#include "kernel/log.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"

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

YOSYS_NAMESPACE_END
