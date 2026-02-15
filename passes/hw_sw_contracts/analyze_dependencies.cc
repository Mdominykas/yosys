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
#include <queue>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN


struct DSU{
    vector<int> dsu;
    vector<vector<int> > adj;
    vector<std::set<int> > sources;
    vector<int> init_sources;
    bool sources_computed = false;
    int n;
    DSU(int n) : n(n){
        adj = vector<vector<int>>(n);
        sources = vector<std::set<int> >(n);
    }

    void connect(int u, int v){
        assert(!sources_computed);
        adj[u].push_back(v);
    }

    void add_source(int source){
        assert(!sources_computed);
        init_sources.push_back(source);
    }

    void compute_sources(){
        assert(!sources_computed);

        for(int init_source : init_sources){
            vector<bool> visited = vector<bool>(n, false);
            std::queue<int> q;
            q.push(init_source);
            while(!q.empty()){
                int g = q.front();
                q.pop();

                if(visited[g]){
                    continue;
                }
                visited[g] = true;

                sources[g].insert(init_source);
                for(int u : adj[g]){
                    q.push(u);
                }
            }
        }

        sources_computed = true;
    }

    std::set<int> get_sources(int v){
        if(!sources_computed){
            compute_sources();
        }
        return sources[v];
    }
};

struct ConfigurationParameters{
    vector<IdString> sinks;
    ConfigurationParameters(string configuration_file){
        std::ifstream input(configuration_file);

        // check if file exists
        if(!input.good()){
            log_error("Invalid configuration file\n");
        }

        int n;
        input >> n;
        for(int i = 0; i < n; i++){
            // TODO: this will fail, when strings are some escaped nonsense
            string s;
            input >> s;
            sinks.push_back(RTLIL::escape_id(s));
        }
    }
};

struct AnalyzeDependencies : public Pass {
    AnalyzeDependencies() : Pass("analyze_dependencies", "Do some analysis on the bits of the circuits") { }
	void help() override
	{
		log("\n");
		log("    analyze_dependencies <configuration_file> <output_file> \n");
		log("\n");
	}

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing ANALYZE_DEPENDENCIES pass.\n");

        if(args.size() != 3){
            assert(false);
        }

        ConfigurationParameters conf = ConfigurationParameters(args[1]);
        string output_file = args[2];

        if(design->selected_modules().size() != 1){
			log_error("More that one module selected\n");
		}

        Module *mod = design->selected_modules()[0];

        std::map<std::pair<IdString, int>, int> wire_bit_to_index;
        std::map<int, std::pair<IdString, int>> index_to_wire_bit;
        for(Wire *wire : mod->wires()){
            for(int i = 0; i < wire->width; i++){
                auto pr = make_pair(wire->name, i);
                int ind = wire_bit_to_index.size();
                wire_bit_to_index[pr] = ind;
                index_to_wire_bit[ind] = pr;
            }
        }

        DSU dsu = DSU(wire_bit_to_index.size());

        vector<int> ff_bits;
        for(Cell *cell : mod->cells()){
            // check for ff cells
            if(RTLIL::builtin_ff_cell_types().count(cell->type) > 0){
                for(auto [portname, sigSpec] : cell->connections()){
                    for(SigBit sigbit: sigSpec.bits()){
                        if(cell->output(portname)){
                            assert(sigbit.wire != NULL);
                            std::pair<IdString, int> pr = make_pair(sigbit.wire->name, sigbit.offset);    
                            int ind = wire_bit_to_index[pr];
                            ff_bits.push_back(ind);
                            dsu.add_source(ind);
                        }
                    }
                }
                continue;
            }

            vector<int> input_indices, output_indices;
            for(auto [portname, sigSpec] : cell->connections()){
                for(SigBit sigbit: sigSpec.bits()){
                    if(sigbit.wire != NULL){
                        std::pair<IdString, int> pr = make_pair(sigbit.wire->name, sigbit.offset);
                        assert(wire_bit_to_index.count(pr) > 0);
                        if(cell->input(portname)){
                            input_indices.push_back(wire_bit_to_index[pr]);
                        }
                        if(cell->output(portname)){
                            output_indices.push_back(wire_bit_to_index[pr]);
                        }
                    }
                }
            }
            for(int inp_id : input_indices){
                for(int out_id : output_indices){
                    dsu.connect(inp_id, out_id);
                }
            }
        }

        vector<std::set<int> > dependencies;
        for(IdString sink : conf.sinks){
            Wire *wire = mod->wire(sink);
            if(wire == NULL){
                const std::string error_msg = "Wire named " + sink.str() + " not found in the module\n";
                log_error("%s", error_msg.c_str());
            }

            std::set<int> dependent_bits;
            for(int i = 0; i < wire->width; i++){
                auto pr = make_pair(wire->name, i);
                assert(wire_bit_to_index.count(pr) > 0);
                int id = wire_bit_to_index.count(pr);
                for(int dep : dsu.get_sources(id)){
                    dependent_bits.insert(dep);
                }
            }
            dependencies.push_back(dependent_bits);
        }

        export_to_json(conf, output_file, dependencies, index_to_wire_bit);
    }

    void export_to_json(ConfigurationParameters conf, string output_file, vector<std::set<int> > dependencies, std::map<int, std::pair<IdString, int>> index_to_wire_bit){
        std::ofstream out(output_file);

        out << "{\n";
        for(int i = 0; i < ((int) conf.sinks.size()); i++){
            out << "{\n";
            std::string sink_name = RTLIL::unescape_id(conf.sinks[i].str());
            out << "\"name\" : " << sink_name << ",\n";
            out << "\"dependencies\" : [\n";
            int cnt = 0;
            for(int dep : dependencies[i]){
                string name = RTLIL::unescape_id(index_to_wire_bit[dep].first);
                int index = index_to_wire_bit[dep].second;
                out << "{\"name\" : " << name << ", \"index\" : " << index << "}";
                if (cnt + 1 < ((int)dependencies[i].size())){
                    out << ",";
                }
                out << "\n";
            }
            out << "]\n";
            out << "}\n";
        }
        out << "}\n";
    }


} AnalyzeDependencies;

PRIVATE_NAMESPACE_END
