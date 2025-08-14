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

bool stringToInt(const std::string &s, int &result) {
    try {
        size_t pos;
        result = std::stoi(s, &pos);

        if (pos != s.size()) {
            return false;
        }
        return true;
    } 
    catch (const std::invalid_argument &) {
        return false;
    } 
    catch (const std::out_of_range &) {
        return false;
    }
}


struct AddEqualityCheck : public Pass {
	AddEqualityCheck() : Pass("add_equality_check", "Adds a wire that is only true if both of the given wires have the same value") { }
	void help() override
	{
		log("\n");
		log("    add_equality_check <out_wirename> <left_wire> <right_wire> --signed\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing ADD_EQUALITY_CHECK pass.\n");

		if(args.size() < ((size_t) 4)){
			log_error("FAILURE: too few arguments for add_equality_check");
			return;
		}

		int is_signed = 0;
		std::string out_name = RTLIL::escape_id(args[1]);
		std::string l_name = RTLIL::escape_id(args[2]);
		std::string r_name = RTLIL::escape_id(args[3]);


		if(design->selected_modules().size() > 1){
			log_warning("WARNING: more that one module selected");
		}

		string is_signed_str = "--signed";

		for(int i = 1; i <= 3; i++){
			if((args[i] == is_signed_str)){
				log_error("FAILURE: signed placed in wrong location");
				return;
			}
		}

		for(size_t i = 4; i < args.size(); i++){
			if(args[i] == is_signed_str){
				is_signed = 1;
			}
		}

		for(auto mod : design->selected_modules()){
			RTLIL::IdString l_id = IdString(l_name);
			RTLIL::IdString r_id = IdString(r_name);
			RTLIL::IdString out_id = IdString(out_name);

			RTLIL::Wire *l_wire = mod->wire(l_id);
			RTLIL::Wire *r_wire = mod->wire(r_id);

			if(l_wire->width !=r_wire->width){
				log_error("FAILURE: The two compared wires have different width");
			}
			int wire_width = l_wire->width;


			if(l_wire == nullptr){
				log_error("ERROR: wire with left name not found");
				return;
			}
			if(r_wire == nullptr){
				log_error("ERROR: wire with right name not found");
				return;
			}

			if(mod->wire(out_name) != nullptr){
				log_error("ERROR: found wire with out_wirename");
				return;
			}

			RTLIL::Wire *out_wire = mod->addWire(out_id);

			std::string eq_cell_name = RTLIL::escape_id(out_name + "_eq_constraint");
			RTLIL::IdString eq_id = IdString(eq_cell_name);
			if(mod->cell(eq_cell_name) != nullptr){
				log_error("ERROR: due to name duplication can't create an equality cell");
				return;
			}

			RTLIL::Cell *eq_cell = mod->addCell(eq_cell_name, IdString("$eq"));
		
			eq_cell->setPort(ID::A, l_wire);
			eq_cell->setPort(ID::B, r_wire);
			eq_cell->setPort(ID::Y, out_wire);


			eq_cell->setParam(ID::A_WIDTH, RTLIL::Const(wire_width));
			eq_cell->setParam(ID::A_SIGNED, RTLIL::Const(is_signed));

			eq_cell->setParam(ID::B_WIDTH, RTLIL::Const(wire_width));
			eq_cell->setParam(ID::B_SIGNED, RTLIL::Const(is_signed));
			
			eq_cell->setParam(ID::Y_WIDTH, RTLIL::Const(1));

		}
		
	}


} AddEqualityCheck;

PRIVATE_NAMESPACE_END
