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

struct PreservePastValues : public Pass {
	PreservePastValues() : Pass("preserve_past_values", "Keeps the value of a wire/register in flip-flop cells for some number of  cycles") { }
	void help() override
	{
		log("\n");
		log("    preserve_past_values <name_of_object> <number_of_cycles>\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing a PRESERVE_PAST_VALUES pass.\n");

		if(args.size() != ((size_t) 3)){
			log_error("FAILURE: Incorrect number of arguments for preserve_past_values");
			return;
		}

		std::string component_name = RTLIL::escape_id(args[1]);
		RTLIL::IdString component_id = IdString(component_name);

		char *endptr;
        errno = 0;
        int number_of_cycles = strtol(args[2].c_str(), &endptr, 10);
        
        if((endptr == args[2].c_str()) || (*endptr != '\0') || (errno == ERANGE)){
            log_error("Incorrectly parsed number of cycles");
        }

		if(design->selected_modules().size() > 1){
			log_warning("WARNING: more that one module selected");
		}

		for(auto mod : design->selected_modules()){
			RTLIL::Wire *wire = mod->wire(RTLIL::escape_id(component_name));
			if (wire == nullptr) {
				const std::string error_msg = "FAILURE: Wire with name \"" + RTLIL::escape_id(component_name) + "\" not found";
	            log_error("%s", error_msg.c_str());
				return;
			}
			int wire_width = wire->width;

			RTLIL::Wire *previous_wire = wire;

			for(int i = 0; i < number_of_cycles; i++){
				std::string flip_flop_cell_name = RTLIL::escape_id(component_name + "_delay_cell_" + std::to_string(i + 1));
				RTLIL::IdString ff_cell_id = IdString(flip_flop_cell_name);

				if(mod->cell(ff_cell_id) != nullptr){
					log_error("Due to name duplication can't create a cell for delay");
					return;
				}

				RTLIL::Cell *flip_flop_cell = mod->addCell(ff_cell_id, ID($ff));
				flip_flop_cell->parameters["\\WIDTH"] = RTLIL::Const(wire_width);

				flip_flop_cell->setPort(ID::D, previous_wire);

				std::string flip_flop_out_wire_name = RTLIL::escape_id(component_name + "_delayed_for_" + std::to_string(i + 1));
				RTLIL::IdString ff_out_id = IdString(flip_flop_out_wire_name);
				if(mod->wire(ff_out_id) != nullptr){
					log_error("Due to name duplication couldn't create a wire");
					return;
				}
				RTLIL::Wire *ff_out_wire = mod->addWire(ff_out_id, wire_width);

				flip_flop_cell->setPort(ID::Q, ff_out_wire);

				previous_wire = ff_out_wire;
			}
		}
		
	}


} HoldInMemory;

PRIVATE_NAMESPACE_END
