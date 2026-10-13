/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
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

#ifndef SIGTOOLS_H
#define SIGTOOLS_H

#include "kernel/yosys.h"

YOSYS_NAMESPACE_BEGIN

struct SigPool
{
	struct bitDef_t : public std::pair<RTLIL::Wire*, int> {
		bitDef_t() : std::pair<RTLIL::Wire*, int>(NULL, 0) { }
		bitDef_t(const RTLIL::SigBit &bit) : std::pair<RTLIL::Wire*, int>(bit.wire, bit.offset) { }
		[[nodiscard]] Hasher hash_into(Hasher h) const {
			h.eat(first->name);
			h.eat(second);
			return h;
		}
	};

	pool<bitDef_t> bits;

	void clear()
	{
		bits.clear();
	}

	void add(const RTLIL::SigSpec &sig)
	{
		for (auto &bit : sig)
			if (bit.wire != NULL)
				bits.insert(bit);
	}

	void add(const SigPool &other)
	{
		for (auto &bit : other.bits)
			bits.insert(bit);
	}

	void del(const RTLIL::SigSpec &sig)
	{
		for (auto &bit : sig)
			if (bit.wire != NULL)
				bits.erase(bit);
	}

	void del(const SigPool &other)
	{
		for (auto &bit : other.bits)
			bits.erase(bit);
	}

	void expand(const RTLIL::SigSpec &from, const RTLIL::SigSpec &to)
	{
		log_assert(GetSize(from) == GetSize(to));
		for (int i = 0; i < GetSize(from); i++) {
			bitDef_t bit_from(from[i]), bit_to(to[i]);
			if (bit_from.first != NULL && bit_to.first != NULL && bits.count(bit_from) > 0)
				bits.insert(bit_to);
		}
	}

	RTLIL::SigSpec extract(const RTLIL::SigSpec &sig) const
	{
		RTLIL::SigSpec result;
		for (auto &bit : sig)
			if (bit.wire != NULL && bits.count(bit))
				result.append(bit);
		return result;
	}

	RTLIL::SigSpec remove(const RTLIL::SigSpec &sig) const
	{
		RTLIL::SigSpec result;
		for (auto &bit : sig)
			if (bit.wire != NULL && bits.count(bit) == 0)
				result.append(bit);
		return result;
	}

	bool check(const RTLIL::SigBit &bit) const
	{
		return bit.wire != NULL && bits.count(bit);
	}

	bool check_any(const RTLIL::SigSpec &sig) const
	{
		for (auto &bit : sig)
			if (bit.wire != NULL && bits.count(bit))
				return true;
		return false;
	}

	bool check_all(const RTLIL::SigSpec &sig) const
	{
		for (auto &bit : sig)
			if (bit.wire != NULL && bits.count(bit) == 0)
				return false;
		return true;
	}

	RTLIL::SigSpec export_one() const
	{
		for (auto &bit : bits)
			return RTLIL::SigSpec(bit.first, bit.second);
		return RTLIL::SigSpec();
	}

	RTLIL::SigSpec export_all() const
	{
		pool<RTLIL::SigBit> sig;
		for (auto &bit : bits)
			sig.insert(RTLIL::SigBit(bit.first, bit.second));
		return sig;
	}

	size_t size() const
	{
		return bits.size();
	}
};

template <typename T, class Compare = void>
struct SigSet
{
	static_assert(!std::is_same<Compare,void>::value, "Default value for `Compare' class not found for SigSet<T>. Please specify.");

	struct bitDef_t : public std::pair<RTLIL::Wire*, int> {
		bitDef_t() : std::pair<RTLIL::Wire*, int>(NULL, 0) { }
		bitDef_t(const RTLIL::SigBit &bit) : std::pair<RTLIL::Wire*, int>(bit.wire, bit.offset) { }
		[[nodiscard]] Hasher hash_into(Hasher h) const {
			h.eat(first->name);
			h.eat(second);
			return h;
		}
	};

	dict<bitDef_t, std::set<T, Compare>> bits;

	void clear()
	{
		bits.clear();
	}

