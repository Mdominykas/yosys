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

// TODO: rename this file to something like utils and add a lot of useful utils

// here the convention is that inputs to the predictor have a prefix inp
IdString remove_input_from_wire_name(IdString wire_name){
	std::string inp_pref = "inp_";
	std::string name_without_inp = RTLIL::unescape_id(wire_name).substr(inp_pref.size());
	return IdString(RTLIL::escape_id(name_without_inp));
}

IdString add_input_to_wire_name(IdString wire_name){
	std::string inp_pref = "inp_";
	return RTLIL::escape_id(inp_pref + RTLIL::unescape_id(wire_name.str()));
}

// All IdString must have already escaped with '\\'
void add_predictor_to_mod(Module *mod, Module *predictor, IdString observation_in_predictor, IdString observation_in_mod, IdString applicability_in_predictor, IdString applicability_in_mod){
	predictor->fixup_ports();

	Cell* predictor_cell = mod->addCell(mod->uniquify(RTLIL::escape_id("predictor_cell")), predictor->name);
	vector<Wire*> input_wires;
	for(Wire *wire : predictor->wires()){
		if(wire->port_input){
			input_wires.push_back(wire);
		}
	}

	// set the input wires
	for(Wire *wire : input_wires){
		IdString pred_input_name = wire->name;
		IdString input_name = remove_input_from_wire_name(pred_input_name);

		predictor_cell->setPort(pred_input_name, SigSpec(mod->wire(input_name)));
	}

	// set the observation
	Wire *obs = mod->addWire(observation_in_mod, predictor->wire(observation_in_predictor));
	predictor_cell->setPort(observation_in_predictor, SigSpec(obs));
	obs->port_output = true;

	// set the applicability
	Wire *applicability = mod->addWire(applicability_in_mod, predictor->wire(applicability_in_predictor));
	predictor_cell->setPort(applicability_in_predictor, applicability);
	applicability->port_output = true;

	mod->fixup_ports();
}

void make_wire_input(Module *predictor, Wire *wire, IdString new_wire_name){
	assert(wire != nullptr);
	IdString input_wire_name = add_input_to_wire_name(new_wire_name);
	Wire *input_wire = predictor->addWire(input_wire_name, wire);
	std::cout << "pridedu nauja wire su pavadinimu: " << input_wire_name.str() << std::endl;
	input_wire->port_input = true;

	// these are difficult to deal with, but may give some optimizations
	assert((predictor->processes.empty()) && (predictor->memories.empty()));

	for(Cell *cell : predictor->cells()){
		for(auto [name, sigSpec] : cell->connections()){
			vector<SigChunk> new_chunks;
			for(auto chunk : sigSpec.chunks()){
				if((chunk.is_wire()) && (chunk.wire->name == wire->name) && (cell->input(name))){
					std::cout << "previous chunk wire was: " << chunk.wire->name.str() << std::endl;
					chunk.wire = input_wire;
				}
				new_chunks.push_back(chunk);
			}
			cell->setPort(name, new_chunks);
		}
	}

	// a check that the wires were really changed
	for(Cell *cell : predictor->cells()){
		for(auto [name, sigSpec] : cell->connections()){
			for(auto chunk : sigSpec.chunks()){
				if((chunk.is_wire()) && (chunk.wire->name == wire->name) && (cell->input(name))){
					std::cout << "now chunk is called: " << chunk.wire->name.str() << std::endl;
					assert(false);
				}
			}
		}
	}

	predictor->fixup_ports();
}

PRIVATE_NAMESPACE_END
