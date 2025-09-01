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
        int hist_len;
        bool extracted_int = stringToInt(args[2], hist_len);
        if(!extracted_int){
            log_error("ERROR: incorrectly parsed int");
        }

        if(design->selected_modules().size() > 1){
			log_error("ERROR: more that one module selected");
		}

        Module *mod = design->selected_modules()[0];

        log_assert(!mod->has_memories());
	    log_assert(!mod->has_processes());


        std::vector<std::string> cell_names;
        std::map<std::string, int> cell_names_to_indices;
        std::vector<std::vector<int> > previous_cells;
        std::vector<bool> is_flip_flop;

        std::map<Wire*, int> wire_to_index;
        std::vector<std::vector<int> > cell_has_wire_as_input, cell_has_wire_as_output;

        for(Cell *cell : mod->cells()){
            cell_names.push_back(cell->name.str());
            cell_names_to_indices[cell->name.str()] = ((int) cell_names_to_indices.size());
            previous_cells.push_back(std::vector<int>());
            is_flip_flop.push_back(RTLIL::builtin_ff_cell_types().count(cell->type) > 0);
        }

        for(Cell *cell : mod->cells()){
            // TODO: 
            // 1. rasti, kurie wires ateina
            // 2. rasti, kurie wires iseina
            // 3. sujungti ateinancius su ieinanciais 
            // 4. paleisti bfs
            int cell_id = cell_names[cell->name];
            for(auto x : cell->connections()){
                IdString name = x.first;
                SigSpec sig = x.second;
                assert((cell->input(name)) || (cell->output(name)));
                assert((!cell->input(name)) || (!cell->output(name)));


                for(SigBit bit : sig.bits()){
                    if(bit.is_wire()){
                        Wire* wire = bit.wire;
                        if(wire_to_index.find(wire) == wire_to_index.end()){
                            wire_to_index[wire] = ((int) wire_to_index.size());
                            cell_has_wire_as_input.push_back(std::vector<int>());
                            cell_has_wire_as_output.push_back(std::vector<int>());
                        }

                        int wire_id = wire_to_index[wire];
                        if(cell->input(name)){
                            cell_has_wire_as_input[wire_id].push_back(cell_id);
                        }
                        if(cell->output(name)){
                            cell_has_wire_as_output[wire_id].push_back(cell_id);
                        }
                    }
                }
            }
        }

        for(int i = 0; i < ((int) wire_to_index.size()); i++){
            for(int inp_cell : cell_has_wire_as_input[i]){
                for(int out_cell : cell_has_wire_as_output[i]){
                    previous_cells[out_cell].push_back(inp_cell);
                }
            }
        }

        const int INF_DIST = 1e9;
        int cell_count = ((int) cell_names.size());
        assert(cell_count < INF_DIST);
        std::vector<int> dist(cell_count, INF_DIST);

        // Now we run bfs on the search tree
        std::deque<int> q;

        Wire *final_wire = mod->wire(RTLIL::escape_id(wire_name));
        if(final_wire == NULL){
            log_error("ERROR: Final wire not found");
        }
        int last_id = wire_to_index[final_wire];

        for(int final_cell : cell_has_wire_as_output[final_wire]){
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
        
        for(auto it : wire_to_index){
            Wire *wire = it.first;
            int wire_id = it.second;
            bool wire_needed = false;
            for(int out_cell : cell_has_wire_as_output[i]){
                if(dist[out_cell] <= hist_len){
                    wire_needed = true;
                }
            }
            if(wire_needed){
                predictor_module->addWire(wire->name, wire);
            }
        }

        for(size_t cell_id = 0; cell_id < cell_names.size(); cell_id++){
            Cell *cell_in_mod = mod->cell(IdString(cell_names[cell_id]));

            Cell *cell_in_predictor = predictor_module->addCell(cell_in_mod->name, cell_in_mod->type);
            cell_in_predictor->parameters = cell_in_mod->parameters;
        	cell_in_predictor->attributes = cell_in_mod->attributes;
            
        }

        predictor_module->fixup_ports();
	}


} ExtractDependencies;

PRIVATE_NAMESPACE_END
