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



struct AddBitwiseNegation : public Pass {
	AddBitwiseNegation() : Pass("add_bitwise_negation", "Adds a wire that the negation of the input wire") { }
	void help() override
	{
		log("\n");
		log("    add_bitwise_negation <out_wirename> <in_wirename>\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing ADD_BITWISE_NEGATION pass.\n");

		if(args.size() < ((size_t) 3)){
			log_error("FAILURE: too few arguments for add_bitwise_negation");
			return;
		}

		std::string out_name = RTLIL::escape_id(args[1]);
		std::string in_name = RTLIL::escape_id(args[2]);

		if(design->selected_modules().size() > 1){
			log_warning("WARNING: more that one module selected");
		}


		for(auto mod : design->selected_modules()){
			RTLIL::IdString out_id = IdString(out_name);
			RTLIL::IdString in_id = IdString(in_name);

			RTLIL::Wire *in_wire = mod->wire(in_id);
			if(in_wire == nullptr){
				log_error("ERROR: wire with input wire name not found");
				return;
			}
			if(mod->wire(out_name) != nullptr){
				log_error("ERROR: found wire with out_wirename");
				return;
			}

			RTLIL::Wire *out_wire = mod->addWire(out_id, in_wire->width);


			std::string not_cell_name = RTLIL::escape_id(in_name + "_negation_to_" + out_name);
			if(mod->cell(not_cell_name) != nullptr){
				log_error("ERROR: due to name duplication can't create an equality cell");
				return;
			}

			RTLIL::Cell *not_cell = mod->addCell(not_cell_name, ID($not));
		
			not_cell->setPort(ID::A, in_wire);
			not_cell->setPort(ID::Y, out_wire);


			not_cell->setParam(ID::A_WIDTH, RTLIL::Const(in_wire->width));
			not_cell->setParam(ID::A_SIGNED, RTLIL::Const(1));
			not_cell->setParam(ID::Y_WIDTH, RTLIL::Const(1));
			

		}
		
	}


} AddBitwiseNegation;

PRIVATE_NAMESPACE_END
