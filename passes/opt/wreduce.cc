/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
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
#include "kernel/modtools.h"

USING_YOSYS_NAMESPACE
using namespace RTLIL;

PRIVATE_NAMESPACE_BEGIN

struct WreduceConfig
{
	std::set<IdString> supported_cell_types;

	WreduceConfig()
	{
		supported_cell_types = {
			"$not", "$pos", "$neg",
			"$and", "$or", "$xor", "$xnor",
			"$shl", "$shr", "$sshl", "$sshr", "$shift", "$shiftx",
			"$lt", "$le", "$eq", "$ne", "$eqx", "$nex", "$ge", "$gt",
			"$add", "$sub", // "$mul", "$div", "$mod", "$pow",
			"$mux", "$pmux"
		};
	}
};

struct WreduceWorker
{
	WreduceConfig *config;
	Module *module;
	ModIndex mi;

	std::set<Cell*, IdString::compare_ptr_by_name<Cell>> work_queue_cells;
	std::set<SigBit> work_queue_bits;

	WreduceWorker(WreduceConfig *config, Module *module) :
			config(config), module(module), mi(module) { }

	void run_cell_mux(Cell *cell)
	{
		// Reduce size of MUX if inputs agree on a value for a bit or a output bit is unused

		SigSpec sig_a = mi.sigmap(cell->getPort("\\A"));
		SigSpec sig_b = mi.sigmap(cell->getPort("\\B"));
		SigSpec sig_s = mi.sigmap(cell->getPort("\\S"));
		SigSpec sig_y = mi.sigmap(cell->getPort("\\Y"));
		std::vector<SigBit> bits_removed;

		for (int i = SIZE(sig_y)-1; i >= 0; i--)
		{
			auto info = mi.query(sig_y[i]);
			if (!info->is_output && SIZE(info->ports) <= 1) {
				bits_removed.push_back(Sx);
				continue;
			}

			SigBit ref = sig_a[i];
			for (int k = 0; k < SIZE(sig_s); k++) {
				if (ref != Sx && sig_b[k*SIZE(sig_a) + i] != Sx && ref != sig_b[k*SIZE(sig_a) + i])
					goto no_match_ab;
				if (sig_b[k*SIZE(sig_a) + i] != Sx)
					ref = sig_b[k*SIZE(sig_a) + i];
			}
			if (0)
		no_match_ab:
				break;
			bits_removed.push_back(ref);
		}

		if (bits_removed.empty())
			return;

		SigSpec sig_removed;
		for (int i = SIZE(bits_removed)-1; i >= 0; i--)
			sig_removed.append_bit(bits_removed[i]);

		if (SIZE(bits_removed) == SIZE(sig_y)) {
			log("Removed cell %s.%s (%s).\n", log_id(module), log_id(cell), log_id(cell->type));
			module->connect(sig_y, sig_removed);
			module->remove(cell);
			return;
		}

		log("Removed top %d bits (of %d) from mux cell %s.%s (%s).\n",
				SIZE(sig_removed), SIZE(sig_y), log_id(module), log_id(cell), log_id(cell->type));

		int n_removed = SIZE(sig_removed);
		int n_kept = SIZE(sig_y) - SIZE(sig_removed);

		SigSpec new_work_queue_bits;
		new_work_queue_bits.append(sig_a.extract(n_kept, n_removed));
		new_work_queue_bits.append(sig_y.extract(n_kept, n_removed));

		SigSpec new_sig_a = sig_a.extract(0, n_kept);
		SigSpec new_sig_y = sig_y.extract(0, n_kept);
		SigSpec new_sig_b;

		for (int k = 0; k < SIZE(sig_s); k++) {
			new_sig_b.append(sig_b.extract(k*SIZE(sig_a), n_kept));
			new_work_queue_bits.append(sig_b.extract(k*SIZE(sig_a) + n_kept, n_removed));
		}

		for (auto bit : new_work_queue_bits)
			work_queue_bits.insert(bit);

		cell->setPort("\\A", new_sig_a);
		cell->setPort("\\B", new_sig_b);
		cell->setPort("\\Y", new_sig_y);
		cell->fixup_parameters();

		module->connect(sig_y.extract(n_kept, n_removed), sig_removed);
	}

	void run_reduce_inport(Cell *cell, char port, int max_port_size, bool &port_signed, bool &did_something)
	{
		port_signed = cell->getParam(stringf("\\%c_SIGNED", port)).as_bool();
		SigSpec sig = mi.sigmap(cell->getPort(stringf("\\%c", port)));

		if (port == 'B' && cell->type.in("$shl", "$shr", "$sshl", "$sshr"))
			port_signed = false;

		int bits_removed = 0;
		if (SIZE(sig) > max_port_size) {
			bits_removed = SIZE(sig) - max_port_size;
			for (auto bit : sig.extract(max_port_size, bits_removed))
				work_queue_bits.insert(bit);
			sig = sig.extract(0, max_port_size);
		}

		if (port_signed) {
			while (SIZE(sig) > 1 && sig[SIZE(sig)-1] == sig[SIZE(sig)-2])
				work_queue_bits.insert(sig[SIZE(sig)-1]), sig.remove(SIZE(sig)-1), bits_removed++;
		} else {
			while (SIZE(sig) > 1 && sig[SIZE(sig)-1] == S0)
				work_queue_bits.insert(sig[SIZE(sig)-1]), sig.remove(SIZE(sig)-1), bits_removed++;
		}

		if (bits_removed) {
			log("Removed top %d bits (of %d) from port %c of cell %s.%s (%s).\n",
					bits_removed, SIZE(sig) + bits_removed, port, log_id(module), log_id(cell), log_id(cell->type));
			cell->setPort(stringf("\\%c", port), sig);
			did_something = true;
		}
	}