	void insert(const RTLIL::SigSpec &sig, T data)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL)
				bits[bit].insert(data);
	}

	void insert(const RTLIL::SigSpec& sig, const std::set<T> &data)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL)
				bits[bit].insert(data.begin(), data.end());
	}

	void erase(const RTLIL::SigSpec& sig)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL)
				bits[bit].clear();
	}

	void erase(const RTLIL::SigSpec &sig, T data)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL)
				bits[bit].erase(data);
	}

	void erase(const RTLIL::SigSpec &sig, const std::set<T> &data)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL)
				bits[bit].erase(data.begin(), data.end());
	}

	void find(const RTLIL::SigSpec &sig, std::set<T> &result)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL) {
				auto &data = bits[bit];
				result.insert(data.begin(), data.end());
			}
	}

	void find(const RTLIL::SigSpec &sig, pool<T> &result)
	{
		for (const auto &bit : sig)
			if (bit.wire != NULL) {
				auto &data = bits[bit];
				result.insert(data.begin(), data.end());
			}
	}

	std::set<T> find(const RTLIL::SigSpec &sig)
	{
		std::set<T> result;
		find(sig, result);
		return result;
	}

	bool has(const RTLIL::SigSpec &sig)
	{
		for (auto &bit : sig)
			if (bit.wire != NULL && bits.count(bit))
				return true;
		return false;
	}
};

template<typename T>
class SigSet<T, typename std::enable_if<!std::is_pointer<T>::value>::type> : public SigSet<T, std::less<T>> {};
template<typename T>
using sort_by_name_id_guard = typename std::enable_if<std::is_same<T,RTLIL::Cell*>::value>::type;
template<typename T>
class SigSet<T, sort_by_name_id_guard<T>> : public SigSet<T, RTLIL::sort_by_name_id<typename std::remove_pointer<T>::type>> {};

/**
 * Key store for the SigMap union-find, replacing idict<SigBit>.
 *
 * The profile of proc/opt_clean on the LibreCore cores is one hash lookup per bit
 * per hop: hashing (wire, offset), one random access into the bucket table, a
 * chain walk, then the entry. A wire's bits are consecutive integers, so the
 * index is kept as a dense per-wire array instead: looking a bit up is one wire
 * lookup plus an array read, and all bits of a chunk share that one lookup
 * (see SigMapView::apply). Indices are assigned in insertion order exactly as
 * idict's were, so iteration and every index-based caller behave the same;
 * constant bits (wire == nullptr) keep a small hash of their own.
 */
struct SigBitIdict
{
	std::vector<RTLIL::SigBit> keys;
	dict<RTLIL::Wire *, std::vector<int>> by_wire;
	dict<RTLIL::SigBit, int> consts;

	class const_iterator
	{
		friend struct SigBitIdict;
	protected:
		const SigBitIdict *container;
		int index;
		const_iterator(const SigBitIdict *container, int index) : container(container), index(index) { }
	public:
		typedef std::forward_iterator_tag iterator_category;
		typedef RTLIL::SigBit value_type;
		typedef ptrdiff_t difference_type;
		typedef RTLIL::SigBit *pointer;
		typedef RTLIL::SigBit &reference;
		const_iterator() : container(nullptr), index(0) { }
		const_iterator operator++() { index++; return *this; }
		bool operator==(const const_iterator &other) const { return index == other.index; }
		bool operator!=(const const_iterator &other) const { return index != other.index; }
		const RTLIL::SigBit &operator*() const { return container->keys[index]; }
		const RTLIL::SigBit *operator->() const { return &container->keys[index]; }
	};

	// Per-wire slot array, created on the first bit of that wire.
	std::vector<int> &slots(RTLIL::Wire *wire)
	{
		auto it = by_wire.find(wire);
		if (it != by_wire.end())
			return it->second;
		return by_wire.emplace(wire, std::vector<int>(wire->width, -1)).first->second;
	}

	// Index of key, inserting it as the next index when absent.
	int operator()(const RTLIL::SigBit &key)
	{
		if (key.wire != nullptr) {
			log_assert(key.offset >= 0 && key.offset < key.wire->width);
			int &i = slots(key.wire)[key.offset];
			if (i < 0) {
				i = GetSize(keys);
				keys.push_back(key);
			}
			return i;
		}
		auto it = consts.find(key);
		if (it != consts.end())
			return it->second;
		consts.emplace(key, GetSize(keys));
		keys.push_back(key);
		return GetSize(keys) - 1;
	}

	int at(const RTLIL::SigBit &key, int defval) const
	{
		if (key.wire != nullptr) {
			auto it = by_wire.find(key.wire);
			if (it == by_wire.end())
				return defval;
			const int i = it->second[key.offset];
			return i < 0 ? defval : i;
		}
		auto it = consts.find(key);
		return it == consts.end() ? defval : it->second;
	}

	int at(const RTLIL::SigBit &key) const
	{
		const int i = at(key, -1);
		if (i < 0)
			throw std::out_of_range("SigBitIdict::at()");
		return i;
	}

	int lookup(const RTLIL::SigBit &key) const { return at(key, -1); }
	int count(const RTLIL::SigBit &key) const { return at(key, -1) < 0 ? 0 : 1; }

