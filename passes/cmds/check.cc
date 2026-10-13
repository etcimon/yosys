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

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celledges.h"
#include "kernel/newcelltypes.h"
#include "kernel/utils.h"
#include "kernel/log_help.h"
#include "kernel/mem.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Coarse loop node. kind -2 is a whole wire, kind -1 is a cell whose
// bit-level edges were too wide to store directly.
using WireNode = std::pair<RTLIL::IdString, int>;

static void wire_finish_order(const dict<WireNode, pool<WireNode>> &fwd, std::vector<WireNode> &order)
{
	pool<WireNode> entered, finished;
	std::vector<WireNode> stack;
	for (auto &it : fwd) {
		if (entered.count(it.first))
			continue;
		stack.push_back(it.first);
		while (!stack.empty()) {
			WireNode node = stack.back();
			if (!entered.count(node)) {
				entered.insert(node);
				auto fit = fwd.find(node);
				if (fit != fwd.end()) {
					for (const WireNode &next : fit->second)
						if (!entered.count(next))
							stack.push_back(next);
				}
			} else {
				stack.pop_back();
				if (finished.insert(node).second)
					order.push_back(node);
			}
		}
	}
}

// Wires that sit in a coarse cycle. A bit-level loop is always inside one of
// these, so the per-bit graph is built only for them.
static pool<RTLIL::IdString> live_loop_wires(const dict<WireNode, pool<WireNode>> &fwd, const dict<WireNode, pool<WireNode>> &rev)
{
	std::vector<WireNode> order;
	wire_finish_order(fwd, order);

	pool<WireNode> seen;
	pool<RTLIL::IdString> live;
	for (auto it = order.rbegin(); it != order.rend(); ++it) {
		if (!seen.insert(*it).second)
			continue;
		pool<WireNode> comp;
		std::vector<WireNode> stack = {*it};
		comp.insert(*it);
		while (!stack.empty()) {
			WireNode node = stack.back();
			stack.pop_back();
			auto rit = rev.find(node);
			if (rit == rev.end())
				continue;
			for (const WireNode &next : rit->second) {
				if (!seen.insert(next).second)
					continue;
				comp.insert(next);
				stack.push_back(next);
			}
		}
		bool self = false;
		if (comp.size() == 1) {
			const WireNode &only = *comp.begin();
			auto fit = fwd.find(only);
			self = fit != fwd.end() && fit->second.count(only);
		}
		if (comp.size() < 2 && !self)
			continue;
		for (const WireNode &node : comp)
			if (node.second == -2)
				live.insert(node.first);
	}
	return live;
}

// Drivers and uses stored per chunk. Warnings expand a bit only when that
// bit is undriven or has more than one counting driver, so a healthy wide
// port is one record instead of one string per bit.
struct ChunkDrivers {
	struct Drv {
		int offset;
		int width;
		int msg0;
		Cell *cell;
		IdString port;
		Wire *inwire;
		bool counts;
		std::string extra;

		bool covers(int bit) const { return bit >= offset && bit < offset + width; }

		std::string describe(int bit) const {
			if (!extra.empty())
				return extra;
			int idx = msg0 + (bit - offset);
			if (cell)
				return stringf("port %s[%d] of cell %s (%s)", port.unescape(), idx, cell, cell->type.unescape());
			if (inwire)
				return stringf("module input %s[%d]", inwire, idx);
			return {};
		}
	};
	struct Use { int offset, width; };

	dict<Wire *, std::vector<Drv>> by_wire;
	dict<Wire *, std::vector<Use>> used;
	dict<SigBit, vector<string>> const_msgs;