	void run_cell(Cell *cell)
	{
		bool did_something = false;

		if (!cell->type.in(config->supported_cell_types))
			return;

		if (cell->type.in("$mux", "$pmux"))
			return run_cell_mux(cell);


		// Reduce size of ports A and B based on constant input bits and size of output port

		int max_port_a_size = cell->hasPort("\\A") ? SIZE(cell->getPort("\\A")) : -1;
		int max_port_b_size = cell->hasPort("\\B") ? SIZE(cell->getPort("\\B")) : -1;

		if (cell->type.in("$not", "$pos", "$neg", "$and", "$or", "$xor", "$add", "$sub")) {
			max_port_a_size = std::min(max_port_a_size, SIZE(cell->getPort("\\Y")));
			max_port_b_size = std::min(max_port_b_size, SIZE(cell->getPort("\\Y")));
		}

		bool port_a_signed = false;
		bool port_b_signed = false;

		if (max_port_a_size >= 0)
			run_reduce_inport(cell, 'A', max_port_a_size, port_a_signed, did_something);

		if (max_port_b_size >= 0)
			run_reduce_inport(cell, 'B', max_port_b_size, port_b_signed, did_something);


		// Reduce size of port Y based on sizes for A and B and unused bits in Y

		SigSpec sig = mi.sigmap(cell->getPort("\\Y"));

		int bits_removed = 0;
		if (port_a_signed && cell->type == "$shr") {
			// do not reduce size of output on $shr cells with signed A inputs
		} else {
			while (SIZE(sig) > 0)
			{
				auto info = mi.query(sig[SIZE(sig)-1]);

				if (info->is_output || SIZE(info->ports) > 1)
					break;

				sig.remove(SIZE(sig)-1);
				bits_removed++;
			}
		}

		if (cell->type.in("$pos", "$add", "$mul", "$and", "$or", "$xor"))
		{
			bool is_signed = cell->getParam("\\A_SIGNED").as_bool();

			int a_size = 0, b_size = 0;
			if (cell->hasPort("\\A")) a_size = SIZE(cell->getPort("\\A"));
			if (cell->hasPort("\\B")) b_size = SIZE(cell->getPort("\\B"));

			int max_y_size = std::max(a_size, b_size);

			if (cell->type == "$add")
				max_y_size++;

			if (cell->type == "$mul")
				max_y_size = a_size + b_size;

			while (SIZE(sig) > 1 && SIZE(sig) > max_y_size) {
				module->connect(sig[SIZE(sig)-1], is_signed ? sig[SIZE(sig)-2] : S0);
				sig.remove(SIZE(sig)-1);
				bits_removed++;
			}
		}

		if (SIZE(sig) == 0) {
			log("Removed cell %s.%s (%s).\n", log_id(module), log_id(cell), log_id(cell->type));
			module->remove(cell);
			return;
		}

		if (bits_removed) {
			log("Removed top %d bits (of %d) from port Y of cell %s.%s (%s).\n",
					bits_removed, SIZE(sig) + bits_removed, log_id(module), log_id(cell), log_id(cell->type));
			cell->setPort("\\Y", sig);
			did_something = true;
		}

		if (did_something) {
			cell->fixup_parameters();
			run_cell(cell);
		}
	}

	static int count_nontrivial_wire_attrs(RTLIL::Wire *w)
	{
		int count = w->attributes.size();
		count -= w->attributes.count("\\src");
		count -= w->attributes.count("\\unused_bits");
		return count;
	}

	void run()
	{
		for (auto c : module->selected_cells())
			work_queue_cells.insert(c);

		while (!work_queue_cells.empty())
		{
			work_queue_bits.clear();
			for (auto c : work_queue_cells)
				run_cell(c);

			work_queue_cells.clear();
			for (auto bit : work_queue_bits)
			for (auto port : mi.query_ports(bit))
				if (module->selected(port.cell))
					work_queue_cells.insert(port.cell);
		}

		for (auto w : module->selected_wires())
		{
			int unused_top_bits = 0;

			if (w->port_id > 0 || count_nontrivial_wire_attrs(w) > 0)
				continue;

			for (int i = SIZE(w)-1; i >= 0; i--) {
				SigBit bit(w, i);
				auto info = mi.query(bit);
				if (info && (info->is_input || info->is_output || SIZE(info->ports) > 0))
					break;
				unused_top_bits++;
			}

			if (0 < unused_top_bits && unused_top_bits < SIZE(w)) {
				log("Removed top %d bits (of %d) from wire %s.%s.\n", unused_top_bits, SIZE(w), log_id(module), log_id(w));
				Wire *nw = module->addWire(NEW_ID, w);
				nw->width = SIZE(w) - unused_top_bits;
				module->connect(nw, SigSpec(w).extract(0, SIZE(nw)));
				module->swap_names(w, nw);
			}
		}
	}
};

struct WreducePass : public Pass {
	WreducePass() : Pass("wreduce", "reduce the word size of operations is possible") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    wreduce [options] [selection]\n");
		log("\n");
		log("This command reduces the word size of operations. For example it will replace\n");
		log("the 32 bit adders in the following code with adders of more appropriate widths:\n");
		log("\n");
		log("    module test(input [3:0] a, b, c, output [7:0] y);\n");
		log("        assign y = a + b + c + 1;\n");
		log("    endmodule\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, Design *design)
	{
		WreduceConfig config;

		log_header("Executing WREDUCE pass (reducing word size of cells).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules())
		{
			if (module->has_processes_warn())
				continue;

			WreduceWorker worker(&config, module);
			worker.run();
		}
	}
} WreducePass;

PRIVATE_NAMESPACE_END