	// The slot array of a wire, or nullptr when none of its bits is stored.
	// Entries are -1 for bits that are not stored.
	const int *indices(RTLIL::Wire *wire) const
	{
		auto it = by_wire.find(wire);
		return it == by_wire.end() ? nullptr : it->second.data();
	}

	const RTLIL::SigBit &operator[](int index) const { return keys.at(index); }

	void swap(SigBitIdict &other)
	{
		keys.swap(other.keys);
		by_wire.swap(other.by_wire);
		consts.swap(other.consts);
	}
	void reserve(size_t n) { keys.reserve(n); }
	size_t size() const { return keys.size(); }
	bool empty() const { return keys.empty(); }
	void clear() { keys.clear(); by_wire.clear(); consts.clear(); }

	const_iterator begin() const { return const_iterator(this, 0); }
	const_iterator element(int n) const { return const_iterator(this, n); }
	const_iterator end() const { return const_iterator(this, GetSize(keys)); }
};

struct SigMapView
{
	// One contiguous alias. Bit (offset + i) maps to image bit i.
	struct AliasRange {
		int offset;
		int width;
		RTLIL::SigChunk image;
	};

	mfp<SigBit, hash_ops<SigBit>, SigBitIdict> database;
	// Wires with at least one bit stored in the union-find. An unaliased
	// chunk is already canonical, so apply() must not unpack it.
	pool<RTLIL::Wire *> touched_wires;
	// Wires that have individual bits in the union-find. A wire that only
	// has AliasRange entries is not in this set.
	pool<RTLIL::Wire *> bit_wires;
	// Source wire -> non-overlapping aliases, ordered by offset.
	dict<RTLIL::Wire *, std::vector<AliasRange>> alias_ranges;
	// Image wire -> (source wire, source offset) of ranges that point at it.
	dict<RTLIL::Wire *, std::vector<std::pair<RTLIL::Wire *, int>>> image_users;
	// Image of a touched wire whose bits map onto one chunk. Rebuilt on the
	// writing thread before any parallel reader of this map.
	dict<RTLIL::Wire *, RTLIL::SigChunk> canon_chunk;
	bool canon_valid = false;

	void touch_bit(const RTLIL::SigBit &bit)
	{
		if (bit.wire != nullptr)
			touched_wires.insert(bit.wire);
	}

	void invalidate_canon()
	{
		canon_chunk.clear();
		canon_valid = false;
	}

	void clear_aliases()
	{
		alias_ranges.clear();
		image_users.clear();
		bit_wires.clear();
	}

	// Ranges on one wire are sorted by offset and do not overlap.
	const AliasRange *covering_alias(const std::vector<AliasRange> &ranges, int offset, int width) const
	{
		if (ranges.empty() || width <= 0)
			return nullptr;
		auto it = std::upper_bound(ranges.begin(), ranges.end(), offset,
				[](int off, const AliasRange &range) { return off < range.offset; });
		if (it == ranges.begin())
			return nullptr;
		--it;
		if (it->offset <= offset && offset + width <= it->offset + it->width)
			return &*it;
		return nullptr;
	}

	const AliasRange *find_alias(RTLIL::Wire *wire, int offset) const
	{
		auto found = alias_ranges.find(wire);
		if (found == alias_ranges.end())
			return nullptr;
		return covering_alias(found->second, offset, 1);
	}

	bool alias_overlaps(RTLIL::Wire *wire, int offset, int width) const
	{
		auto found = alias_ranges.find(wire);
		if (found == alias_ranges.end() || width <= 0)
			return false;
		const std::vector<AliasRange> &ranges = found->second;
		auto it = std::upper_bound(ranges.begin(), ranges.end(), offset,
				[](int off, const AliasRange &range) { return off < range.offset; });
		if (it != ranges.begin()) {
			auto prev = it;
			--prev;
			if (offset < prev->offset + prev->width && prev->offset < offset + width)
				return true;
		}
		for (; it != ranges.end() && it->offset < offset + width; ++it)
			if (offset < it->offset + it->width)
				return true;
		return false;
	}

	static RTLIL::SigBit alias_bit(const AliasRange &range, int offset)
	{
		const int delta = offset - range.offset;
		if (range.image.wire == nullptr)
			return RTLIL::SigBit(range.image.data[delta]);
		return RTLIL::SigBit(range.image.wire, range.image.offset + delta);
	}

	RTLIL::SigChunk compose_image(RTLIL::SigChunk image) const
	{
		for (int hop = 0; hop < 32 && image.wire != nullptr; hop++) {
			auto found = alias_ranges.find(image.wire);
			if (found == alias_ranges.end())
				break;
			const AliasRange *cover = covering_alias(found->second, image.offset, image.width);
			if (cover == nullptr)
				break;
			image = cover->image.extract(image.offset - cover->offset, image.width);
		}
		return image;
	}