	void add_sig(const SigSpec &sig, Cell *cell, IdString port, Wire *inwire, bool counts, const std::string &extra)
	{
		int at = 0;
		for (auto const &chunk : sig.chunks()) {
			if (chunk.wire == nullptr) {
				for (int k = 0; k < chunk.width; k++) {
					std::string text;
					if (!extra.empty())
						text = extra;
					else if (cell != nullptr && counts)
						text = stringf("port %s[%d] of cell %s (%s)", port.unescape(), at + k, cell, cell->type.unescape());
					else if (inwire != nullptr && counts)
						text = stringf("module input %s[%d]", inwire, at + k);
					if (!text.empty())
						const_msgs[SigBit(chunk.data[k])].push_back(text);
				}
			} else {
				by_wire[chunk.wire].push_back(Drv{chunk.offset, chunk.width, at, cell, port, inwire, counts, extra});
			}
			at += chunk.width;
		}
	}

	void add_use(const SigSpec &sig)
	{
		for (auto const &chunk : sig.chunks())
			if (chunk.wire != nullptr)
				used[chunk.wire].push_back(Use{chunk.offset, chunk.width});
	}

	Cell *last_cell(SigBit bit) const
	{
		if (bit.wire == nullptr)
			return nullptr;
		auto found = by_wire.find(bit.wire);
		if (found == by_wire.end())
			return nullptr;
		Cell *cell = nullptr;
		for (auto const &drv : found->second)
			if (drv.cell != nullptr && drv.covers(bit.offset))
				cell = drv.cell;
		return cell;
	}

	void warn(Module *module, int &counter) const
	{
		for (auto const &it : const_msgs) {
			string message = stringf("Drivers conflicting with a constant %s driver:\n", log_signal(it.first));
			for (auto const &str : it.second)
				message += stringf("    %s\n", str);
			log_warning("%s", message);
			counter++;
		}

		pool<Wire *> wires;
		for (auto const &it : by_wire)
			wires.insert(it.first);
		for (auto const &it : used)
			wires.insert(it.first);

		for (auto wire : wires) {
			const std::vector<Drv> *drvs = nullptr;
			auto dit = by_wire.find(wire);
			if (dit != by_wire.end())
				drvs = &dit->second;
			const std::vector<Use> *uses = nullptr;
			auto uit = used.find(wire);
			if (uit != used.end())
				uses = &uit->second;

			std::vector<std::pair<int, int>> events;
			if (drvs) {
				for (auto const &drv : *drvs)
					if (drv.counts) {
						events.push_back({drv.offset, 1});
						events.push_back({drv.offset + drv.width, -1});
					}
			}
			std::sort(events.begin(), events.end());
			int count = 0;
			int prev = 0;
			std::vector<std::pair<int, int>> multi;
			for (auto ev : events) {
				if (count > 1 && ev.first > prev)
					multi.push_back({prev, ev.first});
				count += ev.second;
				prev = ev.first;
			}
			for (auto range : multi) {
				for (int bit = range.first; bit < range.second; bit++) {
					string message = stringf("multiple conflicting drivers for %s.%s:\n", module, log_signal(SigBit(wire, bit)));
					if (drvs) {
						for (auto const &drv : *drvs) {
							if (!drv.covers(bit))
								continue;
							std::string text = drv.describe(bit);
							if (!text.empty())
								message += stringf("    %s\n", text);
						}
					}
					log_warning("%s", message);
					counter++;
				}
			}

			std::vector<std::pair<int, int>> covered;
			if (drvs) {
				for (auto const &drv : *drvs)
					covered.push_back({drv.offset, drv.offset + drv.width});
				std::sort(covered.begin(), covered.end());
				std::vector<std::pair<int, int>> merged;
				for (auto iv : covered) {
					if (merged.empty() || iv.first > merged.back().second)
						merged.push_back(iv);
					else
						merged.back().second = std::max(merged.back().second, iv.second);
				}
				covered.swap(merged);
			}
			if (!uses)
				continue;
			for (auto use : *uses) {
				int cursor = use.offset;
				int end = use.offset + use.width;
				for (auto iv : covered) {
					if (iv.second <= cursor)
						continue;
					if (iv.first >= end)
						break;
					if (iv.first > cursor) {
						for (int bit = cursor; bit < std::min(iv.first, end); bit++) {
							log_warning("Wire %s.%s is used but has no driver.\n", module, log_signal(SigBit(wire, bit)));
							counter++;
						}
					}
					cursor = std::max(cursor, iv.second);
					if (cursor >= end)
						break;
				}
				for (int bit = cursor; bit < end; bit++) {
					log_warning("Wire %s.%s is used but has no driver.\n", module, log_signal(SigBit(wire, bit)));
					counter++;
				}
			}
		}
	}
};

