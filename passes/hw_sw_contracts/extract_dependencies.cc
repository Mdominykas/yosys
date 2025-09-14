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

        typedef Wire PredWire;
        typedef Wire ModWire;
        typedef Wire PredInpWire;

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

        if(!mod->connections().empty()){
            log_error("ERROR: module's connections are not empty. Run opt_clean pass before");
        }

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

        ModWire *final_wire = mod->wire(RTLIL::escape_id(wire_name));
        if(final_wire == NULL){
            log_error("ERROR: Final wire not found");
        }

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
        vector<PredWire*> predictor_wires(wire_to_index.size(), nullptr);
        std::set<ModWire*> wires_for_inputs;
        std::map<ModWire*, PredWire*> module_wire_to_predictor_wire;
        std::map<PredWire*, ModWire*> predictor_wire_to_module_wire;

        std::set<PredWire*> relevant_ff_wires_in_predictor;

        std::map<PredWire*, PredInpWire*> use_in_pred;
        
        for(auto [wire, wire_id] : wire_to_index){
            bool wire_needed = false;
            bool should_be_used_as_input = wire_used_as_output_for[wire_id].empty();
            bool should_be_used_as_ff = false;
            for(int out_cell : wire_used_as_output_for[wire_id]){
                if(dist[out_cell] <= hist_len){
                    wire_needed = true;
                    if((dist[out_cell] <= hist_len) && (is_flip_flop[out_cell])){
                        should_be_used_as_ff = true;
                    }
                }
            }
            for(int inp_cell : wire_used_as_input_for[wire_id]){
                if(dist[inp_cell] <= hist_len){
                    wire_needed = true;
                }
            }


            if(wire_needed){
                PredWire* new_wire = predictor_module->addWire(wire->name, wire);

                use_in_pred[new_wire] = new_wire;

                if(should_be_used_as_input){
                    // std::cout << "Kaip inputas turi buti naudojama wire: " << wire->name.str() << std::endl;
                    wires_for_inputs.insert(wire);
                }
                new_wire->port_input = false;
                new_wire->port_output = false;
                assert(new_wire != nullptr);
                predictor_wires[wire_id] = new_wire;
                module_wire_to_predictor_wire[wire] = new_wire;
                predictor_wire_to_module_wire[new_wire] = wire;
                if(should_be_used_as_ff){
                    relevant_ff_wires_in_predictor.insert(new_wire);
                }
            }

        }

        log_assert(module_wire_to_predictor_wire.find(final_wire) != module_wire_to_predictor_wire.end());
        module_wire_to_predictor_wire[final_wire]->port_output = true;

        std::map<ModWire*, PredInpWire*> module_wire_to_input_wire;

        std::map<Wire*, Wire*> input_wire_to_use_instead_of_predictor_wire;

        for(ModWire *wire : wires_for_inputs){
            IdString input_name = IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire->name.str())));
            // std::cout << "pridesiu inputo wire: " << input_name.str() << std::endl;
            
            PredInpWire* input_wire = predictor_module->addWire(input_name, wire);
            input_wire->port_input = true;
            input_wire->port_output = false;
            module_wire_to_input_wire[wire] = input_wire;
            use_in_pred[module_wire_to_predictor_wire[wire]] = input_wire;
        }

        for(PredWire *wire : relevant_ff_wires_in_predictor){
            IdString input_name = IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire->name.str())));
            // std::cout << "(del ff) pridesiu inputo wire: " << input_name.str() << std::endl;
            
            PredInpWire* input_wire = predictor_module->addWire(input_name, wire);
            input_wire->port_input = true;
            input_wire->port_output = false;
            module_wire_to_input_wire[predictor_wire_to_module_wire[wire]] = input_wire;
            use_in_pred[wire] = input_wire;
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
                        if((cell_in_mod->input(specName)) && (relevant_ff_wires_in_predictor.find(newChunk.wire) != relevant_ff_wires_in_predictor.end())){
                            newChunk.wire = use_in_pred[newChunk.wire];
                        }
                        else if (wires_for_inputs.find(predictor_wire_to_module_wire[newChunk.wire]) != wires_for_inputs.end()){
                            newChunk.wire = use_in_pred[newChunk.wire];
                        }
                        assert(newChunk.wire->module == predictor_module);
                    }
                    sigChunks.push_back(newChunk);
                }
                cell_in_predictor->setPort(specName, sigChunks);
            }
            
        }

        predictor_module->fixup_ports();

        design->add(predictor_module);

        Cell* predictor_cell = mod->addCell(RTLIL::escape_id(wire_name + "_predictor"), predictor_module->name);
        for(Wire *wire : wires_for_inputs){
            IdString input_name = IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire->name.str())));

            predictor_cell->setPort(input_name, SigSpec(wire));
        }

        for(Wire *wire : relevant_ff_wires_in_predictor){
            IdString input_name = IdString(RTLIL::escape_id("inp_" + RTLIL::unescape_id(wire->name.str())));

            predictor_cell->setPort(input_name, SigSpec(predictor_wire_to_module_wire[wire]));
        }


        Wire *pred_out = mod->addWire(RTLIL::escape_id(wire_name + "_pred"), final_wire);
        predictor_cell->setPort(module_wire_to_predictor_wire[final_wire]->name, SigSpec(pred_out));

        // TODO: figure out, when I need to call this function and what it does
        predictor_module->fixup_ports();
        mod->fixup_ports();

        // unrolling layers of sequentionality

        vector<vector<Cell*> > cells_in_layers = {{}};
        for(Cell *pred_cell : predictor_module->cells()){
            cells_in_layers.back().push_back(pred_cell);
        }

        for(int level = 0; level < conf.prediction_bound; level++){
            // we are on level 'level' and we are constructing level 'level+1'
            cells_in_layers.push_back({});
            for(Cell* cell : cells_in_layers[level]){
                IdString new_name = this->next_level_name(cell->name, level);
                std::cout << "pridesiu cell pavadinimu: " << new_name.str() << " ir tipu: " << cell->type.str() << std::endl;
                Cell* new_cell = predictor_module->addCell(new_name, cell);
                cells_in_layers.back().push_back(new_cell);
            }
            std::map<PredWire*, PredWire*> wire_in_next_layer;

            for(Cell *cell : cells_in_layers.back()){
                for(auto [name, sigSpec] : cell->connections()){
                    for(auto chunk : sigSpec.chunks()){
                        if((chunk.wire != NULL) && (wire_in_next_layer.find(chunk.wire) == wire_in_next_layer.end())){
                            IdString new_name = this->next_level_name(chunk.wire->name, level);
                            Wire* new_wire = predictor_module->addWire(new_name, chunk.wire);
                            new_wire->port_input = false;
                            new_wire->port_output = false;
                            wire_in_next_layer[chunk.wire] = new_wire;
                        }
                    }
                }
            }


            for(Cell *cell : cells_in_layers.back()){
                vector<std::pair<IdString, SigSpec> > new_connections;
                for(auto [name, sigSpec] : cell->connections()){
                    vector<RTLIL::SigChunk> sigChunks;
                    for(auto chunk : sigSpec.chunks()){
                        SigChunk newChunk = SigChunk(chunk);
                        if(chunk.wire != NULL){
                            std::cout << "(konstruojant lygius) vietoje wire: " << newChunk.wire->name.str() << " naudosiu: " << wire_in_next_layer[chunk.wire]->name.str() << std::endl;
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
        }

        std::cout << "uzeinu i ff taisymo vieta" << std::endl;
        for(int level = 0; level < conf.prediction_bound; level++){
            std::cout << "dabar level = " << level << std::endl;
            std::map<SigBit, SigBit> replace_bit_with_past;
            for(size_t i = 0; i < cells_in_layers[level + 1].size(); i++){
                Cell *cell = cells_in_layers[level + 1][i];
                Cell *prev_cell = cells_in_layers[level][i];
                std::cout << "lyginsiu cellus: " << cell->name.str() << " su praeitu: " << prev_cell->name.str() << std::endl;
                if(cell->type == IdString("$dff")){
                    // ff outputs on current level
                    vector<SigBit> outBits = cell->connections().at(IdString("\\Q")).bits();

                    // inputs on the previous layer
                    vector<SigBit> inpBits = prev_cell->connections().at(IdString("\\D")).bits();
                    assert(inpBits.size() == outBits.size());
                    for(size_t index = 0; index < inpBits.size(); index++){
                        if(outBits[index].is_wire()){
                            replace_bit_with_past[outBits[index]] = inpBits[index];
                            
                            // debuginimui
                            std::cout << "nutariu pakesti: " << outBits[index].wire->name.str() << " with: ";
                            if(inpBits[index].is_wire()){
                                std::cout << inpBits[index].wire->name.str() << std::endl;
                            }
                            else{
                                std::cout << " kazkokia reiksme" << std::endl;
                            }

                        }
                    }
                }
            }

            for(Cell *cell : cells_in_layers[level + 1]){
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

        predictor_module->fixup_ports();
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


} ExtractDependencies;

PRIVATE_NAMESPACE_END