	// Record one wire when every bit maps onto one chunk. A wire that already
	// has an entry is left alone.
	void cache_uniform_wire(RTLIL::Wire *wire)
	{
		if (wire == nullptr || wire->width <= 1 || canon_chunk.count(wire))
			return;
		SigBit first(wire, 0);
		apply(first);
		const bool as_wire = first.wire != nullptr;
		std::vector<RTLIL::State> consts;
		if (!as_wire)
			consts.push_back(first.data);
		bool uniform = true;
		for (int i = 1; i < wire->width; i++) {
			SigBit bit(wire, i);
			apply(bit);
			if (as_wire) {
				if (bit.wire != first.wire || bit.offset != first.offset + i) {
					uniform = false;
					break;
				}
			} else if (bit.wire != nullptr) {
				uniform = false;
				break;
			} else {
				consts.push_back(bit.data);
			}
		}
		if (!uniform)
			return;
		if (as_wire)
			canon_chunk.emplace(wire, RTLIL::SigChunk(first.wire, first.offset, wire->width));
		else
			canon_chunk.emplace(wire, RTLIL::SigChunk(RTLIL::Const(std::move(consts))));
	}

	// One entry per touched wire that still maps as a single chunk.
	// A full-wire alias is stored without walking its bits.
	void rebuild_canon()
	{
		if (canon_valid)
			return;
		canon_chunk.clear();
		for (auto &it : alias_ranges) {
			RTLIL::Wire *wire = it.first;
			if (wire == nullptr || wire->width <= 1)
				continue;
			const std::vector<AliasRange> &ranges = it.second;
			if (ranges.size() != 1)
				continue;
			const AliasRange &range = ranges[0];
			if (range.offset != 0 || range.width != wire->width)
				continue;
			RTLIL::SigChunk image = compose_image(range.image);
			if (image.width != wire->width)
				continue;
			if (image.wire != nullptr && (bit_wires.count(image.wire) ||
					alias_overlaps(image.wire, image.offset, image.width)))
				continue;
			canon_chunk.emplace(wire, image);
		}
		for (RTLIL::Wire *wire : touched_wires)
			cache_uniform_wire(wire);
		canon_valid = true;
	}

	// One step of either the chunk alias or the per-bit union-find.
	// A constant tie-off can sit on the union-find root's wire, so callers
	// must keep stepping: stopping at the root drops that constant.
	RTLIL::SigBit resolve_step(const RTLIL::SigBit &bit) const
	{
		if (!alias_ranges.empty()) {
			if (const AliasRange *range = find_alias(bit.wire, bit.offset))
				return alias_bit(*range, bit.offset);
		}
		return database.find(bit);
	}

	// Hops 'hop'.. of the resolution loop below, shared with the chunk walker.
	void apply_from(RTLIL::SigBit &bit, const RTLIL::SigBit &origin, int hop) const
	{
		for (; hop < 4096 && bit.wire != nullptr; hop++) {
			RTLIL::SigBit next = resolve_step(bit);
			if (next == bit)
				break;
			bit = next;
			if (!bit.wire || bit == origin)
				break;
		}
	}

	// Modify bit to its representative
	void apply(RTLIL::SigBit &bit) const
	{
		if (bit.wire != nullptr && !touched_wires.count(bit.wire))
			return;
		const RTLIL::SigBit origin = bit;
		apply_from(bit, origin, 0);
	}

