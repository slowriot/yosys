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

// [[CITE]] Berkeley Logic Interchange Format (BLIF)
// University of California. Berkeley. July 28, 1992
// http://www.ece.cmu.edu/~ee760/760docs/blif.pdf

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct BlifDumperConfig
{
	bool icells_mode;
	bool conn_mode;
	bool impltf_mode;
	bool gates_mode;
	bool param_mode;

	std::string buf_type, buf_in, buf_out;
	std::string true_type, true_out, false_type, false_out;

	BlifDumperConfig() : icells_mode(false), conn_mode(false), impltf_mode(false), gates_mode(false), param_mode(false) { }
};

struct BlifDumper
{
	std::ostream &f;
	RTLIL::Module *module;
	RTLIL::Design *design;
	BlifDumperConfig *config;
	CellTypes ct;

	BlifDumper(std::ostream &f, RTLIL::Module *module, RTLIL::Design *design, BlifDumperConfig *config) :
			f(f), module(module), design(design), config(config), ct(design)
	{
	}

	std::vector<std::string> cstr_buf;

	const char *cstr(RTLIL::IdString id)
	{
		std::string str = RTLIL::unescape_id(id);
		for (size_t i = 0; i < str.size(); i++)
			if (str[i] == '#' || str[i] == '=')
				str[i] = '?';
		cstr_buf.push_back(str);
		return cstr_buf.back().c_str();
	}

	const char *cstr(RTLIL::SigBit sig)
	{
		if (sig.wire == NULL)
			return sig == RTLIL::State::S1 ?  "$true" : "$false";

		std::string str = RTLIL::unescape_id(sig.wire->name);
		for (size_t i = 0; i < str.size(); i++)
			if (str[i] == '#' || str[i] == '=')
				str[i] = '?';

		if (sig.wire->width != 1)
			str += stringf("[%d]", sig.offset);

		cstr_buf.push_back(str);
		return cstr_buf.back().c_str();
	}

	const char *subckt_or_gate(std::string cell_type)
	{
		if (!config->gates_mode)
			return "subckt";
		if (!design->modules_.count(RTLIL::escape_id(cell_type)))
			return "gate";
		if (design->modules_.at(RTLIL::escape_id(cell_type))->get_bool_attribute("\\blackbox"))
			return "gate";
		return "subckt";
	}