struct CheckPass : public Pass {
	CheckPass() : Pass("check", "check for obvious problems in the design") { }
	bool formatted_help() override {
		auto *help = PrettyHelp::get_current();
		help->set_group("passes/status");
		return false;
	}
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    check [options] [selection]\n");
		log("\n");
		log("This pass identifies the following problems in the current design:\n");
		log("\n");
		log("  - combinatorial loops\n");
		log("  - two or more conflicting drivers for one wire\n");
		log("  - used wires that do not have a driver\n");
		log("  - cells of an internal ($-prefixed) type that is neither a known cell type\n");
		log("    nor a module in the design\n");
		log("\n");
		log("Options:\n");
		log("\n");
		log("    -noinit\n");
		log("        also check for wires which have the 'init' attribute set\n");
		log("\n");
		log("    -initdrv\n");
		log("        also check for wires that have the 'init' attribute set and are not\n");
		log("        driven by an FF cell type\n");
		log("\n");
		log("    -mapped\n");
		log("        also check for internal cells that have not been mapped to cells of the\n");
		log("        target architecture\n");
		log("\n");
		log("    -nolatches\n");
		log("        also check for latch cells ($dlatch, $adlatch, $dlatchsr and their\n");
		log("        $_DLATCH_*/$_DLATCHSR_* mappings) remaining in the design. Use this\n");
		log("        before techmapping in flows that must not emit latches. Cells marked\n");
		log("        with the 'always_latch' attribute are not reported.\n");
		log("\n");
		log("    -latchonly\n");
		log("        check only for latch cells (as listed under -nolatches), skipping all\n");
		log("        other checks.\n");
		log("\n");
		log("    -allow-tbuf\n");
		log("        modify the -mapped behavior to still allow $_TBUF_ cells\n");
		log("\n");
		log("    -assert\n");
		log("        produce a runtime error if any problems are found in the current design\n");
		log("\n");
		log("    -force-detailed-loop-check\n");
		log("        for the detection of combinatorial loops, use a detailed connectivity\n");
		log("        model for all internal cells for which it is available. This disables\n");
		log("        falling back to a simpler overapproximating model for those cells for\n");
		log("        which the detailed model is expected costly.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		int counter = 0;
		bool noinit = false;
		bool initdrv = false;
		bool mapped = false;
		bool nolatches = false;
		bool latchonly = false;
		bool allow_tbuf = false;
		bool assert_mode = false;
		bool force_detailed_loop_check = false;
		bool suggest_detail = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-noinit") {
				noinit = true;
				continue;
			}
			if (args[argidx] == "-initdrv") {
				initdrv = true;
				continue;
			}
			if (args[argidx] == "-mapped") {
				mapped = true;
				continue;
			}
			if (args[argidx] == "-nolatches") {
				nolatches = true;
				continue;
			}
			if (args[argidx] == "-latchonly") {
				latchonly = true;
				continue;
			}
			if (args[argidx] == "-allow-tbuf") {
				allow_tbuf = true;
				continue;
			}
			if (args[argidx] == "-assert") {
				assert_mode = true;
				continue;
			}
			if (args[argidx] == "-force-detailed-loop-check") {
				force_detailed_loop_check = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		log_header(design, "Executing CHECK pass (checking for obvious problems).\n");

		for (auto module : design->selected_whole_modules_warn())
		{
			log("Checking module %s...\n", module);

			// latch-only mode only flags latches, skipping the (potentially false-positive mid-flow) undriven/driver/loop checks below
			if (latchonly) {
				for (auto cell : module->cells())
					if (!cell->get_bool_attribute(ID::always_latch) && (
						cell->type.in(ID($dlatch), ID($adlatch), ID($dlatchsr)) ||
						cell->type.begins_with("$_DLATCH_") || cell->type.begins_with("$_DLATCHSR_"))) {
						log_warning("Cell %s.%s is a latch of type %s.\n", module, cell, cell->type.unescape());
						counter++;
					}
				continue;
			}

			SigMap sigmap(module);
			ChunkDrivers drivers;
			TopoSort<std::pair<RTLIL::IdString, int>> topo;
			dict<WireNode, pool<WireNode>> coarse_fwd, coarse_rev;
			for (auto &proc_it : module->processes)
			{
				pool<SigBit> proc_driven_bits;
				std::vector<RTLIL::CaseRule*> all_cases = {&proc_it.second->root_case};
				for (size_t i = 0; i < all_cases.size(); i++) {
					for (auto action : all_cases[i]->actions) {
						drivers.add_sig(sigmap(action.first), nullptr, IdString(), nullptr, false,
								stringf("action %s <= %s (case rule) in process %s",
										log_signal(action.first), log_signal(action.second), proc_it.first.unescape()));
						for (auto bit : sigmap(action.first))
							if (bit.wire)
								proc_driven_bits.insert(bit);
						drivers.add_use(sigmap(action.second));
					}
					for (auto switch_ : all_cases[i]->switches) {
						for (auto case_ : switch_->cases) {
							all_cases.push_back(case_);
							for (auto compare : case_->compare)
								drivers.add_use(sigmap(compare));
						}
					}
				}
				for (auto &sync : proc_it.second->syncs) {
					drivers.add_use(sigmap(sync->signal));
					for (auto action : sync->actions) {
						drivers.add_sig(sigmap(action.first), nullptr, IdString(), nullptr, false,
								stringf("action %s <= %s (sync rule) in process %s",
										log_signal(action.first), log_signal(action.second), proc_it.first.unescape()));
						if (sync->type != RTLIL::SyncType::STi)
							for (auto bit : sigmap(action.first))
								if (bit.wire)
									proc_driven_bits.insert(bit);
						drivers.add_use(sigmap(action.second));
					}
					for (auto memwr : sync->mem_write_actions) {
						drivers.add_use(sigmap(memwr.address));
						drivers.add_use(sigmap(memwr.data));
						drivers.add_use(sigmap(memwr.enable));
					}
				}
				for (auto bit : proc_driven_bits)
					drivers.add_sig(bit, nullptr, IdString(), nullptr, true, {});
			}

			struct CircuitEdgesDatabase : AbstractCellEdgesDatabase {
				TopoSort<std::pair<RTLIL::IdString, int>> *bit_topo;
				dict<WireNode, pool<WireNode>> *coarse_fwd;
				dict<WireNode, pool<WireNode>> *coarse_rev;
				const pool<RTLIL::IdString> *live_wires;
				SigMap sigmap;
				bool force_detail;

				CircuitEdgesDatabase(TopoSort<std::pair<RTLIL::IdString, int>> *bit_topo,
						     dict<WireNode, pool<WireNode>> *coarse_fwd,
						     dict<WireNode, pool<WireNode>> *coarse_rev,
						     const pool<RTLIL::IdString> *live_wires, SigMap &sigmap, bool force_detail)
					: bit_topo(bit_topo), coarse_fwd(coarse_fwd), coarse_rev(coarse_rev), live_wires(live_wires),
					  sigmap(sigmap), force_detail(force_detail) {}

				void link_wire(WireNode from, WireNode to)
				{
					(*coarse_fwd)[from].insert(to);
					(*coarse_fwd)[to];
					(*coarse_rev)[to].insert(from);
				}

				bool wire_kept(const RTLIL::SigBit &bit) const
				{
					if (!bit.wire)
						return false;
					if (live_wires && !live_wires->count(bit.wire->name))
						return false;
					return true;
				}

				void add_edge(RTLIL::Cell *cell, RTLIL::IdString from_port, int from_bit,
							  RTLIL::IdString to_port, int to_bit, int) override {
					// Const operator[] does not unpack. A writable copy would
					// unpack the whole port once per bit.
					const SigSpec &from_portsig = cell->getPort(from_port);
					const SigSpec &to_portsig = cell->getPort(to_port);
					log_assert(from_bit >= 0 && from_bit < from_portsig.size());
					log_assert(to_bit >= 0 && to_bit < to_portsig.size());
					SigBit from = sigmap(from_portsig[from_bit]);
					SigBit to = sigmap(to_portsig[to_bit]);

					if (!from.wire || !to.wire)
						return;
					if (live_wires && (!live_wires->count(from.wire->name) || !live_wires->count(to.wire->name)))
						return;
					if (bit_topo)
						bit_topo->edge(std::make_pair(from.wire->name, from.offset), std::make_pair(to.wire->name, to.offset));
					else
						link_wire(WireNode(from.wire->name, -2), WireNode(to.wire->name, -2));
				}

				bool detail_costly(Cell *cell) {
					// Only those cell types for which the edge data can expode quadratically
					// in port widths are those for us to check.
					if (!cell->type.in(
							ID($add), ID($sub),
							ID($shl), ID($shr), ID($sshl), ID($sshr), ID($shift), ID($shiftx),
							ID($pmux), ID($bmux)))
						return false;

					int in_widths = 0, out_widths = 0;

					if (cell->type.in(ID($pmux), ID($bmux))) {
						// We're skipping inputs A and B, since each of their bits contributes only one edge
						in_widths = GetSize(cell->getPort(ID::S));
						out_widths = GetSize(cell->getPort(ID::Y));
					} else {
						for (auto &conn : cell->connections()) {
							if (cell->input(conn.first))
								in_widths += conn.second.size();
							if (cell->output(conn.first))
								out_widths += conn.second.size();
						}
					}

					const int threshold = 1024;

					// if the multiplication may overflow we will catch it here
					if (in_widths + out_widths >= threshold)
						return true;

					if (in_widths * out_widths >= threshold)
						return true;

					return false;
				}

				bool port_can_be_live(const SigSpec &raw) const
				{
					if (!live_wires)
						return true;
					auto hit = [&](RTLIL::Wire *wire) {
						return wire != nullptr && (live_wires->count(wire->name) || sigmap.touched_wires.count(wire));
					};
					if (raw.is_chunk())
						return hit(raw.as_chunk().wire);
					for (auto const &chunk : raw.chunks())
						if (hit(chunk.wire))
							return true;
					return false;
				}

				bool add_edges_from_cell(Cell *cell) {
					// The wire filter never needs a per-bit edge list. One edge per
					// connected wire is enough to decide which wires can be in a loop.
					if (!bit_topo) {
						pool<RTLIL::IdString> in_wires, out_wires;
						for (auto &conn : cell->connections()) {
							SigSpec sig = sigmap(conn.second);
							for (auto const &chunk : sig.chunks()) {
								if (!chunk.wire)
									continue;
								if (cell->input(conn.first))
									in_wires.insert(chunk.wire->name);
								if (cell->output(conn.first))
									out_wires.insert(chunk.wire->name);
							}
						}
						for (auto &name : in_wires)
							link_wire(WireNode(name, -2), WireNode(cell->name, -1));
						for (auto &name : out_wires)
							link_wire(WireNode(cell->name, -1), WireNode(name, -2));
						return true;
					}

					// Detailed edges index every port bit. A cell whose wires are
					// outside the coarse cycle cannot add a bit edge.
					if (live_wires) {
						bool hits = false;
						for (auto &conn : cell->connections()) {
							if (port_can_be_live(conn.second)) {
								hits = true;
								break;
							}
						}
						if (!hits)
							return true;
					}

					if (force_detail || !detail_costly(cell)) {
						if (AbstractCellEdgesDatabase::add_edges_from_cell(cell))
							return true;
					}

					// Bit fallback for cells whose detailed edges would be quadratic.
					// A wire that is not in the coarse cycle contributes no edge.
					// Skip the chunk before visiting its bits.
					{
						auto add_port_bits = [&](const SigSpec &raw, bool as_output) {
							SigSpec sig = sigmap(raw);
							for (auto const &chunk : sig.chunks()) {
								if (chunk.wire == nullptr)
									continue;
								if (live_wires && !live_wires->count(chunk.wire->name))
									continue;
								for (int i = 0; i < chunk.width; i++) {
									std::pair<RTLIL::IdString, int> bit_node(chunk.wire->name, chunk.offset + i);
									std::pair<RTLIL::IdString, int> cell_node(cell->name, -1);
									if (as_output)
										bit_topo->edge(cell_node, bit_node);
									else
										bit_topo->edge(bit_node, cell_node);
								}
							}
						};
						for (auto &conn : cell->connections()) {
							if (cell->input(conn.first))
								add_port_bits(conn.second, false);
							if (cell->output(conn.first))
								add_port_bits(conn.second, true);
						}
					}

					// Return false to signify the fallback
					return false;
				}
			};

			CircuitEdgesDatabase edges_db(nullptr, &coarse_fwd, &coarse_rev, nullptr, sigmap, force_detailed_loop_check);

			pool<Cell *> coarsened_cells;
			for (auto cell : module->cells())
			{
				if (cell->type.begins_with("$") && !yosys_celltypes.cell_known(cell->type) && design->module(cell->type) == nullptr) {
					log_warning("Cell %s.%s has unknown internal type %s.\n", module, cell, cell->type.unescape());
					counter++;
				}

				if (mapped && cell->type.begins_with("$") && design->module(cell->type) == nullptr) {
					if (allow_tbuf && cell->type == ID($_TBUF_)) goto cell_allowed;
					log_warning("Cell %s.%s is an unmapped internal cell of type %s.\n", module, cell, cell->type.unescape());
					counter++;
				cell_allowed:;
				}

				if (
					nolatches && !cell->get_bool_attribute(ID::always_latch) && (
					cell->type.in(ID($dlatch), ID($adlatch), ID($dlatchsr)) ||
					cell->type.begins_with("$_DLATCH_") || cell->type.begins_with("$_DLATCHSR_"))
				) {
					log_warning("Cell %s.%s is a latch of type %s.\n", module, cell, cell->type.unescape());
					counter++;
				}

				for (auto &conn : cell->connections()) {
					bool input = cell->input(conn.first);
					bool output = cell->output(conn.first);
					SigSpec sig = sigmap(conn.second);
					if (input)
						drivers.add_use(sig);
					if (output)
						drivers.add_sig(sig, cell, conn.first, nullptr, output && !input, {});
				}

				if (yosys_celltypes.cell_evaluable(cell->type) || cell->type.in(ID($mem_v2), ID($memrd), ID($memrd_v2), ID($barrier)) \
						|| cell->is_builtin_ff()) {
					if (!edges_db.add_edges_from_cell(cell))
						coarsened_cells.insert(cell);
				}
			}

			pool<SigBit> init_bits;

			for (auto wire : module->wires()) {
				if (wire->port_input)
					drivers.add_sig(sigmap(wire), nullptr, IdString(), wire, !wire->port_output, {});
				if (wire->port_output)
					drivers.add_use(sigmap(wire));
				if (wire->attributes.count(ID::init)) {
					Const initval = wire->attributes.at(ID::init);
					for (int i = 0; i < GetSize(initval) && i < GetSize(wire); i++)
						if (initval[i] == State::S0 || initval[i] == State::S1)
							init_bits.insert(sigmap(SigBit(wire, i)));
					if (noinit) {
						log_warning("Wire %s.%s has an unprocessed 'init' attribute.\n", module, wire);
						counter++;
					}
				}
			}

			drivers.warn(module, counter);

			pool<RTLIL::IdString> live_wires = live_loop_wires(coarse_fwd, coarse_rev);
			if (!live_wires.empty()) {
				CircuitEdgesDatabase bit_db(&topo, nullptr, nullptr, &live_wires, sigmap, force_detailed_loop_check);
				for (auto cell : module->cells()) {
					if (yosys_celltypes.cell_evaluable(cell->type) || cell->type.in(ID($mem_v2), ID($memrd), ID($memrd_v2), ID($barrier)) \
							|| cell->is_builtin_ff())
						if (!bit_db.add_edges_from_cell(cell))
							coarsened_cells.insert(cell);
				}
			}

			topo.sort();
			for (auto &loop : topo.loops) {
				string message = stringf("found logic loop in module %s:\n", module);

				// `loop` only contains wire bits, or an occasional special helper node for cells for
				// which we have done the edges fallback. The cell and its ports that led to an edge are
				// a piece of information we need to recover now. For that we need to have the previous
				// wire bit of the loop at hand.
				SigBit prev;
				for (auto it = loop.rbegin(); it != loop.rend(); it++)
				if (it->second != -1) { // skip the fallback helper nodes
					prev = SigBit(module->wire(it->first), it->second);
					break;
				}
				log_assert(prev != SigBit());

				for (auto &pair : loop) {
					if (pair.second == -1)
						continue; // helper node for edges fallback, we can ignore it

					struct MatchingEdgePrinter : AbstractCellEdgesDatabase {
						std::string &message;
						SigMap &sigmap;
						SigBit from, to;
						int nhits;
						const int HITS_LIMIT = 3;

						MatchingEdgePrinter(std::string &message, SigMap &sigmap, SigBit from, SigBit to)
							: message(message), sigmap(sigmap), from(from), to(to), nhits(0) {}

						void add_edge(RTLIL::Cell *cell, RTLIL::IdString from_port, int from_bit,
									  RTLIL::IdString to_port, int to_bit, int) override {
							SigBit edge_from = sigmap(cell->getPort(from_port))[from_bit];
							SigBit edge_to = sigmap(cell->getPort(to_port))[to_bit];

							if (edge_from == from && edge_to == to && nhits++ < HITS_LIMIT)
								message += stringf("      %s[%d] --> %s[%d]\n", from_port.unescape(), from_bit,
												   to_port.unescape(), to_bit);
							if (nhits == HITS_LIMIT)
								message += "      ...\n";
						}
					};

					Wire *wire = module->wire(pair.first);
					log_assert(wire);
					SigBit bit(module->wire(pair.first), pair.second);
					Cell *driver = drivers.last_cell(bit);
					log_assert(driver);

					std::string driver_src;
					if (driver->has_attribute(ID::src)) {
						std::string src_attr = driver->get_src_attribute();
						driver_src = stringf(" source: %s", src_attr);
					}

					message += stringf("    cell %s (%s)%s\n", driver, driver->type.unescape(), driver_src);

					if (!coarsened_cells.count(driver)) {
						MatchingEdgePrinter printer(message, sigmap, prev, bit);
						printer.add_edges_from_cell(driver);
					} else {
						message += "      (cell's internal connectivity overapproximated; loop may be a false positive)\n";
						suggest_detail = true;
					}

					if (wire->name.isPublic()) {
						std::string wire_src;
						if (wire->has_attribute(ID::src)) {
							std::string src_attr = wire->get_src_attribute();
							wire_src = stringf(" source: %s", src_attr);
						}
						message += stringf("    wire %s%s\n", log_signal(SigBit(wire, pair.second)), wire_src);
					}

					prev = bit;
				}
				log_warning("%s", message);
				counter++;
			}

			if (initdrv)
			{
				for (auto cell : module->cells())
				{
					if (cell->is_builtin_ff() == 0)
						continue;

					for (auto bit : sigmap(cell->getPort(ID::Q)))
						init_bits.erase(bit);
				}

				SigSpec init_sig(init_bits);
				init_sig.sort_and_unify();

				for (auto chunk : init_sig.chunks()) {
					log_warning("Wire %s.%s has 'init' attribute and is not driven by an FF cell.\n", module, log_signal(chunk));
					counter++;
				}
			}
		}

		log("Found and reported %d problems.\n", counter);

		if (suggest_detail)
			log("Consider re-running with '-force-detailed-loop-check' to rule out false positives.\n");

		if (assert_mode && counter > 0)
			log_error("Found %d problems in 'check -assert'.\n", counter);
	}
} CheckPass;