	// Every bit of a touched chunk, appended to 'out', bit-for-bit what
	// apply(SigBit&) returns.
	//
	// The per-bit loop hops through the alias ranges one bit at a time, and a
	// chain of partial connections (w3 <- w2 <- w1 <- in, each a chunk alias)
	// made every bit of a wide port pay the whole chain: one dict lookup and
	// one binary search per hop per bit, with nothing to compress. Bits that one
	// alias covers all land in one span of its image, so this walks spans: each
	// hop is taken once for a run of bits, and a span that no alias covers takes
	// the union-find step per bit with the wire's slot array looked up once.
	// Spans are a worklist writing into their output positions, so a deep chain
	// costs no stack. What the walk cannot reproduce exactly (an image back onto
	// the origin wire, where the per-bit loop's cycle rule decides, or its hop
	// budget) is replayed per bit for exactly those bits.
	void apply_chunk(const RTLIL::SigChunk &chunk, std::vector<RTLIL::SigBit> &out) const
	{
		struct Span {
			RTLIL::Wire *wire;
			int offset;
			int width;
			int pos;    // first output index
			int depth;  // hop number of these bits
		};
		const int start = GetSize(out);
		if (chunk.width == 1) {
			RTLIL::SigBit bit(chunk.wire, chunk.offset);
			apply_from(bit, RTLIL::SigBit(chunk.wire, chunk.offset), 0);
			out.push_back(bit);
			return;
		}
		out.resize(start + chunk.width);
		std::vector<Span> work;
		work.push_back(Span{chunk.wire, chunk.offset, chunk.width, start, 0});
		while (!work.empty()) {
			const Span span = work.back();
			work.pop_back();
			if (span.depth >= 4096 || (span.wire == chunk.wire && span.depth > 0)) {
				for (int k = 0; k < span.width; k++) {
					const RTLIL::SigBit origin_bit(chunk.wire, chunk.offset + (span.pos + k - start));
					RTLIL::SigBit bit = origin_bit;
					apply_from(bit, origin_bit, 0);
					out[span.pos + k] = bit;
				}
				continue;
			}
			const std::vector<AliasRange> *ranges = nullptr;
			if (!alias_ranges.empty()) {
				auto found = alias_ranges.find(span.wire);
				if (found != alias_ranges.end() && !found->second.empty())
					ranges = &found->second;
			}
			const int *idx = nullptr;
			bool idx_known = false;
			int pos = span.offset;
			const int end = span.offset + span.width;
			while (pos < end) {
				const int out_pos = span.pos + (pos - span.offset);
				const AliasRange *range = nullptr;
				int uncovered_end = end;
				if (ranges != nullptr) {
					auto it = std::upper_bound(ranges->begin(), ranges->end(), pos,
							[](int off, const AliasRange &r) { return off < r.offset; });
					if (it != ranges->end())
						uncovered_end = std::min(end, it->offset);
					if (it != ranges->begin()) {
						--it;
						if (it->offset <= pos && pos < it->offset + it->width)
							range = &*it;
					}
				}
				if (range != nullptr) {
					const int span_end = std::min(end, range->offset + range->width);
					const int n = span_end - pos;
					if (n == 1) {
						// A single bit has nothing to share; the per-bit loop is the
						// leaner path for it from here on.
						const RTLIL::SigBit origin_bit(chunk.wire, chunk.offset + (out_pos - start));
						RTLIL::SigBit bit = alias_bit(*range, pos);
						if (bit.wire != nullptr && bit != origin_bit)
							apply_from(bit, origin_bit, span.depth + 1);
						out[out_pos] = bit;
						pos = span_end;
						continue;
					}
					const RTLIL::SigChunk image = range->image.extract(pos - range->offset, n);
					if (image.wire == nullptr) {
						for (int j = 0; j < n; j++)
							out[out_pos + j] = RTLIL::SigBit(image.data[j]);
					} else {
						work.push_back(Span{image.wire, image.offset, image.width, out_pos, span.depth + 1});
					}
					pos = span_end;
					continue;
				}
				// No alias covers [pos, uncovered_end): the union-find step per bit.
				if (!idx_known) {
					idx = database.store().indices(span.wire);
					idx_known = true;
				}
				for (; pos < uncovered_end; pos++) {
					const int o = span.pos + (pos - span.offset);
					RTLIL::SigBit bit(span.wire, pos);
					const int i = idx ? idx[pos] : -1;
					if (i >= 0) {
						const RTLIL::SigBit &root = database[database.ifind(i)];
						if (root != bit) {
							bit = root;
							const RTLIL::SigBit origin_bit(chunk.wire, chunk.offset + (o - start));
							if (bit.wire != nullptr && bit != origin_bit)
								apply_from(bit, origin_bit, span.depth + 1);
						}
					}
					out[o] = bit;
				}
			}
		}
	}

	// Rebuild sig from its chunks, resolving the touched ones through apply_chunk.
	void apply_by_chunks(RTLIL::SigSpec &sig) const
	{
		std::vector<RTLIL::SigBit> bits;
		bits.reserve(sig.size());
		for (auto const &chunk : sig.chunks()) {
			if (chunk.wire == nullptr || !touched_wires.count(chunk.wire)) {
				for (int k = 0; k < chunk.width; k++)
					bits.push_back(chunk[k]);
				continue;
			}
			apply_chunk(chunk, bits);
		}
		sig = RTLIL::SigSpec(bits);
	}