	void dump()
	{
		f << stringf("\n");
		f << stringf(".model %s\n", cstr(module->name));

		std::map<int, RTLIL::Wire*> inputs, outputs;

		for (auto &wire_it : module->wires_) {
			RTLIL::Wire *wire = wire_it.second;
			if (wire->port_input)
				inputs[wire->port_id] = wire;
			if (wire->port_output)
				outputs[wire->port_id] = wire;
		}

		f << stringf(".inputs");
		for (auto &it : inputs) {
			RTLIL::Wire *wire = it.second;
			for (int i = 0; i < wire->width; i++)
				f << stringf(" %s", cstr(RTLIL::SigSpec(wire, i)));
		}
		f << stringf("\n");

		f << stringf(".outputs");
		for (auto &it : outputs) {
			RTLIL::Wire *wire = it.second;
			for (int i = 0; i < wire->width; i++)
				f << stringf(" %s", cstr(RTLIL::SigSpec(wire, i)));
		}
		f << stringf("\n");

		if (!config->impltf_mode) {
			if (!config->false_type.empty())
				f << stringf(".%s %s %s=$false\n", subckt_or_gate(config->false_type),
						config->false_type.c_str(), config->false_out.c_str());
			else
				f << stringf(".names $false\n");
			if (!config->true_type.empty())
				f << stringf(".%s %s %s=$true\n", subckt_or_gate(config->true_type),
						config->true_type.c_str(), config->true_out.c_str());
			else
				f << stringf(".names $true\n1\n");
		}

		for (auto &cell_it : module->cells_)
		{
			RTLIL::Cell *cell = cell_it.second;

			if (!config->icells_mode && cell->type == "$_NOT_") {
				f << stringf(".names %s %s\n0 1\n",
						cstr(cell->getPort("\\A")), cstr(cell->getPort("\\Y")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$_AND_") {
				f << stringf(".names %s %s %s\n11 1\n",
						cstr(cell->getPort("\\A")), cstr(cell->getPort("\\B")), cstr(cell->getPort("\\Y")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$_OR_") {
				f << stringf(".names %s %s %s\n1- 1\n-1 1\n",
						cstr(cell->getPort("\\A")), cstr(cell->getPort("\\B")), cstr(cell->getPort("\\Y")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$_XOR_") {
				f << stringf(".names %s %s %s\n10 1\n01 1\n",
						cstr(cell->getPort("\\A")), cstr(cell->getPort("\\B")), cstr(cell->getPort("\\Y")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$_MUX_") {
				f << stringf(".names %s %s %s %s\n1-0 1\n-11 1\n",
						cstr(cell->getPort("\\A")), cstr(cell->getPort("\\B")),
						cstr(cell->getPort("\\S")), cstr(cell->getPort("\\Y")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$_DFF_N_") {
				f << stringf(".latch %s %s fe %s\n",
						cstr(cell->getPort("\\D")), cstr(cell->getPort("\\Q")), cstr(cell->getPort("\\C")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$_DFF_P_") {
				f << stringf(".latch %s %s re %s\n",
						cstr(cell->getPort("\\D")), cstr(cell->getPort("\\Q")), cstr(cell->getPort("\\C")));
				continue;
			}

			if (!config->icells_mode && cell->type == "$lut") {
				f << stringf(".names");
				auto &inputs = cell->getPort("\\A");
				auto width = cell->parameters.at("\\WIDTH").as_int();
				log_assert(inputs.size() == width);
				for (int i = 0; i < inputs.size(); i++) {
					f << stringf(" %s", cstr(inputs.extract(i, 1)));
				}
				auto &output = cell->getPort("\\Y");
				log_assert(output.size() == 1);
				f << stringf(" %s", cstr(output));
				f << stringf("\n");
				auto mask = cell->parameters.at("\\LUT").as_string();
				for (int i = 0; i < (1 << width); i++) {
					if (mask[i] == '0') continue;
					for (int j = width-1; j >= 0; j--) {
						f << ((i>>j)&1 ? '1' : '0');
					}
					f << stringf(" %c\n", mask[i]);
				}
				continue;
			}

			f << stringf(".%s %s", subckt_or_gate(cell->type.str()), cstr(cell->type));
			for (auto &conn : cell->connections())
			for (int i = 0; i < conn.second.size(); i++) {
				if (conn.second.size() == 1)
					f << stringf(" %s", cstr(conn.first));
				else
					f << stringf(" %s[%d]", cstr(conn.first), i);
				f << stringf("=%s", cstr(conn.second.extract(i, 1)));
			}
			f << stringf("\n");

			if (config->param_mode)
				for (auto &param : cell->parameters) {
					f << stringf(".param %s ", RTLIL::id2cstr(param.first));
					if (param.second.flags & RTLIL::CONST_FLAG_STRING) {
						std::string str = param.second.decode_string();
						f << stringf("\"");
						for (char ch : str)
							if (ch == '"' || ch == '\\')
								f << stringf("\\%c", ch);
							else if (ch < 32 || ch >= 127)
								f << stringf("\\%03o", ch);
							else
								f << stringf("%c", ch);
						f << stringf("\"\n");
					} else
						f << stringf("%s\n", param.second.as_string().c_str());
				}
		}

		for (auto &conn : module->connections())
		for (int i = 0; i < conn.first.size(); i++)
			if (config->conn_mode)
				f << stringf(".conn %s %s\n", cstr(conn.second.extract(i, 1)), cstr(conn.first.extract(i, 1)));
			else if (!config->buf_type.empty())
				f << stringf(".%s %s %s=%s %s=%s\n", subckt_or_gate(config->buf_type), config->buf_type.c_str(), config->buf_in.c_str(), cstr(conn.second.extract(i, 1)),
						config->buf_out.c_str(), cstr(conn.first.extract(i, 1)));
			else
				f << stringf(".names %s %s\n1 1\n", cstr(conn.second.extract(i, 1)), cstr(conn.first.extract(i, 1)));


		f << stringf(".end\n");
	}

	static void dump(std::ostream &f, RTLIL::Module *module, RTLIL::Design *design, BlifDumperConfig &config)
	{
		BlifDumper dumper(f, module, design, &config);
		dumper.dump();
	}
};

struct BlifBackend : public Backend {
	BlifBackend() : Backend("blif", "write design to BLIF file") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_blif [options] [filename]\n");
		log("\n");
		log("Write the current design to an BLIF file.\n");
		log("\n");
		log("    -top top_module\n");
		log("        set the specified module as design top module\n");
		log("\n");
		log("    -buf <cell-type> <in-port> <out-port>\n");
		log("        use cells of type <cell-type> with the specified port names for buffers\n");
		log("\n");
		log("    -true <cell-type> <out-port>\n");
		log("    -false <cell-type> <out-port>\n");
		log("        use the specified cell types to drive nets that are constant 1 or 0\n");
		log("\n");
		log("The following options can be useful when the generated file is not going to be\n");
		log("read by a BLIF parser but a custom tool. It is recommended to not name the output\n");
		log("file *.blif when any of this options is used.\n");
		log("\n");
		log("    -icells\n");
		log("        do not translate Yosys's internal gates to generic BLIF logic\n");
		log("        functions. Instead create .subckt or .gate lines for all cells.\n");
		log("\n");
		log("    -gates\n");
		log("        print .gate instead of .subckt lines for all cells that are not\n");
		log("        instantiations of other modules from this design.\n");
		log("\n");
		log("    -conn\n");
		log("        do not generate buffers for connected wires. instead use the\n");
		log("        non-standard .conn statement.\n");
		log("\n");
		log("    -param\n");
		log("        use the non-standard .param statement to write module parameters\n");
		log("\n");
		log("    -impltf\n");
		log("        do not write definitions for the $true and $false wires.\n");
		log("\n");
	}
	virtual void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design)
	{
		std::string top_module_name;
		std::string buf_type, buf_in, buf_out;
		std::string true_type, true_out;
		std::string false_type, false_out;
		BlifDumperConfig config;

		log_header("Executing BLIF backend.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-top" && argidx+1 < args.size()) {
				top_module_name = args[++argidx];
				continue;
			}
			if (args[argidx] == "-buf" && argidx+3 < args.size()) {
				config.buf_type = args[++argidx];
				config.buf_in = args[++argidx];
				config.buf_out = args[++argidx];
				continue;
			}
			if (args[argidx] == "-true" && argidx+2 < args.size()) {
				config.true_type = args[++argidx];
				config.true_out = args[++argidx];
				continue;
			}
			if (args[argidx] == "-false" && argidx+2 < args.size()) {
				config.false_type = args[++argidx];
				config.false_out = args[++argidx];
				continue;
			}
			if (args[argidx] == "-icells") {
				config.icells_mode = true;
				continue;
			}
			if (args[argidx] == "-gates") {
				config.gates_mode = true;
				continue;
			}
			if (args[argidx] == "-conn") {
				config.conn_mode = true;
				continue;
			}
			if (args[argidx] == "-param") {
				config.param_mode = true;
				continue;
			}
			if (args[argidx] == "-impltf") {
				config.impltf_mode = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		if (top_module_name.empty())
			for (auto & mod_it:design->modules_)
				if (mod_it.second->get_bool_attribute("\\top"))
					top_module_name = mod_it.first.str();

		*f << stringf("# Generated by %s\n", yosys_version_str);

		std::vector<RTLIL::Module*> mod_list;

		for (auto module_it : design->modules_)
		{
			RTLIL::Module *module = module_it.second;
			if (module->get_bool_attribute("\\blackbox"))
				continue;

			if (module->processes.size() != 0)
				log_error("Found unmapped processes in module %s: unmapped processes are not supported in BLIF backend!\n", RTLIL::id2cstr(module->name));
			if (module->memories.size() != 0)
				log_error("Found munmapped emories in module %s: unmapped memories are not supported in BLIF backend!\n", RTLIL::id2cstr(module->name));

			if (module->name == RTLIL::escape_id(top_module_name)) {
				BlifDumper::dump(*f, module, design, config);
				top_module_name.clear();
				continue;
			}

			mod_list.push_back(module);
		}

		if (!top_module_name.empty())
			log_error("Can't find top module `%s'!\n", top_module_name.c_str());

		for (auto module : mod_list)
			BlifDumper::dump(*f, module, design, config);
	}
} BlifBackend;

PRIVATE_NAMESPACE_END
