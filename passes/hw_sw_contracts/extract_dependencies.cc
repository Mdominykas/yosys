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

struct ExtractDependencies : public Pass {
	ExtractDependencies() : Pass("extract_dependencies", "Finds all the cells that are influencing that wire and puts them in a separate module") { }
	void help() override
	{
		log("\n");
		log("    extract_dependencies important_wire hist_len\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing EXTRACT_DEPENDENCIES pass.\n");

        if(args.size() != 3){
			log_error("ERROR: Incorrect number of arguments");
		}

        std::string wire_name = args[1];

        char *endptr;
        errno = 0;
        int hist_len = strtol(args[2].c_str(), &endptr, 10);
        
        if((endptr == args[2].c_str()) || (*endptr != '\0') || (errno == ERANGE)){
            log_error("ERROR: incorrectly parsed numbers");
        }

        if(design->selected_modules().size() > 1){
			log_error("ERROR: more that one module selected");
		}

        Module *mod = design->selected_modules()[0];

        log_assert(!mod->has_memories());
	    log_assert(!mod->has_processes());


        std::vector<RTLIL::IdString> cell_names;
        std::map<RTLIL::IdString, int> cell_names_to_indices;
        std::vector<std::vector<int> > previous_cells;
        std::vector<bool> is_flip_flop;

        std::map<Wire*, size_t> wire_to_index;
        std::vector<std::vector<int> > wire_used_as_input_for, wire_used_as_output_for;

        for(Cell *cell : mod->cells()){
            cell_names_to_indices[cell->name] = ((int) cell_names.size());
            cell_names.push_back(cell->name);
            previous_cells.push_back(std::vector<int>());
            is_flip_flop.push_back(RTLIL::builtin_ff_cell_types().count(cell->type) > 0);
        }

        for(Cell *cell : mod->cells()){
            int cell_id = cell_names_to_indices[cell->name];
            for(auto x : cell->connections()){
                IdString name = x.first;
                SigSpec sig = x.second;
                assert((cell->input(name)) || (cell->output(name)));
                assert((!cell->input(name)) || (!cell->output(name)));


                for(SigBit bit : sig.bits()){
                    if(bit.is_wire()){
                        Wire* wire = bit.wire;
                        std::cout << "radau wire su adresu: " << wire << std::endl;
                        if(wire_to_index.find(wire) == wire_to_index.end()){
                            wire_to_index[wire] = wire_to_index.size();
                            wire_used_as_input_for.push_back(std::vector<int>());
                            wire_used_as_output_for.push_back(std::vector<int>());
                        }

                        int wire_id = wire_to_index[wire];
                        if(cell->input(name)){
                            wire_used_as_input_for[wire_id].push_back(cell_id);
                        }
                        else if(cell->output(name)){
                            wire_used_as_output_for[wire_id].push_back(cell_id);
                        }
                        else{
                            assert(false);
                        }
                    }
                }
            }
        }

        for(size_t i = 0; i < wire_to_index.size(); i++){
            for(int inp_cell : wire_used_as_input_for[i]){
                for(int out_cell : wire_used_as_output_for[i]){
                    previous_cells[out_cell].push_back(inp_cell);
                }
            }
        }

        const int INF_DIST = 1e9;
        int cell_count = ((int) cell_names.size());
        assert(cell_count < INF_DIST);
        std::vector<int> dist(cell_count, INF_DIST);

        // Now we run bfs on the dependency tree
        std::deque<int> q;

        Wire *final_wire = mod->wire(RTLIL::escape_id(wire_name));
        if(final_wire == NULL){
            log_error("ERROR: Final wire not found");
        }

        std::cout << "final wire index is: " << final_wire << std::endl;

        if(wire_to_index.find(final_wire) == wire_to_index.end()){
            log_error("ERROR: final wire was not processed during the dependency analysis");
        }
        int last_id = wire_to_index[final_wire];

        for(int final_cell : wire_used_as_output_for[last_id]){
            q.push_back(final_cell);
            dist[final_cell] = 0;
        }

        while(!q.empty()){
            int cur = q.front();
            q.pop_front();

            int time_here = is_flip_flop[cur] ? 1 : 0;
            for(int pr : previous_cells[cur]){
                if(dist[pr] > dist[cur] + time_here){
                    dist[pr] = dist[cur] + time_here;
                    if(time_here == 0){
                        q.push_front(pr);
                    }
                    else{
                        q.push_back(pr);
                    }
                }
            }
        }

        Module *predictor_module = new Module();
		predictor_module->name = IdString(RTLIL::escape_id("predictor_" + RTLIL::unescape_id(mod->name.str())));
        vector<Wire*> predictor_wires(wire_to_index.size(), nullptr);
        
        for(auto [wire, wire_id] : wire_to_index){
            bool wire_needed = false;
            for(int out_cell : wire_used_as_output_for[wire_id]){
                if(dist[out_cell] <= hist_len){
                    wire_needed = true;
                }
            }
            for(int inp_cell : wire_used_as_input_for[wire_id]){
                if(dist[inp_cell] <= hist_len){
                    wire_needed = true;
                }
            }

            if(wire_needed){
                Wire* new_wire = predictor_module->addWire(wire->name, wire);
                assert(new_wire != nullptr);
                predictor_wires[wire_id] = new_wire;
            }

        }

        for(size_t cell_id = 0; cell_id < cell_names.size(); cell_id++){
            if(dist[cell_id] > hist_len){
                continue;
            }
            Cell *cell_in_mod = mod->cell(cell_names[cell_id]);

            Cell *cell_in_predictor = predictor_module->addCell(cell_in_mod->name, cell_in_mod->type);
            cell_in_predictor->parameters = cell_in_mod->parameters;
        	cell_in_predictor->attributes = cell_in_mod->attributes;
            for(auto [specName, sigSpec] : cell_in_mod->connections()){
                vector<RTLIL::SigChunk> sigChunks;
                for(auto chunk : sigSpec.chunks()){
                    SigChunk newChunk = SigChunk(chunk);
                    if(chunk.wire != NULL){
                        size_t wire_id = wire_to_index[chunk.wire];
                        assert(predictor_wires[wire_id] != NULL);
                        newChunk.wire = predictor_wires[wire_id];
                        
                    }
                    sigChunks.push_back(newChunk);
                }
                cell_in_predictor->setPort(specName, sigChunks);
            }
            
        }

        predictor_module->fixup_ports();

        design->add(predictor_module);

	}


} ExtractDependencies;

PRIVATE_NAMESPACE_END