	void apply(RTLIL::SigSpec &sig) const
	{
		// Nothing in this module was connected. A multi-chunk signal is already
		// canonical, and walking its bits is what made wide $pmux ports slower.
		if (touched_wires.empty())
			return;
		if (!sig.is_chunk()) {
			bool touches = false;
			for (auto const &chunk : sig.chunks()) {
				if (chunk.wire != nullptr && touched_wires.count(chunk.wire)) {
					touches = true;
					break;
				}
			}
			if (!touches)
				return;
			apply_by_chunks(sig);
			return;
		}
		const RTLIL::SigChunk chunk = sig.as_chunk();
		if (chunk.wire == nullptr || !touched_wires.count(chunk.wire))
			return;
		auto found = canon_chunk.find(chunk.wire);
		if (found == canon_chunk.end()) {
			apply_by_chunks(sig);
			return;
		}
		const RTLIL::SigChunk &image = found->second;
		if (chunk.offset == 0 && chunk.width == chunk.wire->width)
			sig = RTLIL::SigSpec(image);
		else if (chunk.offset >= 0 && chunk.offset + chunk.width <= image.width)
			sig = RTLIL::SigSpec(image.extract(chunk.offset, chunk.width));
		else
			apply_by_chunks(sig);
	}

	RTLIL::SigBit operator()(RTLIL::SigBit bit) const
	{
		apply(bit);
		return bit;
	}

	RTLIL::SigSpec operator()(RTLIL::SigSpec sig) const
	{
		apply(sig);
		return sig;
	}

	RTLIL::SigSpec operator()(RTLIL::Wire *wire) const
	{
		SigSpec sig(wire);
		apply(sig);
		return sig;
	}
};

/**
 * SigMap wraps a union-find "database"
 * to map SigBits of a module to canonical representative SigBits.
 * SigBits that are connected share a set in the underlying database.
 * If a SigBit has a const state (impl: bit.wire is nullptr),
 * it's promoted to a representative.
 */
struct SigMap final : public SigMapView
{
	SigMap(RTLIL::Module *module = NULL)
	{
		if (module != NULL)
			set(module);
	}

	void swap(SigMap &other)
	{
		database.swap(other.database);
		touched_wires.swap(other.touched_wires);
		bit_wires.swap(other.bit_wires);
		alias_ranges.swap(other.alias_ranges);
		image_users.swap(other.image_users);
		canon_chunk.swap(other.canon_chunk);
		std::swap(canon_valid, other.canon_valid);
	}

	void clear()
	{
		database.clear();
		touched_wires.clear();
		clear_aliases();
		canon_chunk.clear();
		canon_valid = false;
	}

	// Rebuild SigMap for all connections in module
	void set(RTLIL::Module *module)
	{
		int bitcount = 0;
		for (auto &it : module->connections())
			bitcount += it.first.size();

		database.clear();
		touched_wires.clear();
		clear_aliases();
		canon_chunk.clear();
		canon_valid = false;
		database.reserve(bitcount);

		for (auto &it : module->connections())
			add(it.first, it.second);
		rebuild_canon();
	}

	void merge_pair(const RTLIL::SigBit &from, const RTLIL::SigBit &to)
	{
		if (from.wire != nullptr)
			bit_wires.insert(from.wire);
		if (to.wire != nullptr)
			bit_wires.insert(to.wire);
		touch_bit(from);
		touch_bit(to);
		int bfi = database.lookup(from);
		int bti = database.lookup(to);

		const RTLIL::SigBit &bf = database[bfi];
		const RTLIL::SigBit &bt = database[bti];

		if (bf.wire || bt.wire) {
			database.imerge(bfi, bti);
			if (bf.wire == nullptr)
				database.ipromote(bfi);
			if (bt.wire == nullptr)
				database.ipromote(bti);
		}
	}

	void forget_image_user(RTLIL::Wire *image, RTLIL::Wire *source, int source_offset)
	{
		if (image == nullptr)
			return;
		auto found = image_users.find(image);
		if (found == image_users.end())
			return;
		std::vector<std::pair<RTLIL::Wire *, int>> kept;
		for (auto &user : found->second)
			if (user.first != source || user.second != source_offset)
				kept.push_back(user);
		if (kept.empty())
			image_users.erase(image);
		else
			found->second.swap(kept);
	}

	void remember_image_user(const RTLIL::SigChunk &image, RTLIL::Wire *source, int source_offset)
	{
		if (image.wire != nullptr)
			image_users[image.wire].push_back(std::make_pair(source, source_offset));
	}

	void erase_alias(RTLIL::Wire *source, int source_offset)
	{
		auto found = alias_ranges.find(source);
		if (found == alias_ranges.end())
			return;
		std::vector<AliasRange> kept;
		for (AliasRange &range : found->second) {
			if (range.offset == source_offset)
				forget_image_user(range.image.wire, source, source_offset);
			else
				kept.push_back(range);
		}
		if (kept.empty())
			alias_ranges.erase(source);
		else
			found->second.swap(kept);
	}

	void materialize_alias(RTLIL::Wire *source, const AliasRange &range)
	{
		for (int i = 0; i < range.width; i++) {
			RTLIL::SigBit from(source, range.offset + i);
			RTLIL::SigBit to = range.image.wire == nullptr
						   ? RTLIL::SigBit(range.image.data[i])
						   : RTLIL::SigBit(range.image.wire, range.image.offset + i);
			merge_pair(from, to);
		}
		erase_alias(source, range.offset);
	}

