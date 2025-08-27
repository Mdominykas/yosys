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

struct CreateProductCircuit : public Pass {
	CreateProductCircuit() : Pass("create_product_circuit", "Creates two modules and adds a suffix \"left\" or \"right\" to each part of the product (unless other names are provided)") { }
	void help() override
	{
		log("\n");
		log("    create_product_circuit [--left <left_name>] [--right <right_name>]\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing CREATE_PRODUCT_CIRCUIT pass.\n");

        std::string left_name = "left", right_name = "right";

		for (size_t argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "--left") {
                if(argidx + 1 == args.size()){
                    log_error("FAILURE: noe argument for left name");
    				return;
                }
				left_name = args[argidx + 1];
				continue;
			}
			else if (args[argidx] == "--right") {
                if(argidx + 1 == args.size()){
                    log_error("FAILURE: no argument for the right name");
    				return;
                }
				right_name = args[argidx + 1];
				continue;
			}
			break;
		}

		std::string left_suffix = "_" + left_name, right_suffix = "_" + right_name;

        if(design->selected_modules().size() > 1){
			log_error("ERROR: more that one module selected");
		}

        Module *mod = design->selected_modules()[0];

    	log_assert(!mod->has_memories());
	    log_assert(!mod->has_processes());

		for(std::string suffix : {left_suffix, right_suffix}){
			Module *prod_module = new Module();
			prod_module->name = IdString(RTLIL::escape_id(mod->name.str() + suffix));
			

			for (auto wire : mod->wires()){
				auto new_name = IdString(wire->name.str() + suffix);
				auto new_wire = prod_module->addWire(new_name, wire->width);
				
				// I am not too sure if you need to copy these or if it sufficient to copy these
				new_wire->width = wire->width;
				new_wire->start_offset = wire->start_offset;
				new_wire->port_id = wire->port_id;
				new_wire->port_input = wire->port_input;
				new_wire->port_output = wire->port_output;
				new_wire->upto = wire->upto;
				new_wire->is_signed = wire->is_signed;

				new_wire->attributes = wire->attributes;
			}

			for (auto cell : mod->cells()){

				auto new_name = IdString(cell->name.str() + suffix);
				Cell *new_cell = prod_module->addCell(new_name, cell->type);

				new_cell->parameters = cell->parameters;
				new_cell->attributes = cell->attributes;

				for(std::pair<IdString, SigSpec> it : cell->connections()){
					IdString name = it.first;
					SigSpec sig = it.second;
					std::vector<RTLIL::SigBit> sig_bits;
					for(SigBit bit : sig.bits()){
						if(bit.wire != NULL){
							bit.wire = prod_module->wire(IdString(bit.wire->name.str() + suffix));
						}
						sig_bits.push_back(bit);
					}
					new_cell->setPort(name, SigSpec(sig_bits));
				}
			}

			for(SigSig it : mod->connections()){
				SigSpec sig1 = it.first;
				std::vector<RTLIL::SigBit> sig1_bits;
				for(SigBit bit : sig1.bits()){
					if(bit.wire != NULL){
						bit.wire = prod_module->wire(IdString(bit.wire->name.str() + suffix));
					}
					sig1_bits.push_back(bit);
				}
				sig1 = SigSpec(sig1_bits);

				SigSpec sig2 = it.second;
				std::vector<RTLIL::SigBit> sig2_bits;
				for(SigBit bit : sig2.bits()){
					if(bit.wire != NULL){
						bit.wire = prod_module->wire(IdString(bit.wire->name.str() + suffix));
					}
					sig2_bits.push_back(bit);
				}
				sig2 = SigSpec(sig2_bits);
				
				prod_module->connect(sig1, sig2);
				
			}

			prod_module->fixup_ports();
			design->add(prod_module);
		}

		Module *main_module = new Module();
		Cell *left_cell = main_module->addCell(RTLIL::escape_id("leftie" + left_suffix), mod->name.str() + left_suffix);
		Cell *right_cell = main_module->addCell(RTLIL::escape_id("rightie" + right_suffix), mod->name.str() + right_suffix);

		// construction of a common module
		for(Wire *wire : mod->wires()){
			if(wire->port_input){
				IdString new_name = wire->name;
				auto new_wire = main_module->addWire(new_name, wire->width);
				
				// I am not too sure if you need to copy these or if it sufficient to copy these
				new_wire->width = wire->width;
				new_wire->start_offset = wire->start_offset;
				new_wire->port_id = wire->port_id;
				new_wire->port_input = wire->port_input;
				new_wire->port_output = wire->port_output;
				new_wire->upto = wire->upto;
				new_wire->is_signed = wire->is_signed;

				new_wire->attributes = wire->attributes;

				left_cell->setPort(wire->name.str() + left_suffix, SigSpec(new_wire));
				right_cell->setPort(wire->name.str() + right_suffix, SigSpec(new_wire));
			}
		}



		main_module->fixup_ports();
		// TODO: check if you can use the same name
		main_module->name = IdString(RTLIL::escape_id("main_" + RTLIL::unescape_id(mod->name.str())));
		design->remove(mod);

		design->add(main_module);
	}


} CreateProductCircuit;

PRIVATE_NAMESPACE_END