struct CheckMemPass : public Pass {
	CheckMemPass() : Pass("check_mem", "check for obvious memory problems in the design") { }
	bool formatted_help() override {
		auto *help = PrettyHelp::get_current();
		help->set_group("passes/status");

		auto content_root = help->get_root();

		content_root->usage("check_mem [selection]");
		content_root->paragraph(
			"This pass identifies the following problems in the current design: "
			"addressing invalid memory."
		);

		content_root->option("-non-const", "also check non-const address signals (may produce false-positives)");
		content_root->option("-assert", "produce a runtime error if any problems are found in the current design");

		return true;
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		int counter = 0;
		bool assert_mode = false;
		bool nonconst_mode = false;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-assert") {
				assert_mode = true;
				continue;
			}
			if (args[argidx] == "-non-const") {
				nonconst_mode = true;
				continue;
			}
			break;
		}

		extra_args(args, argidx, design);

		log_header(design, "Executing CHECK_MEM pass.\n");

		for (auto *module : design->selected_unboxed_modules_warn()) {
			for (auto mem : Mem::get_selected_memories(module)) {
				int min_addr = mem.mem->start_offset;
				int max_addr = mem.mem->size + min_addr - 1;
				for (auto &init : mem.inits) {
					int start = init.addr.as_int();
					if (start < min_addr) {
						log_warning("Mem %s.%s starts at %d but initializes address %d.\n", module, mem.mem, min_addr, start);
						counter++;
					}
					int end = start + (GetSize(init.data) / mem.width) - 1;
					if (end > max_addr) {
						log_warning("Mem %s.%s ends at %d but initializes address %d.\n", module, mem.mem, max_addr, end);
						counter++;
					}
				}

				auto check_addr = [min_addr, max_addr, &counter, module, &mem, &nonconst_mode](SigSpec &addr_sig, const char* access) {
					if (addr_sig.is_fully_const()) {
						auto addr = addr_sig.as_int();
						if (addr < min_addr || addr > max_addr) {
							log_warning("Mem %s.%s contains entries for addresses %d..%d but %s address %d.\n", module, mem.mem, min_addr, max_addr, access, addr);
							counter++;
						}
					} else if (nonconst_mode) {
						// TODO check addr_sig.has_const() for constant MSb/LSb that may change effective min/max
						// TODO consider sat solver for variable addresses
						int addr_sig_min = 0;
						int addr_sig_max = (1 << addr_sig.size()) - 1;
						if (min_addr > addr_sig_min || max_addr < addr_sig_max) {
							log_warning("Mem %s.%s contains entries for addresses %d..%d but has a potentially dangerous non-const input %s\n", module, mem.mem, min_addr, max_addr, log_signal(addr_sig));
							counter++;
						}
					}
				};

				// TODO test ABITS and WIDTH?
				// TODO can we limit ports via selection?
				for (auto &rd_port : mem.rd_ports)
					check_addr(rd_port.addr, "reads");
				for (auto &wr_port : mem.wr_ports)
					check_addr(wr_port.addr, "writes");
			}
		}

		if (assert_mode && counter > 0)
			log_error("Found %d problems in 'check_mem -assert'.\n", counter);
	}
} CheckMemPass;

PRIVATE_NAMESPACE_END