	// Rewrite aliases whose image lies inside [wire, offset, width) so they
	// point at replacement, which is the new image of that slice.
	void retarget_image(RTLIL::Wire *wire, int offset, int width, const RTLIL::SigChunk &replacement)
	{
		auto found = image_users.find(wire);
		if (found == image_users.end())
			return;
		const std::vector<std::pair<RTLIL::Wire *, int>> users = found->second;
		for (auto user : users) {
			auto source_ranges = alias_ranges.find(user.first);
			if (source_ranges == alias_ranges.end())
				continue;
			for (size_t i = 0; i < source_ranges->second.size(); i++) {
				AliasRange &range = source_ranges->second[i];
				if (range.offset != user.second || range.image.wire != wire)
					continue;
				const bool inside = range.image.offset >= offset &&
						    range.image.offset + range.width <= offset + width;
				if (!inside) {
					AliasRange copy = range;
					materialize_alias(user.first, copy);
					break;
				}
				const int delta = range.image.offset - offset;
				forget_image_user(wire, user.first, range.offset);
				range.image = replacement.extract(delta, range.width);
				remember_image_user(range.image, user.first, range.offset);
				break;
			}
		}
	}

	bool try_alias_chunk(const RTLIL::SigChunk &from, const RTLIL::SigChunk &to)
	{
		if (from.wire == nullptr || from.width != to.width || from.width <= 0)
			return false;
		// A wire is either aliased by ranges or stored bit by bit.
		if (bit_wires.count(from.wire) || alias_overlaps(from.wire, from.offset, from.width)) {
			auto found = alias_ranges.find(from.wire);
			if (found != alias_ranges.end()) {
				const std::vector<AliasRange> all = found->second;
				for (const AliasRange &range : all)
					materialize_alias(from.wire, range);
			}
			return false;
		}

		RTLIL::SigChunk image = compose_image(to);
		retarget_image(from.wire, from.offset, from.width, image);
		if (image.wire == from.wire && image.offset == from.offset && image.width == from.width)
			return true;

		touch_bit(RTLIL::SigBit(from.wire, from.offset));
		AliasRange created{from.offset, from.width, image};
		std::vector<AliasRange> &ranges = alias_ranges[from.wire];
		// Bit-at-a-time callers (opt_clean's wire map) append the next offset.
		// A linear scan from begin() on each insert is quadratic in the width.
		auto insert_at = std::lower_bound(ranges.begin(), ranges.end(), created.offset,
				[](const AliasRange &range, int off) { return range.offset < off; });
		if (insert_at != ranges.begin()) {
			auto prev = insert_at - 1;
			if (prev->offset + prev->width == created.offset && images_abut(prev->image, created.image)) {
				extend_image(prev->image, created.image);
				prev->width += created.width;
				while (insert_at != ranges.end() &&
						prev->offset + prev->width == insert_at->offset &&
						images_abut(prev->image, insert_at->image)) {
					forget_image_user(insert_at->image.wire, from.wire, insert_at->offset);
					extend_image(prev->image, insert_at->image);
					prev->width += insert_at->width;
					insert_at = ranges.erase(insert_at);
				}
				return true;
			}
		}
		insert_at = ranges.insert(insert_at, created);
		remember_image_user(insert_at->image, from.wire, insert_at->offset);
		auto next = insert_at + 1;
		while (next != ranges.end() &&
				insert_at->offset + insert_at->width == next->offset &&
				images_abut(insert_at->image, next->image)) {
			forget_image_user(next->image.wire, from.wire, next->offset);
			extend_image(insert_at->image, next->image);
			insert_at->width += next->width;
			next = ranges.erase(next);
		}
		return true;
	}

	static bool images_abut(const RTLIL::SigChunk &left, const RTLIL::SigChunk &right)
	{
		if (left.wire != nullptr || right.wire != nullptr)
			return left.wire != nullptr && left.wire == right.wire &&
					left.offset + left.width == right.offset;
		return true;
	}

	static void extend_image(RTLIL::SigChunk &left, const RTLIL::SigChunk &right)
	{
		if (left.wire != nullptr) {
			left.width += right.width;
			return;
		}
		left.data.insert(left.data.end(), right.data.begin(), right.data.end());
		left.width += right.width;
	}

