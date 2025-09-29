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
	ConfigurationFile conf;
    
    ExtractDependencies() : Pass("extract_dependencies", "Finds all the cells that are influencing that wire and puts them in a separate module") { }
	void help() override
	{
		log("\n");
		log("    extract_dependencies <configuration_file>\n");
		log("\n");
	}

    typedef Wire PredWire;
    typedef Wire ModWire;
    typedef Wire PredInpWire;


	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing EXTRACT_DEPENDENCIES pass.\n");

        if(args.size() != 2){
			log_error("Incorrect number of arguments\n");
		}

        // TODO: make it exact
        int hist_len = 100;

        if(design->selected_modules().size() > 1){
			log_error("More that one module selected\n");
		}

        conf = ConfigurationFile(args[1]);

        Module *mod = design->selected_modules()[0];

        Wire *clock_wire = mod->wire(IdString(RTLIL::escape_id(conf.clock_name)));
        if(clock_wire == nullptr){
            log_error("Clock wire not found\n");
        }

        log_assert(!mod->has_memories());
	    log_assert(!mod->has_processes());
        
        deal_with_connections(mod);
        log_assert(mod->connections().empty());

        for(PredictorConfiguration pred_conf : conf.predictors){
            construct_predictor(design, mod, clock_wire, pred_conf, hist_len);
        }

	}

    void construct_predictor(Design *design, Module *mod, Wire *clock_wire, PredictorConfiguration pred_conf, int hist_len){
            std::string wire_name = pred_conf.output_wire;
            ModWire *final_wire = mod->wire(RTLIL::escape_id(wire_name));
            if(final_wire == NULL){
                log_error("Final wire not found\n");
            }

            Module *predictor_module = new Module();
            predictor_module->name = IdString(RTLIL::escape_id(pred_conf.output_name_in_pred + "_predictor_"));

            // we also want to add all the wires that influence the retirements
            vector<Wire*> relevant_wires;
            for(auto name_vec : {pred_conf.enter_wires, pred_conf.busy_wires, pred_conf.exit_wires}){
                for(std::string name : name_vec){
                    Wire *wire = mod->wire(RTLIL::escape_id(name));
                    assert(wire != nullptr);
                    relevant_wires.push_back(wire);
                }
            }
            vector<Cell*> cells_to_add_to_pred = this->find_reverse_reachable_cells(mod, final_wire, clock_wire, relevant_wires, hist_len);


            std::set<Wire*> input_wires;

            vector<vector<Cell*> > cells_in_layers;
        
            vector<Cell*> first_layer = add_layer_of_cells(predictor_module, cells_to_add_to_pred, input_wires, 0);
            cells_in_layers.push_back(first_layer);

            // construction of all the layers
            for(int level = 1; level < pred_conf.prediction_bound; level++){
                vector<Cell*> new_cells = this->add_layer_of_cells(predictor_module, cells_in_layers.back(), input_wires, level);
                cells_in_layers.push_back(new_cells);
            }

            add_ff_data_as_module_inputs(predictor_module, cells_in_layers[0]);

            // rewire ff wires
            assert(pred_conf.prediction_bound == ((int) cells_in_layers.size()));
            // for(int level = 1; level < conf.prediction_bound; level++){
            for(int level = 1; level < pred_conf.prediction_bound; level++){
                rewire_ff_to_previous_level(cells_in_layers[level - 1], cells_in_layers[level]);
            }

            vector<IdString> stage_retirement_wires;
            for(auto name : pred_conf.exit_wires){
                Wire *wire = mod->wire(RTLIL::escape_id(name));

                if(wire == nullptr){
                    log_error("Retirement wire named %s not found in the module", name.c_str());
                }

                stage_retirement_wires.push_back(wire->name);
            }
            add_output(predictor_module, final_wire->name, stage_retirement_wires, pred_conf);

            remove_ff_cells(predictor_module);

            add_predictor_module_to_main_module(design, mod, wire_name, predictor_module, pred_conf);
    }

    vector<Cell*> find_reverse_reachable_cells(Module* mod, Wire *final_wire, Wire *clock_wire, vector<Wire*> relevant_wires, int hist_len){
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
                    log_error("All flip flops should have been converted to the '$dff' type");
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
                bool is_input_or_output = ((cell->input(sigName)) || (cell->output(sigName)));
                if(!is_input_or_output){
                    std::cout << "Cell " << cell->name.str() << " has connection named: " << sigName.str() << ", that is neither input nor output" << std::endl;
                    assert(is_input_or_output);
                }
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

        // this is done in case retirement wire doesn't influence anything later on
        relevant_wires.push_back(final_wire);
        for(Wire *last_wire : relevant_wires){
            assert(wire_to_index.find(last_wire) != wire_to_index.end());
            int last_id = wire_to_index[last_wire];
            for(int final_cell : wire_used_as_output_for[last_id]){
                q.push_back(final_cell);
                dist[final_cell] = 0;
            }
        }

        std::set<int> visited;
        while(!q.empty()){
            int cur = q.front();
            q.pop_front();
            if(visited.find(cur) != visited.end()){
                continue;
            }
            visited.insert(cur);

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
            log_error("Final wire was not processed during the dependency analysis");
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
        for(Cell *cell : first_layer){
            if(cell->type != IdString("$dff")){
                continue;
            }

            vector<SigChunk> outChunks = cell->connections().at(IdString("\\Q")).chunks();
            assert(outChunks.size() == 1); // TODO: implement some handling when this doesn't hold
            assert(outChunks[0].wire);
            IdString new_name = rename_to_input_wire(name_without_level(outChunks[0].wire->name));
            Wire *new_wire = predictor_module->addWire(new_name, outChunks[0].wire->width);
            new_wire->port_input = true;

            cell->setPort(IdString("\\D"), new_wire);
        }

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
                if((cell->type == IdString("$dff")) && (cell->output(name))){
                    continue;
                }
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

    void add_predictor_module_to_main_module(Design *design, Module *mod, std::string final_wire_name, Module *predictor_module, PredictorConfiguration pred_conf){
        predictor_module->fixup_ports();
        design->add(predictor_module);

        Wire *final_wire = mod->wire(RTLIL::escape_id(final_wire_name));

        Cell* predictor_cell = mod->addCell(RTLIL::escape_id(pred_conf.output_name_in_mod + "_cell"), predictor_module->name);
        vector<Wire*> input_wires;
        for(Wire *wire : predictor_module->wires()){
            if(wire->port_input){
                input_wires.push_back(wire);
            }
        }


        for(Wire *wire : input_wires){
            IdString pred_input_name = wire->name;
            IdString input_name = remove_input_from_wire_name(pred_input_name);

            predictor_cell->setPort(pred_input_name, SigSpec(mod->wire(input_name)));
        }

        Wire *pred_out = mod->addWire(RTLIL::escape_id(pred_conf.output_name_in_mod), final_wire);
        predictor_cell->setPort(final_output_in_pred_name(pred_conf), SigSpec(pred_out));
        pred_out->port_output = true;

        Wire *pred_ret = mod->addWire(RTLIL::escape_id(pred_conf.retirement_name_in_mod));
        predictor_cell->setPort(RTLIL::escape_id(pred_conf.retirement_name_in_pred), pred_ret);
        pred_ret->port_output = true;

        mod->fixup_ports();
    }

    vector<Wire*> get_wires_across_layers(Module *predictor_module, IdString wire_name_in_mod, int number_of_layers){
        vector<Wire*> ans;
        IdString current_name = wire_name_in_mod;
        for(int level = 0; level < number_of_layers; level++){
            current_name = next_level_name(current_name, level);
            Wire *wire = predictor_module->wire(current_name);
            assert(wire != nullptr);
            ans.push_back(wire);
        }
        return ans;
    }

    void add_output(Module *predictor_module, IdString final_wire_name_in_mod, vector<IdString> retirement_wire_names, PredictorConfiguration pred_conf){
        vector<Wire*> retirement_wires = get_first_satisfying_path(predictor_module, retirement_wire_names, pred_conf);
        
        set_output_to_first_matching(predictor_module, final_wire_name_in_mod, retirement_wires, pred_conf);
        set_predictor_retired_wire(predictor_module, retirement_wires, pred_conf);
    }

    vector<Wire*> get_first_satisfying_path(Module *predictor_module, vector<IdString> retirement_wire_names, PredictorConfiguration pred_conf){
        vector<vector<Wire*> > preprocessed_retirements;
        for(size_t retirement_id = 0; retirement_id < retirement_wire_names.size(); retirement_id++){
            IdString retirement = retirement_wire_names[retirement_id];
            vector<Wire*> retirement_wires = get_wires_across_layers(predictor_module, retirement, pred_conf.prediction_bound);
            
            vector<Wire*> preprocessed_layer;

            IdString redux_retirement_name = IdString(name_without_level(retirement_wires[0]->name).str() + "_redux");
            for(size_t layer = 0; layer < retirement_wires.size(); layer++){
                // TODO: this will crash if I use the same retirement twice
                Cell *reduce_or_cell = predictor_module->addCell(redux_retirement_name.str() + "_cell_" + std::to_string(layer), "$reduce_or");

                // reduction to one bit
                SigSpec ret_spec = SigSpec(retirement_wires[layer]);
                reduce_or_cell->setPort("\\A", ret_spec);
                reduce_or_cell->setParam(ID::A_SIGNED, false);
                reduce_or_cell->setParam(ID::A_WIDTH, retirement_wires[layer]->width);
                reduce_or_cell->setParam(ID::Y_WIDTH, 1);
                Wire *reduced_retirement = predictor_module->addWire(redux_retirement_name.str() + "_wire" + std::to_string(layer), 1);
                reduce_or_cell->setPort("\\Y", reduced_retirement);


                Wire *preprocessed_wire = reduced_retirement;
                // or of previous layer
                if(layer > 0){
                    Cell *or_cell = predictor_module->addCell(predictor_module->uniquify(RTLIL::escape_id("or_cell")), "$_OR_");
                    or_cell->setPort(ID::A, preprocessed_wire);
                    or_cell->setPort(ID::B, preprocessed_layer.back());
                    
                    Wire *or_output = predictor_module->addWire(predictor_module->uniquify(RTLIL::escape_id("or_output")));
                    or_cell->setPort(ID::Y, or_output);

                    preprocessed_wire = or_output;
                }

                // and of previous retirement
                if(retirement_id > 0){
                    Cell *and_cell = predictor_module->addCell(predictor_module->uniquify(RTLIL::escape_id("and_cell")), "$_AND_");
                    and_cell->setPort(ID::A, preprocessed_wire);
                    and_cell->setPort(ID::B, preprocessed_retirements.back()[layer]);
                    
                    Wire *and_output = predictor_module->addWire(predictor_module->uniquify(RTLIL::escape_id("and_output")));
                    and_cell->setPort(ID::Y, and_output);

                    preprocessed_wire = and_output;
                }

                preprocessed_layer.push_back(preprocessed_wire);

            }
            preprocessed_retirements.push_back(preprocessed_layer);
        }
        
        return preprocessed_retirements.back();
    }

    void set_output_to_first_matching(Module *predictor_module, IdString final_wire_name_in_mod, vector<Wire*> retirement_wires, PredictorConfiguration pred_conf){
        vector<Wire*> final_wires = get_wires_across_layers(predictor_module, final_wire_name_in_mod, pred_conf.prediction_bound);
        
        assert(!final_wires.empty());
        assert(final_wires.size() == retirement_wires.size());
        
        IdString pred_name = IdString(name_without_level(final_wires[0]->name).str() + "_pred");
        // TODO: will crash when processor are above 32 bits
        assert(final_wires[0]->width <= 32);
        SigSpec prev_val = SigSpec(pred_conf.default_prediction, final_wires[0]->width);
        
        for(int layer = ((int)final_wires.size()) - 1; layer >= 0; layer--){
            // mux
            Cell *mux_cell = predictor_module->addCell(pred_name.str() + "_cell" + std::to_string(final_wires.size() - layer), "$mux");
            
            SigSpec cur_val = SigSpec(final_wires[layer]);
            assert(cur_val.bits().size() == prev_val.bits().size());
            mux_cell->setParam(ID::WIDTH, final_wires[layer]->width);
            mux_cell->setPort(ID::A, prev_val);
            // if S is true, then mux selects ID::B
            mux_cell->setPort(ID::B, cur_val);
            mux_cell->setPort(ID::S, retirement_wires[layer]);
            Wire *pred_out = predictor_module->addWire(pred_name.str() + "_wire" + std::to_string(final_wires.size() - layer), final_wires[layer]->width);
            SigSpec out_val = SigSpec(pred_out);
            mux_cell->setPort(ID::Y, out_val);

            prev_val = out_val;
        }

        Wire *final_predictor_wire = predictor_module->addWire(final_output_in_pred_name(pred_conf), prev_val.chunks()[0].wire);
        predictor_module->connect(final_predictor_wire, prev_val);
        final_predictor_wire->port_output = true;
    }

    void set_predictor_retired_wire(Module *predictor_module, vector<Wire*> retirement_wires, PredictorConfiguration pred_conf){
        assert(!retirement_wires.empty());

        Wire* last_or_result = retirement_wires[0];
        for(size_t i = 1; i < retirement_wires.size(); i++){
            Cell *or_cell = predictor_module->addCell(predictor_module->uniquify(RTLIL::escape_id("or_cell_for_final_retirement")), "$_OR_");
            or_cell->setPort(ID::A, last_or_result);
            or_cell->setPort(ID::B, retirement_wires[i]);
            
            Wire *or_output = predictor_module->addWire(predictor_module->uniquify(RTLIL::escape_id("or_output_for_final_retirement")));
            or_cell->setPort(ID::Y, or_output);

            last_or_result = or_output;
        }

        Wire *retirement_in_pred = predictor_module->addWire(RTLIL::escape_id(pred_conf.retirement_name_in_pred));
        predictor_module->connect(retirement_in_pred, last_or_result);
        retirement_in_pred->port_output = true;
    }

    void deal_with_connections(Module *mod){
        int cnt = 0;
        // TODO: yosys' manual says "$buf" is an experimental feature and and it shouldn't be used.
        // so if something serious breaks I might need to change something
        for(auto [s1, s2] : mod->connections()){
            assert(s1.is_wire()); // output should be a wire
            Cell *buf_cell = mod->addCell("$my_buf_cell_" + std::to_string(cnt), "$buf");
            buf_cell->setParam(ID::WIDTH, s1.size());
            buf_cell->setPort(ID::A, s2);
            buf_cell->setPort(ID::Y, s1); // Y is the output

            cnt++;
        }

        mod->new_connections({});
        mod->fixup_ports();
    }

    IdString name_without_level(IdString name){
        std::string cur_name = name.str();
        while(std::isdigit(cur_name.back())){
            cur_name.pop_back();
            assert(!cur_name.empty());
        }
        std::string lvl = "_level_";
        for(size_t i = 0; i < lvl.size(); i++){
            cur_name.pop_back();
        }

        return IdString(cur_name);
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

    IdString remove_input_from_wire_name(IdString wire_name){
        std::string inp_pref = "inp_";
        std::string name_without_inp = RTLIL::unescape_id(wire_name).substr(inp_pref.size());
        return IdString(RTLIL::escape_id(name_without_inp));
    }

    void remove_ff_cells(Module *predictor_module){
        vector<Cell*> ff_cells;
        for(auto cell : predictor_module->cells()){
            if(cell->type == IdString("$dff")){
                ff_cells.push_back(cell);
            }
        }
        for(auto cell : ff_cells){
            predictor_module->remove(cell);
        }
    }

    IdString final_output_in_pred_name(PredictorConfiguration pred_conf){
        return RTLIL::escape_id(pred_conf.output_name_in_pred);
    }
} ExtractDependencies;

PRIVATE_NAMESPACE_END
