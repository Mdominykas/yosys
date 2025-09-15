#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "passes/hw_sw_contracts/configuration_file.cc"
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
		log("    extract_dependencies <important_wire> <hist_len> <configuration_file>\n");
		log("\n");
	}

    typedef Wire PredWire;
    typedef Wire ModWire;
    typedef Wire PredInpWire;


	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing EXTRACT_DEPENDENCIES pass.\n");

        if(args.size() != 4){
			log_error("ERROR: Incorrect number of arguments");
		}

        std::string wire_name = args[1];

        char *endptr;
        errno = 0;
        int hist_len = strtol(args[2].c_str(), &endptr, 10);
        
        if((endptr == args[2].c_str()) || (*endptr != '\0') || (errno == ERANGE)){
            log_error("ERROR: incorrectly parsed numbers");
        }

        hist_len = 100;

        if(design->selected_modules().size() > 1){
			log_error("ERROR: more that one module selected");
		}

        ConfigurationFile conf = ConfigurationFile(args[3]);

        Module *mod = design->selected_modules()[0];

        Wire *clock_wire = mod->wire(IdString(RTLIL::escape_id(conf.clock_name)));
        if(clock_wire == nullptr){
            log_error("ERROR: clock wire not found");
        }

        log_assert(!mod->has_memories());
	    log_assert(!mod->has_processes());


        if(!mod->connections().empty()){
            log_error("ERROR: module's connections are not empty. Run opt_clean pass before");
        }

        ModWire *final_wire = mod->wire(RTLIL::escape_id(wire_name));
        if(final_wire == NULL){
            log_error("ERROR: Final wire not found");
        }


        Module *predictor_module = new Module();
		predictor_module->name = IdString(RTLIL::escape_id("predictor_" + RTLIL::unescape_id(mod->name.str())));

        vector<Cell*> cells_to_add_to_pred = this->find_reverse_reachable_cells(mod, final_wire, clock_wire, hist_len);


        std::set<Wire*> input_wires;

        vector<vector<Cell*> > cells_in_layers;
    
        vector<Cell*> first_layer = add_layer_of_cells(predictor_module, cells_to_add_to_pred, input_wires, 0);
        cells_in_layers.push_back(first_layer);

        // construction of all the layers
        for(int level = 1; level < conf.prediction_bound; level++){
            vector<Cell*> new_cells = this->add_layer_of_cells(predictor_module, cells_in_layers.back(), input_wires, level);
            cells_in_layers.push_back(new_cells);
        }


        // rewire ff wires
        // assert(conf.prediction_bound == ((int) cells_in_layers.size()));
        // for(int level = 1; level < conf.prediction_bound; level++){
        //     rewire_ff_to_previous_level(cells_in_layers[level - 1], cells_in_layers[level]);
        // }

        // add_ff_data_as_module_inputs(predictor_module, cells_in_layers[0]);

        add_predictor_module_to_main_module(design, mod, predictor_module);
	}






    vector<Cell*> find_reverse_reachable_cells(Module* mod, Wire *final_wire, Wire *clock_wire, int hist_len){
        std::vector<RTLIL::IdString> cell_names;
        std::map<RTLIL::IdString, int> cell_names_to_indices;
        std::vector<std::vector<int> > previous_cells;
        std::vector<bool> is_flip_flop;

        std::map<ModWire*, size_t> wire_to_index;
        std::vector<std::vector<int> > wire_used_as_input_for, wire_used_as_output_for;

        for(Cell *cell : mod->cells()){
            cell_names_to_indices[cell->name] = ((int) cell_names.size());
            cell_names.push_back(cell->name);
            previous_cells.push_back(std::vector<int>());
            bool is_flip_flop_cell = false;
            if(RTLIL::builtin_ff_cell_types().count(cell->type) > 0){
                auto con = cell->connections();
                if(cell->type != IdString("$dff")){
                    log_error("ERROR: all flip flops should have been converted to the '$dff' type");
                }

                auto c_name = IdString("\\CLK");
                auto sigspec = con.at(c_name);
                if((sigspec.is_wire()) && (sigspec.as_wire() == clock_wire)){
                    is_flip_flop_cell = true;
                }
            }
            is_flip_flop.push_back(is_flip_flop_cell);
        }

        for(Cell *cell : mod->cells()){
            int cell_id = cell_names_to_indices[cell->name];
            for(auto [sigName, sig] : cell->connections()){
                assert((cell->input(sigName)) || (cell->output(sigName)));
                assert((!cell->input(sigName)) || (!cell->output(sigName)));


                for(SigBit bit : sig.bits()){
                    if(bit.is_wire()){
                        Wire* wire = bit.wire;
                        if(wire_to_index.find(wire) == wire_to_index.end()){
                            wire_to_index[wire] = wire_to_index.size();
                            wire_used_as_input_for.push_back(std::vector<int>());
                            wire_used_as_output_for.push_back(std::vector<int>());
                        }

                        int wire_id = wire_to_index[wire];
                        if(cell->input(sigName)){
                            wire_used_as_input_for[wire_id].push_back(cell_id);
                        }
                        else if(cell->output(sigName)){
                            wire_used_as_output_for[wire_id].push_back(cell_id);
                        }
                        else{
                            assert(false);
                        }
                    }
                }

            }
        }

        
        int last_id = wire_to_index[final_wire];

        for(size_t i = 0; i < wire_to_index.size(); i++){

        for(int inp_cell : wire_used_as_input_for[i]){
                for(int out_cell : wire_used_as_output_for[i]){
                    previous_cells[inp_cell].push_back(out_cell);
                }
            }
        }

        const int INF_DIST = 1e9;
        int cell_count = ((int) cell_names.size());
        assert(cell_count < INF_DIST);
        std::vector<int> dist(cell_count, INF_DIST);

        // Now we run bfs on the dependency tree
        std::deque<int> q;


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

        if(wire_to_index.find(final_wire) == wire_to_index.end()){
            log_error("ERROR: final wire was not processed during the dependency analysis");
        }

        vector<Cell*> ans;
        for(size_t index = 0; index < cell_names.size(); index++){
            if(dist[index] <= hist_len){
                ans.push_back(mod->cell(cell_names[index]));
            }
        }
        return ans;
    }

    vector<Cell*> add_layer_of_cells(Module *predictor_module, vector<Cell*> layer, std::set<Wire*> &input_wires, int level){
        vector<Cell*> new_layer;
        
        // create new cells
        for(Cell* cell : layer){
            IdString new_name = this->next_level_name(cell->name, level);
            Cell* new_cell = predictor_module->addCell(new_name, cell);
            new_layer.push_back(new_cell);
        }
        std::map<Wire*, PredWire*> wire_in_next_layer;

        // construction of all the wires
        for(Cell *cell : new_layer){
            for(auto [name, sigSpec] : cell->connections()){
                for(auto chunk : sigSpec.chunks()){
                    if((chunk.wire != NULL) && (wire_in_next_layer.find(chunk.wire) == wire_in_next_layer.end())){
                        IdString new_name = this->next_level_name(chunk.wire->name, level);
                        Wire* new_wire;
                        if((chunk.wire->module != predictor_module) && (chunk.wire->port_input)){
                            IdString input_wire_name = rename_to_input_wire(chunk.wire->name);
                            new_wire = predictor_module->addWire(input_wire_name, chunk.wire);
                            new_wire->port_input = true;
                            input_wires.insert(new_wire);

                        }
                        else if(input_wires.find(chunk.wire) == input_wires.end()){
                            new_wire = predictor_module->addWire(new_name, chunk.wire);
                            new_wire->port_input = false;
                        }
                        else{
                            new_wire = chunk.wire;
                        }
                         
                        assert(new_wire->module == predictor_module);
                        new_wire->port_output = false;
                        wire_in_next_layer[chunk.wire] = new_wire;
                    }
                }
            }
        }

        // wiring wires to the same level
        for(Cell *cell : new_layer){
            vector<std::pair<IdString, SigSpec> > new_connections;
            for(auto [name, sigSpec] : cell->connections()){
                vector<RTLIL::SigChunk> sigChunks;
                for(auto chunk : sigSpec.chunks()){
                    SigChunk newChunk = SigChunk(chunk);
                    if(chunk.wire != NULL){
                        newChunk.wire = wire_in_next_layer[chunk.wire];
                    }
                    sigChunks.push_back(newChunk);
                }

                SigSpec sig_before_ff = SigSpec(sigChunks);
                new_connections.push_back({name, sig_before_ff});
            }
            for(auto [name, sigSpec] : new_connections){
                cell->setPort(name, sigSpec);
            }
        }
        return new_layer;
    }

    void add_ff_data_as_module_inputs(Module *predictor_module, vector<Cell*> first_layer){
        // rewire input wires to use the main input wire
        // for(int level = 0; level < conf.prediction_bound; level++){
        //     for(Cell *cell : cells_in_layers[level + 1]){
        //         vector<std::pair<IdString, SigSpec> > new_connections;
        //         for(auto [name, sigSpec] : cell->connections()){
        //             vector<SigBit> sig_bits = sigSpec.bits();
        //             for(size_t i = 0; i < sig_bits.size(); i++){
        //                 if(main_input_wire.find(sig_bits[i].wire) != main_input_wire.end()){
        //                     sig_bits[i].wire = main_input_wire[sig_bits[i].wire];
        //                 }
        //             }

        //             SigSpec sig_after_ff = SigSpec(sig_bits);
        //             new_connections.push_back({name, sig_after_ff});
        //         }
        //         for(auto [name, sigSpec] : new_connections){
        //             cell->setPort(name, sigSpec);
        //         }
        //     }
        // }

    }

    void rewire_ff_to_previous_level(vector<Cell*> previous_level, vector<Cell*> current_level){
        std::map<SigBit, SigBit> replace_bit_with_past;

        // find how ff output wires should be rewired
        for(size_t i = 0; i < current_level.size(); i++){
            Cell *cell = current_level[i];
            Cell *prev_cell = previous_level[i];
            if(cell->type == IdString("$dff")){
                // ff outputs on current level
                vector<SigBit> outBits = cell->connections().at(IdString("\\Q")).bits();

                // inputs on the previous layer
                vector<SigBit> inpBits = prev_cell->connections().at(IdString("\\D")).bits();
                assert(inpBits.size() == outBits.size());
                for(size_t index = 0; index < inpBits.size(); index++){
                    if(outBits[index].is_wire()){
                        replace_bit_with_past[outBits[index]] = inpBits[index];

                    }
                }
            }
        }

        // rewire ff output wires
        for(Cell *cell : current_level){
            vector<std::pair<IdString, SigSpec> > new_connections;
            for(auto [name, sigSpec] : cell->connections()){
                vector<SigBit> sig_bits = sigSpec.bits();
                for(size_t i = 0; i < sig_bits.size(); i++){
                    if(replace_bit_with_past.find(sig_bits[i]) != replace_bit_with_past.end()){
                        sig_bits[i] = replace_bit_with_past[sig_bits[i]];
                    }
                }

                SigSpec sig_after_ff = SigSpec(sig_bits);
                new_connections.push_back({name, sig_after_ff});
            }
            for(auto [name, sigSpec] : new_connections){
                cell->setPort(name, sigSpec);
            }
        }

    }

    void add_predictor_module_to_main_module(Design *design, Module *mod, Module *predictor_module){
        predictor_module->fixup_ports();
        design->add(predictor_module);

        // TODO: write this part
        // Cell* predictor_cell = mod->addCell(RTLIL::escape_id(wire_name + "_predictor"), predictor_module->name);
        // for(Wire *wire : wires_for_inputs){
        //     IdString input_name = IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire->name.str())));

        //     predictor_cell->setPort(input_name, SigSpec(wire));
        // }

        // for(Wire *wire : relevant_ff_wires_in_predictor){
        //     IdString input_name = IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire->name.str())));

        //     predictor_cell->setPort(input_name, SigSpec(predictor_wire_to_module_wire[wire]));
        // }

        // Wire *pred_out = mod->addWire(RTLIL::escape_id(wire_name + "_pred"), final_wire);
        // predictor_cell->setPort(module_wire_to_predictor_wire[final_wire]->name, SigSpec(pred_out));

        // mod->fixup_ports();

    }

    IdString next_level_name(IdString name, int next_level){
        if(next_level == 0){
            return IdString(name.str() + "_level_" + std::to_string(next_level));
        }
        else{
            assert(next_level > 0);
            std::string cur_name = name.str();
            size_t to_remove = std::to_string(next_level - 1).size();
            assert(cur_name.size() > to_remove);
            for(size_t i = 0; i < to_remove; i++){
                cur_name.pop_back();
            }

            return IdString(cur_name + std::to_string(next_level));
        }
    }

    IdString rename_to_input_wire(IdString wire_name){
        return IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire_name)));
    }

} ExtractDependencies;

PRIVATE_NAMESPACE_END