	// Add connections from "from" to "to". A single untouched chunk is one
	// range. Anything that cuts an existing range falls back to bits.
	void add(const RTLIL::SigSpec& from, const RTLIL::SigSpec& to)
	{
		log_assert(GetSize(from) == GetSize(to));
		invalidate_canon();
		if (from.is_chunk() && to.is_chunk() && try_alias_chunk(from.as_chunk(), to.as_chunk()))
			return;

		for (int i = 0; i < GetSize(from); i++)
			merge_pair(from[i], to[i]);
	}

	// Make bit the representative of its set when that set lives entirely in
	// the union-find. A chunk alias past the root (a constant tie-off, or a
	// slice tied to another wire) must stay: promoting the root would drop it.
	void add(const RTLIL::SigBit &bit)
	{
		if (bit.wire == nullptr)
			return;
		RTLIL::SigBit mfp_rep = database.find(bit);
		SigBit resolved = bit;
		apply(resolved);
		if (resolved != mfp_rep)
			return;
		if (mfp_rep.wire == nullptr)
			return;
		database.promote(bit);
	}

	void add(const RTLIL::SigSpec &sig)
	{
		for (const auto &bit : sig)
			add(bit);
	}

	inline void add(Wire *wire) { return add(RTLIL::SigSpec(wire)); }

	// All non-const bits
	RTLIL::SigSpec allbits() const
	{
		RTLIL::SigSpec sig;
		for (const auto &bit : database)
			if (bit.wire != nullptr)
				sig.append(bit);
		return sig;
	}
};

/**
 * SiValgMap wraps a union-find "database" to map SigBits of a module to
 * canonical representative SigBits plus some optional Val value associated with the bits.
 * Val has a commutative, associative, idempotent operator|=, a default constructor
 * which constructs an identity element, and a copy constructor.
 * SigBits that are connected share a set in the underlying database;
 * the associated value is the "sum" of all the values associated with the contributing bits.
 * If any of the SigBits in a set are a constant, the canonical SigBit is a constant.
 */
template <class Val>
struct SigValMap final : public SigMapView
{
	dict<SigBit, Val> values;

	void swap(SigValMap<Val> &other)
	{
		database.swap(other.database);
		touched_wires.swap(other.touched_wires);
		bit_wires.swap(other.bit_wires);
		alias_ranges.swap(other.alias_ranges);
		image_users.swap(other.image_users);
		canon_chunk.swap(other.canon_chunk);
		std::swap(canon_valid, other.canon_valid);
		values.swap(other.values);
	}

	void clear()
	{
		database.clear();
		touched_wires.clear();
		clear_aliases();
		canon_chunk.clear();
		canon_valid = false;
		values.clear();
	}

	// Rebuild SigMap for all connections in module
	void set(RTLIL::Module *module)
	{
		int bitcount = 0;
		for (auto &it : module->connections())
			bitcount += it.first.size();

		database.clear();
		touched_wires.clear();
		clear_aliases();
		canon_chunk.clear();
		canon_valid = false;
		values.clear();
		database.reserve(bitcount);

		for (auto &it : module->connections())
			add(it.first, it.second);
		rebuild_canon();
	}

	// Add connections from "from" to "to", bit-by-bit.
	void add(const RTLIL::SigSpec& from, const RTLIL::SigSpec& to)
	{
		log_assert(GetSize(from) == GetSize(to));
		invalidate_canon();

		for (int i = 0; i < GetSize(from); i++)
		{
			touch_bit(from[i]);
			touch_bit(to[i]);
			int bfi = database.lookup(from[i]);
			int bti = database.lookup(to[i]);
			if (bfi == bti) {
				continue;
			}

			const RTLIL::SigBit &bf = database[bfi];
			const RTLIL::SigBit &bt = database[bti];
			if (bf.wire == nullptr) {
				// bf is constant so make it the canonical representative.
				database.imerge(bti, bfi);
				merge_value(bt, bf);
			} else {
				// Make bt the canonical representative.
				database.imerge(bfi, bti);
				merge_value(bf, bt);
			}
		}
	}

	void addVal(const RTLIL::SigBit &bit, const Val &val)
	{
		values[database.find(bit)] |= val;
	}

	void addVal(const RTLIL::SigSpec &sig, const Val &val)
	{
		for (const auto &bit : sig)
			addVal(bit, val);
	}

	Val apply_and_get_value(RTLIL::SigBit &bit) const
	{
		bit = database.find(bit);
		auto it = values.find(bit);
		return it == values.end() ? Val() : it->second;
	}

private:
	void merge_value(const RTLIL::SigBit &from, const RTLIL::SigBit &to)
	{
		auto it = values.find(from);
		if (it == values.end()) {
			return;
		}
		// values[to] could resize the underlying `entries` so
		// finish using `it` first.
		Val v = it->second;
		values.erase(it);
		values[to] |= v;
	}
};

YOSYS_NAMESPACE_END

#endif /* SIGTOOLS_H */
