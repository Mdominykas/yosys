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

#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN



struct AddLessOrEqualWire : public Pass {
	AddLessOrEqualWire() : Pass("add_less_or_equal_wire", "Adds a wire that is the logical outcome of comparing wire_a <= wire_b") { }
	void help() override
	{
		log("\n");
		log("    add_less_or_equal_wire <out_wirename> <wire_a> <wire_b>\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing ADD_LESS_OR_EQUAL_WIRE pass.\n");

		if(args.size() < ((size_t) 4)){
			log_error("FAILURE: too few arguments for add_less_or_equal_wire");
			return;
		}

		std::string out_name = RTLIL::escape_id(args[1]);
		std::string wire_a_name = RTLIL::escape_id(args[2]);
		std::string wire_b_name = RTLIL::escape_id(args[3]);

		if(design->selected_modules().size() > 1){
			log_warning("WARNING: more that one module selected");
		}


		for(auto mod : design->selected_modules()){
			RTLIL::Wire *wire_a = mod->wire(wire_a_name);
			RTLIL::Wire *wire_b = mod->wire(wire_b_name);
			if(wire_a == nullptr){
				log_error("Input wire_a named %s not found", wire_a_name);
				return;
			}
			if(wire_b == nullptr){
				log_error("Input wire_b named %s not found", wire_b_name);
				return;
			}
			if (mod->wire(out_name) != nullptr) {
				log_error("Found output wire with named %s ", out_name);
				return;
			}

			assert(wire_a->width == wire_b->width);

			RTLIL::Wire *out_wire = mod->addWire(out_name, 1);

			std::string out_cell_name = RTLIL::escape_id(RTLIL::unescape_id(wire_a_name) + "_comparing_less_or_equal_with" + RTLIL::unescape_id(wire_b_name) + "_gives_" + RTLIL::unescape_id(out_name));
			if(mod->cell(out_cell_name) != nullptr){
				log_error("Due to name duplication can't create a comparison cell");
				return;
			}

			RTLIL::Cell *out_cell = mod->addCell(out_cell_name, ID($le));
		
			out_cell->setPort(ID::A, wire_a);
			out_cell->setPort(ID::B, wire_b);
			out_cell->setPort(ID::Y, out_wire);

			out_cell->setParam(ID::A_SIGNED, RTLIL::Const(0));
			out_cell->setParam(ID::B_SIGNED, RTLIL::Const(0));

			out_cell->setParam(ID::A_WIDTH, RTLIL::Const(wire_a->width));
			out_cell->setParam(ID::B_WIDTH, RTLIL::Const(wire_b->width));
			out_cell->setParam(ID::Y_WIDTH, RTLIL::Const(1));
			

		}
		
	}


} AddBitwiseNegation;

PRIVATE_NAMESPACE_END
