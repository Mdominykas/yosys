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

struct AndOfMultipleWires : public Pass {
	AndOfMultipleWires() : Pass("and_of_multiple_wires", "Creates a wire named output_wire_name that is the output of and for all the wires named in the input_file") { }
	void help() override
	{
		log("\n");
		log("    and_of_multiple_wires wire_file output_wire_name\n");
        log("    In the wire_file each wire should be written in a separate line.\n");
        log("    The input file should begin with the number of wires. \n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing AND_OF_MULTIPLE_WIRES pass.\n");

        if(args.size() != 3){
			log_error("ERROR: Incorrect number of arguments");
		}

        std::string wire_file = args[1];
        std::string output_wire = args[2];

        if(design->selected_modules().size() > 1){
			log_error("ERROR: more that one module selected");
		}

        Module *mod = design->selected_modules()[0];


        std::vector<IdString> wire_ids;
        std::ifstream readFile(wire_file);
        int wire_cnt;
        readFile >> wire_cnt;

        if(wire_cnt <= 1){
            log_error("ERROR: file doesn't contain the required wires");
        }

        for(int i = 0; i < wire_cnt; i++){
            std::string cur_name;
            readFile >> cur_name;
            wire_ids.push_back(IdString(RTLIL::escape_id(cur_name)));
        }

        RTLIL::Wire *cum_and = mod->wire(wire_ids[0]);


        for(int i = 0; i < wire_cnt; i++){

            std::string current_output_name;
            if(i + 1 == wire_cnt){
                current_output_name = output_wire;
            }
            else{
                current_output_name = output_wire + "_partial_" + std::to_string(i);
            }

            RTLIL::IdString and_cell_name = RTLIL::escape_id("cell_for_"  + current_output_name);

            RTLIL::Cell *and_cell = mod->addCell(and_cell_name, IdString("$and"));

            and_cell->setPort(ID::A, cum_and);
			and_cell->setPort(ID::B, mod->wire(wire_ids[i]));

            RTLIL::Wire *out_wire = mod->addWire(RTLIL::escape_id(current_output_name));

			and_cell->setPort(ID::Y, out_wire);

            and_cell->setParam(ID::A_SIGNED, false);
            and_cell->setParam(ID::B_SIGNED, false);

            and_cell->setParam(ID::A_WIDTH, 1);
            and_cell->setParam(ID::B_WIDTH, 1);
            and_cell->setParam(ID::Y_WIDTH, 1);


            cum_and = out_wire;
        }

        

        

	}


} AndOfMultipleWires;

PRIVATE_NAMESPACE_END
